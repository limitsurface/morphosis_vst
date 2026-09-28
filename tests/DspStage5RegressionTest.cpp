#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#define private public
#include "MorphosisDSP.h"
#undef private

namespace
{

using morphosis::MorphBlend;
using morphosis::MorphosisDSP;
using morphosis::ParameterSnapshot;
using morphosis::ProcessingMode;
using morphosis::XYInterpolationMode;

void require (bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit (EXIT_FAILURE);
}

ParameterSnapshot parametersFor (ProcessingMode mode,
                                 XYInterpolationMode interpolation,
                                 double dryWet,
                                 bool encoded = false)
{
    ParameterSnapshot parameters;
    parameters.preset = 0;
    parameters.mode = mode;
    parameters.xyInterpolation = interpolation;
    parameters.xyEncodedDomain = encoded;
    parameters.xyPresets = { 0, 0, 0, 0 };
    parameters.sequencePresets.fill (0);
    parameters.sequenceLength = morphosis::kSequenceSlotCount;
    parameters.dryWet = dryWet;
    parameters.softClip = false;
    parameters.blend = {};
    parameters.blend.presets = { 0, 0, 0, 0 };
    return parameters;
}

std::vector<std::array<float, 2>> renderImpulse (MorphosisDSP& dsp,
                                                 double sampleRate,
                                                 int blockSize,
                                                 int channels,
                                                 ProcessingMode mode,
                                                 XYInterpolationMode interpolation,
                                                 double dryWet,
                                                 bool encoded = false,
                                                 bool bypassed = false)
{
    constexpr int renderSamples = 144;
    const auto parameters = parametersFor (mode, interpolation, dryWet, encoded);
    std::vector<std::array<float, 2>> output (renderSamples);
    for (int blockStart = 0; blockStart < renderSamples; blockStart += blockSize)
    {
        dsp.beginBlock (parameters, true);
        const auto blockEnd = std::min (renderSamples, blockStart + blockSize);
        for (int sample = blockStart; sample < blockEnd; ++sample)
        {
            dsp.advanceSample();
            for (int channelIndex = 0; channelIndex < channels; ++channelIndex)
            {
                const auto channel = (sample / blockSize) % 2 == 0
                                   ? channelIndex : channels - 1 - channelIndex;
                const auto impulse = sample == 0 ? (channel == 0 ? 0.2f : -0.13f) : 0.0f;
                output[static_cast<std::size_t> (sample)][static_cast<std::size_t> (channel)] =
                    dsp.processSampleNoAdvance (impulse, channel, bypassed);
            }
        }
    }
    (void) sampleRate;
    return output;
}

void checkImpulseLatencyMatrix()
{
    constexpr std::array<double, 3> sampleRates { 44100.0, 48000.0, 96000.0 };
    constexpr std::array<int, 3> blockSizes { 1, 64, 512 };

    for (const auto sampleRate : sampleRates)
    {
        for (const auto blockSize : blockSizes)
        {
            for (const auto channels : { 1, 2 })
            {
                MorphosisDSP dsp;
                dsp.prepare (sampleRate, blockSize);
                struct Case
                {
                    ProcessingMode mode;
                    XYInterpolationMode interpolation;
                    double dryWet;
                    bool encoded;
                    bool bypassed;
                    const char* label;
                };
                constexpr std::array<Case, 9> cases {{
                    { ProcessingMode::preset, XYInterpolationMode::descriptor, 0.0, false, false,
                      "preset dry impulse" },
                    { ProcessingMode::preset, XYInterpolationMode::descriptor, 1.0, false, false,
                      "preset wet impulse" },
                    { ProcessingMode::sequencer, XYInterpolationMode::descriptor, 1.0, false, false,
                      "sequencer impulse" },
                    { ProcessingMode::xy, XYInterpolationMode::descriptor, 1.0, false, false,
                      "descriptor XY impulse" },
                    { ProcessingMode::xy, XYInterpolationMode::descriptor, 1.0, true, false,
                      "encoded XY impulse" },
                    { ProcessingMode::xy, XYInterpolationMode::response, 0.0, false, false,
                      "Response dry impulse" },
                    { ProcessingMode::xy, XYInterpolationMode::response, 1.0, false, false,
                      "Response wet impulse" },
                    { ProcessingMode::preset, XYInterpolationMode::descriptor, 1.0, false, true,
                      "bypassed impulse" },
                    { ProcessingMode::xy, XYInterpolationMode::descriptor, 0.0, true, false,
                      "encoded XY dry impulse" }
                }};

                for (const auto& test : cases)
                {
                    dsp.reset();
                    const auto output = renderImpulse (dsp, sampleRate, blockSize, channels,
                                                       test.mode, test.interpolation, test.dryWet,
                                                       test.encoded, test.bypassed);
                    for (int channel = 0; channel < channels; ++channel)
                    {
                        int firstNonzero = -1;
                        for (int sample = 0; sample < static_cast<int> (output.size()); ++sample)
                        {
                            if (std::abs (output[static_cast<std::size_t> (sample)]
                                               [static_cast<std::size_t> (channel)]) > 1.0e-7f)
                            {
                                firstNonzero = sample;
                                break;
                            }
                        }
                        if (firstNonzero != morphosis::kMorphosisLatencySamples)
                        {
                            std::cerr << "latency case=" << test.label << " rate=" << sampleRate
                                      << " block=" << blockSize << " channels=" << channels
                                      << " channel=" << channel << " first=" << firstNonzero << '\n';
                            require (false, "impulse first arrives at the constant 64-sample latency");
                        }
                    }
                }
            }
        }
    }
}

float referenceInput (int sample, int channel)
{
    if (sample < 0)
        return 0.0f;
    const auto phase = static_cast<double> (sample) * (channel == 0 ? 0.071 : 0.053);
    return static_cast<float> (0.08 + 0.017 * std::sin (phase) + 0.009 * std::cos (phase * 0.37));
}

void checkContinuousModeAndBypassTransitions()
{
    constexpr std::array<double, 3> sampleRates { 44100.0, 48000.0, 96000.0 };
    constexpr std::array<int, 3> blockSizes { 1, 64, 512 };
    constexpr std::array<ProcessingMode, 5> modes {
        ProcessingMode::preset, ProcessingMode::sequencer, ProcessingMode::xy,
        ProcessingMode::xy, ProcessingMode::preset
    };

    for (const auto sampleRate : sampleRates)
    {
        for (const auto blockSize : blockSizes)
        {
            MorphosisDSP dsp;
            dsp.prepare (sampleRate, blockSize);
            const auto segmentLength = std::max (128, blockSize * 2);
            const auto totalSamples = segmentLength * static_cast<int> (modes.size());
            for (int blockStart = 0; blockStart < totalSamples; blockStart += blockSize)
            {
                const auto segment = std::min (static_cast<int> (modes.size()) - 1,
                                               blockStart / segmentLength);
                const auto interpolation = segment == 3 ? XYInterpolationMode::response
                                                        : XYInterpolationMode::descriptor;
                const auto parameters = parametersFor (modes[static_cast<std::size_t> (segment)],
                                                      interpolation, 1.0);
                dsp.beginBlock (parameters, true);
                const auto blockEnd = std::min (totalSamples, blockStart + blockSize);
                for (int sample = blockStart; sample < blockEnd; ++sample)
                {
                    dsp.advanceSample();
                    const auto bypassed = segment == 1 || segment == 3;
                    for (int channel = 1; channel >= 0; --channel)
                    {
                        const auto input = referenceInput (sample, channel);
                        const auto output = dsp.processSampleNoAdvance (input, channel, bypassed);
                        if (sample >= morphosis::kMorphosisLatencySamples)
                        {
                            const auto expected = referenceInput (
                                sample - morphosis::kMorphosisLatencySamples, channel);
                            if (bypassed)
                                require (std::abs (output - expected) < 1.0e-6f,
                                         "bypassed audio is unity dry at the shared delay");
                            else
                            {
                                require (std::abs (output) > 0.035f,
                                         "continuous nonzero input has no hole at mode edges");
                                require (std::abs (output - expected) < 0.025f,
                                         "Null-cube mode output follows the fixed-delay reference");
                            }
                        }
                    }
                }
            }

            require (dsp.firChannels[0].historyBlockCount
                         >= static_cast<std::uint64_t> (totalSamples / morphosis::kExperimentalFirPartitionSize),
                     "FIR input history remains current while Response is inactive");
        }
    }
}

std::uint64_t appendFloat (std::uint64_t hash, float value)
{
    std::uint32_t bits = 0;
    std::memcpy (&bits, &value, sizeof (bits));
    for (int byte = 0; byte < 4; ++byte)
    {
        hash ^= static_cast<std::uint8_t> (bits >> (byte * 8));
        hash *= 1099511628211ull;
    }
    return hash;
}

void checkSettledIirIsOnlyDelayed()
{
    constexpr int referenceSamples = 2048;
    constexpr int totalSamples = referenceSamples + morphosis::kMorphosisLatencySamples;
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 512);
    ParameterSnapshot parameters;
    parameters.preset = 185;
    parameters.softClip = false;
    parameters.dryWet = 1.0;
    dsp.beginBlock (parameters, false);

    auto hash = 14695981039346656037ull;
    for (int sample = 0; sample < totalSamples; ++sample)
    {
        dsp.advanceSample();
        const auto input = sample < referenceSamples
            ? static_cast<float> (0.12 * std::sin (sample * 0.037)
                                  + (sample == 0 ? 0.18 : 0.0)
                                  - (sample == 517 ? 0.11 : 0.0))
            : 0.0f;
        const auto output = dsp.processSampleNoAdvance (input, 0);
        if (sample >= morphosis::kMorphosisLatencySamples)
            hash = appendFloat (hash, output);
    }

    constexpr std::uint64_t expectedPreEditHash = 0xc02f71220fbabc70ull;
    require (hash == expectedPreEditHash,
             "settled non-Response preset output matches pre-edit samples after 64-sample shift");
}

} // namespace

int main()
{
    checkImpulseLatencyMatrix();
    checkContinuousModeAndBypassTransitions();
    checkSettledIirIsOnlyDelayed();
    std::cout << "Stage 5 DSP latency and transition checks passed.\n";
    return EXIT_SUCCESS;
}
