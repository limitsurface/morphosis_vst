#pragma once

#include "MorphosisDSP.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace morphosis
{

enum class SequenceSync : int
{
    straight = 0,
    dotted = 1,
    triplet = 2
};

// These indices are intentionally stable because they are stored in plugin
// state and exposed to hosts through the APVTS parameter.
enum class SequenceDivision : int
{
    fourBars = 0,
    twoBars = 1,
    oneBar = 2,
    half = 3,
    quarter = 4,
    eighth = 5,
    sixteenth = 6,
    thirtySecond = 7
};

inline double sequenceDivisionQuarterBeats (int division) noexcept
{
    switch (static_cast<SequenceDivision> (std::clamp (division, 0, 7)))
    {
        case SequenceDivision::fourBars: return 16.0;
        case SequenceDivision::twoBars: return 8.0;
        case SequenceDivision::oneBar: return 4.0;
        case SequenceDivision::half: return 2.0;
        case SequenceDivision::quarter: return 1.0;
        case SequenceDivision::eighth: return 0.5;
        case SequenceDivision::sixteenth: return 0.25;
        case SequenceDivision::thirtySecond: return 0.125;
    }
    return 1.0;
}

inline double sequenceSyncMultiplier (int sync) noexcept
{
    switch (static_cast<SequenceSync> (std::clamp (sync, 0, 2)))
    {
        case SequenceSync::straight: return 1.0;
        case SequenceSync::dotted: return 1.5;
        case SequenceSync::triplet: return 2.0 / 3.0;
    }
    return 1.0;
}

inline double sequencePeriodQuarterBeats (int division, int sync) noexcept
{
    return sequenceDivisionQuarterBeats (division) * sequenceSyncMultiplier (sync);
}

inline int sequenceStepFromPosition (double position) noexcept
{
    const auto safe = std::clamp (std::isfinite (position) ? position : 0.0, 0.0, 1.0);
    return std::clamp (static_cast<int> (std::lround (
                            safe * static_cast<double> (kSequenceSlotCount - 1))),
                       0, kSequenceSlotCount - 1);
}

inline MorphBlend makeHostSequenceBlend (
    const std::array<int, kSequenceSlotCount>& presets,
    const std::array<bool, kSequenceSlotCount>& nonlinearSources,
    int length,
    int step,
    double phase,
    double glide,
    double minimumGlideFraction) noexcept
{
    const auto safeLength = std::clamp (length, 1, kSequenceSlotCount);
    const auto safeStep = ((step % safeLength) + safeLength) % safeLength;
    const auto safePhase = std::clamp (std::isfinite (phase) ? phase : 0.0, 0.0, 1.0);
    const auto safeGlide = std::clamp (std::isfinite (glide) ? glide : 1.0, 0.0, 1.0);
    const auto minimum = std::clamp (std::isfinite (minimumGlideFraction)
                                         ? minimumGlideFraction : 0.0,
                                     0.0, 1.0);
    // The minimum applies to every requested glide below that floor, not only
    // to the exact zero setting. Keep the floor bounded by one full step.
    const auto glideFraction = std::min (1.0, std::max (safeGlide, minimum));

    MorphBlend blend;
    const auto current = static_cast<std::size_t> (safeStep);
    const auto previous = static_cast<std::size_t> ((safeStep + safeLength - 1) % safeLength);
    blend.presets[0] = presets[previous];
    blend.presets[1] = presets[current];
    blend.nonlinearSources[0] = nonlinearSources[previous];
    blend.nonlinearSources[1] = nonlinearSources[current];
    blend.count = 2;

    if (glideFraction <= 0.0 || safePhase >= glideFraction)
    {
        blend.count = 1;
        blend.presets[0] = presets[current];
        blend.nonlinearSources[0] = nonlinearSources[current];
        blend.weights[0] = 1.0;
        return blend;
    }

    const auto amount = std::clamp (safePhase / glideFraction, 0.0, 1.0);
    blend.weights[0] = 1.0 - amount;
    blend.weights[1] = amount;
    return blend;
}

} // namespace morphosis
