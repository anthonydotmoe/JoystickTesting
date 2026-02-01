#include "LogUtils.h"

#include "RegistryUtils.h"

#include <Windows.h>

#include <cstdio>
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

void AppendLogLine(const std::wstring& line)
{
    LogState& state = GetLogState();
    std::scoped_lock lock(state.mutex);
    EnsureLoggingInitialized(state);

    if (!state.debugEnabled || !state.consoleReady)
        return;

    const std::wstring timestamped = BuildTimestampedLine(line);
    HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!outputHandle || outputHandle == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteConsoleW(outputHandle, timestamped.c_str(),
        static_cast<DWORD>(timestamped.size()), &written, nullptr);
    WriteConsoleW(outputHandle, L"\r\n", 2, &written, nullptr);
}
