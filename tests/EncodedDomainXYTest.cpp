#include "MorphosisDSP.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace
{

using morphosis::CoefficientSet;
using morphosis::MorphBlend;
using morphosis::MorphosisDSP;
using morphosis::NativeDescriptorSet;

constexpr std::uint32_t kMask = 0xffffffffu;
constexpr double kPi = 3.1415926535897932384626433832795;

void require (bool condition, const char* message)
{
    if (! condition)
        throw std::runtime_error (message);
}

std::int64_t signedValue (std::uint32_t value, int bits) noexcept
{
    const auto mask = (std::uint64_t (1) << bits) - 1u;
    const auto clipped = static_cast<std::uint64_t> (value) & mask;
    const auto sign = std::uint64_t (1) << (bits - 1);
    return static_cast<std::int64_t> ((clipped & sign) != 0
                                          ? clipped - (std::uint64_t (1) << bits)
                                          : clipped);
}

std::uint32_t interpolationWeights (int coordinate) noexcept
{
    const auto q = std::clamp (coordinate, 0, 32768);
    return ((static_cast<std::uint32_t> (-q) & 0xffffu) << 16)
         | (static_cast<std::uint32_t> (q - 32768) & 0xffffu);
}

std::uint32_t dot (std::uint32_t a, std::uint32_t b) noexcept
{
    const auto low = signedValue (a, 16) * signedValue (b, 16);
    const auto high = signedValue (a >> 16, 16) * signedValue (b >> 16, 16);
    return static_cast<std::uint32_t> (static_cast<std::uint64_t> (low + high) & kMask);
}

std::uint32_t packStage (std::uint32_t a, std::uint32_t b) noexcept
{
    const auto signedA = signedValue (a, 32);
    const auto shifted = static_cast<std::int32_t> (signedA >> 15);
    return ((b << 1) & 0xffff0000u) | (static_cast<std::uint32_t> (shifted) & 0xffffu);
}

std::uint32_t interpolate (const std::array<std::uint32_t, 4>& words,
                           const std::array<int, 3>& coordinates) noexcept
{
    const auto w1 = interpolationWeights (coordinates[0]);
    const auto w4 = interpolationWeights (coordinates[1]);
    const auto w5 = interpolationWeights (coordinates[2]);
    const std::array<std::uint32_t, 4> d {
        dot (w1, words[0]), dot (w1, words[1]), dot (w1, words[2]), dot (w1, words[3])
    };
    const auto a = dot (w4, packStage (d[0], d[1]));
    const auto b = dot (w4, packStage (d[2], d[3]));
    return static_cast<std::uint32_t> (0u - dot (w5, packStage (a, b)));
}

std::uint32_t expandInterpolated (std::uint32_t value, bool angle) noexcept
{
    const auto exponent = (value >> 26) & 15u;
    const auto mantissa = (value >> 15) & 2047u;
    if (angle)
        return (mantissa | 2048u) << exponent;

    const auto nonzero = exponent != 0u ? 1u : 0u;
    return (mantissa | (nonzero << 11)) << (exponent - nonzero);
}

float floatFromBits (std::uint32_t bits) noexcept
{
    float value = 0.0f;
    std::memcpy (&value, &bits, sizeof (value));
    return value;
}

std::array<std::uint32_t, 4> readField (const morphosis::cube_data::CubeRecord& record,
                                        int offset) noexcept
{
    std::array<std::uint32_t, 4> words {};
    for (int index = 0; index < 4; ++index)
        words[static_cast<std::size_t> (index)] = record.words[
            static_cast<std::size_t> (offset + index)] & (index == 0 ? 0x7fffffffu : 0xffffffffu);
    return words;
}

std::array<int, 3> coordinates (double frequency, double morph, double transform) noexcept
{
    const auto coordinate = [] (double value) noexcept
    {
        if (! std::isfinite (value))
            value = 0.0;
        value = std::clamp (value, -5.0, 5.0);
        return static_cast<int> (std::lround ((value + 5.0) * 32768.0 / 10.0));
    };
    return { coordinate (transform), coordinate (frequency), coordinate (morph) };
}

double encodedCoordinate (const std::array<std::uint32_t, 4>& words,
                          const std::array<int, 3>& coords) noexcept
{
    return static_cast<double> (interpolate (words, coords) & 0x3fffffffU) / 32768.0;
}

double decode (double value, bool angle, std::uint32_t scaleBits) noexcept
{
    const auto code = std::clamp (static_cast<long long> (std::floor (value)), 0ll, 32767ll);
    const auto reconstructed = static_cast<std::uint32_t> (code) << 15;
    const auto expanded = static_cast<float> (expandInterpolated (reconstructed, angle));
    return static_cast<double> (static_cast<float> (
        static_cast<double> (expanded) * static_cast<double> (floatFromBits (scaleBits))));
}

struct ReferenceSource
{
    std::array<double, morphosis::kStageCount> poleAngleQ {};
    std::array<double, morphosis::kStageCount> poleRadiusQ {};
    std::array<double, morphosis::kStageCount> zeroAngleQ {};
    std::array<double, morphosis::kStageCount> zeroRadiusQ {};
    std::array<bool, morphosis::kStageCount> normalizationFlags {};
    double gainQ = 0.0;
};

ReferenceSource extract (int preset, const std::array<int, 3>& coords)
{
    ReferenceSource result;
    const auto& record = morphosis::cube_data::kCubes[static_cast<std::size_t> (preset)];
    for (int stageIndex = 0; stageIndex < morphosis::kStageCount; ++stageIndex)
    {
        const auto offset = stageIndex * 16;
        const auto stage = static_cast<std::size_t> (stageIndex);
        result.poleAngleQ[stage] = encodedCoordinate (readField (record, offset), coords);
        result.poleRadiusQ[stage] = encodedCoordinate (readField (record, offset + 4), coords);
        result.normalizationFlags[stage] =
            (record.words[static_cast<std::size_t> (offset + 8)] & 0x80000000u) != 0;
        if (stageIndex < morphosis::kStageCount - 1)
        {
            result.zeroAngleQ[stage] = encodedCoordinate (
                readField (record, offset + 8), coords);
            result.zeroRadiusQ[stage] = encodedCoordinate (
                readField (record, offset + 12), coords);
        }
    }
    const std::array<std::uint32_t, 4> gainWords {
        record.words[112], record.words[113], record.words[114], record.words[115]
    };
    result.gainQ = encodedCoordinate (gainWords, coords);
    return result;
}

NativeDescriptorSet referenceNative (const MorphBlend& rawBlend,
                                     double frequency,
                                     double morph,
                                     double transform)
{
    MorphBlend blend = rawBlend;
    double total = 0.0;
    for (int index = 0; index < blend.count; ++index)
        total += std::max (0.0, blend.weights[static_cast<std::size_t> (index)]);
    for (int index = 0; index < blend.count; ++index)
        blend.weights[static_cast<std::size_t> (index)] = std::max (
            0.0, blend.weights[static_cast<std::size_t> (index)]) / std::max (total, 1.0e-12);

    const auto coords = coordinates (frequency, morph, transform);
    std::array<int, morphosis::kMaxBlendSources> uniquePresets {};
    std::array<ReferenceSource, morphosis::kMaxBlendSources> uniqueSources {};
    std::array<double, morphosis::kMaxBlendSources> uniqueWeights {};
    int uniqueCount = 0;
    for (int index = 0; index < blend.count; ++index)
    {
        const auto weight = blend.weights[static_cast<std::size_t> (index)];
        if (weight <= 0.0)
            continue;
        const auto preset = blend.presets[static_cast<std::size_t> (index)];
        int unique = -1;
        for (int candidate = 0; candidate < uniqueCount; ++candidate)
            if (uniquePresets[static_cast<std::size_t> (candidate)] == preset)
                unique = candidate;
        if (unique < 0)
        {
            unique = uniqueCount++;
            uniquePresets[static_cast<std::size_t> (unique)] = preset;
            uniqueSources[static_cast<std::size_t> (unique)] = extract (preset, coords);
        }
        uniqueWeights[static_cast<std::size_t> (unique)] += weight;
    }

    NativeDescriptorSet result;
    result.gain = 0.0;
    result.valid = false;
    result.usedSafeFallback = false;
    for (auto& stage : result.stages)
    {
        stage.poleAngle = 0.0;
        stage.poleRadius = 0.0;
        stage.zeroAngle = 0.0;
        stage.zeroRadius = 0.0;
        stage.normalizationExponent = 0.0;
    }
    for (int unique = 0; unique < uniqueCount; ++unique)
    {
        const auto weight = uniqueWeights[static_cast<std::size_t> (unique)];
        const auto& source = uniqueSources[static_cast<std::size_t> (unique)];
        result.gain += weight * source.gainQ;
        for (int stageIndex = 0; stageIndex < morphosis::kStageCount; ++stageIndex)
        {
            const auto stage = static_cast<std::size_t> (stageIndex);
            result.stages[stage].poleAngle += weight * source.poleAngleQ[stage];
            result.stages[stage].poleRadius += weight * source.poleRadiusQ[stage];
            result.stages[stage].normalizationExponent += weight * static_cast<double> (
                source.normalizationFlags[stage]);
            if (stageIndex < morphosis::kStageCount - 1)
            {
                result.stages[stage].zeroAngle += weight * source.zeroAngleQ[stage];
                result.stages[stage].zeroRadius += weight * source.zeroRadiusQ[stage];
            }
        }
    }

    for (int stageIndex = 0; stageIndex < morphosis::kStageCount; ++stageIndex)
    {
        auto& stage = result.stages[static_cast<std::size_t> (stageIndex)];
        stage.poleAngle = decode (stage.poleAngle, true, 0x32c90fdbu);
        stage.poleRadius = 1.0 - decode (stage.poleRadius, false, 0x32800800u);
        if (stageIndex < morphosis::kStageCount - 1)
        {
            stage.zeroAngle = decode (stage.zeroAngle, true, 0x32c90fdbu);
            stage.zeroRadius = 1.0 - decode (stage.zeroRadius, false, 0x32800800u);
        }
    }
    result.gain = decode (result.gain, false, 0x338007ffu);
    result.valid = true;
    return result;
}

MorphBlend blendFor (const std::array<int, morphosis::kMaxBlendSources>& presets,
                     const std::array<double, morphosis::kMaxBlendSources>& weights)
{
    MorphBlend result;
    result.count = morphosis::kMaxBlendSources;
    result.presets = presets;
    result.weights = weights;
    result.nonlinearSources.fill (false);
    return result;
}

double coefficientError (const CoefficientSet& left, const CoefficientSet& right) noexcept
{
    double error = std::abs (left.gain - right.gain);
    for (std::size_t index = 0; index < left.stages.size(); ++index)
    {
        const auto& a = left.stages[index];
        const auto& b = right.stages[index];
        error = std::max ({ error, std::abs (a.a - b.a), std::abs (a.b - b.b),
                            std::abs (a.inputGain - b.inputGain),
                            std::abs (a.radius - b.radius),
                            std::abs (a.zeroRadius - b.zeroRadius),
                            std::abs (a.normalizationExponent - b.normalizationExponent) });
    }
    return error;
}

double responsePeakDb (const CoefficientSet& coefficients, double rate) noexcept
{
    double peak = -300.0;
    for (int index = 0; index < 512; ++index)
    {
        const auto proportion = static_cast<double> (index) / 511.0;
        const auto frequency = 20.0 * std::pow (rate * 0.5 / 20.0, proportion);
        const auto magnitude = std::max (MorphosisDSP::responseMagnitude (
            coefficients, frequency, rate), 1.0e-12);
        peak = std::max (peak, 20.0 * std::log10 (magnitude));
    }
    return peak;
}

void checkReferenceAndIdentities()
{
    const std::array<double, morphosis::kMaxBlendSources> weights { 0.13, 0.27, 0.31, 0.29 };
    const std::array<int, morphosis::kMaxBlendSources> sources { 148, 147, 149, 146 };
    const std::array<int, morphosis::kMaxBlendSources> permuted { 146, 149, 147, 148 };
    const std::array<double, morphosis::kMaxBlendSources> permutedWeights { 0.29, 0.31, 0.27, 0.13 };
    const auto blend = blendFor (sources, weights);
    const auto expectedNative = referenceNative (blend, 0.37, -1.25, 2.1);
    const auto expected = MorphosisDSP::adaptNativeDescriptorForTesting (
        expectedNative, 48000.0);
    const auto actual = MorphosisDSP::makeEncodedDomainCoefficients (
        blend, 0.37, -1.25, 2.1, 48000.0);
    require (coefficientError (actual, expected) < 1.0e-8,
             "encoded production coefficients match independent reference");

    const auto permutation = MorphosisDSP::makeEncodedDomainCoefficients (
        blendFor (permuted, permutedWeights), 0.37, -1.25, 2.1, 48000.0);
    require (coefficientError (actual, permutation) < 1.0e-8,
             "encoded blend is invariant under source permutation");

    for (int preset = 0; preset < morphosis::cube_data::kRecordCount; ++preset)
    {
        std::array<int, morphosis::kMaxBlendSources> repeated { preset, preset, preset, preset };
        const auto repeatedBlend = blendFor (repeated, weights);
        const auto encoded = MorphosisDSP::makeEncodedDomainCoefficients (
            repeatedBlend, 0.0, 0.0, 0.0, 48000.0);
        const auto ordinary = MorphosisDSP::makeCoefficients (
            preset, 0.0, 0.0, 0.0, 48000.0);
        require (coefficientError (encoded, ordinary) < 1.0e-12,
                 "all 289 repeated-source identities match ordinary decoding");
    }

    std::array<int, morphosis::kMaxBlendSources> nulls { 0, 0, 0, 0 };
    const auto nullBlend = blendFor (nulls, weights);
    const auto nullEncoded = MorphosisDSP::makeEncodedDomainCoefficients (
        nullBlend, 0.41, 0.63, -0.22, 96000.0);
    const auto nullOrdinary = MorphosisDSP::makeCoefficients (
        0, 0.41, 0.63, -0.22, 96000.0);
    require (coefficientError (nullEncoded, nullOrdinary) < 1.0e-12,
             "all-Null interior identity matches ordinary decoding");

    // Exercise the floor/code boundary and retained low fractional bits in the
    // independent decoder. The expected step is intentional encoded-domain
    // quantization, not a continuous limit.
    const auto below = decode (12.99999, false, 0x32800800u);
    const auto above = decode (13.00001, false, 0x32800800u);
    require (std::isfinite (below) && std::isfinite (above) && above >= below,
             "encoded exponent/mantissa boundary reference is finite and monotonic");

    const std::array<std::array<double, 3>, 8> corners {
        std::array<double, 3> { -5.0, -5.0, -5.0 },
        std::array<double, 3> {  5.0, -5.0, -5.0 },
        std::array<double, 3> { -5.0,  5.0, -5.0 },
        std::array<double, 3> {  5.0,  5.0, -5.0 },
        std::array<double, 3> { -5.0, -5.0,  5.0 },
        std::array<double, 3> {  5.0, -5.0,  5.0 },
        std::array<double, 3> { -5.0,  5.0,  5.0 },
        std::array<double, 3> {  5.0,  5.0,  5.0 }
    };
    for (int preset = 0; preset < morphosis::cube_data::kRecordCount; ++preset)
        for (const auto& corner : corners)
        {
            const std::array<int, morphosis::kMaxBlendSources> repeated {
                preset, preset, preset, preset
            };
            const auto encoded = MorphosisDSP::makeEncodedDomainCoefficients (
                blendFor (repeated, weights), corner[0], corner[1], corner[2], 48000.0);
            const auto ordinary = MorphosisDSP::makeCoefficients (
                preset, corner[0], corner[1], corner[2], 48000.0);
            require (coefficientError (encoded, ordinary) < 1.0e-12,
                     "all 289 source identities match ordinary decoding at every cube corner");
        }

    // Merging duplicate IDs before numeric construction must be invariant to
    // slot spelling, including a source split across non-adjacent entries.
    const auto split = blendFor ({ 0, 43, 0, 184 }, { 0.17, 0.23, 0.29, 0.31 });
    const auto merged = blendFor ({ 0, 43, 184, 184 }, { 0.46, 0.23, 0.31, 0.0 });
    const auto splitCoefficients = MorphosisDSP::makeEncodedDomainCoefficients (
        split, -1.7, 2.2, -0.9, 44100.0);
    const auto mergedCoefficients = MorphosisDSP::makeEncodedDomainCoefficients (
        merged, -1.7, 2.2, -0.9, 44100.0);
    require (coefficientError (splitCoefficients, mergedCoefficients) < 1.0e-12,
             "duplicate encoded sources merge without changing coefficients");

    const std::array<int, morphosis::kMaxBlendSources> quadPresets { 0, 43, 184, 288 };
    const std::array<bool, morphosis::kMaxBlendSources> noNonlinear {
        false, false, false, false
    };
    for (const auto x : { 0.0, 1.0 })
        for (const auto y : { 0.0, 1.0 })
        {
            const auto blendAtCorner = MorphosisDSP::makeBilinearBlend (
                quadPresets, noNonlinear, x, y);
            const auto encoded = MorphosisDSP::makeEncodedDomainCoefficients (
                blendAtCorner, 0.0, 0.0, 0.0, 48000.0);
            const auto cornerIndex = (x >= 0.5 ? 1 : 0) + (y < 0.5 ? 2 : 0);
            const auto ordinary = MorphosisDSP::makeCoefficients (
                quadPresets[static_cast<std::size_t> (cornerIndex)],
                0.0, 0.0, 0.0, 48000.0);
            require (coefficientError (encoded, ordinary) < 1.0e-12,
                     "encoded XY corners preserve source identity");
        }

    for (const auto x : { 1.0e-9, 1.0 - 1.0e-9 })
        for (const auto y : { 1.0e-9, 1.0 - 1.0e-9 })
        {
            const auto nearCorner = MorphosisDSP::makeEncodedDomainCoefficients (
                MorphosisDSP::makeBilinearBlend (quadPresets, noNonlinear, x, y),
                1.1, -0.7, 2.3, 96000.0);
            require (nearCorner.valid && ! nearCorner.usedSafeFallback,
                     "encoded near-corner blend remains valid without fallback");
        }
}

void checkCorpusAndDynamics()
{
    const std::array<std::array<int, morphosis::kMaxBlendSources>, 6> named {
        std::array<int, 4> { 148, 147, 149, 146 },
        std::array<int, 4> { 43, 0, 0, 0 },
        std::array<int, 4> { 0, 43, 0, 0 },
        std::array<int, 4> { 0, 0, 0, 0 },
        std::array<int, 4> { 184, 185, 186, 187 },
        std::array<int, 4> { 17, 18, 19, 20 }
    };
    const std::array<double, morphosis::kMaxBlendSources> quadWeights { 0.25, 0.25, 0.25, 0.25 };
    std::mt19937 generator (99539473u);
    std::uniform_int_distribution<int> presetDistribution (0, morphosis::cube_data::kRecordCount - 1);
    double worstPeak = -300.0;
    int worstPreset = -1;
    double worstRate = 0.0;
    int worstCase = -1;
    std::array<int, morphosis::kMaxBlendSources> worstQuad {};
    std::size_t fallbackCount = 0;

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto& quad : named)
        {
            const auto coefficients = MorphosisDSP::makeEncodedDomainCoefficients (
                blendFor (quad, quadWeights), 0.0, 0.0, 0.0, rate);
            require (coefficients.valid && ! coefficients.usedSafeFallback,
                     "named encoded corpus case has no coefficient fallback");
            const auto peak = responsePeakDb (coefficients, rate);
            if (peak > worstPeak)
            {
                worstPeak = peak;
                worstPreset = quad[0];
                worstRate = rate;
                worstCase = -1;
                worstQuad = quad;
            }
        }

        for (int caseIndex = 0; caseIndex < 32; ++caseIndex)
        {
            std::array<int, morphosis::kMaxBlendSources> quad {};
            for (auto& preset : quad)
                preset = presetDistribution (generator);
            const auto x = 0.05 + 0.90 * static_cast<double> (caseIndex % 8) / 7.0;
            const auto y = 0.05 + 0.90 * static_cast<double> (caseIndex / 8) / 3.0;
            MorphBlend blend = MorphosisDSP::makeBilinearBlend (
                quad, std::array<bool, 4> { false, false, false, false }, x, y);
            const auto coefficients = MorphosisDSP::makeEncodedDomainCoefficients (
                blend, -3.0 + caseIndex * 0.19, 1.0 - caseIndex * 0.07,
                -2.0 + caseIndex * 0.11, rate);
            if (! coefficients.valid || coefficients.usedSafeFallback)
            {
                ++fallbackCount;
                std::cout << "encoded_fallback rate=" << rate
                          << " case=" << caseIndex
                          << " quad=" << quad[0] << ',' << quad[1] << ','
                          << quad[2] << ',' << quad[3]
                          << " coords=" << (-3.0 + caseIndex * 0.19)
                          << ',' << (1.0 - caseIndex * 0.07)
                          << ',' << (-2.0 + caseIndex * 0.11)
                          << " valid=" << coefficients.valid
                          << " usedSafeFallback=" << coefficients.usedSafeFallback
                          << '\n';
            }
            require (coefficients.valid, "random encoded corpus case remains finite");
            const auto peak = responsePeakDb (coefficients, rate);
            if (peak > worstPeak)
            {
                worstPeak = peak;
                worstPreset = quad[0];
                worstRate = rate;
                worstCase = caseIndex;
                worstQuad = quad;
            }
        }
    }

    double dynamicWorstPeak = 0.0;
    std::size_t dynamicFallbackCount = 0;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        MorphosisDSP dsp;
        dsp.prepare (rate, 64);
        morphosis::ParameterSnapshot parameters;
        parameters.mode = morphosis::ProcessingMode::xy;
        parameters.xyEncodedDomain = true;
        parameters.xyPresets = named[0];
        parameters.xyX = 0.5;
        parameters.xyY = 0.5;
        parameters.softClip = false;
        parameters.dryWet = 1.0;

        double peak = 0.0;
        for (int block = 0; block < 64; ++block)
        {
            parameters.xyX = 0.02 + 0.96 * static_cast<double> (block % 32) / 31.0;
            parameters.xyY = 0.98 - 0.96 * static_cast<double> (block % 32) / 31.0;
            dsp.beginBlock (parameters);
            for (int sample = 0; sample < 64; ++sample)
            {
                const auto input = 0.001f * std::sin (0.017f * static_cast<float> (
                    block * 64 + sample));
                const auto left = dsp.processSample (input, 0);
                const auto right = dsp.processSample (-input, 1);
                require (std::isfinite (left) && std::isfinite (right),
                         "encoded dynamic stereo output remains finite");
                peak = std::max ({ peak, std::abs (static_cast<double> (left)),
                                   std::abs (static_cast<double> (right)) });
            }
        }
        require (std::isfinite (peak), "encoded dynamic peak is finite");
        dynamicWorstPeak = std::max (dynamicWorstPeak, peak);
        dynamicFallbackCount += dsp.getCoefficientFallbackCountForTesting();
        require (dsp.getCoefficientFallbackCountForTesting() == 0,
                 "encoded dynamic path does not enter safe fallback");
        dsp.releaseResources();
    }

    std::cout << "encoded_corpus_worst_peak_db=" << worstPeak
              << " worst_rate=" << worstRate
              << " worst_case=" << worstCase
              << " worst_quad=" << worstQuad[0] << ',' << worstQuad[1] << ','
              << worstQuad[2] << ',' << worstQuad[3]
              << " worst_first_source=" << worstPreset
              << " fallback_cases=" << fallbackCount
              << " dynamic_worst_peak=" << dynamicWorstPeak
              << " dynamic_fallbacks=" << dynamicFallbackCount << '\n';
    require (fallbackCount == 0, "encoded corpus has no safe-fallback churn");
}

void runBenchmark()
{
    constexpr int blockSize = 512;
    constexpr double durationSeconds = 2.0;
    const std::array<int, morphosis::kMaxBlendSources> sources { 0, 17, 184, 288 };
    double aggregateChecksum = 0.0;

    std::cout << "Encoded-domain XY Release benchmark (stereo, four distinct sources, "
                 "two seconds per case)\n";
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto moving : { false, true })
        {
            MorphosisDSP dsp;
            dsp.prepare (rate, blockSize);
            dsp.setTelemetry (nullptr, nullptr, nullptr);
            morphosis::ParameterSnapshot parameters;
            parameters.mode = morphosis::ProcessingMode::xy;
            parameters.xyEncodedDomain = true;
            parameters.xyPresets = sources;
            parameters.softClip = false;
            parameters.internalDistortion = false;
            parameters.dryWet = 1.0;

            const auto total = static_cast<std::int64_t> (
                std::llround (rate * durationSeconds));
            double peak = 0.0;
            volatile double checksum = 0.0;
            const auto start = std::chrono::steady_clock::now();
            for (std::int64_t processed = 0; processed < total; processed += blockSize)
            {
                const auto phase = static_cast<double> (processed) / rate;
                parameters.xyX = moving ? 0.5 + 0.49 * std::sin (2.0 * kPi * 0.37 * phase)
                                         : 0.5;
                parameters.xyY = moving ? 0.5 + 0.49 * std::cos (2.0 * kPi * 0.23 * phase)
                                         : 0.5;
                parameters.frequency = moving ? 4.0 * std::sin (2.0 * kPi * 0.11 * phase)
                                              : -1.0;
                parameters.morph = moving ? 4.0 * std::cos (2.0 * kPi * 0.07 * phase)
                                           : 1.0;
                parameters.transform = moving ? 3.0 * std::sin (2.0 * kPi * 0.13 * phase)
                                              : 0.5;
                dsp.beginBlock (parameters);
                const auto count = std::min<std::int64_t> (blockSize, total - processed);
                for (std::int64_t sample = 0; sample < count; ++sample)
                {
                    const auto index = processed + sample;
                    const auto input = static_cast<float> (
                        0.19 * std::sin (0.013 * static_cast<double> (index))
                        + 0.03 * std::cos (0.071 * static_cast<double> (index)));
                    const auto left = dsp.processSample (input, 0);
                    const auto right = dsp.processSample (input * 0.71f, 1);
                    peak = std::max ({ peak, std::abs (static_cast<double> (left)),
                                       std::abs (static_cast<double> (right)) });
                    checksum += static_cast<double> (left) + static_cast<double> (right);
                }
            }
            const auto elapsed = std::chrono::duration<double> (
                std::chrono::steady_clock::now() - start).count();
            const auto stereoSamples = static_cast<double> (total);
            aggregateChecksum += checksum;
            std::cout << "encoded_cpu rate=" << rate
                      << " moving=" << (moving ? 1 : 0)
                      << " elapsed_s=" << elapsed
                      << " ns_per_stereo_sample=" << elapsed * 1.0e9 / stereoSamples
                      << " peak=" << peak
                      << " fallbacks=" << dsp.getCoefficientFallbackCountForTesting()
                      << " checksum=" << checksum << '\n';
            dsp.releaseResources();
        }
    std::cout << "encoded_cpu_aggregate_checksum=" << aggregateChecksum << '\n';
}

} // namespace

int main (int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string (argv[1]) == "--benchmark")
        {
            runBenchmark();
            return 0;
        }
        checkReferenceAndIdentities();
        checkCorpusAndDynamics();
        std::cout << "Encoded-domain XY reference, corpus, and dynamic tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Encoded-domain XY test failed: " << error.what() << '\n';
        return 1;
    }
}
