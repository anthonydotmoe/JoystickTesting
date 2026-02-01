#pragma once

#include <Windows.h>
#include <string>

struct JoystickState
{
    double x;
    double y;
    double z;
};

void StartNetworkWorker();
void StopNetworkWorker();
void SubmitJoystickState(const JoystickState& state);
std::wstring GetNetworkStatusText();
bool GetInvertYSetting();
void SetInvertYSetting(bool enabled);
void SubmitReturnHomeSetting(bool disabled);
bool ConsumeReturnHomeSettingUpdate(bool* disabled);
