#include "JoystickApp.h"

#include "DirectInputManager.h"
#include "JoystickNetwork.h"
#include "res.h"

#include <commctrl.h>
#include <shellapi.h>
#include <wchar.h>

namespace {
INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
bool ShouldFilterXInputDevices();
}

int RunJoystickApp(HINSTANCE instance)
{
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

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
