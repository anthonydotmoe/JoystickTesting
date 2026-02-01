#include "LogUtils.h"

#include "RegistryUtils.h"

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace {
constexpr wchar_t kRegistrySubkey[] = L"SOFTWARE\\JoystickTesting";
constexpr wchar_t kRegistryDebugName[] = L"Debug";

struct LogState
{
    std::mutex mutex;
    bool initialized = false;
    bool debugEnabled = false;
    bool consoleReady = false;
};

LogState& GetLogState()
{
    static LogState state;
    return state;
}

void EnsureLoggingInitialized(LogState& state)
{
    if (state.initialized)
        return;

    state.initialized = true;
    DWORD value = 0;
    if (ReadRegistryDword(kRegistrySubkey, kRegistryDebugName, &value) && value == 1)
    {
        state.debugEnabled = true;
        if (AllocConsole())
        {
            FILE* outFile = nullptr;
            if (freopen_s(&outFile, "CONOUT$", "w", stdout) == 0)
            {
                setvbuf(stdout, nullptr, _IONBF, 0);
                state.consoleReady = true;
            }
        }
    }
}

std::wstring BuildTimestampedLine(const std::wstring& line)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::wostringstream stream;
    stream << L"["
           << st.wYear << L"-"
           << std::setw(2) << std::setfill(L'0') << st.wMonth << L"-"
           << std::setw(2) << std::setfill(L'0') << st.wDay << L" "
           << std::setw(2) << std::setfill(L'0') << st.wHour << L":"
           << std::setw(2) << std::setfill(L'0') << st.wMinute << L":"
           << std::setw(2) << std::setfill(L'0') << st.wSecond
           << L"] " << line;
    return stream.str();
}
}

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
    LogState& state = GetLogState();
    std::scoped_lock lock(state.mutex);
    EnsureLoggingInitialized(state);

    const std::wstring timestamped = BuildTimestampedLine(line);
    if (state.debugEnabled && state.consoleReady)
    {
        HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (outputHandle && outputHandle != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteConsoleW(outputHandle, timestamped.c_str(),
                static_cast<DWORD>(timestamped.size()), &written, nullptr);
            WriteConsoleW(outputHandle, L"\r\n", 2, &written, nullptr);
            return;
        }
    }

    const std::wstring path = GetLogFilePath();
    std::wofstream stream(path, std::ios::app);
    if (!stream.is_open())
        return;

    stream << timestamped << L"\n";
}
