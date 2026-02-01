#include "LogUtils.h"

#include <Windows.h>

#include <fstream>
#include <iomanip>
#include <mutex>

std::wstring GetLogFilePath()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
        return L"JoystickTesting.log";

    std::wstring path(modulePath);
    const size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
        path.resize(lastSlash + 1);
    path += L"JoystickTesting.log";
    return path;
}

void AppendLogLine(const std::wstring& line)
{
    static std::mutex logMutex;
    const std::wstring path = GetLogFilePath();

    std::scoped_lock lock(logMutex);
    std::wofstream stream(path, std::ios::app);
    if (!stream.is_open())
        return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    stream << L"["
           << st.wYear << L"-"
           << std::setw(2) << std::setfill(L'0') << st.wMonth << L"-"
           << std::setw(2) << std::setfill(L'0') << st.wDay << L" "
           << std::setw(2) << std::setfill(L'0') << st.wHour << L":"
           << std::setw(2) << std::setfill(L'0') << st.wMinute << L":"
           << std::setw(2) << std::setfill(L'0') << st.wSecond
           << L"] " << line << L"\n";
}
