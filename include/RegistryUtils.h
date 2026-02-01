#pragma once

#include <Windows.h>

#include <string>

std::wstring ReadRegistryStringValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName);
bool ReadRegistryDwordValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName, DWORD* outValue);
std::wstring ReadRegistryString(const wchar_t* subkey, const wchar_t* valueName);
bool ReadRegistryDword(const wchar_t* subkey, const wchar_t* valueName, DWORD* outValue);
bool EnsureRegistryKey(const wchar_t* subkey);
bool EnsureRegistryStringValue(const wchar_t* subkey, const wchar_t* valueName, const std::wstring& value);
bool WriteRegistryDword(const wchar_t* subkey, const wchar_t* valueName, DWORD value);
bool WriteRegistryString(const wchar_t* subkey, const wchar_t* valueName, const std::wstring& value);
bool EnsureRegistryDwordValue(const wchar_t* subkey, const wchar_t* valueName, DWORD value);
