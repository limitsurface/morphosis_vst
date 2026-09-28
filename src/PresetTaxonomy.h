#pragma once

#include <cstddef>

// Generated grouping metadata only; runtime preset names remain in CubeData.h.
// Source taxonomy: recovered recommended preset catalog (see docs/PROVENANCE.md).
// SHA-256: 2A8D2222D09B028FA58EE46BF7AF626FBDE9DBC321C18CA406928683B4301721
// Source taxonomy: recovered manual-aligned preset catalog (see docs/PROVENANCE.md).
// SHA-256: 427DF1465D75AEBD32841C7819BC774A64E6107C2F0E306127429788F8EEF366
// The JSON files are not runtime dependencies and no preset DSP/data is copied here.

namespace morphosis::preset_taxonomy
{

enum class Grouping
{
    recommended,
    manual
};

struct Subcategory
{
    const char* name;
    const int* presetIds;
    std::size_t presetCount;
};

struct Category
{
    const char* name;
    const Subcategory* subcategories;
    std::size_t subcategoryCount;
};

struct Taxonomy
{
    const Category* categories;
    std::size_t categoryCount;
};

inline constexpr int recommended_ids_00_00[] = { 0 };
inline constexpr Subcategory recommended_subcategories_00_00 { "Bypass", recommended_ids_00_00, 1 };
inline constexpr Subcategory recommended_categories_00_subcategories[] =
{
    recommended_subcategories_00_00,
};
inline constexpr Category recommended_categories_00 { "00 Utility / Bypass", recommended_categories_00_subcategories, 1 };

inline constexpr int recommended_ids_01_00[] = { 70, 71, 72, 73, 74, 75 };
inline constexpr Subcategory recommended_subcategories_01_00 { "Equalization", recommended_ids_01_00, 6 };
inline constexpr int recommended_ids_01_01[] = { 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69 };
inline constexpr Subcategory recommended_subcategories_01_01 { "Standard Multimode", recommended_ids_01_01, 26 };
inline constexpr int recommended_ids_01_02[] = { 189, 198, 199, 200, 201, 202, 203, 204 };
inline constexpr Subcategory recommended_subcategories_01_02 { "Vari-Pole & Slope", recommended_ids_01_02, 8 };
inline constexpr Subcategory recommended_categories_01_subcategories[] =
{
    recommended_subcategories_01_00,
    recommended_subcategories_01_01,
    recommended_subcategories_01_02,
};
inline constexpr Category recommended_categories_01 { "01 Classic & Vari-Pole", recommended_categories_01_subcategories, 3 };

inline constexpr int recommended_ids_02_00[] = { 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42 };
inline constexpr Subcategory recommended_subcategories_02_00 { "Diphthongs & Words", recommended_ids_02_00, 22 };
inline constexpr int recommended_ids_02_01[] = { 43, 99, 100, 230, 231, 232, 233, 234 };
inline constexpr Subcategory recommended_subcategories_02_01 { "Vowel Spaces & Formants", recommended_ids_02_01, 8 };
inline constexpr Subcategory recommended_categories_02_subcategories[] =
{
    recommended_subcategories_02_00,
    recommended_subcategories_02_01,
};
inline constexpr Category recommended_categories_02 { "02 Vocal & Speech", recommended_categories_02_subcategories, 2 };

inline constexpr int recommended_ids_03_00[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
inline constexpr Subcategory recommended_subcategories_03_00 { "Classic Flangers", recommended_ids_03_00, 20 };
inline constexpr int recommended_ids_03_01[] = { 117, 118, 141, 142, 168, 225, 226, 227, 267 };
inline constexpr Subcategory recommended_subcategories_03_01 { "Phasers & Chorus", recommended_ids_03_01, 9 };
inline constexpr Subcategory recommended_categories_03_subcategories[] =
{
    recommended_subcategories_03_00,
    recommended_subcategories_03_01,
};
inline constexpr Category recommended_categories_03 { "03 Flangers, Phasers & Chorus", recommended_categories_03_subcategories, 2 };

inline constexpr int recommended_ids_04_00[] = { 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 261, 262, 263 };
inline constexpr Subcategory recommended_subcategories_04_00 { "Brass & Horns", recommended_ids_04_00, 14 };
inline constexpr int recommended_ids_04_01[] = { 93, 264, 265, 266 };
inline constexpr Subcategory recommended_subcategories_04_01 { "Drums & Percussion", recommended_ids_04_01, 4 };
inline constexpr int recommended_ids_04_02[] = { 101, 102, 103, 104, 105, 107, 108, 109, 247, 248, 249, 250, 251 };
inline constexpr Subcategory recommended_subcategories_04_02 { "Guitars & Basses", recommended_ids_04_02, 13 };
inline constexpr int recommended_ids_04_03[] = { 94, 95, 96, 97, 98, 110, 111, 112, 113, 114, 115, 116, 236, 237, 238, 239 };
inline constexpr Subcategory recommended_subcategories_04_03 { "Keys & Mallets", recommended_ids_04_03, 16 };
inline constexpr int recommended_ids_04_04[] = { 240, 241, 242, 243, 244, 245, 246 };
inline constexpr Subcategory recommended_subcategories_04_04 { "Strings & Cellos", recommended_ids_04_04, 7 };
inline constexpr int recommended_ids_04_05[] = { 91, 92, 252, 253, 254, 255, 256, 257, 258, 259, 260 };
inline constexpr Subcategory recommended_subcategories_04_05 { "Winds & Reeds", recommended_ids_04_05, 11 };
inline constexpr Subcategory recommended_categories_04_subcategories[] =
{
    recommended_subcategories_04_00,
    recommended_subcategories_04_01,
    recommended_subcategories_04_02,
    recommended_subcategories_04_03,
    recommended_subcategories_04_04,
    recommended_subcategories_04_05,
};
inline constexpr Category recommended_categories_04 { "04 Acoustic & Physical Modeling", recommended_categories_04_subcategories, 6 };

inline constexpr int recommended_ids_05_00[] = { 143, 145, 146, 147, 148, 149, 150, 152, 153, 154, 155, 156, 157, 158, 159, 161, 162, 164 };
inline constexpr Subcategory recommended_subcategories_05_00 { "Band Shapers & Resonances", recommended_ids_05_00, 18 };
inline constexpr int recommended_ids_05_01[] = { 132, 133, 134, 135, 136, 137, 138, 139, 140, 170, 180 };
inline constexpr Subcategory recommended_subcategories_05_01 { "Deep Sweeps & Cavities", recommended_ids_05_01, 11 };
inline constexpr int recommended_ids_05_02[] = { 76, 77, 78, 79 };
inline constexpr Subcategory recommended_subcategories_05_02 { "Hybrid Sweeps & Notches", recommended_ids_05_02, 4 };
inline constexpr int recommended_ids_05_03[] = { 131, 166, 171, 172, 173, 174, 178, 179 };
inline constexpr Subcategory recommended_subcategories_05_03 { "Velocity & Dynamic Motion", recommended_ids_05_03, 8 };
inline constexpr Subcategory recommended_categories_05_subcategories[] =
{
    recommended_subcategories_05_00,
    recommended_subcategories_05_01,
    recommended_subcategories_05_02,
    recommended_subcategories_05_03,
};
inline constexpr Category recommended_categories_05 { "05 Resonant Sweeps & Motion", recommended_categories_05_subcategories, 4 };

inline constexpr int recommended_ids_06_00[] = { 122, 124, 228, 229, 235 };
inline constexpr Subcategory recommended_subcategories_06_00 { "Boosts & Formant Bands", recommended_ids_06_00, 5 };
inline constexpr int recommended_ids_06_01[] = { 119, 120, 121, 123, 125, 126, 181, 182, 183, 184, 185, 186, 187, 219, 220, 221, 222, 223, 224 };
inline constexpr Subcategory recommended_subcategories_06_01 { "Harmonic Series & Partials", recommended_ids_06_01, 19 };
inline constexpr int recommended_ids_06_02[] = { 211, 212, 213, 214, 215, 216, 217, 218 };
inline constexpr Subcategory recommended_subcategories_06_02 { "Parametric Octave Arrays", recommended_ids_06_02, 8 };
inline constexpr int recommended_ids_06_03[] = { 205, 206, 207, 208, 209, 210 };
inline constexpr Subcategory recommended_subcategories_06_03 { "Pitch & Interval Trackers", recommended_ids_06_03, 6 };
inline constexpr int recommended_ids_06_04[] = { 127, 128, 129, 130 };
inline constexpr Subcategory recommended_subcategories_06_04 { "Waveshaping Transitions", recommended_ids_06_04, 4 };
inline constexpr Subcategory recommended_categories_06_subcategories[] =
{
    recommended_subcategories_06_00,
    recommended_subcategories_06_01,
    recommended_subcategories_06_02,
    recommended_subcategories_06_03,
    recommended_subcategories_06_04,
};
inline constexpr Category recommended_categories_06 { "06 Harmonic, Metric & Tracking", recommended_categories_06_subcategories, 5 };

inline constexpr int recommended_ids_07_00[] = { 106, 144, 151, 160, 163, 167, 169, 188, 190, 194, 277, 278, 286 };
inline constexpr Subcategory recommended_subcategories_07_00 { "Analog Warmth & Saturated Poles", recommended_ids_07_00, 13 };
inline constexpr int recommended_ids_07_01[] = { 195, 196, 197, 284, 287, 288 };
inline constexpr Subcategory recommended_subcategories_07_01 { "Extreme Distortion (.4 Models)", recommended_ids_07_01, 6 };
inline constexpr Subcategory recommended_categories_07_subcategories[] =
{
    recommended_subcategories_07_00,
    recommended_subcategories_07_01,
};
inline constexpr Category recommended_categories_07 { "07 Saturated & Distortion", recommended_categories_07_subcategories, 2 };

inline constexpr int recommended_ids_08_00[] = { 268, 269, 270, 271, 272, 273, 274, 275, 276, 279, 280 };
inline constexpr Subcategory recommended_subcategories_08_00 { "Acoustic Objects & Clangs", recommended_ids_08_00, 11 };
inline constexpr int recommended_ids_08_01[] = { 191, 192, 193, 281, 282 };
inline constexpr Subcategory recommended_subcategories_08_01 { "Spatial Profiling & Pan", recommended_ids_08_01, 5 };
inline constexpr int recommended_ids_08_02[] = { 165, 175, 176, 177, 283, 285 };
inline constexpr Subcategory recommended_subcategories_08_02 { "Textures, Noise & Arpeggios", recommended_ids_08_02, 6 };
inline constexpr Subcategory recommended_categories_08_subcategories[] =
{
    recommended_subcategories_08_00,
    recommended_subcategories_08_01,
    recommended_subcategories_08_02,
};
inline constexpr Category recommended_categories_08 { "08 Experimental, Textures & FX", recommended_categories_08_subcategories, 3 };

inline constexpr Category recommended_categories[] =
{
    recommended_categories_00,
    recommended_categories_01,
    recommended_categories_02,
    recommended_categories_03,
    recommended_categories_04,
    recommended_categories_05,
    recommended_categories_06,
    recommended_categories_07,
    recommended_categories_08,
};
inline constexpr Taxonomy recommended_taxonomy { recommended_categories, 9 };

inline constexpr int manual_ids_00_00[] = { 0 };
inline constexpr Subcategory manual_subcategories_00_00 { "Bypass", manual_ids_00_00, 1 };
inline constexpr Subcategory manual_categories_00_subcategories[] =
{
    manual_subcategories_00_00,
};
inline constexpr Category manual_categories_00 { "01 Null / Bypass", manual_categories_00_subcategories, 1 };

inline constexpr int manual_ids_01_00[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
inline constexpr Subcategory manual_subcategories_01_00 { "Classic Flangers", manual_ids_01_00, 20 };
inline constexpr Subcategory manual_categories_01_subcategories[] =
{
    manual_subcategories_01_00,
};
inline constexpr Category manual_categories_01 { "02 Flangers", manual_categories_01_subcategories, 1 };

inline constexpr int manual_ids_02_00[] = { 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43 };
inline constexpr Subcategory manual_subcategories_02_00 { "Vocal Transitions", manual_ids_02_00, 23 };
inline constexpr Subcategory manual_categories_02_subcategories[] =
{
    manual_subcategories_02_00,
};
inline constexpr Category manual_categories_02 { "03 Diphthong & Vocal Filters", manual_categories_02_subcategories, 1 };

inline constexpr int manual_ids_03_00[] = { 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69 };
inline constexpr Subcategory manual_subcategories_03_00 { "Multimode & Poles", manual_ids_03_00, 26 };
inline constexpr Subcategory manual_categories_03_subcategories[] =
{
    manual_subcategories_03_00,
};
inline constexpr Category manual_categories_03 { "04 Standard Filters", manual_categories_03_subcategories, 1 };

inline constexpr int manual_ids_04_00[] = { 70, 71, 72, 73, 74, 75 };
inline constexpr Subcategory manual_subcategories_04_00 { "Parametric EQ", manual_ids_04_00, 6 };
inline constexpr Subcategory manual_categories_04_subcategories[] =
{
    manual_subcategories_04_00,
};
inline constexpr Category manual_categories_04 { "05 Equalization Filters", manual_categories_04_subcategories, 1 };

inline constexpr int manual_ids_05_00[] = { 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116 };
inline constexpr Subcategory manual_subcategories_05_00 { "06a Acoustic & Keys", manual_ids_05_00, 41 };
inline constexpr int manual_ids_05_01[] = { 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130 };
inline constexpr Subcategory manual_subcategories_05_01 { "06b Harmonic & Waveshaping", manual_ids_05_01, 14 };
inline constexpr int manual_ids_05_02[] = { 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164 };
inline constexpr Subcategory manual_subcategories_05_02 { "06c Sweeps & Cavities", manual_ids_05_02, 34 };
inline constexpr int manual_ids_05_03[] = { 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194 };
inline constexpr Subcategory manual_subcategories_05_03 { "06d Dynamics & Spatial", manual_ids_05_03, 30 };
inline constexpr Subcategory manual_categories_05_subcategories[] =
{
    manual_subcategories_05_00,
    manual_subcategories_05_01,
    manual_subcategories_05_02,
    manual_subcategories_05_03,
};
inline constexpr Category manual_categories_05 { "06 Complex Filters", manual_categories_05_subcategories, 4 };

inline constexpr int manual_ids_06_00[] = { 195, 196, 197, 287, 288 };
inline constexpr Subcategory manual_subcategories_06_00 { "Distortion Models", manual_ids_06_00, 5 };
inline constexpr Subcategory manual_categories_06_subcategories[] =
{
    manual_subcategories_06_00,
};
inline constexpr Category manual_categories_06 { "07 Distortions", manual_categories_06_subcategories, 1 };

inline constexpr int manual_ids_07_00[] = { 198, 199, 200, 201, 202, 203, 204 };
inline constexpr Subcategory manual_subcategories_07_00 { "Vari-Pole", manual_ids_07_00, 7 };
inline constexpr Subcategory manual_categories_07_subcategories[] =
{
    manual_subcategories_07_00,
};
inline constexpr Category manual_categories_07 { "08 Vari-Pole Filters", manual_categories_07_subcategories, 1 };

inline constexpr int manual_ids_08_00[] = { 205, 206, 207, 208, 209, 210 };
inline constexpr Subcategory manual_subcategories_08_00 { "Pitch Tracking", manual_ids_08_00, 6 };
inline constexpr Subcategory manual_categories_08_subcategories[] =
{
    manual_subcategories_08_00,
};
inline constexpr Category manual_categories_08 { "09 Tracking Filters", manual_categories_08_subcategories, 1 };

inline constexpr int manual_ids_09_00[] = { 211, 212, 213, 214, 215, 216, 217, 218 };
inline constexpr Subcategory manual_subcategories_09_00 { "Parametric Tracking", manual_ids_09_00, 8 };
inline constexpr Subcategory manual_categories_09_subcategories[] =
{
    manual_subcategories_09_00,
};
inline constexpr Category manual_categories_09 { "10 Parametric Tracking Filters", manual_categories_09_subcategories, 1 };

inline constexpr int manual_ids_10_00[] = { 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229 };
inline constexpr Subcategory manual_subcategories_10_00 { "Harmonic Shifters & Phasers", manual_ids_10_00, 11 };
inline constexpr Subcategory manual_categories_10_subcategories[] =
{
    manual_subcategories_10_00,
};
inline constexpr Category manual_categories_10 { "11 Harmonic Shifters", manual_categories_10_subcategories, 1 };

inline constexpr int manual_ids_11_00[] = { 230, 231, 232, 233, 234 };
inline constexpr Subcategory manual_subcategories_11_00 { "Vowel Formants", manual_ids_11_00, 5 };
inline constexpr Subcategory manual_categories_11_subcategories[] =
{
    manual_subcategories_11_00,
};
inline constexpr Category manual_categories_11 { "12 Vocal Formants", manual_categories_11_subcategories, 1 };

inline constexpr int manual_ids_12_00[] = { 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267 };
inline constexpr Subcategory manual_subcategories_12_00 { "Instrument Resonances", manual_ids_12_00, 33 };
inline constexpr Subcategory manual_categories_12_subcategories[] =
{
    manual_subcategories_12_00,
};
inline constexpr Category manual_categories_12 { "13 Instrument Formant Filters", manual_categories_12_subcategories, 1 };

inline constexpr int manual_ids_13_00[] = { 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286 };
inline constexpr Subcategory manual_subcategories_13_00 { "Acoustic Objects & Textures", manual_ids_13_00, 19 };
inline constexpr Subcategory manual_categories_13_subcategories[] =
{
    manual_subcategories_13_00,
};
inline constexpr Category manual_categories_13 { "14 Miscellaneous & FX", manual_categories_13_subcategories, 1 };

inline constexpr Category manual_categories[] =
{
    manual_categories_00,
    manual_categories_01,
    manual_categories_02,
    manual_categories_03,
    manual_categories_04,
    manual_categories_05,
    manual_categories_06,
    manual_categories_07,
    manual_categories_08,
    manual_categories_09,
    manual_categories_10,
    manual_categories_11,
    manual_categories_12,
    manual_categories_13,
};
inline constexpr Taxonomy manual_taxonomy { manual_categories, 14 };

inline constexpr const Taxonomy& forGrouping (Grouping grouping) noexcept
{
    return grouping == Grouping::recommended ? recommended_taxonomy : manual_taxonomy;
}

} // namespace morphosis::preset_taxonomy
