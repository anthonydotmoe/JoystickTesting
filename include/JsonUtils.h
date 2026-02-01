#pragma once

#include "JoystickNetwork.h"

#include <string>
#include <vector>

namespace JsonUtils
{
bool TryParseReturnHomeDisabled(const std::string& body, bool* outDisabled);
bool TryParseCameraList(const std::string& body, std::vector<CameraInfo>* cameras);
}

