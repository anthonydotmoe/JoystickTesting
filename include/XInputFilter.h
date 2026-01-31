#pragma once

#include <Windows.h>

HRESULT SetupForIsXInputDevice();
bool IsXInputDevice(const GUID* productGuid);
void CleanupForIsXInputDevice();
