#pragma once

#include "CubeData.h"
#include "PresetTaxonomy.h"

#include <array>
#include <vector>

#include <juce_core/juce_core.h>

namespace morphosis::preset_picker
{

using FavouriteFlags = std::array<bool, cube_data::kRecordCount>;

inline const preset_taxonomy::Subcategory* singleSubcategoryFor (
    const preset_taxonomy::Category& category) noexcept
{
    return category.subcategoryCount == 1 && category.subcategories != nullptr
         ? &category.subcategories[0] : nullptr;
}

inline const char* categoryContentsName (const preset_taxonomy::Category& category) noexcept
{
    if (const auto* direct = singleSubcategoryFor (category))
        return direct->name;
    return category.name;
}

inline void toggleFavourite (FavouriteFlags& flags, int index) noexcept
{
    if (index >= 0 && index < cube_data::kRecordCount)
        flags[static_cast<std::size_t> (index)] = ! flags[static_cast<std::size_t> (index)];
}

inline bool matchesQuery (int index, const juce::String& name,
                          const juce::String& rawQuery)
{
    const auto query = rawQuery.trim().toLowerCase();
    if (query.isEmpty())
        return true;

    const auto displayedId = juce::String::formatted ("%03d", index + 1);
    return displayedId.toLowerCase().contains (query)
        || name.toLowerCase().contains (query);
}

inline std::vector<int> search (const juce::String& rawQuery)
{
    std::vector<int> result;
    const auto query = rawQuery.trim();
    result.reserve (query.isEmpty() ? cube_data::kRecordCount : 32);

    for (int index = 0; index < cube_data::kRecordCount; ++index)
        if (matchesQuery (index,
                          juce::String::fromUTF8 (cube_data::kCubes[static_cast<std::size_t> (index)].name),
                          query))
            result.push_back (index);

    return result;
}

inline std::vector<int> favourites (const FavouriteFlags& flags)
{
    std::vector<int> result;
    for (int index = 0; index < cube_data::kRecordCount; ++index)
        if (flags[static_cast<std::size_t> (index)])
            result.push_back (index);
    return result;
}

inline juce::String encodeFavourites (const FavouriteFlags& flags)
{
    juce::String encoded;
    encoded.preallocateBytes (cube_data::kRecordCount);
    for (const auto flag : flags)
        encoded += flag ? '1' : '0';
    return encoded;
}

inline FavouriteFlags decodeFavourites (const juce::String& encoded)
{
    FavouriteFlags flags {};
    for (int index = 0; index < cube_data::kRecordCount; ++index)
        flags[static_cast<std::size_t> (index)] = encoded[index] == '1';
    return flags;
}

} // namespace morphosis::preset_picker
