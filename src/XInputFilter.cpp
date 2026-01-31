#ifndef _WIN32_DCOM
#define _WIN32_DCOM
#endif

#include "XInputFilter.h"

#include "ComHelpers.h"
#include "ComPtr.h"

#include <wbemidl.h>
#include <wchar.h>

namespace {
struct XINPUT_DEVICE_NODE
{
    DWORD dwVidPid;
    XINPUT_DEVICE_NODE* pNext;
};

XINPUT_DEVICE_NODE* g_xinputDeviceList = nullptr;
}

HRESULT SetupForIsXInputDevice()
{
    ScopedComInit comInit;
    if (!comInit.ok())
        return comInit.hr();

    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(
        CLSID_WbemLocator,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        reinterpret_cast<LPVOID*>(locator.put()));
    if (FAILED(hr))
        return hr;

    ScopedBstr nameSpace(L"\\\\.\\root\\cimv2");
    ScopedBstr deviceId(L"DeviceID");
    ScopedBstr className(L"Win32_PNPEntity");
    if (!nameSpace.valid() || !deviceId.valid() || !className.valid())
        return E_OUTOFMEMORY;

    ComPtr<IWbemServices> services;
    hr = locator->ConnectServer(nameSpace.get(), nullptr, nullptr, 0L,
        0L, nullptr, nullptr, services.put());
    if (FAILED(hr))
        return hr;

    (void)CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, 0);

    ComPtr<IEnumWbemClassObject> enumDevices;
    hr = services->CreateInstanceEnum(className.get(), 0, nullptr, enumDevices.put());
    if (FAILED(hr))
        return hr;

    for (;;)
    {
        IWbemClassObject* rawDevices[20] = {};
        DWORD returned = 0;
        hr = enumDevices->Next(10000, 20, rawDevices, &returned);
        if (FAILED(hr))
            return hr;
        if (returned == 0)
            break;

        for (DWORD i = 0; i < returned; ++i)
        {
            if (!rawDevices[i])
                continue;

            ComPtr<IWbemClassObject> device;
            device.attach(rawDevices[i]);

            ScopedVariant var;
            hr = device->Get(deviceId.get(), 0L, var.get(), nullptr, nullptr);
            if (SUCCEEDED(hr) && var.value().vt == VT_BSTR && var.value().bstrVal != nullptr)
            {
                if (wcsstr(var.value().bstrVal, L"IG_"))
                {
                    DWORD pid = 0, vid = 0;
                    WCHAR* strVid = wcsstr(var.value().bstrVal, L"VID_");
                    if (strVid && swscanf(strVid, L"VID_%4X", &vid) != 1)
                        vid = 0;
                    WCHAR* strPid = wcsstr(var.value().bstrVal, L"PID_");
                    if (strPid && swscanf(strPid, L"PID_%4X", &pid) != 1)
                        pid = 0;

                    DWORD vidPid = MAKELONG(vid, pid);

                    XINPUT_DEVICE_NODE* newNode = new XINPUT_DEVICE_NODE;
                    if (newNode)
                    {
                        newNode->dwVidPid = vidPid;
                        newNode->pNext = g_xinputDeviceList;
                        g_xinputDeviceList = newNode;
                    }
                }
            }
        }
    }

    return S_OK;
}

bool IsXInputDevice(const GUID* productGuid)
{
    XINPUT_DEVICE_NODE* node = g_xinputDeviceList;
    while (node)
    {
        if (node->dwVidPid == productGuid->Data1)
            return true;
        node = node->pNext;
    }

    return false;
}

void CleanupForIsXInputDevice()
{
    XINPUT_DEVICE_NODE* node = g_xinputDeviceList;
    while (node)
    {
        XINPUT_DEVICE_NODE* toDelete = node;
        node = node->pNext;
        delete toDelete;
    }
    g_xinputDeviceList = nullptr;
}
