#include "JoystickNetwork.h"

#include <Windows.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr auto kSendInterval = std::chrono::milliseconds(100);

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

HRESULT PostSampleWinHttp(const std::string& payload)
{
    DWORD bytesAvailable = 0;

    HINTERNET hSession = WinHttpOpen(
        L"WinHTTP Example/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!hSession)
        return E_FAIL;

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        L"example.com",
        INTERNET_DEFAULT_HTTPS_PORT,
        0);

    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return E_FAIL;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        L"/post",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return E_FAIL;
    }

    const wchar_t* headers =
        L"Content-Type: application/json\r\n";

    BOOL results = WinHttpSendRequest(
        hRequest,
        headers,
        -1L,
        (LPVOID)payload.data(),
        static_cast<DWORD>(payload.size()),
        static_cast<DWORD>(payload.size()),
        0);

    if (!results)
        goto cleanup;

    results = WinHttpReceiveResponse(hRequest, nullptr);
    if (!results)
        goto cleanup;

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
    } while (bytesAvailable > 0);

cleanup:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return S_OK;
}

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
            PostSampleWinHttp(payload);

            lock.lock();
            nextSend = std::chrono::steady_clock::now() + kSendInterval;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_ = false;
    bool stopRequested_ = false;
    bool hasState_ = false;
    JoystickState latestState_ = {};
};

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
