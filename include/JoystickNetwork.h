#pragma once

#include <Windows.h>

struct JoystickState
{
    double x;
    double y;
    double z;
};

void StartNetworkWorker();
void StopNetworkWorker();
void SubmitJoystickState(const JoystickState& state);
