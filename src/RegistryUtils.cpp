#include "RegistryUtils.h"

#include <winreg.h>

bool EnsureRegistryKey(const wchar_t* subkey)
{
    HKEY key = nullptr;
    const LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (key)
        RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static bool EnsureRegistryValueImpl(const wchar_t* subkey,
    const wchar_t* valueName,
    DWORD type,
    const BYTE* data,
    DWORD size)
{
    HKEY key = nullptr;
    const LONG openResult = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (openResult != ERROR_SUCCESS)
        return false;

    DWORD existingType = 0;
    LONG queryResult = RegQueryValueExW(key, valueName, nullptr, &existingType, nullptr, nullptr);
    if (queryResult == ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return true;
    }

    const LONG setResult = RegSetValueExW(key, valueName, 0, type, data, size);
    RegCloseKey(key);
    return setResult == ERROR_SUCCESS;
}

std::wstring ReadRegistryStringValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS)
        return L"";

    std::wstring buffer;
    buffer.resize(size / sizeof(wchar_t));
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_SZ, &type, buffer.data(), &size) != ERROR_SUCCESS)
        return L"";

    if (!buffer.empty() && buffer.back() == L'\0')
        buffer.pop_back();
    return buffer;
}

bool ReadRegistryDwordValue(HKEY root, const wchar_t* subkey, const wchar_t* valueName, DWORD* outValue)
{
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD value = 0;
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS)
        return false;

    if (outValue)
        *outValue = value;
    return true;
}

std::wstring ReadRegistryString(const wchar_t* subkey, const wchar_t* valueName)
{
    std::wstring value = ReadRegistryStringValue(HKEY_CURRENT_USER, subkey, valueName);
    if (!value.empty())
        return value;
    return ReadRegistryStringValue(HKEY_LOCAL_MACHINE, subkey, valueName);
}

bool ReadRegistryDword(const wchar_t* subkey, const wchar_t* valueName, DWORD* outValue)
{
    if (ReadRegistryDwordValue(HKEY_CURRENT_USER, subkey, valueName, outValue))
        return true;
    return ReadRegistryDwordValue(HKEY_LOCAL_MACHINE, subkey, valueName, outValue);
}

bool EnsureRegistryStringValue(const wchar_t* subkey, const wchar_t* valueName, const std::wstring& value)
{
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return EnsureRegistryValueImpl(subkey, valueName, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), size);
}

bool WriteRegistryDword(const wchar_t* subkey, const wchar_t* valueName, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    const LONG result = RegSetValueExW(
        key,
        valueName,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool WriteRegistryString(const wchar_t* subkey, const wchar_t* valueName, const std::wstring& value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(
        key,
        valueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool EnsureRegistryDwordValue(const wchar_t* subkey, const wchar_t* valueName, DWORD value)
{
    const BYTE* data = reinterpret_cast<const BYTE*>(&value);
    return EnsureRegistryValueImpl(subkey, valueName, REG_DWORD, data, sizeof(value));
}
