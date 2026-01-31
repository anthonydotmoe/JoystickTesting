#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "JoystickNetwork.h"

#include <winsock2.h>
#include <Windows.h>
#include <winhttp.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

void SendJoystickUdpBroadcast(const std::string& payload)
{
    WSADATA wsaData;
    SOCKET socketHandle;
    SOCKADDR_IN sockAddr;
    hostent* host;
    std::string baseip = "255.255.255.255";

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cout << "WSAStartup failed.\n";
        system("pause");
    }

    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    host = gethostbyname(baseip.c_str());

    sockAddr.sin_port = htons(8888);
    sockAddr.sin_family = AF_INET;
    sockAddr.sin_addr.s_addr = *((unsigned long*)host->h_addr);

    if (connect(socketHandle, reinterpret_cast<SOCKADDR*>(&sockAddr), sizeof(sockAddr)) != 0)
    {
    }

    send(socketHandle, payload.c_str(), static_cast<int>(strlen(payload.c_str())), 0);

    std::cout << "\n\nPress ANY key to close.\n\n";
    std::cin.ignore();
    std::cin.get();

    closesocket(socketHandle);
    WSACleanup();
}

HRESULT PostSampleWinHttp()
{
    const std::string postData = R"({"hello":"world"})";
    DWORD bytesAvailable = 0;

    HINTERNET hSession = WinHttpOpen(
        L"WinHTTP Example/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!hSession)
    {
        std::cerr << "WinHttpOpen failed\n";
        return 1;
    }

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        L"test.com",
        INTERNET_DEFAULT_HTTPS_PORT,
        0);

    if (!hConnect)
    {
        std::cerr << "WinHttpConnect failed\n";
        WinHttpCloseHandle(hSession);
        return 1;
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
        std::cerr << "WinHttpOpenRequest failed\n";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }

    const wchar_t* headers =
        L"Content-Type: application/json\r\n";

    BOOL results = WinHttpSendRequest(
        hRequest,
        headers,
        -1L,
        (LPVOID)postData.data(),
        (DWORD)postData.size(),
        (DWORD)postData.size(),
        0);

    if (!results)
    {
        std::cerr << "WinHttpSendRequest failed\n";
        goto cleanup;
    }

    results = WinHttpReceiveResponse(hRequest, nullptr);
    if (!results)
    {
        std::cerr << "WinHttpReceiveResponse failed\n";
        goto cleanup;
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

        std::cout << buffer;
    } while (bytesAvailable > 0);

cleanup:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return S_OK;
}
