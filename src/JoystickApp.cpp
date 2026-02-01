#include "JoystickApp.h"

#include "DirectInputManager.h"
#include "JoystickNetwork.h"
#include "RegistryUtils.h"
#include "StringUtils.h"
#include "res.h"

#include <commctrl.h>
#include <shellapi.h>
#include <wchar.h>

namespace {
INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
bool ShouldFilterXInputDevices();
void EnsureRegistryDefaults();

constexpr wchar_t kRegistrySubkey[] = L"SOFTWARE\\JoystickTesting";
constexpr wchar_t kRegistryControllerAddress[] = L"Controller Address";
constexpr wchar_t kRegistryUsername[] = L"Username";
constexpr wchar_t kRegistryPassword[] = L"Password";
constexpr wchar_t kRegistryInvertYName[] = L"Invert Y";
constexpr wchar_t kRegistryDebugName[] = L"Debug";

std::wstring GetDialogItemText(HWND hDlg, int controlId)
{
    wchar_t buffer[512] = {};
    const int length = GetDlgItemTextW(hDlg, controlId, buffer,
        static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    if (length <= 0)
        return L"";
    return std::wstring(buffer, length);
}

void LoadSettingsDialog(HWND hDlg)
{
    SetDlgItemTextW(hDlg, IDC_SETTINGS_ADDRESS,
        ReadRegistryString(kRegistrySubkey, kRegistryControllerAddress).c_str());
    SetDlgItemTextW(hDlg, IDC_SETTINGS_USERNAME,
        ReadRegistryString(kRegistrySubkey, kRegistryUsername).c_str());
    SetDlgItemTextW(hDlg, IDC_SETTINGS_PASSWORD,
        ReadRegistryString(kRegistrySubkey, kRegistryPassword).c_str());
}

bool SaveSettingsDialog(HWND hDlg)
{
    const std::wstring address = TrimWide(GetDialogItemText(hDlg, IDC_SETTINGS_ADDRESS));
    const std::wstring username = TrimWide(GetDialogItemText(hDlg, IDC_SETTINGS_USERNAME));
    const std::wstring password = TrimWide(GetDialogItemText(hDlg, IDC_SETTINGS_PASSWORD));

    const bool saved =
        WriteRegistryString(kRegistrySubkey, kRegistryControllerAddress, address) &&
        WriteRegistryString(kRegistrySubkey, kRegistryUsername, username) &&
        WriteRegistryString(kRegistrySubkey, kRegistryPassword, password);

    if (!saved)
    {
        MessageBox(hDlg, TEXT("Failed to save settings."),
            TEXT("Settings"), MB_ICONERROR | MB_OK);
        return false;
    }

    NotifyNetworkConfigChanged();
    RequestCameraListRefresh();
    return true;
}
}

int RunJoystickApp(HINSTANCE instance)
{
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    EnsureRegistryDefaults();
    SetFilterOutXInputDevices(ShouldFilterXInputDevices());

    DialogBox(instance, MAKEINTRESOURCE(IDD_JOYST_IMM), nullptr, MainDlgProc);
    return 0;
}

namespace {
bool ShouldFilterXInputDevices()
{
    bool filter = false;
    int numArgs = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    if (!argList)
        return false;

    for (int iArg = 1; iArg < numArgs; ++iArg)
    {
        LPWSTR arg = argList[iArg];
        if (!arg)
            continue;

        if (*arg == L'/' || *arg == L'-')
        {
            ++arg;
            const int argLen = static_cast<int>(wcslen(L"noxinput"));
            if (_wcsnicmp(arg, L"noxinput", argLen) == 0 && arg[argLen] == 0)
            {
                filter = true;
                break;
            }
        }
    }

    LocalFree(argList);
    return filter;
}

void EnsureRegistryDefaults()
{
    EnsureRegistryKey(kRegistrySubkey);
    EnsureRegistryStringValue(kRegistrySubkey, kRegistryControllerAddress, L"");
    EnsureRegistryStringValue(kRegistrySubkey, kRegistryUsername, L"");
    EnsureRegistryStringValue(kRegistrySubkey, kRegistryPassword, L"");
    EnsureRegistryDwordValue(kRegistrySubkey, kRegistryInvertYName, 0);
    EnsureRegistryDwordValue(kRegistrySubkey, kRegistryDebugName, 0);
}

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
        case WM_INITDIALOG:
            LoadSettingsDialog(hDlg);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                    if (SaveSettingsDialog(hDlg))
                        EndDialog(hDlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }

    return FALSE;
}

INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
        case WM_INITDIALOG:
            if (FAILED(InitDirectInput(hDlg)))
            {
                MessageBox(nullptr, TEXT("Error Initializing DirectInput"),
                    TEXT("DirectInput Sample"), MB_ICONERROR | MB_OK);
                EndDialog(hDlg, 0);
            }

            CheckDlgButton(hDlg, IDC_INVERT_Y, GetInvertYSetting() ? BST_CHECKED : BST_UNCHECKED);
            StartNetworkWorker();
            RequestCameraListRefresh();
            CheckDlgButton(hDlg, IDC_DISABLE_RETURN_HOME, BST_UNCHECKED);
            SetTimer(hDlg, 0, 1000 / 30, nullptr);
            return TRUE;

        case WM_ACTIVATE:
            if (WA_INACTIVE != wParam)
            {
                AcquireJoystick();
            }
            return TRUE;

        case WM_TIMER:
            if (FAILED(UpdateInputState(hDlg)))
            {
                KillTimer(hDlg, 0);
                MessageBox(nullptr, TEXT("Error Reading Input State. ")
                    TEXT("The sample will now exit."), TEXT("DirectInput Sample"),
                    MB_ICONERROR | MB_OK);
                EndDialog(hDlg, TRUE);
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDCANCEL:
                    EndDialog(hDlg, 0);
                    return TRUE;
                case IDC_INVERT_Y:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        const bool enabled = (IsDlgButtonChecked(hDlg, IDC_INVERT_Y) == BST_CHECKED);
                        SetInvertYSetting(enabled);
                    }
                    return TRUE;
                case IDC_DISABLE_RETURN_HOME:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        const bool enabled =
                            (IsDlgButtonChecked(hDlg, IDC_DISABLE_RETURN_HOME) == BST_CHECKED);
                        SubmitReturnHomeSetting(!enabled);
                    }
                    return TRUE;
                case IDC_CAMERA_REFRESH:
                    if (HIWORD(wParam) == BN_CLICKED)
                        RequestCameraListRefresh();
                    return TRUE;
                case IDC_SETTINGS_BUTTON:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        HINSTANCE instance = reinterpret_cast<HINSTANCE>(
                            GetWindowLongPtr(hDlg, GWLP_HINSTANCE));
                        DialogBox(instance, MAKEINTRESOURCE(IDD_SETTINGS), hDlg, SettingsDlgProc);
                    }
                    return TRUE;
            }
            break;

        case WM_DESTROY:
            KillTimer(hDlg, 0);
            StopNetworkWorker();
            FreeDirectInput();
            return TRUE;
    }

    return FALSE;
}
}
