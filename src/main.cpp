#include <Windows.h>

HINSTANCE g_hInstance;

BOOL InitApp(void)
{
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow)
{

    MSG msg;
    HWND hwnd;

    g_hInstance = hInstance;

    if (!InitApp()) return 0;

    if (FAILED(CoInitialize(NULL)))
    {
        MessageBox(NULL, L"CoInitialize Failed!", L"Blah!", MB_OK);
        return -1;
    }

    CoUninitialize();
    return 0;
}