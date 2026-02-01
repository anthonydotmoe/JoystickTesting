#include "JsonUtils.h"

#include "StringUtils.h"

#include <cctype>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace
{
bool TryExtractJsonObjectRange(const std::string& body,
    const std::string& key,
    size_t* outStart,
    size_t* outEnd)
{
    size_t pos = body.find(key);
    if (pos == std::string::npos)
        return false;

    pos = body.find(':', pos + key.size());
    if (pos == std::string::npos)
        return false;

    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
        ++pos;

    if (pos >= body.size() || body[pos] != '{')
        return false;

    bool inString = false;
    bool escape = false;
    int depth = 0;
    for (size_t i = pos; i < body.size(); ++i)
    {
        const char ch = body[i];
        if (inString)
        {
            if (escape)
            {
                escape = false;
            }
            else if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
            continue;
        }

        if (ch == '{')
        {
            if (depth == 0 && outStart)
                *outStart = i;
            ++depth;
            continue;
        }

        if (ch == '}')
        {
            --depth;
            if (depth == 0)
            {
                if (outEnd)
                    *outEnd = i;
                return true;
            }
        }
    }

    return false;
}

bool TryParseReturnHomeDisabledFromRange(const std::string& body,
    size_t start,
    size_t end,
    bool* outDisabled)
{
    if (start >= body.size() || end <= start || end >= body.size())
        return false;

    const std::string_view view(body.data() + start, end - start + 1);
    const std::string key = "\"returnHomeAfterInactivityMs\"";
    size_t pos = view.find(key);
    if (pos == std::string::npos)
        return false;

    pos = view.find(':', pos + key.size());
    if (pos == std::string::npos)
        return false;

    ++pos;
    while (pos < view.size() && std::isspace(static_cast<unsigned char>(view[pos])))
        ++pos;

    if (pos >= view.size())
        return false;

    if (view.compare(pos, 4, "null") == 0 || view.compare(pos, 4, "NULL") == 0)
    {
        if (outDisabled)
            *outDisabled = true;
        return true;
    }

    if (view[pos] == '"')
    {
        ++pos;
        const size_t endQuote = view.find('"', pos);
        if (endQuote == std::string::npos)
            return false;

        const std::string value(view.substr(pos, endQuote - pos));
        if (value == "null" || value == "NULL")
        {
            if (outDisabled)
                *outDisabled = true;
            return true;
        }

        char* endPtr = nullptr;
        const long long parsed = std::strtoll(value.c_str(), &endPtr, 10);
        if (endPtr == value.c_str() || *endPtr != '\0')
            return false;

        if (outDisabled)
            *outDisabled = (parsed <= 0);
        return true;
    }

    const char* startPtr = view.data() + pos;
    char* endPtr = nullptr;
    const long long parsed = std::strtoll(startPtr, &endPtr, 10);
    if (endPtr == startPtr)
        return false;

    if (outDisabled)
        *outDisabled = (parsed <= 0);
    return true;
}

bool TryParseJsonString(const std::string& body,
    size_t start,
    size_t end,
    size_t* outNext,
    std::string* outValue)
{
    if (start > end || body[start] != '"')
        return false;

    std::string value;
    bool escape = false;
    for (size_t i = start + 1; i <= end; ++i)
    {
        const char ch = body[i];
        if (escape)
        {
            switch (ch)
            {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'u':
                {
                    if (i + 4 > end)
                        return false;
                    unsigned int codePoint = 0;
                    for (size_t j = 0; j < 4; ++j)
                    {
                        const char hex = body[i + 1 + j];
                        codePoint <<= 4;
                        if (hex >= '0' && hex <= '9')
                            codePoint += static_cast<unsigned int>(hex - '0');
                        else if (hex >= 'A' && hex <= 'F')
                            codePoint += static_cast<unsigned int>(hex - 'A' + 10);
                        else if (hex >= 'a' && hex <= 'f')
                            codePoint += static_cast<unsigned int>(hex - 'a' + 10);
                        else
                            return false;
                    }
                    if (codePoint <= 0x7F)
                        value.push_back(static_cast<char>(codePoint));
                    else
                        value.push_back('?');
                    i += 4;
                    break;
                }
                default:
                    value.push_back(ch);
                    break;
            }
            escape = false;
            continue;
        }

        if (ch == '\\')
        {
            escape = true;
            continue;
        }

        if (ch == '"')
        {
            if (outNext)
                *outNext = i + 1;
            if (outValue)
                *outValue = value;
            return true;
        }

        value.push_back(ch);
    }

    return false;
}

bool SkipJsonValue(const std::string& body, size_t start, size_t end, size_t* outNext)
{
    if (start > end)
        return false;

    const char first = body[start];
    if (first == '"')
        return TryParseJsonString(body, start, end, outNext, nullptr);

    if (first == '{' || first == '[')
    {
        const char open = first;
        const char close = (first == '{') ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escape = false;
        for (size_t i = start; i <= end; ++i)
        {
            const char ch = body[i];
            if (inString)
            {
                if (escape)
                {
                    escape = false;
                }
                else if (ch == '\\')
                {
                    escape = true;
                }
                else if (ch == '"')
                {
                    inString = false;
                }
                continue;
            }

            if (ch == '"')
            {
                inString = true;
                continue;
            }

            if (ch == open)
            {
                ++depth;
                continue;
            }

            if (ch == close)
            {
                --depth;
                if (depth == 0)
                {
                    if (outNext)
                        *outNext = i + 1;
                    return true;
                }
            }
        }

        return false;
    }

    size_t pos = start;
    for (; pos <= end; ++pos)
    {
        const char ch = body[pos];
        if (ch == ',' || ch == '}' || ch == ']')
            break;
    }

    if (outNext)
        *outNext = pos;
    return true;
}

bool TryExtractNextJsonObjectRange(const std::string& body,
    size_t searchStart,
    size_t* outStart,
    size_t* outEnd)
{
    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t objectStart = std::string::npos;

    for (size_t i = searchStart; i < body.size(); ++i)
    {
        const char ch = body[i];
        if (inString)
        {
            if (escape)
            {
                escape = false;
            }
            else if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
            continue;
        }

        if (ch == '{')
        {
            if (depth == 0)
                objectStart = i;
            ++depth;
            continue;
        }

        if (ch == '}')
        {
            if (depth == 0)
                continue;
            --depth;
            if (depth == 0 && objectStart != std::string::npos)
            {
                if (outStart)
                    *outStart = objectStart;
                if (outEnd)
                    *outEnd = i;
                return true;
            }
        }
    }

    return false;
}

bool TryParseCameraInfo(const std::string& body,
    size_t start,
    size_t end,
    CameraInfo* outCamera)
{
    if (start >= body.size() || end >= body.size() || start >= end)
        return false;

    if (body[start] != '{')
        return false;

    size_t pos = start + 1;
    std::string id;
    std::string name;
    std::string state;

    while (pos <= end)
    {
        while (pos <= end && (std::isspace(static_cast<unsigned char>(body[pos])) || body[pos] == ','))
            ++pos;

        if (pos > end || body[pos] == '}')
            break;

        if (body[pos] != '"')
        {
            ++pos;
            continue;
        }

        std::string key;
        size_t nextPos = pos;
        if (!TryParseJsonString(body, pos, end, &nextPos, &key))
            return false;

        pos = nextPos;
        while (pos <= end && std::isspace(static_cast<unsigned char>(body[pos])))
            ++pos;
        if (pos > end || body[pos] != ':')
            return false;
        ++pos;
        while (pos <= end && std::isspace(static_cast<unsigned char>(body[pos])))
            ++pos;
        if (pos > end)
            return false;

        if (key == "id" || key == "name" || key == "state")
        {
            if (body[pos] == '"')
            {
                std::string value;
                if (!TryParseJsonString(body, pos, end, &nextPos, &value))
                    return false;
                if (key == "id")
                    id = std::move(value);
                else if (key == "name")
                    name = std::move(value);
                else if (key == "state")
                    state = std::move(value);
                pos = nextPos;
            }
            else
            {
                if (!SkipJsonValue(body, pos, end, &nextPos))
                    return false;
                pos = nextPos;
            }
        }
        else
        {
            if (!SkipJsonValue(body, pos, end, &nextPos))
                return false;
            pos = nextPos;
        }
    }

    if (id.empty())
        return false;

    if (outCamera)
    {
        outCamera->id = Utf8ToWide(id);
        outCamera->name = Utf8ToWide(name.empty() ? id : name);
        outCamera->state = Utf8ToWide(state);
    }

    return true;
}
}

namespace JsonUtils
{
bool TryParseReturnHomeDisabled(const std::string& body, bool* outDisabled)
{
    if (body.empty())
        return false;

    size_t start = 0;
    size_t end = 0;
    if (TryExtractJsonObjectRange(body, "\"ptz\"", &start, &end) &&
        TryParseReturnHomeDisabledFromRange(body, start, end, outDisabled))
    {
        return true;
    }

    return TryParseReturnHomeDisabledFromRange(body, 0, body.size() - 1, outDisabled);
}

bool TryParseCameraList(const std::string& body, std::vector<CameraInfo>* cameras)
{
    if (!cameras)
        return false;

    cameras->clear();

    size_t searchStart = 0;
    size_t objStart = 0;
    size_t objEnd = 0;
    while (TryExtractNextJsonObjectRange(body, searchStart, &objStart, &objEnd))
    {
        CameraInfo camera;
        if (TryParseCameraInfo(body, objStart, objEnd, &camera))
            cameras->push_back(std::move(camera));
        searchStart = objEnd + 1;
    }

    return true;
}
}
