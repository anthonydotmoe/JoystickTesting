#include "JoystickNetwork.h"

#include "LogUtils.h"
#include "RegistryUtils.h"
#include "StringUtils.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr auto kSendInterval = std::chrono::milliseconds(16);
constexpr wchar_t kRegistrySubkey[] = L"SOFTWARE\\JoystickTesting";
constexpr wchar_t kRegistryInvertYName[] = L"Invert Y";
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
        TrimWide(ReadRegistryString(kRegistrySubkey, L"Controller Address"));
    const std::wstring userName = TrimWide(ReadRegistryString(kRegistrySubkey, L"Username"));
    const std::wstring password = TrimWide(ReadRegistryString(kRegistrySubkey, L"Password"));

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

std::string BuildReturnHomePayload(bool disabled)
{
    if (disabled)
        return "{\"ptz\":{\"returnHomeAfterInactivityMs\":null}}";

    return "{\"ptz\":{\"returnHomeAfterInactivityMs\":" +
        std::to_string(kReturnHomeAfterInactivityMs) + "}}";
}

bool TryExtractJsonObjectRange(const std::string& body,
    const std::string& key,
    size_t* outStart,
    size_t* outEnd)
{
    size_t pos = body.find(key);
    if (pos == std::string::npos)
        return false;

    pos = body.find(':', pos + key.size());
    if (pos == std::string::npos)
        return false;

    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
        ++pos;

    if (pos >= body.size() || body[pos] != '{')
        return false;

    bool inString = false;
    bool escape = false;
    int depth = 0;
    for (size_t i = pos; i < body.size(); ++i)
    {
        const char ch = body[i];
        if (inString)
        {
            if (escape)
            {
                escape = false;
            }
            else if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
            continue;
        }

        if (ch == '{')
        {
            if (depth == 0 && outStart)
                *outStart = i;
            ++depth;
            continue;
        }

        if (ch == '}')
        {
            --depth;
            if (depth == 0)
            {
                if (outEnd)
                    *outEnd = i;
                return true;
            }
        }
    }

    return false;
}

bool TryParseReturnHomeDisabledFromRange(const std::string& body,
    size_t start,
    size_t end,
    bool* outDisabled)
{
    if (start >= body.size() || end <= start || end >= body.size())
        return false;

    const std::string_view view(body.data() + start, end - start + 1);
    const std::string key = "\"returnHomeAfterInactivityMs\"";
    size_t pos = view.find(key);
    if (pos == std::string::npos)
        return false;

    pos = view.find(':', pos + key.size());
    if (pos == std::string::npos)
        return false;

    ++pos;
    while (pos < view.size() && std::isspace(static_cast<unsigned char>(view[pos])))
        ++pos;

    if (pos >= view.size())
        return false;

    if (view.compare(pos, 4, "null") == 0 || view.compare(pos, 4, "NULL") == 0)
    {
        if (outDisabled)
            *outDisabled = true;
        return true;
    }

    if (view[pos] == '"')
    {
        ++pos;
        const size_t endQuote = view.find('"', pos);
        if (endQuote == std::string::npos)
            return false;

        const std::string value(view.substr(pos, endQuote - pos));
        if (value == "null" || value == "NULL")
        {
            if (outDisabled)
                *outDisabled = true;
            return true;
        }

        char* endPtr = nullptr;
        const long long parsed = std::strtoll(value.c_str(), &endPtr, 10);
        if (endPtr == value.c_str() || *endPtr != '\0')
            return false;

        if (outDisabled)
            *outDisabled = (parsed <= 0);
        return true;
    }

    const char* startPtr = view.data() + pos;
    char* endPtr = nullptr;
    const long long parsed = std::strtoll(startPtr, &endPtr, 10);
    if (endPtr == startPtr)
        return false;

    if (outDisabled)
        *outDisabled = (parsed <= 0);
    return true;
}

bool TryParseReturnHomeDisabled(const std::string& body, bool* outDisabled)
{
    size_t start = 0;
    size_t end = 0;
    if (TryExtractJsonObjectRange(body, "\"ptz\"", &start, &end))
    {
        if (TryParseReturnHomeDisabledFromRange(body, start, end, outDisabled))
            return true;
    }

    return TryParseReturnHomeDisabledFromRange(body, 0, body.size() - 1, outDisabled);
}

bool TryParseJsonString(const std::string& body,
    size_t start,
    size_t end,
    size_t* outNext,
    std::string* outValue)
{
    if (start > end || body[start] != '"')
        return false;

    std::string value;
    bool escape = false;
    for (size_t i = start + 1; i <= end; ++i)
    {
        const char ch = body[i];
        if (escape)
        {
            switch (ch)
            {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'u':
                {
                    if (i + 4 > end)
                        return false;
                    unsigned int codePoint = 0;
                    for (size_t j = 0; j < 4; ++j)
                    {
                        const char hex = body[i + 1 + j];
                        codePoint <<= 4;
                        if (hex >= '0' && hex <= '9')
                            codePoint += static_cast<unsigned int>(hex - '0');
                        else if (hex >= 'A' && hex <= 'F')
                            codePoint += static_cast<unsigned int>(hex - 'A' + 10);
                        else if (hex >= 'a' && hex <= 'f')
                            codePoint += static_cast<unsigned int>(hex - 'a' + 10);
                        else
                            return false;
                    }
                    if (codePoint <= 0x7F)
                        value.push_back(static_cast<char>(codePoint));
                    else
                        value.push_back('?');
                    i += 4;
                    break;
                }
                default:
                    value.push_back(ch);
                    break;
            }
            escape = false;
            continue;
        }

        if (ch == '\\')
        {
            escape = true;
            continue;
        }

        if (ch == '"')
        {
            if (outNext)
                *outNext = i + 1;
            if (outValue)
                *outValue = value;
            return true;
        }

        value.push_back(ch);
    }

    return false;
}

bool SkipJsonValue(const std::string& body, size_t start, size_t end, size_t* outNext)
{
    if (start > end)
        return false;

    const char first = body[start];
    if (first == '"')
        return TryParseJsonString(body, start, end, outNext, nullptr);

    if (first == '{' || first == '[')
    {
        const char open = first;
        const char close = (first == '{') ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escape = false;
        for (size_t i = start; i <= end; ++i)
        {
            const char ch = body[i];
            if (inString)
            {
                if (escape)
                {
                    escape = false;
                }
                else if (ch == '\\')
                {
                    escape = true;
                }
                else if (ch == '"')
                {
                    inString = false;
                }
                continue;
            }

            if (ch == '"')
            {
                inString = true;
                continue;
            }

            if (ch == open)
            {
                ++depth;
                continue;
            }

            if (ch == close)
            {
                --depth;
                if (depth == 0)
                {
                    if (outNext)
                        *outNext = i + 1;
                    return true;
                }
            }
        }

        return false;
    }

    size_t pos = start;
    for (; pos <= end; ++pos)
    {
        const char ch = body[pos];
        if (ch == ',' || ch == '}' || ch == ']')
            break;
    }

    if (outNext)
        *outNext = pos;
    return true;
}

bool TryExtractNextJsonObjectRange(const std::string& body,
    size_t searchStart,
    size_t* outStart,
    size_t* outEnd)
{
    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t objectStart = std::string::npos;

    for (size_t i = searchStart; i < body.size(); ++i)
    {
        const char ch = body[i];
        if (inString)
        {
            if (escape)
            {
                escape = false;
            }
            else if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
            continue;
        }

        if (ch == '{')
        {
            if (depth == 0)
                objectStart = i;
            ++depth;
            continue;
        }

        if (ch == '}')
        {
            if (depth == 0)
                continue;
            --depth;
            if (depth == 0 && objectStart != std::string::npos)
            {
                if (outStart)
                    *outStart = objectStart;
                if (outEnd)
                    *outEnd = i;
                return true;
            }
        }
    }

    return false;
}

bool TryParseCameraInfo(const std::string& body,
    size_t start,
    size_t end,
    CameraInfo* outCamera)
{
    if (start >= body.size() || end >= body.size() || start >= end)
        return false;

    if (body[start] != '{')
        return false;

    size_t pos = start + 1;
    std::string id;
    std::string name;
    std::string state;

    while (pos <= end)
    {
        while (pos <= end && (std::isspace(static_cast<unsigned char>(body[pos])) || body[pos] == ','))
            ++pos;

        if (pos > end || body[pos] == '}')
            break;

        if (body[pos] != '"')
        {
            ++pos;
            continue;
        }

        std::string key;
        size_t nextPos = pos;
        if (!TryParseJsonString(body, pos, end, &nextPos, &key))
            return false;

        pos = nextPos;
        while (pos <= end && std::isspace(static_cast<unsigned char>(body[pos])))
            ++pos;
        if (pos > end || body[pos] != ':')
            return false;
        ++pos;
        while (pos <= end && std::isspace(static_cast<unsigned char>(body[pos])))
            ++pos;
        if (pos > end)
            return false;

        if (key == "id" || key == "name" || key == "state")
        {
            if (body[pos] == '"')
            {
                std::string value;
                if (!TryParseJsonString(body, pos, end, &nextPos, &value))
                    return false;
                if (key == "id")
                    id = std::move(value);
                else if (key == "name")
                    name = std::move(value);
                else if (key == "state")
                    state = std::move(value);
                pos = nextPos;
            }
            else
            {
                if (!SkipJsonValue(body, pos, end, &nextPos))
                    return false;
                pos = nextPos;
            }
        }
        else
        {
            if (!SkipJsonValue(body, pos, end, &nextPos))
                return false;
            pos = nextPos;
        }
    }

    if (id.empty())
        return false;

    if (outCamera)
    {
        outCamera->id = Utf8ToWide(id);
        outCamera->name = Utf8ToWide(name.empty() ? id : name);
        outCamera->state = Utf8ToWide(state);
    }

    return true;
}

bool TryParseCameraList(const std::string& body, std::vector<CameraInfo>* cameras)
{
    if (!cameras)
        return false;

    cameras->clear();

    size_t searchStart = 0;
    size_t objStart = 0;
    size_t objEnd = 0;
    while (TryExtractNextJsonObjectRange(body, searchStart, &objStart, &objEnd))
    {
        CameraInfo camera;
        if (TryParseCameraInfo(body, objStart, objEnd, &camera))
            cameras->push_back(std::move(camera));
        searchStart = objEnd + 1;
    }

    return true;
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

            if (refreshCameraList)
            {
                if (EnsureLogin())
                {
                    HttpResponse response = {};
                    DWORD error = 0;
                    std::wstring errorText;
                    std::string responseBody;
                    const HRESULT hr = SendJsonRequestWithReauth(L"GET",
                        GetNetworkConfig().cameraListPath, "",
                        &response, &error, &errorText, &responseBody);
                    if (FAILED(hr))
                    {
                        SetStatusError(L"Camera list failed", error, errorText);
                    }
                    else
                    {
                        SetStatusHttp(L"Camera list", response.status);
                        if (response.status >= 200 && response.status < 300)
                        {
                            std::vector<CameraInfo> cameras;
                            if (TryParseCameraList(responseBody, &cameras))
                            {
                                AppendLogLine(L"Camera list parsed: " +
                                    std::to_wstring(cameras.size()));
                                std::scoped_lock listLock(mutex_);
                                cameraList_ = std::move(cameras);
                                hasCameraListUpdate_ = true;
                            }
                            else
                            {
                                AppendLogLine(L"Camera list parse failed");
                            }
                        }
                    }
                    if (response.status == 401 || response.status == 403)
                    {
                        SetStatusHttp(L"Unauthorized", response.status);
                        ResetAuth();
                    }
                }
            }

            if (queryReturnHome)
            {
                if (!hasCameraSelection)
                {
                    SetStatus(L"No camera selected");
                }
                else if (EnsureLogin())
                {
                    HttpResponse response = {};
                    DWORD error = 0;
                    std::wstring errorText;
                    std::string responseBody;
                    const HRESULT hr = SendJsonRequestWithReauth(L"GET",
                        cameraPath, "",
                        &response, &error, &errorText, &responseBody);
                    if (FAILED(hr))
                    {
                        SetStatusError(L"Return home query failed", error, errorText);
                    }
                    else
                    {
                        SetStatusHttp(L"Return home query", response.status);
                        if (response.status >= 200 && response.status < 300)
                        {
                            bool disabled = false;
                            const bool parsed = TryParseReturnHomeDisabled(responseBody, &disabled);
                            if (parsed)
                            {
                                std::wstring parsedLine = L"Return home parsed: ";
                                parsedLine += disabled ? L"disabled" : L"enabled";
                                AppendLogLine(parsedLine);
                                std::scoped_lock stateLock(returnHomeStateMutex_);
                                returnHomeDisabledState_ = disabled;
                                hasReturnHomeSettingUpdate_ = true;
                            }
                            else
                            {
                                AppendLogLine(L"Return home parse failed");
                            }
                            if (parsed)
                            {
                                std::scoped_lock stateLock(mutex_);
                                returnHomeStateKnown_ = true;
                            }
                        }
                    }
                    if (response.status == 401 || response.status == 403)
                    {
                        SetStatusHttp(L"Unauthorized", response.status);
                        ResetAuth();
                    }
                }
            }

            if (sendReturnHome)
            {
                if (!hasCameraSelection)
                {
                    SetStatus(L"No camera selected");
                }
                else if (EnsureLogin())
                {
                    const std::string payload = BuildReturnHomePayload(returnHomeDisabled);
                    HttpResponse response = {};
                    DWORD error = 0;
                    std::wstring errorText;
                    const HRESULT hr = SendJsonRequestWithReauth(L"PATCH",
                        cameraPath, payload,
                        &response, &error, &errorText, nullptr);
                    if (FAILED(hr))
                    {
                        SetStatusError(L"Return home update failed", error, errorText);
                    }
                    else
                    {
                        SetStatusHttp(L"Return home updated", response.status);
                    }

                    if (response.status == 401 || response.status == 403)
                    {
                        SetStatusHttp(L"Unauthorized", response.status);
                        ResetAuth();
                    }
                }
            }

            if (sendMove)
            {
                if (!hasCameraSelection)
                {
                    SetStatus(L"No camera selected");
                }
                else if (EnsureLogin())
                {
                    const std::string payload = BuildMovePayload(snapshot);
                    HttpResponse response = {};
                    DWORD error = 0;
                    std::wstring errorText;
                    const HRESULT hr = SendJsonRequestWithReauth(L"POST",
                        movePath,
                        payload, &response, &error, &errorText, nullptr);
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
        if (FAILED(SendJsonRequest(connection_, L"POST", config.loginPath, payload, L"", L"", &response,
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
        HRESULT hr = SendJsonRequest(connection_, method, path, payload,
            cookieHeader_, csrfToken_, response, outWin32Error, outErrorText, outResponseBody, this);
        if (FAILED(hr))
            return hr;

        if (response && (response->status == 401 || response->status == 403))
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
                cookieHeader_, csrfToken_, response, outWin32Error,
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
    bool configLoaded_ = false;
    bool callbackInstalled_ = false;

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
