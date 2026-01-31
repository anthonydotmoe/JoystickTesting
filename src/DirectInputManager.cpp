#define STRICT
#define DIRECTINPUT_VERSION 0x0800

#include "DirectInputManager.h"

#include "JoystickNetwork.h"
#include "XInputFilter.h"
#include "res.h"

#include <tchar.h>

#pragma warning(push)
#pragma warning(disable : 6000 28251 4996)
#include <dinput.h>
#include <dinputd.h>
#pragma warning(pop)

#include <string>

namespace {
#define SAFE_DELETE(p)  { if (p) { delete (p);     (p) = nullptr; } }
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }

LPDIRECTINPUT8 g_directInput = nullptr;
LPDIRECTINPUTDEVICE8 g_joystick = nullptr;
bool g_filterOutXinputDevices = false;

struct DI_ENUM_CONTEXT
{
    DIJOYCONFIG* preferredJoyCfg;
    bool preferredJoyCfgValid;
};

BOOL CALLBACK EnumObjectsCallback(const DIDEVICEOBJECTINSTANCE* pdidoi, VOID* pContext);
BOOL CALLBACK EnumJoysticksCallback(const DIDEVICEINSTANCE* pdidInstance, VOID* pContext);
}

void SetFilterOutXInputDevices(bool enable)
{
    g_filterOutXinputDevices = enable;
}

void AcquireJoystick()
{
    if (g_joystick)
        g_joystick->Acquire();
}

HRESULT InitDirectInput(HWND hDlg)
{
    HRESULT hr = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
        IID_IDirectInput8, reinterpret_cast<void**>(&g_directInput), nullptr);
    if (FAILED(hr))
        return hr;

    if (g_filterOutXinputDevices)
        SetupForIsXInputDevice();

    DIJOYCONFIG preferredJoyCfg = {};
    DI_ENUM_CONTEXT enumContext = {};
    enumContext.preferredJoyCfg = &preferredJoyCfg;
    enumContext.preferredJoyCfgValid = false;

    IDirectInputJoyConfig8* joyConfig = nullptr;
    hr = g_directInput->QueryInterface(IID_IDirectInputJoyConfig8, reinterpret_cast<void**>(&joyConfig));
    if (FAILED(hr))
        return hr;

    preferredJoyCfg.dwSize = sizeof(preferredJoyCfg);
    if (SUCCEEDED(joyConfig->GetConfig(0, &preferredJoyCfg, DIJC_GUIDINSTANCE)))
        enumContext.preferredJoyCfgValid = true;
    SAFE_RELEASE(joyConfig);

    hr = g_directInput->EnumDevices(DI8DEVCLASS_GAMECTRL,
        EnumJoysticksCallback, &enumContext, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr))
        return hr;

    if (g_filterOutXinputDevices)
        CleanupForIsXInputDevice();

    if (!g_joystick)
    {
        MessageBox(nullptr, TEXT("Joystick not found. The sample will now exit."),
            TEXT("DirectInput Sample"), MB_ICONERROR | MB_OK);
        EndDialog(hDlg, 0);
        return S_OK;
    }

    hr = g_joystick->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(hr))
        return hr;

    hr = g_joystick->SetCooperativeLevel(hDlg, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
    if (FAILED(hr))
        return hr;

    hr = g_joystick->EnumObjects(EnumObjectsCallback, reinterpret_cast<VOID*>(hDlg), DIDFT_ALL);
    if (FAILED(hr))
        return hr;

    return S_OK;
}

void FreeDirectInput()
{
    if (g_joystick)
        g_joystick->Unacquire();

    SAFE_RELEASE(g_joystick);
    SAFE_RELEASE(g_directInput);
}

HRESULT UpdateInputState(HWND hDlg)
{
    HRESULT hr = S_OK;
    TCHAR strText[512] = {};
    DIJOYSTATE2 js = {};

    if (!g_joystick)
        return S_OK;

    hr = g_joystick->Poll();
    if (FAILED(hr))
    {
        hr = g_joystick->Acquire();
        while (hr == DIERR_INPUTLOST)
            hr = g_joystick->Acquire();
        return S_OK;
    }

    hr = g_joystick->GetDeviceState(sizeof(DIJOYSTATE2), &js);
    if (FAILED(hr))
        return hr;

    _stprintf_s(strText, 512, TEXT("%ld"), js.lX);
    SetWindowText(GetDlgItem(hDlg, IDC_X_AXIS), strText);
    _stprintf_s(strText, 512, TEXT("%ld"), js.lY);
    SetWindowText(GetDlgItem(hDlg, IDC_Y_AXIS), strText);
    _stprintf_s(strText, 512, TEXT("%ld"), js.lZ);
    SetWindowText(GetDlgItem(hDlg, IDC_Z_AXIS), strText);
    _stprintf_s(strText, 512, TEXT("%ld"), js.lRx);

    _tcscpy_s(strText, 512, TEXT(""));
    for (int i = 0; i < 128; i++)
    {
        if (js.rgbButtons[i] & 0x80)
        {
            TCHAR sz[128];
            _stprintf_s(sz, 128, TEXT("%02d "), i);
            _tcscat_s(strText, 512, sz);
        }
    }
    SetWindowText(GetDlgItem(hDlg, IDC_BUTTONS), strText);

    std::string joyVariables = "X:" + std::to_string(js.lX) + "," +
        "Y:" + std::to_string(js.lY) + "," +
        "Z:" + std::to_string(js.lZ) + ",";

    SendJoystickUdpBroadcast(joyVariables);

    return PostSampleWinHttp();
}

namespace {
BOOL CALLBACK EnumJoysticksCallback(const DIDEVICEINSTANCE* pdidInstance, VOID* pContext)
{
    auto enumContext = reinterpret_cast<DI_ENUM_CONTEXT*>(pContext);

    if (g_filterOutXinputDevices && IsXInputDevice(&pdidInstance->guidProduct))
        return DIENUM_CONTINUE;

    if (enumContext->preferredJoyCfgValid &&
        !IsEqualGUID(pdidInstance->guidInstance, enumContext->preferredJoyCfg->guidInstance))
        return DIENUM_CONTINUE;

    HRESULT hr = g_directInput->CreateDevice(pdidInstance->guidInstance, &g_joystick, nullptr);
    if (FAILED(hr))
        return DIENUM_CONTINUE;

    return DIENUM_STOP;
}

BOOL CALLBACK EnumObjectsCallback(const DIDEVICEOBJECTINSTANCE* pdidoi, VOID* pContext)
{
    HWND hDlg = reinterpret_cast<HWND>(pContext);

    static int sliderCount = 0;
    static int povCount = 0;

    if (pdidoi->dwType & DIDFT_AXIS)
    {
        DIPROPRANGE diprg = {};
        diprg.diph.dwSize = sizeof(DIPROPRANGE);
        diprg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        diprg.diph.dwHow = DIPH_BYID;
        diprg.diph.dwObj = pdidoi->dwType;
        diprg.lMin = -255;
        diprg.lMax = +255;

        if (FAILED(g_joystick->SetProperty(DIPROP_RANGE, &diprg.diph)))
            return DIENUM_STOP;
    }

    if (pdidoi->guidType == GUID_XAxis)
    {
        EnableWindow(GetDlgItem(hDlg, IDC_X_AXIS), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_X_AXIS_TEXT), TRUE);
    }
    if (pdidoi->guidType == GUID_YAxis)
    {
        EnableWindow(GetDlgItem(hDlg, IDC_Y_AXIS), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_Y_AXIS_TEXT), TRUE);
    }
    if (pdidoi->guidType == GUID_ZAxis)
    {
        EnableWindow(GetDlgItem(hDlg, IDC_Z_AXIS), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_Z_AXIS_TEXT), TRUE);
    }
    if (pdidoi->guidType == GUID_RxAxis)
    {
        EnableWindow(GetDlgItem(hDlg, IDC_X_ROT), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_X_ROT_TEXT), TRUE);
    }
    if (pdidoi->guidType == GUID_RyAxis)
    {
        EnableWindow(GetDlgItem(hDlg, IDC_Y_ROT), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_Y_ROT_TEXT), TRUE);
    }
    if (pdidoi->guidType == GUID_RzAxis)
    {
        EnableWindow(GetDlgItem(hDlg, IDC_Z_ROT), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_Z_ROT_TEXT), TRUE);
    }
    if (pdidoi->guidType == GUID_Slider)
    {
        switch (sliderCount++)
        {
            case 0:
                EnableWindow(GetDlgItem(hDlg, IDC_SLIDER0), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_SLIDER0_TEXT), TRUE);
                break;

            case 1:
                EnableWindow(GetDlgItem(hDlg, IDC_SLIDER1), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_SLIDER1_TEXT), TRUE);
                break;
        }
    }

    if (pdidoi->guidType == GUID_POV)
    {
        switch (povCount++)
        {
            case 0:
                EnableWindow(GetDlgItem(hDlg, IDC_POV0), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_POV0_TEXT), TRUE);
                break;

            case 1:
                EnableWindow(GetDlgItem(hDlg, IDC_POV1), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_POV1_TEXT), TRUE);
                break;

            case 2:
                EnableWindow(GetDlgItem(hDlg, IDC_POV2), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_POV2_TEXT), TRUE);
                break;
        }
    }

    EnableWindow(GetDlgItem(hDlg, IDC_NetResponse), TRUE);
    EnableWindow(GetDlgItem(hDlg, IDC_NetResponse_TEXT), TRUE);

    return DIENUM_CONTINUE;
}
}
