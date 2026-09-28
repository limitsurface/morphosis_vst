#include "PluginProcessor.h"
#include "MorphosisSequencer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace
{

void require (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit (EXIT_FAILURE);
    }
}

void setParameter (MorphosisAudioProcessor& processor, const char* id, float value)
{
    auto& state = processor.getParameters();
    auto* parameter = state.getParameter (id);
    require (parameter != nullptr, "graph test parameter exists");
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

double coefficientDistance (const morphosis::CoefficientSet& left,
                            const morphosis::CoefficientSet& right)
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

void processBlock (MorphosisAudioProcessor& processor, int blockSize,
                   std::int64_t firstSample = 0)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    for (int sample = 0; sample < blockSize; ++sample)
    {
        const auto phase = static_cast<double> (firstSample + sample);
        const auto value = static_cast<float> (0.19 * std::sin (phase * 0.017)
                                                + 0.03 * std::cos (phase * 0.071));
        buffer.setSample (0, sample, value);
        buffer.setSample (1, sample, value * 0.73f);
    }
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
}

MorphosisAudioProcessor::GraphSnapshot popSnapshot (MorphosisAudioProcessor& processor)
{
    MorphosisAudioProcessor::GraphSnapshot snapshot;
    require (processor.popLatestGraphSnapshot (snapshot),
             "audio block publishes a complete graph snapshot");
    return snapshot;
}

void configureLinearParameters (MorphosisAudioProcessor& processor)
{
    setParameter (processor, morphosis::parameter_ids::frequency, 0.0f);
    setParameter (processor, morphosis::parameter_ids::morph, 0.0f);
    setParameter (processor, morphosis::parameter_ids::transform, 0.0f);
    setParameter (processor, morphosis::parameter_ids::inputGainDb, 0.0f);
    setParameter (processor, morphosis::parameter_ids::preClipGainDb, 0.0f);
    setParameter (processor, morphosis::parameter_ids::postClipGainDb, 0.0f);
    setParameter (processor, morphosis::parameter_ids::softClip, 0.0f);
    setParameter (processor, morphosis::parameter_ids::internalDistortion, 0.0f);
}

void checkManualSnapshot()
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 64;
    MorphosisAudioProcessor processor;
    configureLinearParameters (processor);
    setParameter (processor, morphosis::parameter_ids::mode, 2.0f);
    setParameter (processor, morphosis::parameter_ids::sequenceSource, 1.0f);

    // These are the UI numbers from the reported reproduction. The plugin
    // stores zero-based cube indices; the twelve remaining slots are UI-001
    // Null entries, so the graph must become flat at the fourth-to-fifth
    // boundary (4/15).
    const std::array<int, 4> uiIds { 144, 198, 157, 223 };
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
    {
        const auto uiId = index < static_cast<int> (uiIds.size())
                            ? uiIds[static_cast<std::size_t> (index)] : 1;
        setParameter (processor, morphosis::parameter_ids::sequenceSlots[
                          static_cast<std::size_t> (index)],
                      static_cast<float> (uiId - 1));
    }

    processor.prepareToPlay (rate, blockSize);
    MorphosisAudioProcessor::GraphSnapshot stale;
    require (! processor.popLatestGraphSnapshot (stale),
             "reprepare starts with no stale graph snapshot");

    const auto positions = std::array<double, 6> { 0.0, 1.0 / 15.0,
                                                    4.0 / 15.0, 0.5,
                                                    14.0 / 15.0, 1.0 };
    for (int positionIndex = 0; positionIndex < static_cast<int> (positions.size()); ++positionIndex)
    {
        const auto position = positions[static_cast<std::size_t> (positionIndex)];
        setParameter (processor, morphosis::parameter_ids::sequencePosition,
                      static_cast<float> (position));
        // Manual position has the production 2 ms control smoother. Let it
        // settle before comparing the graph snapshot to the static oracle.
        const auto settleBlocks = positionIndex == 0 ? 1 : 64;
        MorphosisAudioProcessor::GraphSnapshot actual;
        for (int block = 0; block < settleBlocks; ++block)
        {
            processBlock (processor, blockSize);
            actual = popSnapshot (processor);
        }
        const auto parameters = processor.getParameterSnapshot();
        const auto slot = juce::jlimit (
            0, morphosis::kSequenceSlotCount - 1,
            juce::roundToInt (static_cast<float> (std::clamp (parameters.sequencePosition, 0.0, 1.0)
                * static_cast<double> (morphosis::kSequenceSlotCount - 1))));
        morphosis::MorphBlend discrete;
        discrete.count = 1;
        discrete.presets[0] = parameters.sequencePresets[static_cast<std::size_t> (slot)];
        discrete.nonlinearSources[0] = parameters.sequenceNonlinearSources[
            static_cast<std::size_t> (slot)];
        discrete.weights[0] = 1.0;
        const auto expected = morphosis::MorphosisDSP::makeBlendedCoefficients (
            discrete, parameters.frequency, parameters.morph, parameters.transform, rate);

        require (actual.sampleRate == rate, "manual graph snapshot carries host rate");
        require (actual.coefficients.valid, "manual graph snapshot is valid");
        require (coefficientDistance (actual.coefficients, expected) < 1.0e-8,
                 "manual graph snapshot follows production all-16 adjacent blend");

        if (position >= 4.0 / 15.0)
        {
            for (const auto frequency : { 20.0, 440.0, 4000.0, 12000.0, 24000.0 })
            {
                const auto response = morphosis::MorphosisDSP::responseMagnitude (
                    actual.coefficients, frequency, rate);
                const auto expectedResponse = morphosis::MorphosisDSP::responseMagnitude (
                    expected, frequency, rate);
                require (std::abs (response - expectedResponse) < 1.0e-8,
                         "manual graph reaches the Null tail at and after 4/15");
            }
        }
    }

    // Leave one entry queued so reprepare must invalidate real stale data,
    // rather than merely observing an already-empty FIFO.
    processBlock (processor, blockSize);
    processor.prepareToPlay (rate, blockSize);
    require (! processor.popLatestGraphSnapshot (stale),
             "reprepare drops a queued graph snapshot from the old generation");
}

void checkHostSequenceSnapshot()
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 512;
    MorphosisAudioProcessor processor;
    configureLinearParameters (processor);
    setParameter (processor, morphosis::parameter_ids::mode, 2.0f);
    setParameter (processor, morphosis::parameter_ids::sequenceSource, 0.0f);
    setParameter (processor, morphosis::parameter_ids::sequenceLength, 4.0f);
    setParameter (processor, morphosis::parameter_ids::sequenceDivision, 7.0f);
    setParameter (processor, morphosis::parameter_ids::sequenceSync, 0.0f);
    const std::array<int, 4> slots { 0, 43, 17, 184 };
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        setParameter (processor, morphosis::parameter_ids::sequenceSlots[
                          static_cast<std::size_t> (index)],
                      static_cast<float> (slots[static_cast<std::size_t> (index % 4)]));

    processor.prepareToPlay (rate, blockSize);
    MorphosisAudioProcessor::GraphSnapshot first;
    MorphosisAudioProcessor::GraphSnapshot last;
    constexpr int blockCount = 16;
    for (int block = 0; block < blockCount; ++block)
    {
        processBlock (processor, blockSize, static_cast<std::int64_t> (block * blockSize));
        const auto current = popSnapshot (processor);
        if (block == 0)
            first = current;
        last = current;
    }

    require (first.sampleRate == rate && last.sampleRate == rate,
             "host graph snapshots retain the prepared sample rate");
    require (coefficientDistance (first.coefficients, last.coefficients) > 1.0e-5,
             "host graph snapshot changes as the fallback host clock advances");

    const auto periodBeats = morphosis::sequencePeriodQuarterBeats (7, 0);
    const auto finalSample = static_cast<double> (blockCount * blockSize - 1);
    const auto ppqPerSample = 120.0 / (60.0 * rate);
    const auto cycle = finalSample * ppqPerSample / periodBeats;
    const auto cycleFloor = std::floor (cycle);
    const auto expectedStep = static_cast<int> ((static_cast<std::int64_t> (cycleFloor) % 4 + 4) % 4);
    const auto expected = morphosis::MorphosisDSP::makeCoefficients (
        slots[static_cast<std::size_t> (expectedStep)], 0.0, 0.0, 0.0, rate);
    require (coefficientDistance (last.coefficients, expected) < 1.0e-8,
             "host graph snapshot follows the live production step and phase");
}

void checkXYAndPresetSnapshots()
{
    constexpr double rate = 44100.0;
    constexpr int blockSize = 1;

    MorphosisAudioProcessor xy;
    configureLinearParameters (xy);
    setParameter (xy, morphosis::parameter_ids::mode, 1.0f);
    setParameter (xy, morphosis::parameter_ids::xyX, 0.37f);
    setParameter (xy, morphosis::parameter_ids::xyY, 0.62f);
    const std::array<int, 4> xySlots { 0, 43, 17, 184 };
    const std::array<const char*, 4> xyIds {
        morphosis::parameter_ids::xyA, morphosis::parameter_ids::xyB,
        morphosis::parameter_ids::xyC, morphosis::parameter_ids::xyD };
    for (int index = 0; index < 4; ++index)
        setParameter (xy, xyIds[static_cast<std::size_t> (index)],
                      static_cast<float> (xySlots[static_cast<std::size_t> (index)]));
    xy.prepareToPlay (rate, blockSize);
    processBlock (xy, blockSize);
    const auto xySnapshot = popSnapshot (xy);
    const auto xyParameters = xy.getParameterSnapshot();
    const auto xyExpected = morphosis::MorphosisDSP::makeBlendedCoefficients (
        morphosis::MorphosisDSP::makeBilinearBlend (
            xyParameters.xyPresets, xyParameters.xyNonlinearSources,
            xyParameters.xyX, xyParameters.xyY),
        xyParameters.frequency, xyParameters.morph, xyParameters.transform, rate);
    require (coefficientDistance (xySnapshot.coefficients, xyExpected) < 1.0e-9,
             "XY graph snapshot follows production bilinear coefficients");

    MorphosisAudioProcessor preset;
    configureLinearParameters (preset);
    setParameter (preset, morphosis::parameter_ids::preset, 43.0f);
    preset.prepareToPlay (rate, blockSize);
    processBlock (preset, blockSize);
    const auto presetSnapshot = popSnapshot (preset);
    const auto presetExpected = morphosis::MorphosisDSP::makeCoefficients (
        43, 0.0, 0.0, 0.0, rate);
    require (coefficientDistance (presetSnapshot.coefficients, presetExpected) < 1.0e-9,
             "ordinary preset graph snapshot follows production coefficients");

    processBlock (preset, blockSize);
    juce::MemoryBlock state;
    preset.getStateInformation (state);
    preset.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    MorphosisAudioProcessor::GraphSnapshot stale;
    require (! preset.popLatestGraphSnapshot (stale),
             "state restore invalidates queued graph snapshots until new audio");
}

} // namespace

int main()
{
    checkManualSnapshot();
    checkHostSequenceSnapshot();
    checkXYAndPresetSnapshots();
    std::cout << "Graph snapshot regression passed: manual all-16 tail, live host steps/glide, "
                 "XY, ordinary preset, sample-rate propagation, and reset invalidation.\n";
    return EXIT_SUCCESS;
}
