#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>

namespace morphosis::ui
{

inline bool isAsciiWhitespace (char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

inline std::string_view trimAsciiWhitespace (std::string_view value) noexcept
{
    while (! value.empty() && isAsciiWhitespace (value.front()))
        value.remove_prefix (1);
    while (! value.empty() && isAsciiWhitespace (value.back()))
        value.remove_suffix (1);
    return value;
}

inline std::optional<double> parseScaledCaptionEntry (
    std::string_view text, double displayMultiplier,
    double minimum, double maximum, std::string_view displaySuffix)
{
    if (! std::isfinite (displayMultiplier) || displayMultiplier <= 0.0
        || ! std::isfinite (minimum) || ! std::isfinite (maximum)
        || minimum > maximum)
        return std::nullopt;

    text = trimAsciiWhitespace (text);
    displaySuffix = trimAsciiWhitespace (displaySuffix);
    if (! displaySuffix.empty() && text.size() >= displaySuffix.size()
        && text.compare (text.size() - displaySuffix.size(), displaySuffix.size(),
                         displaySuffix) == 0)
    {
        text.remove_suffix (displaySuffix.size());
        text = trimAsciiWhitespace (text);
    }

    if (text.empty())
        return std::nullopt;

    auto index = std::size_t { 0 };
    if (text[index] == '+' || text[index] == '-')
        ++index;

    auto digitCount = std::size_t { 0 };
    while (index < text.size() && text[index] >= '0' && text[index] <= '9')
    {
        ++index;
        ++digitCount;
    }

    if (index < text.size() && text[index] == '.')
    {
        ++index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9')
        {
            ++index;
            ++digitCount;
        }
    }

    if (digitCount == 0)
        return std::nullopt;

    if (index < text.size() && (text[index] == 'e' || text[index] == 'E'))
    {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-'))
            ++index;

        const auto exponentStart = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9')
            ++index;
        if (index == exponentStart)
            return std::nullopt;
    }

    if (index != text.size())
        return std::nullopt;

    auto parseStart = text.data();
    if (text.front() == '+')
        ++parseStart;

    double displayedValue = 0.0;
    const auto parsed = std::from_chars (parseStart, text.data() + text.size(),
                                         displayedValue, std::chars_format::general);
    if (parsed.ec != std::errc {}
        || parsed.ptr != text.data() + text.size()
        || ! std::isfinite (displayedValue))
        return std::nullopt;

    const auto parameterValue = displayedValue / displayMultiplier;
    if (! std::isfinite (parameterValue))
        return std::nullopt;

    return std::clamp (parameterValue, minimum, maximum);
}

} // namespace morphosis::ui
