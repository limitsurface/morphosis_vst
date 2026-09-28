#pragma once

#include "CubeData.h"

#include <cstddef>

namespace morphosis
{

// The product layout rule is deliberately based on the embedded catalog name:
// a `.4` record is the documented distortion family in this design pass.
// This is a routing rule for the plugin, not a claim about a recovered factory
// parameter default.
constexpr bool hasDotFourSuffix (const char* name) noexcept
{
    if (name == nullptr)
        return false;

    std::size_t length = 0;
    while (name[length] != '\0')
        ++length;

    return length >= 2 && name[length - 2] == '.' && name[length - 1] == '4';
}

constexpr bool usesDotFourDistortionLayout (int preset) noexcept
{
    if (preset < 0 || preset >= cube_data::kRecordCount)
        return false;

    return hasDotFourSuffix (cube_data::kCubes[static_cast<std::size_t> (preset)].name);
}

// Keep the old function name as a narrow compatibility alias for existing
// callers while making the new `.4` rule the single source of truth.
constexpr bool usesDocumentedDistortionLayout (int preset) noexcept
{
    return usesDotFourDistortionLayout (preset);
}

} // namespace morphosis
