// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlPathPolicy.h"

#include "ControlJson.h"

#include <vector>

namespace OpenTune {

namespace {

std::vector<std::string_view> splitSegments(std::string_view text, char separator)
{
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            segments.push_back(text.substr(start));
            return segments;
        }
        segments.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

bool hasControlCharacter(std::string_view text)
{
    for (const char c : text)
        if (static_cast<unsigned char>(c) < 0x20)
            return true;
    return false;
}

bool isEmptyOrDotSegment(std::string_view segment)
{
    return segment.empty() || segment == "." || segment == "..";
}

std::string upperAscii(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// Windows maps these names to devices, also with an extension (NUL.wav) and with trailing spaces.
bool isReservedDeviceName(std::string_view segment)
{
    std::string_view stem = segment.substr(0, segment.find('.'));
    while (!stem.empty() && stem.back() == ' ')
        stem.remove_suffix(1);
    const std::string name = upperAscii(stem);
    if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" || name == "CONIN$" || name == "CONOUT$")
        return true;
    if (name.size() < 3 || (name.compare(0, 3, "COM") != 0 && name.compare(0, 3, "LPT") != 0))
        return false;
    const std::string_view tail = std::string_view(name).substr(3);
    if (tail.size() == 1 && tail[0] >= '0' && tail[0] <= '9')
        return true;
    // Superscript one, two and three (U+00B9, U+00B2, U+00B3) in UTF-8.
    return tail == "\xC2\xB9" || tail == "\xC2\xB2" || tail == "\xC2\xB3";
}

std::string checkWindowsPath(std::string_view path)
{
    if (hasControlCharacter(path) || !isValidUtf8(path))
        return "path_invalid_character";
    if (path.substr(0, 4) == "\\\\?\\" || path.substr(0, 4) == "\\\\.\\")
        return "path_device_namespace";
    if (path.substr(0, 2) == "\\\\")
        return "path_unc";
    const bool driveLetter = path.size() >= 2
                             && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
                             && path[1] == ':';
    if (driveLetter && (path.size() < 3 || path[2] != '\\'))
        return "path_drive_relative";
    if (!driveLetter)
        return "path_not_absolute";
    const std::string_view rest = path.substr(3);
    if (rest.find(':') != std::string_view::npos)
        return "path_alternate_stream";
    if (rest.find('/') != std::string_view::npos)
        return "path_not_normalized";
    const auto segments = splitSegments(rest, '\\');
    for (const auto segment : segments)
        if (isEmptyOrDotSegment(segment) || segment.back() == '.' || segment.back() == ' ')
            return "path_not_normalized";
    for (const auto segment : segments)
        if (isReservedDeviceName(segment))
            return "path_reserved_name";
    return {};
}

std::string checkPosixPath(std::string_view path)
{
    if (hasControlCharacter(path) || !isValidUtf8(path))
        return "path_invalid_character";
    if (path.empty() || path[0] != '/')
        return "path_not_absolute";
    for (const auto segment : splitSegments(path.substr(1), '/'))
        if (isEmptyOrDotSegment(segment))
            return "path_not_normalized";
    return {};
}

} // namespace

ControlPathStyle controlNativePathStyle()
{
#if defined(_WIN32)
    return ControlPathStyle::Windows;
#else
    return ControlPathStyle::Posix;
#endif
}

std::string controlCheckAbsolutePath(std::string_view utf8Path, ControlPathStyle style)
{
    return style == ControlPathStyle::Windows ? checkWindowsPath(utf8Path) : checkPosixPath(utf8Path);
}

std::string controlJoinPath(std::string_view directory, std::string_view relative, ControlPathStyle style)
{
    const char separator = style == ControlPathStyle::Windows ? '\\' : '/';
    std::string out(directory);
    if (!out.empty() && out.back() != separator)
        out += separator;
    for (const char c : relative)
        out += (c == '/' || c == '\\') ? separator : c;
    return out;
}

} // namespace OpenTune
