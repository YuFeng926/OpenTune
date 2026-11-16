// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlLogTail.h"

#include "ControlJson.h"

#include <algorithm>
#include <fstream>

namespace OpenTune {

namespace {

/** Secrets shorter than this are not redacted: the token and the proof are at least 128 bits of hex. */
constexpr std::size_t kMinSecretBytes = 8;

char lowerAscii(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/** Replaces every occurrence of needle in text (ASCII letters compared without case) by replacement. */
std::string replaceAll(std::string text, std::string_view needle, std::string_view replacement)
{
    if (needle.empty() || text.size() < needle.size())
        return text;
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        bool match = i + needle.size() <= text.size();
        for (std::size_t k = 0; match && k < needle.size(); ++k)
            match = lowerAscii(text[i + k]) == lowerAscii(needle[k]);
        if (match) {
            out.append(replacement);
            i += needle.size();
        } else {
            out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}

/** Length of the UTF-8 sequence a lead byte starts; 0 for a byte that cannot start one. */
std::size_t sequenceLength(unsigned char lead)
{
    if (lead < 0x80)
        return 1;
    if (lead >= 0xc2 && lead < 0xe0)
        return 2;
    if (lead >= 0xe0 && lead < 0xf0)
        return 3;
    if (lead >= 0xf0 && lead < 0xf5)
        return 4;
    return 0;
}

/** The text with every byte that is not part of a valid UTF-8 sequence replaced by '?'. */
std::string toValidUtf8(std::string_view text)
{
    if (isValidUtf8(text))
        return std::string(text);
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t length = sequenceLength(static_cast<unsigned char>(text[i]));
        if (length != 0 && i + length <= text.size() && isValidUtf8(text.substr(i, length))) {
            out.append(text.substr(i, length));
            i += length;
        } else {
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

/** The text cut to at most maxCodePoints code points. The text is valid UTF-8. */
std::string cutToCodePoints(std::string text, std::size_t maxCodePoints)
{
    std::size_t bytes = 0;
    for (std::size_t points = 0; bytes < text.size() && points < maxCodePoints; ++points)
        bytes += sequenceLength(static_cast<unsigned char>(text[bytes]));
    if (bytes < text.size())
        text.resize(bytes);
    return text;
}

} // namespace

std::vector<std::string> controlHomeDirectorySpellings(std::string_view homeDirectory)
{
    std::string home(homeDirectory);
    while (home.size() > 1 && (home.back() == '/' || home.back() == '\\'))
        home.pop_back();
    std::vector<std::string> spellings;
    if (home.size() < 2)
        return spellings;
    std::string forward = home;
    std::string backward = home;
    std::replace(forward.begin(), forward.end(), '\\', '/');
    std::replace(backward.begin(), backward.end(), '/', '\\');
    std::string doubled;
    for (const char c : backward) {
        doubled.push_back(c);
        if (c == '\\')
            doubled.push_back('\\');
    }
    for (const auto& candidate : {home, forward, backward, doubled})
        if (std::find(spellings.begin(), spellings.end(), candidate) == spellings.end())
            spellings.push_back(candidate);
    std::stable_sort(spellings.begin(), spellings.end(),
                     [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    return spellings;
}

std::string controlRedactLogLine(std::string_view line, const std::vector<std::string>& secrets,
                                 const std::vector<std::string>& homeDirectorySpellings)
{
    std::string text = toValidUtf8(line);
    for (const auto& secret : secrets)
        if (secret.size() >= kMinSecretBytes)
            text = replaceAll(std::move(text), secret, kControlRedactedText);
    for (const auto& home : homeDirectorySpellings)
        text = replaceAll(std::move(text), home, "~");
    return cutToCodePoints(std::move(text), kControlLogLineMaxCodePoints);
}

ControlLogTail controlReadLogTail(const std::filesystem::path& file, std::size_t maxLines,
                                  const std::vector<std::string>& secrets,
                                  const std::vector<std::string>& homeDirectorySpellings)
{
    ControlLogTail tail;
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return tail;
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0)
        return tail;
    const auto size = static_cast<std::size_t>(end);
    // One byte before the window tells whether the window starts at the start of a line.
    const std::size_t start = size > kControlLogTailWindowBytes ? size - kControlLogTailWindowBytes - 1 : 0;
    std::string buffer(size - start, '\0');
    stream.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = stream.gcount();
    if (got < 0 || (got == 0 && size != 0))
        return tail;
    buffer.resize(static_cast<std::size_t>(got));
    tail.readable = true;

    std::size_t begin = 0;
    if (start > 0) {
        tail.truncated = true;
        // The first line of the window is complete only when the byte before it ends a line.
        if (buffer[0] == '\n') {
            begin = 1;
        } else {
            const auto newline = buffer.find('\n');
            begin = newline == std::string::npos ? buffer.size() : newline + 1;
        }
    }
    std::vector<std::string_view> lines;
    while (begin < buffer.size()) {
        auto newline = buffer.find('\n', begin);
        if (newline == std::string::npos)
            newline = buffer.size();
        std::string_view line(buffer.data() + begin, newline - begin);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        begin = newline + 1;
    }
    if (lines.size() > maxLines) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(maxLines));
        tail.truncated = true;
    }
    tail.lines.reserve(lines.size());
    for (const auto& line : lines)
        tail.lines.push_back(controlRedactLogLine(line, secrets, homeDirectorySpellings));
    return tail;
}

} // namespace OpenTune
