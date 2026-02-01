#include "JoystickNetwork.h"

#include "JsonUtils.h"
#include "LogUtils.h"
#include "RegistryUtils.h"
#include "StringUtils.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr auto kSendInterval = std::chrono::milliseconds(16);
constexpr wchar_t kRegistrySubkey[] = L"SOFTWARE\\JoystickTesting";
constexpr wchar_t kRegistryInvertYName[] = L"Invert Y";
constexpr wchar_t kRegistryControllerAddressName[] = L"Controller Address";
constexpr wchar_t kRegistryUsernameName[] = L"Username";
constexpr wchar_t kRegistryPasswordName[] = L"Password";
constexpr wchar_t kRegistryUseApiKeyName[] = L"Use API Key";
constexpr wchar_t kRegistryApiKeyName[] = L"API Key";
constexpr DWORD kReturnHomeAfterInactivityMs = 60000;

struct NetworkConfig
{
    std::wstring host = L"192.168.3.251";
    INTERNET_PORT port = 443;
    std::wstring loginPath = L"/api/auth/login";
    std::wstring cameraBasePath = L"/proxy/protect/api/cameras/";
    std::wstring cameraMoveSuffix = L"/move";
    std::wstring cameraListPath = L"/proxy/protect/integration/v1/cameras";
    std::string username;
    std::string password;
    std::string apiKey;
    bool useApiKey = false;
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
    const std::wstring controllerAddress =
        TrimWide(ReadRegistryString(kRegistrySubkey, kRegistryControllerAddressName));
    const std::wstring userName = TrimWide(ReadRegistryString(kRegistrySubkey, kRegistryUsernameName));
    const std::wstring password = TrimWide(ReadRegistryString(kRegistrySubkey, kRegistryPasswordName));
    const std::wstring apiKey = TrimWide(ReadRegistryString(kRegistrySubkey, kRegistryApiKeyName));
    DWORD useApiKeyValue = 0;
    ReadRegistryDword(kRegistrySubkey, kRegistryUseApiKeyName, &useApiKeyValue);

    if (controllerAddress.empty())
        return false;

    ApplyHostAndPort(config, controllerAddress);
    config.username = WideToUtf8(userName);
    config.password = WideToUtf8(password);
    config.apiKey = WideToUtf8(apiKey);
    config.useApiKey = (useApiKeyValue != 0);
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

std::string BuildReturnHomePayload(bool disabled)
{
    if (disabled)
        return "{\"ptz\":{\"returnHomeAfterInactivityMs\":null}}";

    return "{\"ptz\":{\"returnHomeAfterInactivityMs\":" +
        std::to_string(kReturnHomeAfterInactivityMs) + "}}";
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
    const wchar_t* method,
    const std::wstring& path,
    const std::string& payload,
    const std::wstring& cookieHeader,
    const std::wstring& csrfToken,
    const std::string& apiKey,
    HttpResponse* response,
    DWORD* outWin32Error,
    std::wstring* outErrorText,
    std::string* outResponseBody,
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
        needsReturnHomeQuery_ = true;
        needsCameraListRefresh_ = true;
        configDirty_ = false;
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

        SendReturnHomeOnStop();

        std::scoped_lock lock(mutex_);
        running_ = false;
        hasState_ = false;
        hasReturnHomeSetting_ = false;
        returnHomeStateKnown_ = false;
        hasCameraListUpdate_ = false;
        cameraList_.clear();
        selectedCameraId_.clear();
        needsCameraListRefresh_ = true;
        configDirty_ = false;
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

    void SubmitReturnHomeSetting(bool disabled)
    {
        {
            std::scoped_lock lock(mutex_);
            returnHomeDisabled_ = disabled;
            hasReturnHomeSetting_ = true;
        }
        cv_.notify_all();
    }

    std::wstring GetStatus()
    {
        std::scoped_lock lock(statusMutex_);
        return status_;
    }

    bool ConsumeReturnHomeSettingUpdate(bool* disabled)
    {
        std::scoped_lock lock(returnHomeStateMutex_);
        if (!hasReturnHomeSettingUpdate_)
            return false;
        hasReturnHomeSettingUpdate_ = false;
        if (disabled)
            *disabled = returnHomeDisabledState_;
        return true;
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

    void NotifyConfigChanged()
    {
        {
            std::scoped_lock lock(mutex_);
            configDirty_ = true;
        }
        cv_.notify_all();
    }

    void RequestCameraListRefresh()
    {
        {
            std::scoped_lock lock(mutex_);
            needsCameraListRefresh_ = true;
        }
        cv_.notify_all();
    }

    bool ConsumeCameraListUpdate(std::vector<CameraInfo>* cameras)
    {
        std::scoped_lock lock(mutex_);
        if (!hasCameraListUpdate_)
            return false;
        hasCameraListUpdate_ = false;
        if (cameras)
            *cameras = cameraList_;
        return true;
    }

    void SetSelectedCameraId(const std::wstring& cameraId)
    {
        std::scoped_lock lock(mutex_);
        if (selectedCameraId_ == cameraId)
            return;
        selectedCameraId_ = cameraId;
        needsReturnHomeQuery_ = true;
        returnHomeStateKnown_ = false;
    }

private:
    void Run()
    {
        std::unique_lock lock(mutex_);
        auto nextSend = std::chrono::steady_clock::now() + kSendInterval;

        for (;;)
        {
            cv_.wait_until(lock, nextSend, [&]() {
                return stopRequested_ || hasState_ || hasReturnHomeSetting_ || needsReturnHomeQuery_ ||
                    needsCameraListRefresh_ || configDirty_;
            });
            if (stopRequested_)
                break;

            if (!hasState_ && !hasReturnHomeSetting_ && !needsReturnHomeQuery_ &&
                !needsCameraListRefresh_ && !configDirty_)
            {
                nextSend = std::chrono::steady_clock::now() + kSendInterval;
                continue;
            }

            const bool sendReturnHome = hasReturnHomeSetting_;
            const bool returnHomeDisabled = returnHomeDisabled_;
            hasReturnHomeSetting_ = false;
            const bool refreshCameraList = needsCameraListRefresh_;
            needsCameraListRefresh_ = false;
            const std::wstring selectedCameraId = selectedCameraId_;
            const bool configDirty = configDirty_;
            configDirty_ = false;

            JoystickState snapshot = latestState_;
            const bool sendMove = hasState_;
            hasState_ = false;
            const bool queryReturnHome = needsReturnHomeQuery_ ||
                (!returnHomeStateKnown_ && (sendMove || sendReturnHome));
            needsReturnHomeQuery_ = false;
            lock.unlock();

            if (configDirty)
            {
                CloseHandles();
                ResetAuth();
                lock.lock();
                nextSend = std::chrono::steady_clock::now() + kSendInterval;
                continue;
            }

            const bool hasCameraSelection = !selectedCameraId.empty();
            std::wstring cameraPath;
            std::wstring movePath;
            if (hasCameraSelection)
            {
                const NetworkConfig& config = GetNetworkConfig();
                cameraPath = config.cameraBasePath + selectedCameraId;
                movePath = cameraPath + config.cameraMoveSuffix;
            }

            HandleCameraListRefresh(refreshCameraList);
            HandleReturnHomeQuery(queryReturnHome, hasCameraSelection, cameraPath);
            HandleReturnHomeUpdate(
                sendReturnHome, returnHomeDisabled, hasCameraSelection, cameraPath);
            HandleMove(sendMove, snapshot, hasCameraSelection, movePath);

            lock.lock();
            nextSend = std::chrono::steady_clock::now() + kSendInterval;
        }
    }

    bool EnsureCameraSelected(bool hasCameraSelection)
    {
        if (hasCameraSelection)
            return true;

        SetStatus(L"No camera selected");
        return false;
    }

    static bool IsHttpSuccess(DWORD status)
    {
        return status >= 200 && status < 300;
    }

    bool HandleUnauthorizedStatus(const HttpResponse& response)
    {
        if (response.status != 401 && response.status != 403)
            return false;

        SetStatusHttp(L"Unauthorized", response.status);
        ResetAuth();
        return true;
    }

    void HandleCameraListRefresh(bool refreshCameraList)
    {
        if (!refreshCameraList || !EnsureLogin())
            return;

        HttpResponse response = {};
        DWORD error = 0;
        std::wstring errorText;
        std::string responseBody;
        const HRESULT hr = SendJsonRequestWithReauth(
            L"GET", GetNetworkConfig().cameraListPath, "", &response, &error, &errorText, &responseBody);
        if (FAILED(hr))
        {
            SetStatusError(L"Camera list failed", error, errorText);
            return;
        }

        SetStatusHttp(L"Camera list", response.status);
        if (HandleUnauthorizedStatus(response) || !IsHttpSuccess(response.status))
            return;

        std::vector<CameraInfo> cameras;
        if (!JsonUtils::TryParseCameraList(responseBody, &cameras))
        {
            AppendLogLine(L"Camera list parse failed");
            return;
        }

        AppendLogLine(L"Camera list parsed: " + std::to_wstring(cameras.size()));
        std::scoped_lock listLock(mutex_);
        cameraList_ = std::move(cameras);
        hasCameraListUpdate_ = true;
    }

    void HandleReturnHomeQuery(bool queryReturnHome,
        bool hasCameraSelection,
        const std::wstring& cameraPath)
    {
        if (!queryReturnHome)
            return;
        if (!EnsureCameraSelected(hasCameraSelection) || !EnsureLogin())
            return;

        HttpResponse response = {};
        DWORD error = 0;
        std::wstring errorText;
        std::string responseBody;
        const HRESULT hr = SendJsonRequestWithReauth(
            L"GET", cameraPath, "", &response, &error, &errorText, &responseBody);
        if (FAILED(hr))
        {
            SetStatusError(L"Return home query failed", error, errorText);
            return;
        }

        SetStatusHttp(L"Return home query", response.status);
        if (HandleUnauthorizedStatus(response) || !IsHttpSuccess(response.status))
            return;

        bool disabled = false;
        if (!JsonUtils::TryParseReturnHomeDisabled(responseBody, &disabled))
        {
            AppendLogLine(L"Return home parse failed");
            return;
        }

        std::wstring parsedLine = L"Return home parsed: ";
        parsedLine += disabled ? L"disabled" : L"enabled";
        AppendLogLine(parsedLine);
        {
            std::scoped_lock stateLock(returnHomeStateMutex_);
            returnHomeDisabledState_ = disabled;
            hasReturnHomeSettingUpdate_ = true;
        }
        {
            std::scoped_lock stateLock(mutex_);
            returnHomeStateKnown_ = true;
        }
    }

    void HandleReturnHomeUpdate(bool sendReturnHome,
        bool returnHomeDisabled,
        bool hasCameraSelection,
        const std::wstring& cameraPath)
    {
        if (!sendReturnHome)
            return;
        if (!EnsureCameraSelected(hasCameraSelection) || !EnsureLogin())
            return;

        const std::string payload = BuildReturnHomePayload(returnHomeDisabled);
        HttpResponse response = {};
        DWORD error = 0;
        std::wstring errorText;
        const HRESULT hr = SendJsonRequestWithReauth(
            L"PATCH", cameraPath, payload, &response, &error, &errorText, nullptr);
        if (FAILED(hr))
        {
            SetStatusError(L"Return home update failed", error, errorText);
            return;
        }

        SetStatusHttp(L"Return home updated", response.status);
        (void)HandleUnauthorizedStatus(response);
    }

    void HandleMove(bool sendMove,
        const JoystickState& snapshot,
        bool hasCameraSelection,
        const std::wstring& movePath)
    {
        if (!sendMove)
            return;
        if (!EnsureCameraSelected(hasCameraSelection) || !EnsureLogin())
            return;

        const std::string payload = BuildMovePayload(snapshot);
        HttpResponse response = {};
        DWORD error = 0;
        std::wstring errorText;
        const HRESULT hr = SendJsonRequestWithReauth(
            L"POST", movePath, payload, &response, &error, &errorText, nullptr);
        if (FAILED(hr))
        {
            SetStatusError(L"Move failed", error, errorText);
            return;
        }

        SetStatusHttp(L"Move", response.status);
        (void)HandleUnauthorizedStatus(response);
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
                WinHttpSetStatusCallback(
                    session_,
                    WinHttpStatusCallback,
                    WINHTTP_CALLBACK_FLAG_SECURE_FAILURE,
                    0);
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
        if (!configLoaded_ && !LoadConfigFromRegistry(config))
        {
            SetStatus(L"Controller address missing");
            return false;
        }
        configLoaded_ = true;

        useApiKey_ = config.useApiKey;
        apiKey_ = config.apiKey;

        if (useApiKey_)
        {
            if (apiKey_.empty())
            {
                SetStatus(L"API key missing");
                return false;
            }
        }
        else if (config.username.empty() || config.password.empty())
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

        if (useApiKey_)
        {
            loggedIn_ = true;
            SetStatus(L"Using API key");
            return true;
        }

        HttpResponse response = {};
        const std::string payload = BuildLoginPayload(config);
        SetStatus(L"Logging in");
        DWORD error = 0;
        std::wstring errorText;
        if (FAILED(SendJsonRequest(connection_, L"POST", config.loginPath, payload, L"", L"", "", &response,
            &error, &errorText, nullptr, this)))
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
        apiKey_.clear();
        useApiKey_ = false;
        configLoaded_ = false;
        needsReturnHomeQuery_ = true;
        returnHomeStateKnown_ = false;
        needsCameraListRefresh_ = true;
    }

    void SendReturnHomeOnStop()
    {
        std::wstring cameraPath;
        {
            std::scoped_lock lock(mutex_);
            if (selectedCameraId_.empty())
                return;
            const NetworkConfig& config = GetNetworkConfig();
            cameraPath = config.cameraBasePath + selectedCameraId_;
        }

        if (!EnsureLogin())
            return;

        const std::string payload = BuildReturnHomePayload(false);
        HttpResponse response = {};
        DWORD error = 0;
        std::wstring errorText;
        SendJsonRequestWithReauth(L"PATCH", cameraPath, payload,
            &response, &error, &errorText, nullptr);
    }

    HRESULT SendJsonRequestWithReauth(const wchar_t* method,
        const std::wstring& path,
        const std::string& payload,
        HttpResponse* response,
        DWORD* outWin32Error,
        std::wstring* outErrorText,
        std::string* outResponseBody)
    {
        static const std::string kEmptyApiKey;
        const std::string& apiKey = useApiKey_ ? apiKey_ : kEmptyApiKey;
        HRESULT hr = SendJsonRequest(connection_, method, path, payload,
            cookieHeader_, csrfToken_, apiKey, response, outWin32Error,
            outErrorText, outResponseBody, this);
        if (FAILED(hr))
            return hr;

        if (!useApiKey_ && response && (response->status == 401 || response->status == 403))
        {
            ResetAuth();
            if (!EnsureLogin())
            {
                if (outWin32Error)
                    *outWin32Error = 0;
                if (outErrorText)
                    *outErrorText = L"Re-login failed";
                return E_FAIL;
            }

            hr = SendJsonRequest(connection_, method, path, payload,
                cookieHeader_, csrfToken_, apiKey, response, outWin32Error,
                outErrorText, outResponseBody, this);
        }

        return hr;
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
    bool hasReturnHomeSetting_ = false;
    bool returnHomeDisabled_ = false;
    bool needsReturnHomeQuery_ = true;
    bool returnHomeStateKnown_ = false;
    bool needsCameraListRefresh_ = true;
    bool configDirty_ = false;
    bool hasCameraListUpdate_ = false;
    std::vector<CameraInfo> cameraList_;
    std::wstring selectedCameraId_;

    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
    bool loggedIn_ = false;
    std::wstring cookieHeader_;
    std::wstring csrfToken_;
    std::string apiKey_;
    bool useApiKey_ = false;
    bool configLoaded_ = false;

    std::mutex statusMutex_;
    std::wstring status_ = L"Idle";

    std::mutex returnHomeStateMutex_;
    bool hasReturnHomeSettingUpdate_ = false;
    bool returnHomeDisabledState_ = false;

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
    const wchar_t* method,
    const std::wstring& path,
    const std::string& payload,
    const std::wstring& cookieHeader,
    const std::wstring& csrfToken,
    const std::string& apiKey,
    HttpResponse* response,
    DWORD* outWin32Error,
    std::wstring* outErrorText,
    std::string* outResponseBody,
    NetworkWorker* statusContext)
{
    HRESULT hr = S_OK;
    DWORD bytesAvailable = 0;
    std::string responseBody;

    if (outWin32Error)
        *outWin32Error = 0;
    if (outErrorText)
        outErrorText->clear();

    const wchar_t* requestMethod = method && *method ? method : L"POST";
    HINTERNET hRequest = WinHttpOpenRequest(
        connection,
        requestMethod,
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

    auto captureRequestError = [&](DWORD error, const wchar_t* action)
    {
        hr = E_FAIL;
        if (outWin32Error)
            *outWin32Error = error;
        if (outErrorText)
            *outErrorText = FormatWin32Error(error);

        std::wstring logLine = action ? action : L"WinHTTP call";
        logLine += L" failed: ";
        logLine += std::to_wstring(error);
        const std::wstring formatted = FormatWin32Error(error);
        if (!formatted.empty())
        {
            logLine += L" ";
            logLine += formatted;
        }
        AppendLogLine(logLine);

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
    };

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
    if (!apiKey.empty())
    {
        headers += L"X-API-Key: ";
        headers += Utf8ToWide(apiKey);
        headers += L"\r\n";
    }

    const bool hasPayload = !payload.empty();
    LPVOID payloadData = hasPayload ? (LPVOID)payload.data() : nullptr;
    const DWORD payloadSize = hasPayload ? static_cast<DWORD>(payload.size()) : 0;
    BOOL results = WinHttpSendRequest(
        hRequest,
        headers.c_str(),
        -1L,
        payloadData,
        payloadSize,
        payloadSize,
        0);

    {
        std::wstring logLine = std::wstring(requestMethod) + L" " + path;
        if (hasPayload)
        {
            logLine += L" payload=";
            logLine += RedactPassword(payload);
        }
        AppendLogLine(logLine);
    }

    if (!results)
    {
        captureRequestError(GetLastError(), L"SendRequest");
        goto cleanup;
    }

    results = WinHttpReceiveResponse(hRequest, nullptr);
    if (!results)
    {
        captureRequestError(GetLastError(), L"ReceiveResponse");
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
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable))
        {
            captureRequestError(GetLastError(), L"QueryDataAvailable");
            goto cleanup;
        }

        if (bytesAvailable == 0)
            break;

        std::string buffer(bytesAvailable, '\0');
        DWORD bytesRead = 0;

        if (!WinHttpReadData(
            hRequest,
            &buffer[0],
            bytesAvailable,
            &bytesRead))
        {
            captureRequestError(GetLastError(), L"ReadData");
            goto cleanup;
        }
        if (bytesRead > 0)
            responseBody.append(buffer.data(), bytesRead);
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
    if (outResponseBody)
        *outResponseBody = std::move(responseBody);
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

void SubmitReturnHomeSetting(bool disabled)
{
    GetWorker().SubmitReturnHomeSetting(disabled);
}

bool ConsumeReturnHomeSettingUpdate(bool* disabled)
{
    return GetWorker().ConsumeReturnHomeSettingUpdate(disabled);
}

std::wstring GetNetworkStatusText()
{
    return GetWorker().GetStatus();
}

void RequestCameraListRefresh()
{
    GetWorker().RequestCameraListRefresh();
}

bool ConsumeCameraListUpdate(std::vector<CameraInfo>* cameras)
{
    return GetWorker().ConsumeCameraListUpdate(cameras);
}

void SelectCameraId(const std::wstring& cameraId)
{
    GetWorker().SetSelectedCameraId(cameraId);
}

void NotifyNetworkConfigChanged()
{
    GetWorker().NotifyConfigChanged();
}

bool GetInvertYSetting()
{
    DWORD value = 0;
    if (!ReadRegistryDword(kRegistrySubkey, kRegistryInvertYName, &value))
        return false;
    return value != 0;
}

void SetInvertYSetting(bool enabled)
{
    WriteRegistryDword(kRegistrySubkey, kRegistryInvertYName, enabled ? 1u : 0u);
}
