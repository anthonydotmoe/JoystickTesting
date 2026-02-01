#pragma once

#include <Windows.h>
#include <string>
#include <vector>

struct JoystickState
{
    double x;
    double y;
    double z;
};

struct CameraInfo
{
    std::wstring id;
    std::wstring name;
    std::wstring state;
};

void StartNetworkWorker();
void StopNetworkWorker();
void SubmitJoystickState(const JoystickState& state);
std::wstring GetNetworkStatusText();
bool GetInvertYSetting();
void SetInvertYSetting(bool enabled);
void SubmitReturnHomeSetting(bool disabled);
bool ConsumeReturnHomeSettingUpdate(bool* disabled);
void RequestCameraListRefresh();
bool ConsumeCameraListUpdate(std::vector<CameraInfo>* cameras);
void SelectCameraId(const std::wstring& cameraId);
void NotifyNetworkConfigChanged();
