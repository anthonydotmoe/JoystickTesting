#ifndef _WIN32_DCOM
#define _WIN32_DCOM
#endif

#include "XInputFilter.h"

#include <oleauto.h>
#include <wbemidl.h>
#include <wchar.h>

namespace {
#define SAFE_DELETE(p)  { if (p) { delete (p);     (p) = nullptr; } }
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }

struct XINPUT_DEVICE_NODE
{
    DWORD dwVidPid;
    XINPUT_DEVICE_NODE* pNext;
};

XINPUT_DEVICE_NODE* g_xinputDeviceList = nullptr;
}

HRESULT SetupForIsXInputDevice()
{
    IWbemServices* services = nullptr;
    IEnumWbemClassObject* enumDevices = nullptr;
    IWbemLocator* locator = nullptr;
    IWbemClassObject* devices[20] = {};
    BSTR deviceId = nullptr;
    BSTR className = nullptr;
    BSTR nameSpace = nullptr;
    DWORD returned = 0;
    [[maybe_unused]] bool cleanupCom = false;
    UINT deviceIndex = 0;
    VARIANT var;
    HRESULT hr;

    hr = CoInitialize(nullptr);
    cleanupCom = SUCCEEDED(hr);

    hr = CoCreateInstance(__uuidof(WbemLocator),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(IWbemLocator),
        reinterpret_cast<LPVOID*>(&locator));
    if (FAILED(hr) || locator == nullptr)
        goto cleanup;

    nameSpace = SysAllocString(L"\\\\.\\root\\cimv2");
    if (nameSpace == nullptr)
        goto cleanup;
    deviceId = SysAllocString(L"DeviceID");
    if (deviceId == nullptr)
        goto cleanup;
    className = SysAllocString(L"Win32_PNPEntity");
    if (className == nullptr)
        goto cleanup;

    hr = locator->ConnectServer(nameSpace, nullptr, nullptr, 0L,
        0L, nullptr, nullptr, &services);
    if (FAILED(hr) || services == nullptr)
        goto cleanup;

    (void)CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, 0);

    hr = services->CreateInstanceEnum(className, 0, nullptr, &enumDevices);
    if (FAILED(hr) || enumDevices == nullptr)
        goto cleanup;

    for (;;)
    {
        hr = enumDevices->Next(10000, 20, devices, &returned);
        if (FAILED(hr))
            goto cleanup;
        if (returned == 0)
            break;

        for (deviceIndex = 0; deviceIndex < returned; deviceIndex++)
        {
            if (!devices[deviceIndex])
                continue;

            hr = devices[deviceIndex]->Get(deviceId, 0L, &var, nullptr, nullptr);
            if (SUCCEEDED(hr) && var.vt == VT_BSTR && var.bstrVal != nullptr)
            {
                if (wcsstr(var.bstrVal, L"IG_"))
                {
                    DWORD pid = 0, vid = 0;
                    WCHAR* strVid = wcsstr(var.bstrVal, L"VID_");
                    if (strVid && swscanf(strVid, L"VID_%4X", &vid) != 1)
                        vid = 0;
                    WCHAR* strPid = wcsstr(var.bstrVal, L"PID_");
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
            SAFE_RELEASE(devices[deviceIndex]);
        }
    }

cleanup:
    if (nameSpace)
        SysFreeString(nameSpace);
    if (deviceId)
        SysFreeString(deviceId);
    if (className)
        SysFreeString(className);
    for (deviceIndex = 0; deviceIndex < 20; deviceIndex++)
        SAFE_RELEASE(devices[deviceIndex]);
    SAFE_RELEASE(enumDevices);
    SAFE_RELEASE(locator);
    SAFE_RELEASE(services);

    return hr;
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
        SAFE_DELETE(toDelete);
    }
    g_xinputDeviceList = nullptr;
}
