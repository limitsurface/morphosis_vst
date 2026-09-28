#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <juce_audio_processors/juce_audio_processors.h>

#define private public
#include "PluginProcessor.h"
#undef private

namespace
{

void require (bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit (EXIT_FAILURE);
}

morphosis::ParameterSnapshot responseParameters()
{
    morphosis::ParameterSnapshot parameters;
    parameters.mode = morphosis::ProcessingMode::xy;
    parameters.xyInterpolation = morphosis::XYInterpolationMode::response;
    parameters.xyPresets = { 0, 0, 0, 0 };
    parameters.xyX = 0.5;
    parameters.xyY = 0.5;
    parameters.softClip = false;
    return parameters;
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

std::uint64_t renderStaticPresetReference (MorphosisAudioProcessor& processor)
{
    processor.prepareToPlay (48000.0, 512);
    morphosis::ParameterSnapshot parameters;
    parameters.preset = 185;
    parameters.softClip = false;
    parameters.dryWet = 1.0;
    processor.dsp.beginBlock (parameters, false);

    auto hash = 14695981039346656037ull;
    for (int sample = 0; sample < 2048; ++sample)
    {
        processor.dsp.advanceSample();
        const auto input = static_cast<float> (0.12 * std::sin (sample * 0.037)
                                               + (sample == 0 ? 0.18 : 0.0)
                                               - (sample == 517 ? 0.11 : 0.0));
        hash = appendFloat (hash, processor.dsp.processSampleNoAdvance (input, 0));
    }
    return hash;
}

} // namespace

int main()
{
    MorphosisAudioProcessor processor;
    require (processor.getLatencySamples() == 0,
             "pre-edit plugin reports zero latency immediately after construction");

    processor.prepareToPlay (48000.0, 512);
    require (processor.getLatencySamples() == 0,
             "pre-edit plugin still reports zero latency after prepareToPlay");

    processor.dsp.beginBlock (responseParameters(), true);
    int firstNonzeroSample = -1;
    for (int sample = 0; sample < 128; ++sample)
    {
        processor.dsp.advanceSample();
        const auto input = sample == 0 ? 0.1f : 0.0f;
        const auto output = processor.dsp.processSampleNoAdvance (input, 0);
        if (firstNonzeroSample < 0 && std::abs (output) > 1.0e-8f)
            firstNonzeroSample = sample;
    }

    require (firstNonzeroSample == 64,
             "pre-edit Response impulse first arrives at sample 64");
    const auto staticPresetHash = renderStaticPresetReference (processor);
    std::cout << "Pre-edit discrepancy reproduced: host reports "
              << processor.getLatencySamples() << " samples; Response impulse arrives at sample "
              << firstNonzeroSample << ".\n"
              << "Pre-edit static preset 185 output hash at 48 kHz: 0x"
              << std::hex << staticPresetHash << std::dec << ".\n";
    return EXIT_SUCCESS;
}
