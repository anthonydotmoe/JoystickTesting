#pragma once

#include <Windows.h>

void SetFilterOutXInputDevices(bool enable);
HRESULT InitDirectInput(HWND dialog);
void AcquireJoystick();
void FreeDirectInput();
HRESULT UpdateInputState(HWND dialog);
