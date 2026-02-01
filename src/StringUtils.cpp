#include "StringUtils.h"

#include <Windows.h>

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return L"";

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return L"";

    std::wstring output;
    output.resize(size);
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), output.data(), size);
    return output;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string output;
    output.resize(size);
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring TrimWide(const std::wstring& value)
{
    if (value.empty())
        return value;

    size_t start = 0;
    while (start < value.size() && value[start] <= L' ')
        ++start;

    size_t end = value.size();
    while (end > start && value[end - 1] <= L' ')
        --end;

    return value.substr(start, end - start);
}
