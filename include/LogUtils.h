#pragma once

#include <string>

struct HWND__;
using HWND = HWND__*;

void SetLogAnchorWindow(HWND window);
void AppendLogLine(const std::wstring& line);
