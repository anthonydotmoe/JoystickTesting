#pragma once

#include <Windows.h>

#include <string>

std::wstring ReadRegistryStringValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName);
bool ReadRegistryDwordValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName, DWORD* outValue);
std::wstring ReadRegistryString(const wchar_t* subkey, const wchar_t* valueName);
bool ReadRegistryDword(const wchar_t* subkey, const wchar_t* valueName, DWORD* outValue);
bool WriteRegistryDword(const wchar_t* subkey, const wchar_t* valueName, DWORD value);
