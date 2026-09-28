#include "MorphosisDSP.h"
#include "MorphosisSequencer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

using morphosis::CoefficientSet;
using morphosis::MorphBlend;
using morphosis::MorphosisDSP;
using morphosis::NativeDescriptorSet;
using morphosis::ParameterSnapshot;
using morphosis::ProcessingMode;

constexpr double kPi = 3.14159265358979323846;

int failures = 0;

void require (bool condition, const std::string& message)
{
    if (! condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void requireNear (double actual, double expected, double tolerance, const std::string& message)
{
    require (std::isfinite (actual) && std::isfinite (expected)
                 && std::abs (actual - expected) <= tolerance,
             message + " actual=" + std::to_string (actual)
                 + " expected=" + std::to_string (expected));
}

double coefficientDistance (const CoefficientSet& left, const CoefficientSet& right)
{
    double result = std::abs (left.gain - right.gain);
    for (std::size_t index = 0; index < left.stages.size(); ++index)
    {
        const auto& a = left.stages[index];
        const auto& b = right.stages[index];
        result = std::max ({ result,
                             std::abs (a.a - b.a),
                             std::abs (a.b - b.b),
                             std::abs (a.inputGain - b.inputGain),
                             std::abs (a.z1 - b.z1),
                             std::abs (a.z2 - b.z2),
                             std::abs (a.radius - b.radius),
                             std::abs (a.normalizationExponent - b.normalizationExponent) });
    }
    return result;
}

MorphBlend twoSourceBlend (int first, int second, double firstWeight, double secondWeight)
{
    MorphBlend blend;
    blend.count = 2;
    blend.presets[0] = first;
    blend.presets[1] = second;
    blend.weights[0] = firstWeight;
    blend.weights[1] = secondWeight;
    return blend;
}

ParameterSnapshot presetParameters (int preset, double frequency = 0.0,
                                    double morph = 0.0, double transform = 0.0)
{
    ParameterSnapshot result;
    result.preset = preset;
    result.frequency = frequency;
    result.morph = morph;
    result.transform = transform;
    result.softClip = false;
    result.internalDistortion = false;
    result.mode = ProcessingMode::preset;
    result.blend.count = 1;
    result.blend.presets[0] = preset;
    result.blend.weights[0] = 1.0;
    return result;
}

ParameterSnapshot xyParameters (const std::array<int, morphosis::kMaxBlendSources>& presets,
                                 const std::array<bool, morphosis::kMaxBlendSources>& nonlinear,
                                 double x = 0.5, double y = 0.5)
{
    ParameterSnapshot result = presetParameters (0, -1.25, 0.75, 2.5);
    result.mode = ProcessingMode::xy;
    result.xyPresets = presets;
    result.xyNonlinearSources = nonlinear;
    result.xyX = x;
    result.xyY = y;
    return result;
}

ParameterSnapshot hostSequenceParameters (int preset)
{
    auto result = presetParameters (preset, 1.0, -1.0, 2.0);
    result.mode = ProcessingMode::sequencer;
    result.sequenceManual = false;
    result.sequenceLength = 4;
    result.sequencePresets = { 0, 43, 17, 184, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0 };
    result.sequenceNonlinearSources.fill (false);
    result.blend.count = 1;
    result.blend.presets[0] = preset;
    result.blend.weights[0] = 1.0;
    return result;
}

double deterministicInput (std::int64_t sample, double rate)
{
    const auto t = static_cast<double> (sample) / rate;
    return 0.21 * std::sin (2.0 * kPi * 233.0 * t)
         + 0.07 * std::cos (2.0 * kPi * 911.0 * t)
         + 0.03 * std::sin (2.0 * kPi * 3711.0 * t + 0.31);
}

void processBlock (MorphosisDSP& processor, const ParameterSnapshot& parameters,
                   int blockSize, std::int64_t firstSample, double& peak,
                   double& sumSquares)
{
    processor.beginBlock (parameters);
    for (int sample = 0; sample < blockSize; ++sample)
    {
        processor.advanceSample();
        const auto input = static_cast<float> (deterministicInput (firstSample + sample,
                                                                    processor.getSampleRate()));
        const auto left = processor.processSampleNoAdvance (input, 0);
        const auto right = processor.processSampleNoAdvance (input, 1);
        require (std::isfinite (left) && std::isfinite (right), "transition audio remains finite");
        peak = std::max (peak, std::max (std::abs (static_cast<double> (left)),
                                         std::abs (static_cast<double> (right))));
        sumSquares += static_cast<double> (left) * left + static_cast<double> (right) * right;
    }
}

void checkAccumulatorAndIdentityPaths()
{
    constexpr auto hostRate = morphosis::kFirmwareNominalRate;
    const auto first = MorphosisDSP::makeNativeDescriptor (0, 0.0, 0.0, 0.0);
    const auto second = MorphosisDSP::makeNativeDescriptor (43, 0.0, 0.0, 0.0);
    require (first.valid && second.valid && ! first.usedSafeFallback && ! second.usedSafeFallback,
             "representative native descriptors are fallback-free");

    const auto mixed = MorphosisDSP::makeBlendedCoefficients (
        twoSourceBlend (0, 43, 0.25, 0.75), 0.0, 0.0, 0.0, hostRate);
    require (mixed.valid && ! mixed.usedSafeFallback,
             "convex two-source blend remains valid without fallback");
    for (std::size_t index = 0; index < mixed.stages.size(); ++index)
    {
        const auto expected = 0.25 * first.stages[index].poleRadius
                            + 0.75 * second.stages[index].poleRadius;
        requireNear (mixed.stages[index].radius, expected, 2.0e-12,
                     "weighted pole radius has additive identity");
    }

    for (const auto weights : std::array<std::array<double, 2>, 3> {
             std::array<double, 2> { 1.0, 0.0 },
             std::array<double, 2> { 1.0 - 1.0e-9, 1.0e-9 },
             std::array<double, 2> { 1.0e-9, 1.0 - 1.0e-9 } })
    {
        const auto coefficient = MorphosisDSP::makeBlendedCoefficients (
            twoSourceBlend (0, 43, weights[0], weights[1]), 0.0, 0.0, 0.0, hostRate);
        require (coefficient.valid && ! coefficient.usedSafeFallback,
                 "endpoint and tiny nonzero blend weights stay fallback-free");
    }

    MorphBlend duplicate;
    duplicate.count = 4;
    duplicate.presets = { 43, 43, 43, 43 };
    duplicate.weights = { 0.1, 0.2, 0.3, 0.4 };
    const auto duplicateResult = MorphosisDSP::makeBlendedCoefficients (
        duplicate, 0.0, 0.0, 0.0, hostRate);
    const auto singleResult = MorphosisDSP::makeCoefficients (43, 0.0, 0.0, 0.0, hostRate);
    require (coefficientDistance (duplicateResult, singleResult) < 1.0e-12,
             "duplicate-source blend collapses exactly to its source");

    const auto permutationA = MorphosisDSP::makeBlendedCoefficients (
        twoSourceBlend (0, 43, 0.25, 0.75), 0.0, 0.0, 0.0, hostRate);
    const auto permutationB = MorphosisDSP::makeBlendedCoefficients (
        twoSourceBlend (43, 0, 0.75, 0.25), 0.0, 0.0, 0.0, hostRate);
    require (coefficientDistance (permutationA, permutationB) < 1.0e-12,
             "source permutation preserves the same coefficient set");

    const std::array<int, morphosis::kMaxBlendSources> allNull { 0, 0, 0, 0 };
    const std::array<bool, morphosis::kMaxBlendSources> allLinear { false, false, false, false };
    const auto allNullBlend = MorphosisDSP::makeBilinearBlend (allNull, allLinear, 0.37, 0.63);
    const auto allNullResult = MorphosisDSP::makeBlendedCoefficients (
        allNullBlend, 0.0, 0.0, 0.0, hostRate);
    require (coefficientDistance (allNullResult, singleResult) > 1.0e-6,
             "test fixture keeps the all-Null and VowelSpace sources distinct");
    const auto allNullReference = MorphosisDSP::makeCoefficients (0, 0.0, 0.0, 0.0, hostRate);
    require (coefficientDistance (
                 MorphosisDSP::makeBlendedCoefficients (
                     allNullBlend, 0.0, 0.0, 0.0, hostRate), allNullReference) < 1.0e-12,
             "all-Null XY blend is an exact identity path");
}

void checkFallbackPolicy()
{
#if defined(MORPHOSIS_TESTING)
    NativeDescriptorSet invalid;
    invalid.valid = false;
    invalid.usedSafeFallback = true;
    invalid.gain = std::numeric_limits<double>::quiet_NaN();
    const auto neutral = MorphosisDSP::adaptNativeDescriptorForTesting (
        invalid, 48000.0);
    require (neutral.valid && neutral.usedSafeFallback && neutral.neutralBypass,
             "invalid standalone descriptor selects an explicit neutral policy");
    requireNear (MorphosisDSP::responseMagnitude (neutral, 20.0, 48000.0),
                 1.0, 1.0e-12, "neutral fallback has unity response at 20 Hz");
    requireNear (MorphosisDSP::responseMagnitude (neutral, 12000.0, 48000.0),
                 1.0, 1.0e-12, "neutral fallback has unity response at 12 kHz");

    std::atomic<bool> safetyFault { false };
    MorphosisDSP processor;
    processor.setTelemetry (nullptr, nullptr, &safetyFault);
    processor.prepare (48000.0, 64);
    processor.beginBlock (presetParameters (0));
    processor.advanceSample();
    require (std::isfinite (processor.processSampleNoAdvance (0.1f, 0)),
             "normal runtime remains finite beside the explicit fault policy");
    require (! safetyFault.load (std::memory_order_relaxed),
             "normal runtime does not raise the fallback diagnostic");
    const auto lastValid = processor.getCurrentCoefficients();
    require (! processor.acceptRuntimeCoefficientsForTesting (neutral),
             "runtime rejects an invalid coefficient candidate");
    require (processor.getCurrentCoefficients().neutralBypass == false
                 && coefficientDistance (processor.getCurrentCoefficients(), lastValid) < 1.0e-12,
             "runtime invalid update preserves the last accepted filter");
    require (safetyFault.load (std::memory_order_relaxed)
                 && processor.getSafetyResetCountForTesting() == 0,
             "runtime invalid update raises a diagnostic without an emergency reset");
#else
    require (false, "MORPHOSIS_TESTING must expose the standalone fault hook");
#endif
}

void checkDenseBlendMatrix()
{
    const std::array<std::array<int, morphosis::kMaxBlendSources>, 4> sourceSets {
        std::array<int, 4> { 0, 0, 0, 0 },
        std::array<int, 4> { 0, 43, 17, 184 },
        std::array<int, 4> { 0, 17, 184, 288 },
        std::array<int, 4> { 185, 162, 197, 288 }
    };
    const std::array<double, 7> positions { 0.0, 1.0e-9, 0.001, 0.25,
                                             0.5, 0.999, 1.0 };
    const std::array<double, 3> rates { 44100.0, 48000.0, 96000.0 };
    std::size_t fallbackCount = 0;
    double largestResponseDb = -std::numeric_limits<double>::infinity();

    for (const auto rate : rates)
        for (const auto& sources : sourceSets)
            for (const auto x : positions)
                for (const auto y : positions)
                {
                    const std::array<bool, 4> linear { false, false, false, false };
                    const auto blend = MorphosisDSP::makeBilinearBlend (sources, linear, x, y);
                    const auto coefficients = MorphosisDSP::makeBlendedCoefficients (
                        blend, -1.87, 2.37, 0.25, rate);
                    if (coefficients.usedSafeFallback)
                        ++fallbackCount;
                    require (coefficients.valid && ! coefficients.usedSafeFallback,
                             "dense XY coefficient matrix remains fallback-free");

                    for (const auto frequency : { 20.0, 1000.0, 12000.0,
                                                  rate * 0.5 - 1.0 })
                    {
                        const auto response = MorphosisDSP::responseMagnitude (
                            coefficients, frequency, rate);
                        require (std::isfinite (response) && response >= 0.0,
                                 "dense XY response is numerically valid");
                        if (response > 0.0)
                            largestResponseDb = std::max (largestResponseDb,
                                                          20.0 * std::log10 (response));
                    }

                    if (sources == sourceSets[0])
                    {
                        const auto reference = MorphosisDSP::makeCoefficients (
                            0, -1.87, 2.37, 0.25, rate);
                        require (coefficientDistance (coefficients, reference) < 1.0e-12,
                                 "all-Null XY grid is coefficient-identical to Null");
                    }
                }

    std::cout << std::setprecision (10)
              << "dense_xy_fallback_count=" << fallbackCount
              << " dense_xy_largest_sampled_response_db=" << largestResponseDb << '\n';
    require (fallbackCount == 0,
             "valid dense XY matrix does not depend on an explicit fallback");
}

void checkRuntimeMatrix()
{
    const std::array<double, 3> rates { 44100.0, 48000.0, 96000.0 };
    const std::array<int, 3> blockSizes { 1, 64, 512 };
    const std::array<bool, morphosis::kMaxBlendSources> linear { false, false, false, false };
    const std::array<bool, morphosis::kMaxBlendSources> mixedNonlinear { false, true, false, true };

    for (const auto rate : rates)
        for (const auto blockSize : blockSizes)
            for (int nonlinearCase = 0; nonlinearCase < 2; ++nonlinearCase)
            {
                const auto& nonlinear = nonlinearCase == 0 ? linear : mixedNonlinear;
                std::atomic<bool> safetyFault { false };
                MorphosisDSP processor;
                processor.setTelemetry (nullptr, nullptr, &safetyFault);
                processor.prepare (rate, blockSize);
                auto parameters = xyParameters ({ 0, 17, 184, 288 }, nonlinear, 0.5, 0.5);

                double peak = 0.0;
                double sumSquares = 0.0;
                std::int64_t sample = 0;
                for (int block = 0; block < 120; ++block)
                {
                    const auto phase = 2.0 * kPi * static_cast<double> (sample) / rate;
                    parameters.xyX = 0.5 + 0.499 * std::sin (phase * 0.73);
                    parameters.xyY = 0.5 + 0.499 * std::cos (phase * 0.41);
                    processBlock (processor, parameters, blockSize, sample, peak, sumSquares);
                    sample += blockSize;
                }

                double tailPeak = 0.0;
                for (int block = 0; block < 32; ++block)
                {
                    processor.beginBlock (parameters);
                    for (int index = 0; index < blockSize; ++index)
                    {
                        processor.advanceSample();
                        const auto left = processor.processSampleNoAdvance (0.0f, 0);
                        const auto right = processor.processSampleNoAdvance (0.0f, 1);
                        tailPeak = std::max (tailPeak,
                                            std::max (std::abs (static_cast<double> (left)),
                                                      std::abs (static_cast<double> (right))));
                    }
                }

                require (processor.getSafetyResetCountForTesting() == 0,
                         "moving XY runtime matrix does not invoke emergency resets");
                if (processor.getCoefficientFallbackCountForTesting() > 0)
                    require (! processor.getCurrentCoefficients().neutralBypass,
                             "runtime fallback preserves the last accepted filter");
                require (std::isfinite (peak) && std::isfinite (sumSquares)
                             && std::isfinite (tailPeak),
                         "moving XY sustained and silent-tail metrics are finite");
                std::cout << "runtime_xy rate=" << rate << " block=" << blockSize
                          << " nonlinear=" << nonlinearCase
                          << " peak=" << peak << " rms="
                          << std::sqrt (sumSquares / std::max (1.0, 2.0 * sample))
                          << " tail_peak=" << tailPeak
                          << " fallback_count=" << processor.getCoefficientFallbackCountForTesting()
                          << " safety_resets=" << processor.getSafetyResetCountForTesting()
                          << " safety=" << (safetyFault.load () ? 1 : 0) << '\n';
            }
}

void checkStereoIsolation()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto blockSize : { 1, 64, 512 })
        {
            std::atomic<bool> safetyFault { false };
            MorphosisDSP processor;
            processor.setTelemetry (nullptr, nullptr, &safetyFault);
            processor.prepare (rate, blockSize);
            const auto parameters = presetParameters (185, -1.87, 2.37, 0.25);
            processor.beginBlock (parameters);
            for (int sample = 0; sample < blockSize * 8; ++sample)
            {
                if (sample % blockSize == 0)
                    processor.beginBlock (parameters);
                processor.advanceSample();
                const auto left = processor.processSampleNoAdvance (0.17f, 0);
                const auto right = processor.processSampleNoAdvance (0.0f, 1);
                require (std::isfinite (left) && right == 0.0f,
                         "stereo filter state remains isolated");
            }
            require (! safetyFault.load (std::memory_order_relaxed),
                     "stereo isolation remains safety-clean");
        }
}

void checkTransitionStateMachine()
{
#if defined(MORPHOSIS_TESTING)
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto blockSize : { 1, 64, 512 })
        {
            std::atomic<bool> safetyFault { false };
            MorphosisDSP processor;
            processor.setTelemetry (nullptr, nullptr, &safetyFault);
            processor.prepare (rate, blockSize);
            auto initial = presetParameters (0);
            double peak = 0.0;
            double squares = 0.0;
            for (int block = 0; block < 3; ++block)
                processBlock (processor, initial, blockSize, block * blockSize, peak, squares);

            auto xyA = xyParameters ({ 0, 43, 17, 184 },
                                     { false, false, false, false }, 0.5, 0.5);
            auto xyB = xyA;
            xyB.xyPresets[0] = 184;
            xyB.xyPresets[1] = 288;
            xyB.xyPresets[2] = 43;
            xyB.xyPresets[3] = 17;

            const auto expectedFirstReset = processor.getTransitionResetCountForTesting() + 1;
            std::vector<double> firstMix;
            auto sample = std::int64_t { 0 };
            const auto fadeSamples = static_cast<int> (std::lround (rate * 0.020));
            const auto totalSamples = 2 * fadeSamples + 2 * blockSize;
            for (int block = 0; sample < totalSamples; ++block)
            {
                const auto& target = block == 0 ? xyA : xyB;
                processor.beginBlock (target);
                const auto count = std::min (blockSize,
                                             static_cast<int> (totalSamples - sample));
                for (int index = 0; index < count; ++index)
                {
                    processor.advanceSample();
                    static_cast<void> (processor.processSampleNoAdvance (0.13f, 0));
                    static_cast<void> (processor.processSampleNoAdvance (0.13f, 1));
                    firstMix.push_back (processor.getFilterMix());
                }
                sample += count;
            }

            std::size_t zeroIndex = firstMix.size();
            for (std::size_t index = 0; index < firstMix.size(); ++index)
            {
                if (firstMix[index] <= 1.0e-7)
                {
                    zeroIndex = index;
                    break;
                }
                if (index > 0)
                    require (firstMix[index] <= firstMix[index - 1] + 1.0e-8,
                             "pending slot edit does not restart or cancel fade-out");
            }
            require (zeroIndex < firstMix.size(), "first topology transition reaches dry zero");
            require (processor.getTransitionResetCountForTesting() == expectedFirstReset,
                     "retargeted transition performs exactly one state reset");

            const auto countBeforeMotion = processor.getTransitionResetCountForTesting();
            auto movingXY = xyB;
            for (int block = 0; block < 12; ++block)
            {
                movingXY.xyX = block % 2 == 0 ? 0.1 : 0.9;
                movingXY.xyY = block % 3 == 0 ? 0.1 : 0.9;
                processBlock (processor, movingXY, blockSize, sample, peak, squares);
                sample += blockSize;
            }
            require (processor.getTransitionResetCountForTesting() == countBeforeMotion,
                     "continuous XY motion does not reset filter state");

            auto sequence = hostSequenceParameters (184);
            const auto expectedSecondReset = countBeforeMotion + 1;
            for (int block = 0; block < (2 * fadeSamples + 2 * blockSize) / blockSize + 2; ++block)
                processBlock (processor, sequence, blockSize, sample + block * blockSize,
                              peak, squares);
            require (processor.getTransitionResetCountForTesting() == expectedSecondReset,
                     "XY-to-host-sequence transition completes exactly once");

            auto backToPreset = presetParameters (43);
            const auto expectedThirdReset = expectedSecondReset + 1;
            for (int block = 0; block < (2 * fadeSamples + 2 * blockSize) / blockSize + 2; ++block)
                processBlock (processor, backToPreset, blockSize,
                              sample + (block + 16) * blockSize, peak, squares);
            require (processor.getTransitionResetCountForTesting() == expectedThirdReset,
                     "host-sequence-to-preset transition completes exactly once");
            require (processor.getSafetyResetCountForTesting() == 0,
                     "transition matrix remains emergency-reset-free");
        }
#else
    require (false, "MORPHOSIS_TESTING must expose transition instrumentation");
#endif
}

} // namespace

int main()
{
    checkAccumulatorAndIdentityPaths();
    checkFallbackPolicy();
    checkDenseBlendMatrix();
    checkRuntimeMatrix();
    checkStereoIsolation();
    checkTransitionStateMachine();

    if (failures != 0)
    {
        std::cerr << failures << " focused SEQ/XY technical regression(s) failed\n";
        return 1;
    }

    std::cout << "All focused SEQ/XY technical regressions passed\n";
    return 0;
}
