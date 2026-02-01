#pragma once

#include <Windows.h>

struct JoystickState
{
    LONG x;
    LONG y;
    LONG z;
};

void StartNetworkWorker();
void StopNetworkWorker();
void SubmitJoystickState(const JoystickState& state);
