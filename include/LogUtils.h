#pragma once

#include <string>

std::wstring GetLogFilePath();
void AppendLogLine(const std::wstring& line);
