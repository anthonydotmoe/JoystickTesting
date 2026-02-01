#include "JoystickNetwork.h"

#include <Windows.h>
#include <winhttp.h>
#include <winreg.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr auto kSendInterval = std::chrono::milliseconds(16);
constexpr wchar_t kRegistrySubkey[] = L"SOFTWARE\\JoystickTesting";
constexpr size_t kMaxLogBodyBytes = 1024;
constexpr wchar_t kRegistryInvertYName[] = L"Invert Y";

struct NetworkConfig
{
    std::wstring host = L"192.168.3.251";
    INTERNET_PORT port = 443;
    std::wstring loginPath = L"/api/auth/login";
    std::wstring movePath = L"/proxy/protect/api/cameras/67a2bce203a1b203e4001891/move";
    std::string username;
    std::string password;
};

NetworkConfig& GetNetworkConfig()
{
    static NetworkConfig config;
    return config;
}

std::string BuildLoginPayload(const NetworkConfig& config)
{
    return "{"
        "\"username\":\"" + config.username + "\","
        "\"password\":\"" + config.password + "\","
        "\"rememberMe\":false,"
        "\"token\":\"\""
        "}";
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

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return L"";

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return L"";

    std::wstring output;
    output.resize(size);
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), output.data(), size);
    return output;
}

std::wstring ReadRegistryStringValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS)
        return L"";

    std::wstring buffer;
    buffer.resize(size / sizeof(wchar_t));
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_SZ, &type, buffer.data(), &size) != ERROR_SUCCESS)
        return L"";

    if (!buffer.empty() && buffer.back() == L'\0')
        buffer.pop_back();
    return buffer;
}

bool ReadRegistryDwordValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName, DWORD* outValue)
{
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD value = 0;
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS)
        return false;

    if (outValue)
        *outValue = value;
    return true;
}

std::wstring ReadRegistryString(const wchar_t* valueName)
{
    std::wstring value = ReadRegistryStringValue(HKEY_CURRENT_USER, kRegistrySubkey, valueName);
    if (!value.empty())
        return value;
    return ReadRegistryStringValue(HKEY_LOCAL_MACHINE, kRegistrySubkey, valueName);
}

bool ReadRegistryDword(const wchar_t* valueName, DWORD* outValue)
{
    if (ReadRegistryDwordValue(HKEY_CURRENT_USER, kRegistrySubkey, valueName, outValue))
        return true;
    return ReadRegistryDwordValue(HKEY_LOCAL_MACHINE, kRegistrySubkey, valueName, outValue);
}

bool WriteRegistryDword(const wchar_t* valueName, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistrySubkey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    const LONG result = RegSetValueExW(
        key,
        valueName,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::wstring TrimWide(const std::wstring& value)
{
    if (value.empty())
        return value;

    size_t start = 0;
    while (start < value.size() && value[start] <= L' ')
        ++start;

    size_t end = value.size();
    while (end > start && value[end - 1] <= L' ')
        --end;

    return value.substr(start, end - start);
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string output;
    output.resize(size);
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    return output;
}

void ApplyHostAndPort(NetworkConfig& config, const std::wstring& address)
{
    if (address.empty())
        return;

    const size_t colon = address.find(L':');
    if (colon == std::wstring::npos)
    {
        config.host = address;
        return;
    }

    config.host = address.substr(0, colon);
    const std::wstring portText = address.substr(colon + 1);
    if (!portText.empty())
    {
        const unsigned long portValue = wcstoul(portText.c_str(), nullptr, 10);
        if (portValue > 0 && portValue <= 65535)
            config.port = static_cast<INTERNET_PORT>(portValue);
    }
}

bool LoadConfigFromRegistry(NetworkConfig& config)
{
    const std::wstring controllerAddress = TrimWide(ReadRegistryString(L"Controller Address"));
    const std::wstring userName = TrimWide(ReadRegistryString(L"Username"));
    const std::wstring password = TrimWide(ReadRegistryString(L"Password"));

    if (controllerAddress.empty() || userName.empty() || password.empty())
        return false;

    ApplyHostAndPort(config, controllerAddress);
    config.username = WideToUtf8(userName);
    config.password = WideToUtf8(password);
    return true;
}

std::string BuildMovePayload(const JoystickState& state)
{
    return "{"
        "\"type\":\"continuous\","
        "\"payload\":{"
        "\"x\":" + std::to_string(state.x) + ","
        "\"y\":" + std::to_string(state.y) + ","
        "\"z\":" + std::to_string(state.z) +
        "}}";
}

struct HttpResponse
{
    DWORD status = 0;
    std::wstring setCookieHeader;
    std::wstring csrfToken;
};

std::wstring ExtractCookiePair(const std::wstring& setCookieHeader)
{
    const size_t end = setCookieHeader.find(L';');
    if (end == std::wstring::npos)
        return setCookieHeader;
    return setCookieHeader.substr(0, end);
}

std::wstring JoinCookies(const std::vector<std::wstring>& cookies)
{
    std::wstring result;
    for (size_t i = 0; i < cookies.size(); ++i)
    {
        if (i > 0)
            result += L"; ";
        result += cookies[i];
    }
    return result;
}

std::wstring ReadHeaderValue(HINTERNET request, DWORD query, const wchar_t* name)
{
    DWORD size = 0;
    if (!WinHttpQueryHeaders(request, query, name, nullptr, &size, nullptr))
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return L"";
    }

    std::wstring buffer;
    buffer.resize(size / sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, query, name, buffer.data(), &size, nullptr))
        return L"";

    if (!buffer.empty() && buffer.back() == L'\0')
        buffer.pop_back();
    return buffer;
}

std::vector<std::wstring> ReadSetCookieHeaders(HINTERNET request)
{
    std::vector<std::wstring> cookies;
    DWORD index = 0;

    for (;;)
    {
        DWORD size = 0;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX,
            nullptr, &size, &index))
        {
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                break;
        }

        std::wstring buffer;
        buffer.resize(size / sizeof(wchar_t));
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX,
            buffer.data(), &size, &index))
        {
            break;
        }

        if (!buffer.empty() && buffer.back() == L'\0')
            buffer.pop_back();

        if (!buffer.empty())
            cookies.push_back(ExtractCookiePair(buffer));
    }

    return cookies;
}

std::wstring FormatWin32Error(DWORD error)
{
    if (error == 0)
        return L"";

    wchar_t* messageBuffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);

    if (length == 0 || !messageBuffer)
        return L"";

    std::wstring message(messageBuffer, length);
    LocalFree(messageBuffer);

    while (!message.empty())
    {
        const wchar_t ch = message.back();
        if (ch == L'\r' || ch == L'\n')
            message.pop_back();
        else
            break;
    }

    return message;
}

std::wstring RedactPassword(const std::string& payload)
{
    const std::string key = "\"password\":\"";
    const size_t start = payload.find(key);
    if (start == std::string::npos)
        return Utf8ToWide(payload);

    const size_t valueStart = start + key.size();
    const size_t end = payload.find("\"", valueStart);
    std::string redacted = payload;
    if (end != std::string::npos)
        redacted.replace(valueStart, end - valueStart, "****");
    return Utf8ToWide(redacted);
}

std::wstring DescribeSecureFailureFlags(DWORD flags)
{
    if (flags == 0)
        return L"";

    std::wstring result;
    auto append = [&](const wchar_t* text)
    {
        if (!result.empty())
            result += L" | ";
        result += text;
    };

    if (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED)
        append(L"CERT_REV_FAILED");
    if (flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT)
        append(L"INVALID_CERT");
    if (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED)
        append(L"CERT_REVOKED");
    if (flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA)
        append(L"INVALID_CA");
    if (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID)
        append(L"CERT_CN_INVALID");
    if (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID)
        append(L"CERT_DATE_INVALID");
    if (flags & WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR)
        append(L"SECURITY_CHANNEL_ERROR");

    return result;
}

class NetworkWorker;

void CALLBACK WinHttpStatusCallback(
    HINTERNET,
    DWORD_PTR context,
    DWORD status,
    LPVOID statusInfo,
    DWORD statusInfoLength);

HRESULT SendJsonRequest(HINTERNET connection,
    const std::wstring& path,
    const std::string& payload,
    const std::wstring& cookieHeader,
    const std::wstring& csrfToken,
    HttpResponse* response,
    DWORD* outWin32Error,
    std::wstring* outErrorText,
    NetworkWorker* statusContext);

class NetworkWorker
{
public:
    void Start()
    {
        std::scoped_lock lock(mutex_);
        if (running_)
            return;
        running_ = true;
        stopRequested_ = false;
        SetStatus(L"Starting");
        worker_ = std::thread(&NetworkWorker::Run, this);
    }

    void Stop()
    {
        {
            std::scoped_lock lock(mutex_);
            if (!running_)
                return;
            stopRequested_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();

        std::scoped_lock lock(mutex_);
        running_ = false;
        hasState_ = false;
        CloseHandles();
        ResetAuth();
        SetStatus(L"Stopped");
    }

    void Submit(const JoystickState& state)
    {
        {
            std::scoped_lock lock(mutex_);
            latestState_ = state;
            hasState_ = true;
        }
        cv_.notify_all();
    }

    std::wstring GetStatus()
    {
        std::scoped_lock lock(statusMutex_);
        return status_;
    }

    void RecordSecureFailure(DWORD flags)
    {
        std::scoped_lock lock(secureFailureMutex_);
        lastSecureFailureFlags_ = flags;
        hasSecureFailureFlags_ = true;
    }

    std::wstring ConsumeSecureFailureFlags()
    {
        std::scoped_lock lock(secureFailureMutex_);
        if (!hasSecureFailureFlags_)
            return L"";
        hasSecureFailureFlags_ = false;
        return DescribeSecureFailureFlags(lastSecureFailureFlags_);
    }

private:
    void Run()
    {
        std::unique_lock lock(mutex_);
        auto nextSend = std::chrono::steady_clock::now() + kSendInterval;

        for (;;)
        {
            cv_.wait_until(lock, nextSend, [&]() { return stopRequested_ || hasState_; });
            if (stopRequested_)
                break;

            if (!hasState_)
            {
                nextSend = std::chrono::steady_clock::now() + kSendInterval;
                continue;
            }

            JoystickState snapshot = latestState_;
            hasState_ = false;
            lock.unlock();

            const std::string payload = BuildMovePayload(snapshot);
            if (EnsureLogin())
            {
                HttpResponse response = {};
                DWORD error = 0;
                std::wstring errorText;
                const HRESULT hr = SendJsonRequest(connection_, GetNetworkConfig().movePath, payload,
                    cookieHeader_, csrfToken_, &response, &error, &errorText, this);
                if (FAILED(hr))
                {
                    SetStatusError(L"Move failed", error, errorText);
                }
                else
                {
                    SetStatusHttp(L"Move", response.status);
                }

                if (response.status == 401 || response.status == 403)
                {
                    SetStatusHttp(L"Unauthorized", response.status);
                    ResetAuth();
                }
            }

            lock.lock();
            nextSend = std::chrono::steady_clock::now() + kSendInterval;
        }
    }

    void EnsureSession()
    {
        if (!session_)
        {
            session_ = WinHttpOpen(
                L"JoystickTesting/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
            if (session_)
            {
                const WINHTTP_STATUS_CALLBACK cb = WinHttpSetStatusCallback(
                    session_,
                    WinHttpStatusCallback,
                    WINHTTP_CALLBACK_FLAG_SECURE_FAILURE,
                    0);
                callbackInstalled_ = (cb != WINHTTP_INVALID_STATUS_CALLBACK);
            }
        }

        if (session_ && !connection_)
        {
            const NetworkConfig& config = GetNetworkConfig();
            connection_ = WinHttpConnect(session_, config.host.c_str(), config.port, 0);
        }
    }

    bool EnsureLogin()
    {
        if (loggedIn_)
            return true;

        NetworkConfig& config = GetNetworkConfig();
        if (!configLoaded_)
        {
            configLoaded_ = LoadConfigFromRegistry(config);
        }

        if (!configLoaded_)
        {
            SetStatus(L"Registry config missing");
            return false;
        }

        if (config.username.empty() || config.password.empty())
        {
            SetStatus(L"Credentials missing");
            return false;
        }

        EnsureSession();
        if (!session_ || !connection_)
        {
            SetStatus(L"Network init failed");
            return false;
        }

        HttpResponse response = {};
        const std::string payload = BuildLoginPayload(config);
        SetStatus(L"Logging in");
        DWORD error = 0;
        std::wstring errorText;
        if (FAILED(SendJsonRequest(connection_, config.loginPath, payload, L"", L"", &response,
            &error, &errorText, this)))
        {
            SetStatusError(L"Login failed", error, errorText);
            return false;
        }

        if (response.status >= 200 && response.status < 300)
        {
            if (!response.setCookieHeader.empty())
                cookieHeader_ = response.setCookieHeader;
            if (!response.csrfToken.empty())
                csrfToken_ = response.csrfToken;
            loggedIn_ = !cookieHeader_.empty() || !csrfToken_.empty();
            if (loggedIn_)
                SetStatusHttp(L"Logged in", response.status);
            else
                SetStatusHttp(L"Login missing cookies", response.status);
        }
        else
        {
            SetStatusHttp(L"Login failed", response.status);
        }

        return loggedIn_;
    }

    void ResetAuth()
    {
        loggedIn_ = false;
        cookieHeader_.clear();
        csrfToken_.clear();
        configLoaded_ = false;
    }

    void CloseHandles()
    {
        if (connection_)
        {
            WinHttpCloseHandle(connection_);
            connection_ = nullptr;
        }
        if (session_)
        {
            WinHttpCloseHandle(session_);
            session_ = nullptr;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_ = false;
    bool stopRequested_ = false;
    bool hasState_ = false;
    JoystickState latestState_ = {};

    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
    bool loggedIn_ = false;
    std::wstring cookieHeader_;
    std::wstring csrfToken_;
    bool configLoaded_ = false;
    bool callbackInstalled_ = false;

    std::mutex statusMutex_;
    std::wstring status_ = L"Idle";

    std::mutex secureFailureMutex_;
    DWORD lastSecureFailureFlags_ = 0;
    bool hasSecureFailureFlags_ = false;

    void SetStatusHttp(const wchar_t* prefix, DWORD status)
    {
        std::wstring message = prefix ? prefix : L"";
        message += L" (HTTP ";
        message += std::to_wstring(status);
        message += L")";
        SetStatus(message.c_str());
    }

    void SetStatusError(const wchar_t* prefix, DWORD error, const std::wstring& errorText)
    {
        std::wstring message = prefix ? prefix : L"";
        message += L" (error ";
        message += std::to_wstring(error);
        if (!errorText.empty())
        {
            message += L": ";
            message += errorText;
        }
        message += L")";
        SetStatus(message.c_str());
    }

    void SetStatus(const wchar_t* text)
    {
        std::scoped_lock lock(statusMutex_);
        status_ = text ? text : L"";
    }
};

HRESULT SendJsonRequest(HINTERNET connection,
    const std::wstring& path,
    const std::string& payload,
    const std::wstring& cookieHeader,
    const std::wstring& csrfToken,
    HttpResponse* response,
    DWORD* outWin32Error,
    std::wstring* outErrorText,
    NetworkWorker* statusContext)
{
    HRESULT hr = S_OK;
    DWORD bytesAvailable = 0;
    std::string responseBody;

    if (outWin32Error)
        *outWin32Error = 0;
    if (outErrorText)
        outErrorText->clear();

    HINTERNET hRequest = WinHttpOpenRequest(
        connection,
        L"POST",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest)
    {
        const DWORD error = GetLastError();
        if (outWin32Error)
            *outWin32Error = error;
        if (outErrorText)
            *outErrorText = FormatWin32Error(error);
        return E_FAIL;
    }

    if (statusContext)
    {
        DWORD_PTR contextValue = reinterpret_cast<DWORD_PTR>(statusContext);
        WinHttpSetOption(hRequest, WINHTTP_OPTION_CONTEXT_VALUE, &contextValue, sizeof(contextValue));
        statusContext->ConsumeSecureFailureFlags();
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!csrfToken.empty())
    {
        headers += L"X-CSRF-Token: ";
        headers += csrfToken;
        headers += L"\r\n";
    }
    if (!cookieHeader.empty())
    {
        headers += L"Cookie: ";
        headers += cookieHeader;
        headers += L"\r\n";
    }

    BOOL results = WinHttpSendRequest(
        hRequest,
        headers.c_str(),
        -1L,
        (LPVOID)payload.data(),
        static_cast<DWORD>(payload.size()),
        static_cast<DWORD>(payload.size()),
        0);

    {
        std::wstring logLine = L"POST " + path + L" payload=" + RedactPassword(payload);
        AppendLogLine(logLine);
    }

    if (!results)
    {
        hr = E_FAIL;
        const DWORD error = GetLastError();
        if (outWin32Error)
            *outWin32Error = error;
        if (outErrorText)
            *outErrorText = FormatWin32Error(error);
        AppendLogLine(L"SendRequest failed: " + std::to_wstring(error) + L" " + FormatWin32Error(error));
        if (error == ERROR_WINHTTP_SECURE_FAILURE && statusContext && outErrorText)
        {
            const std::wstring flags = statusContext->ConsumeSecureFailureFlags();
            if (!flags.empty())
            {
                if (!outErrorText->empty())
                    *outErrorText += L" | ";
                *outErrorText += L"TLS flags: ";
                *outErrorText += flags;
            }
        }
        goto cleanup;
    }

    results = WinHttpReceiveResponse(hRequest, nullptr);
    if (!results)
    {
        hr = E_FAIL;
        const DWORD error = GetLastError();
        if (outWin32Error)
            *outWin32Error = error;
        if (outErrorText)
            *outErrorText = FormatWin32Error(error);
        AppendLogLine(L"ReceiveResponse failed: " + std::to_wstring(error) + L" " + FormatWin32Error(error));
        if (error == ERROR_WINHTTP_SECURE_FAILURE && statusContext && outErrorText)
        {
            const std::wstring flags = statusContext->ConsumeSecureFailureFlags();
            if (!flags.empty())
            {
                if (!outErrorText->empty())
                    *outErrorText += L" | ";
                *outErrorText += L"TLS flags: ";
                *outErrorText += flags;
            }
        }
        goto cleanup;
    }

    if (response)
    {
        DWORD statusSize = sizeof(response->status);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &response->status,
            &statusSize,
            nullptr);

        response->setCookieHeader.clear();
        response->csrfToken.clear();

        const auto cookies = ReadSetCookieHeaders(hRequest);
        if (!cookies.empty())
            response->setCookieHeader = JoinCookies(cookies);

        response->csrfToken = ReadHeaderValue(hRequest, WINHTTP_QUERY_CUSTOM, L"X-CSRF-Token");
    }

    do
    {
        bytesAvailable = 0;
        WinHttpQueryDataAvailable(hRequest, &bytesAvailable);

        if (bytesAvailable == 0)
            break;

        std::string buffer(bytesAvailable, '\0');
        DWORD bytesRead = 0;

        WinHttpReadData(
            hRequest,
            &buffer[0],
            bytesAvailable,
            &bytesRead);
        if (bytesRead > 0 && responseBody.size() < kMaxLogBodyBytes)
        {
            const size_t remaining = kMaxLogBodyBytes - responseBody.size();
            responseBody.append(buffer.data(), std::min<size_t>(bytesRead, remaining));
        }
    } while (bytesAvailable > 0);

cleanup:
    WinHttpCloseHandle(hRequest);
    if (response)
    {
        std::wstring line = L"HTTP " + std::to_wstring(response->status) + L" for " + path;
        if (!responseBody.empty())
        {
            line += L" body=";
            line += Utf8ToWide(responseBody);
        }
        AppendLogLine(line);
    }
    return hr;
}

void CALLBACK WinHttpStatusCallback(
    HINTERNET,
    DWORD_PTR context,
    DWORD status,
    LPVOID statusInfo,
    DWORD statusInfoLength)
{
    if (status != WINHTTP_CALLBACK_STATUS_SECURE_FAILURE)
        return;
    if (!statusInfo || statusInfoLength < sizeof(DWORD))
        return;

    auto* worker = reinterpret_cast<NetworkWorker*>(context);
    if (!worker)
        return;

    const DWORD flags = *reinterpret_cast<DWORD*>(statusInfo);
    worker->RecordSecureFailure(flags);
}

NetworkWorker& GetWorker()
{
    static NetworkWorker worker;
    return worker;
}
}

void StartNetworkWorker()
{
    GetWorker().Start();
}

void StopNetworkWorker()
{
    GetWorker().Stop();
}

void SubmitJoystickState(const JoystickState& state)
{
    GetWorker().Submit(state);
}

std::wstring GetNetworkStatusText()
{
    return GetWorker().GetStatus();
}

bool GetInvertYSetting()
{
    DWORD value = 0;
    if (!ReadRegistryDword(kRegistryInvertYName, &value))
        return false;
    return value != 0;
}

void SetInvertYSetting(bool enabled)
{
    WriteRegistryDword(kRegistryInvertYName, enabled ? 1u : 0u);
}
