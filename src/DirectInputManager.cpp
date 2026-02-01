#define STRICT
#define DIRECTINPUT_VERSION 0x0800

#include "DirectInputManager.h"

#include "ComPtr.h"
#include "JoystickNetwork.h"
#include "XInputFilter.h"
#include "res.h"

#include <tchar.h>
#include <algorithm>
#include <cmath>

#pragma warning(push)
#pragma warning(disable : 6000 28251 4996)
#include <dinput.h>
#include <dinputd.h>
#pragma warning(pop)

namespace {
constexpr double kDeadzoneMagnitude = 20.0; // Raw axis magnitude threshold.
constexpr double kAxisMaxMagnitude = 255.0;
constexpr double kOutputMaxMagnitude = 750.0;
constexpr double kTwistDeadzone = 10.0;
ComPtr<IDirectInput8> g_directInput;
ComPtr<IDirectInputDevice8> g_joystick;
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
        IID_IDirectInput8, reinterpret_cast<void**>(g_directInput.put()), nullptr);
    if (FAILED(hr))
        return hr;

    if (g_filterOutXinputDevices)
        SetupForIsXInputDevice();

    DIJOYCONFIG preferredJoyCfg = {};
    DI_ENUM_CONTEXT enumContext = {};
    enumContext.preferredJoyCfg = &preferredJoyCfg;
    enumContext.preferredJoyCfgValid = false;

    ComPtr<IDirectInputJoyConfig8> joyConfig;
    hr = g_directInput->QueryInterface(IID_IDirectInputJoyConfig8,
        reinterpret_cast<void**>(joyConfig.put()));
    if (FAILED(hr))
        return hr;

    preferredJoyCfg.dwSize = sizeof(preferredJoyCfg);
    if (SUCCEEDED(joyConfig->GetConfig(0, &preferredJoyCfg, DIJC_GUIDINSTANCE)))
        enumContext.preferredJoyCfgValid = true;

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

    g_joystick.reset();
    g_directInput.reset();
}

HRESULT UpdateInputState(HWND hDlg)
{
    HRESULT hr = S_OK;
    TCHAR strText[512] = {};
    DIJOYSTATE2 js = {};
    static bool wasActive = false;

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

    const double x = static_cast<double>(js.lX);
    const double y = static_cast<double>(js.lY);
    double z = static_cast<double>(js.lZ);
    if (std::abs(z) <= kTwistDeadzone)
        z = 0.0;
    const double magnitude = std::sqrt((x * x) + (y * y) + (z * z));
    const double maxMagnitude = kAxisMaxMagnitude;
    double displayX = 0.0;
    double displayY = 0.0;
    double displayZ = 0.0;

    if (magnitude >= kDeadzoneMagnitude && magnitude > 0.0)
    {
        const double invMagnitude = 1.0 / magnitude;
        const double scaledMagnitude = std::clamp(
            (magnitude - kDeadzoneMagnitude) / (maxMagnitude - kDeadzoneMagnitude),
            0.0,
            1.0);
        const double outputScale = scaledMagnitude * kOutputMaxMagnitude;

        displayX = x * invMagnitude * outputScale;
        const double ySign = (IsDlgButtonChecked(hDlg, IDC_INVERT_Y) == BST_CHECKED) ? -1.0 : 1.0;
        displayY = y * invMagnitude * outputScale * ySign;
        displayZ = z * invMagnitude * outputScale;
        JoystickState state = {};
        state.x = displayX;
        state.y = displayY;
        state.z = displayZ;
        SubmitJoystickState(state);
        wasActive = true;
    }
    else
    {
        if (wasActive)
        {
            JoystickState neutral = {};
            neutral.x = 0.0;
            neutral.y = 0.0;
            neutral.z = 0.0;
            SubmitJoystickState(neutral);
        }
        wasActive = false;
    }

    _stprintf_s(strText, 512, TEXT("%.3f"), displayX);
    SetWindowText(GetDlgItem(hDlg, IDC_X_AXIS), strText);
    _stprintf_s(strText, 512, TEXT("%.3f"), displayY);
    SetWindowText(GetDlgItem(hDlg, IDC_Y_AXIS), strText);
    _stprintf_s(strText, 512, TEXT("%.3f"), displayZ);
    SetWindowText(GetDlgItem(hDlg, IDC_Z_AXIS), strText);

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

    const std::wstring networkStatus = GetNetworkStatusText();
    SetWindowText(GetDlgItem(hDlg, IDC_NetResponse), networkStatus.c_str());

    return S_OK;
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

    HRESULT hr = g_directInput->CreateDevice(pdidInstance->guidInstance, g_joystick.put(), nullptr);
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
