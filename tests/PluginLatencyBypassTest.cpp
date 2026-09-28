#include "PluginProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{

void require (bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit (EXIT_FAILURE);
}

void setParameter (MorphosisAudioProcessor& processor, const char* parameterId, float value)
{
    auto* parameter = processor.getParameters().getParameter (parameterId);
    require (parameter != nullptr, "parameter exists for latency test");
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

std::array<std::vector<float>, 2> renderImpulse (MorphosisAudioProcessor& processor,
                                                 int totalSamples,
                                                 int hostBlockSize,
                                                 bool bypassed)
{
    std::array<std::vector<float>, 2> output {
        std::vector<float> (static_cast<std::size_t> (totalSamples)),
        std::vector<float> (static_cast<std::size_t> (totalSamples))
    };
    juce::MidiBuffer midi;
    for (int blockStart = 0; blockStart < totalSamples; blockStart += hostBlockSize)
    {
        const auto frames = std::min (hostBlockSize, totalSamples - blockStart);
        juce::AudioBuffer<float> buffer (2, frames);
        for (int sample = 0; sample < frames; ++sample)
        {
            const auto absolute = blockStart + sample;
            buffer.setSample (0, sample, absolute == 0 ? 0.2f : 0.0f);
            buffer.setSample (1, sample, absolute == 0 ? -0.13f : 0.0f);
        }
        if (bypassed)
            processor.processBlockBypassed (buffer, midi);
        else
            processor.processBlock (buffer, midi);
        for (int sample = 0; sample < frames; ++sample)
        {
            const auto absolute = blockStart + sample;
            output[0][static_cast<std::size_t> (absolute)] = buffer.getSample (0, sample);
            output[1][static_cast<std::size_t> (absolute)] = buffer.getSample (1, sample);
        }
    }
    return output;
}

void verifyImpulse (MorphosisAudioProcessor& processor,
                    double sampleRate,
                    int blockSize,
                    bool bypassed,
                    bool response,
                    double dryWet)
{
    processor.prepareToPlay (sampleRate, blockSize);
    require (processor.getLatencySamples() == morphosis::kMorphosisLatencySamples,
             "host latency remains 64 samples after prepareToPlay");

    setParameter (processor, morphosis::parameter_ids::mode,
                  response ? static_cast<float> (morphosis::ProcessingMode::xy) : 0.0f);
    setParameter (processor, morphosis::parameter_ids::xyInterpolation,
                  response ? 1.0f : 0.0f);
    setParameter (processor, morphosis::parameter_ids::dryWet, static_cast<float> (dryWet));
    setParameter (processor, morphosis::parameter_ids::inputGainDb, bypassed ? 12.0f : 0.0f);
    setParameter (processor, morphosis::parameter_ids::preClipGainDb, bypassed ? 12.0f : 0.0f);
    setParameter (processor, morphosis::parameter_ids::postClipGainDb, bypassed ? 12.0f : 0.0f);
    setParameter (processor, morphosis::parameter_ids::softClip, bypassed ? 1.0f : 0.0f);

    constexpr int totalSamples = 160;
    const auto output = renderImpulse (processor, totalSamples, blockSize, bypassed);
    for (int channel = 0; channel < 2; ++channel)
    {
        int firstNonzero = -1;
        for (int sample = 0; sample < totalSamples; ++sample)
        {
            const auto value = output[static_cast<std::size_t> (channel)]
                                      [static_cast<std::size_t> (sample)];
            if (std::abs (value) > 1.0e-7f)
            {
                firstNonzero = sample;
                break;
            }
        }
        require (firstNonzero == morphosis::kMorphosisLatencySamples,
                 "processor callback impulse first arrives at sample 64");
    }

    if (bypassed)
    {
        require (std::abs (output[0][morphosis::kMorphosisLatencySamples] - 0.2f) < 1.0e-7f,
                 "host bypass returns unity dry, ignoring every gain and clip stage");
        require (std::abs (output[1][morphosis::kMorphosisLatencySamples] + 0.13f) < 1.0e-7f,
                 "stereo bypass delay remains independent per channel");
    }
}

void checkHostLatencyAndCallbacks()
{
    constexpr std::array<double, 3> sampleRates { 44100.0, 48000.0, 96000.0 };
    constexpr std::array<int, 3> blockSizes { 1, 64, 512 };
    MorphosisAudioProcessor processor;
    require (processor.getLatencySamples() == morphosis::kMorphosisLatencySamples,
             "host latency is declared immediately after construction");

    for (const auto sampleRate : sampleRates)
    {
        for (const auto blockSize : blockSizes)
        {
            verifyImpulse (processor, sampleRate, blockSize, false, false, 0.0);
            verifyImpulse (processor, sampleRate, blockSize, false, false, 1.0);
            verifyImpulse (processor, sampleRate, blockSize, false, true, 0.0);
            verifyImpulse (processor, sampleRate, blockSize, false, true, 1.0);
            verifyImpulse (processor, sampleRate, blockSize, true, false, 0.5);
            require (processor.getLatencySamples() == morphosis::kMorphosisLatencySamples,
                     "host latency does not vary with block size or bypass callbacks");
        }
    }
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    checkHostLatencyAndCallbacks();
    std::cout << "Stage 5 processor latency and bypass checks passed.\n";
    return EXIT_SUCCESS;
}
