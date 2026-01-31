#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#include <Windows.h>
#include <string>

void SendJoystickUdpBroadcast(const std::string& payload);
HRESULT PostSampleWinHttp();
