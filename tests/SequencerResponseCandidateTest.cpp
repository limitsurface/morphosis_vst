#include "PluginProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace
{

int failures = 0;

void require (bool condition, const char* message)
{
    if (! condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

morphosis::ParameterSnapshot manualParameters (double position,
                                               double transitionMs = 20.0)
{
    morphosis::ParameterSnapshot result;
    result.mode = morphosis::ProcessingMode::sequencer;
    result.sequenceManual = true;
    result.sequencePosition = position;
    result.manualTransitionMs = transitionMs;
    result.sequencePresets = { 0, 43, 17, 184, 7, 11, 19, 23,
                               29, 31, 37, 41, 47, 53, 59, 61 };
    result.sequenceNonlinearSources.fill (false);
    result.softClip = false;
    result.dryWet = 1.0;
    return result;
}

void runSamples (morphosis::MorphosisDSP& dsp, int count, double& peak)
{
    for (int index = 0; index < count; ++index)
    {
        dsp.advanceSample();
        const auto input = static_cast<float> (0.17 * std::sin (0.013 * index)
                                                + 0.03 * std::cos (0.071 * index));
        const auto left = dsp.processSampleNoAdvance (input, 0);
        const auto right = dsp.processSampleNoAdvance (input * 0.71f, 1);
        require (std::isfinite (left) && std::isfinite (right), "sequencer candidate remains finite");
        peak = std::max (peak, std::max (std::abs (static_cast<double> (left)),
                                         std::abs (static_cast<double> (right))));
    }
}

void checkManualDiscreteAndRetargeting()
{
    morphosis::MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    double peak = 0.0;

    auto parameters = manualParameters (0.0);
    dsp.beginBlock (parameters);
    runSamples (dsp, 256, peak);
    require (dsp.getCurrentBlend().presets[0] == 0, "manual position zero selects slot one");

    parameters.sequencePosition = 0.5;
    dsp.beginBlock (parameters);
    runSamples (dsp, 1200, peak);
    std::cout << "manual_mid slot=" << dsp.getCurrentBlend().presets[0]
              << " mix=" << dsp.getFilterMix()
              << " resets=" << dsp.getTransitionResetCountForTesting() << '\n';
    // lround(0.5 * 15) is the normal nearest-slot boundary: slot nine here.
    require (dsp.getCurrentBlend().presets[0] == 29,
             "manual position uses discrete nearest-slot selection");
    const auto oneReset = dsp.getTransitionResetCountForTesting();
    require (oneReset == 1, "manual slot change resets the filter exactly once");

    dsp.beginBlock (parameters);
    runSamples (dsp, 1200, peak);
    require (dsp.getTransitionResetCountForTesting() == oneReset,
             "repeating the same manual slot does not retrigger the bridge");

    parameters.sequencePosition = 1.0 / 15.0;
    dsp.beginBlock (parameters);
    runSamples (dsp, 100, peak);
    parameters.sequencePosition = 1.0;
    dsp.beginBlock (parameters);
    runSamples (dsp, 1800, peak);
    std::cout << "manual_retarget slot=" << dsp.getCurrentBlend().presets[0]
              << " mix=" << dsp.getFilterMix()
              << " resets=" << dsp.getTransitionResetCountForTesting() << '\n';
    require (dsp.getCurrentBlend().presets[0] == 61,
             "latest manual destination wins during a rapid retarget");
    require (dsp.getTransitionResetCountForTesting() <= oneReset + 2,
             "rapid manual retarget does not restart an unbounded fade queue");
    require (peak < 1000.0, "manual bridge remains bounded without a hidden limiter");

    const auto beforeReverse = dsp.getTransitionResetCountForTesting();
    dsp.setSequenceTarget (29, false, 960);
    runSamples (dsp, 160, peak);
    dsp.setSequenceTarget (61, false, 960);
    runSamples (dsp, 900, peak);
    require (dsp.getCurrentBlend().presets[0] == 61,
             "returning to the active manual slot restores the latest destination");
    require (dsp.getTransitionResetCountForTesting() == beforeReverse,
             "reverse retarget cancels a pending manual bridge without a duplicate reset");
}

void checkManualTransitionDurations()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto durationMs : { 5.0, 20.0, 250.0 })
        {
            morphosis::MorphosisDSP dsp;
            dsp.prepare (rate, 64);
            auto parameters = manualParameters (1.0, durationMs);
            parameters.sequenceManual = false;
            parameters.preset = 61;
            parameters.blend.count = 1;
            parameters.blend.presets[0] = 61;
            parameters.blend.weights[0] = 1.0;
            parameters.sequencePresets[0] = 61;
            dsp.beginBlock (parameters);
            double peak = 0.0;
            runSamples (dsp, 256, peak);

            const auto transitionSamples = std::max (2, static_cast<int> (
                std::lround (rate * durationMs / 1000.0)));
            // Isolate the bridge clock from the separate 2 ms manual-position
            // coordinate smoother: the production manual parameter maps to
            // this same total exit+enter duration once a slot is selected.
            dsp.setSequenceTarget (0, false, transitionSamples);
            const auto samples = transitionSamples + static_cast<int> (
                std::lround (rate * 0.004));
            double minimumMix = 1.0;
            for (int sample = 0; sample < samples; ++sample)
            {
                dsp.advanceSample();
                minimumMix = std::min (minimumMix,
                                       static_cast<double> (dsp.getSequenceTransitionMix()));
                static_cast<void> (dsp.processSampleNoAdvance (0.1f, 0));
                static_cast<void> (dsp.processSampleNoAdvance (0.08f, 1));
            }
            require (minimumMix <= 1.0e-6,
                     "manual transition duration reaches a fully dry bridge");
            require (dsp.getTransitionResetCountForTesting() == 1,
                     "manual duration performs exactly one swap");
            require (std::isfinite (peak), "manual duration matrix remains finite");
        }
}

void checkHostDurationAndBlockSizes()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto blockSize : { 1, 64, 512 })
        {
            morphosis::MorphosisDSP dsp;
            dsp.prepare (rate, blockSize);
            auto initial = manualParameters (0.0);
            initial.sequenceManual = false;
            dsp.beginBlock (initial);
            double peak = 0.0;
            runSamples (dsp, blockSize * 3, peak);

            const auto before = dsp.getTransitionResetCountForTesting();
            dsp.setSequenceTarget (184, false, std::max (2, static_cast<int> (rate * 0.002)));
            double minimumMix = 1.0;
            for (int index = 0; index < static_cast<int> (rate * 0.006); ++index)
            {
                dsp.advanceSample();
                minimumMix = std::min (minimumMix, static_cast<double> (dsp.getFilterMix()));
                static_cast<void> (dsp.processSampleNoAdvance (0.11f, 0));
                static_cast<void> (dsp.processSampleNoAdvance (0.07f, 1));
            }
            require (minimumMix <= 1.0e-6, "host transition reaches a fully dry bridge");
            require (dsp.getTransitionResetCountForTesting() == before + 1,
                     "host transition performs one state swap per destination");
            require (std::isfinite (peak), "host duration matrix remains finite");
        }
}

void setParameter (MorphosisAudioProcessor& processor, const char* id, float value)
{
    auto& state = processor.getParameters();
    if (auto* parameter = state.getParameter (id))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    else
        require (false, "candidate parameter exists");
}

void processBlock (MorphosisAudioProcessor& processor, int blockSize, int firstSample)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    for (int index = 0; index < blockSize; ++index)
    {
        const auto phase = static_cast<double> (firstSample + index);
        const auto value = static_cast<float> (0.13 * std::sin (phase * 0.017)
                                                + 0.04 * std::cos (phase * 0.061));
        buffer.setSample (0, index, value);
        buffer.setSample (1, index, value * 0.73f);
    }
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
}

void checkExperimentalResponseWorkerAndGraph()
{
    MorphosisAudioProcessor processor;
    setParameter (processor, morphosis::parameter_ids::mode, 1.0f);
    setParameter (processor, morphosis::parameter_ids::xyInterpolation, 1.0f);
    setParameter (processor, morphosis::parameter_ids::softClip, 0.0f);
    setParameter (processor, morphosis::parameter_ids::dryWet, 1.0f);
    setParameter (processor, morphosis::parameter_ids::xyA, 0.0f);
    setParameter (processor, morphosis::parameter_ids::xyB, 43.0f);
    setParameter (processor, morphosis::parameter_ids::xyC, 17.0f);
    setParameter (processor, morphosis::parameter_ids::xyD, 184.0f);
    processor.prepareToPlay (48000.0, 64);

    bool sawResponseGraph = false;
    bool graphWasFinite = true;
    for (int block = 0; block < 240; ++block)
    {
        processBlock (processor, 64, block * 64);
        MorphosisAudioProcessor::GraphSnapshot snapshot;
        while (processor.popLatestGraphSnapshot (snapshot))
        {
            if (! snapshot.responseFIR)
                continue;
            sawResponseGraph = true;
            for (const auto value : snapshot.responseDb)
                graphWasFinite = graphWasFinite && std::isfinite (value);
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    require (sawResponseGraph, "experimental mode publishes a realized FIR graph snapshot");
    require (graphWasFinite, "realized FIR graph response remains finite");

    // Switching back leaves the descriptor algorithm as the default and does
    // not alter the remembered response-mode parameter value.
    setParameter (processor, morphosis::parameter_ids::xyInterpolation, 0.0f);
    for (int block = 0; block < 6; ++block)
        processBlock (processor, 64, 16000 + block * 64);
    MorphosisAudioProcessor::GraphSnapshot descriptorSnapshot;
    bool sawDescriptor = false;
    while (processor.popLatestGraphSnapshot (descriptorSnapshot))
        sawDescriptor = sawDescriptor || ! descriptorSnapshot.responseFIR;
    require (sawDescriptor, "algorithm switch publishes the descriptor graph again");
}

float getParameterValue (MorphosisAudioProcessor& processor, const char* id)
{
    if (const auto* raw = processor.getParameters().getRawParameterValue (id))
        return raw->load (std::memory_order_relaxed);
    return std::numeric_limits<float>::quiet_NaN();
}

void checkStateRecallAndMigration()
{
    auto fresh = std::make_unique<MorphosisAudioProcessor>();
    require (getParameterValue (*fresh, morphosis::parameter_ids::xyInterpolation) > 0.5f,
             "fresh instances default to Response XY interpolation");
    require (getParameterValue (*fresh, morphosis::parameter_ids::xyEncodedDomain) < 0.5f,
             "fresh instances default with encoded XY override off");

    const auto roundTripXYMode = [] (float selector, float encoded)
    {
        auto source = std::make_unique<MorphosisAudioProcessor>();
        setParameter (*source, morphosis::parameter_ids::xyInterpolation, selector);
        setParameter (*source, morphosis::parameter_ids::xyEncodedDomain, encoded);
        juce::MemoryBlock state;
        source->getStateInformation (state);

        auto restored = std::make_unique<MorphosisAudioProcessor>();
        restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        return std::array<float, 2> {
            getParameterValue (*restored, morphosis::parameter_ids::xyInterpolation),
            getParameterValue (*restored, morphosis::parameter_ids::xyEncodedDomain) };
    };

    const auto descriptor = roundTripXYMode (0.0f, 0.0f);
    require (descriptor[0] < 0.5f && descriptor[1] < 0.5f,
             "explicit Descriptor mode recalls without encoded override");
    const auto response = roundTripXYMode (1.0f, 0.0f);
    require (response[0] > 0.5f && response[1] < 0.5f,
             "explicit Response mode recalls without encoded override");
    const auto encoded = roundTripXYMode (0.0f, 1.0f);
    require (encoded[0] < 0.5f && encoded[1] > 0.5f,
             "explicit Encoded mode recalls and keeps the underlying selector");

    auto source = std::make_unique<MorphosisAudioProcessor>();
    setParameter (*source, morphosis::parameter_ids::manualTransitionMs, 125.0f);
    setParameter (*source, morphosis::parameter_ids::xyInterpolation, 1.0f);
    setParameter (*source, morphosis::parameter_ids::xyEncodedDomain, 1.0f);
    juce::MemoryBlock state;
    source->getStateInformation (state);

    auto restored = std::make_unique<MorphosisAudioProcessor>();
    restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    require (std::abs (getParameterValue (*restored, morphosis::parameter_ids::manualTransitionMs)
                       - 125.0f) < 1.0e-4f,
             "manual transition duration survives state recall");
    require (getParameterValue (*restored, morphosis::parameter_ids::xyInterpolation) > 0.5f,
             "legacy XY selector survives state recall");
    require (getParameterValue (*restored, morphosis::parameter_ids::xyEncodedDomain) > 0.5f,
             "encoded XY override survives state recall");

    const std::unique_ptr<juce::XmlElement> xml (
        juce::AudioProcessor::getXmlFromBinary (state.getData(),
                                                static_cast<int> (state.getSize())));
    require (xml != nullptr, "candidate state has a parseable XML envelope");
    if (xml == nullptr)
        return;

    auto oldValueTree = juce::ValueTree::fromXml (*xml);
    for (int index = oldValueTree.getNumChildren() - 1; index >= 0; --index)
    {
        const auto id = oldValueTree.getChild (index).getProperty ("id").toString();
        if (id == morphosis::parameter_ids::manualTransitionMs
            || id == morphosis::parameter_ids::xyInterpolation
            || id == morphosis::parameter_ids::xyEncodedDomain)
            oldValueTree.removeChild (index, nullptr);
    }
    oldValueTree.setProperty ("stateVersion", 3, nullptr);
    const auto oldXml = oldValueTree.createXml();
    require (oldXml != nullptr, "old state fixture is serializable");
    if (oldXml == nullptr)
        return;
    juce::MemoryBlock oldState;
    juce::AudioProcessor::copyXmlToBinary (*oldXml, oldState);

    auto migrated = std::make_unique<MorphosisAudioProcessor>();
    migrated->setStateInformation (oldState.getData(), static_cast<int> (oldState.getSize()));
    require (std::abs (getParameterValue (*migrated, morphosis::parameter_ids::manualTransitionMs)
                       - 20.0f) < 1.0e-4f,
             "old state without manual duration migrates to 20 ms");
    require (getParameterValue (*migrated, morphosis::parameter_ids::xyInterpolation) < 0.5f,
             "old state without XY selector retains descriptor mode");
    require (getParameterValue (*migrated, morphosis::parameter_ids::xyEncodedDomain) < 0.5f,
             "old state without encoded override retains descriptor mode");

    // The migration assertion above is specifically meaningful because the
    // receiving processor now has Response as its fresh-instance default.
    require (migrated->getParameterSnapshot().xyInterpolation
                 == morphosis::XYInterpolationMode::descriptor,
             "missing XY selector is explicitly migrated to historical Descriptor mode");
}

void checkExperimentalEndpointApproximation()
{
    const std::array<const char*, morphosis::kMaxBlendSources> xyIds {
        morphosis::parameter_ids::xyA, morphosis::parameter_ids::xyB,
        morphosis::parameter_ids::xyC, morphosis::parameter_ids::xyD };
    const std::array<int, morphosis::kMaxBlendSources> sources { 0, 43, 17, 184 };
    const auto render = [&] (double x, double y,
                             const std::array<int, morphosis::kMaxBlendSources>& selected)
    {
        MorphosisAudioProcessor processor;
        setParameter (processor, morphosis::parameter_ids::mode, 1.0f);
        setParameter (processor, morphosis::parameter_ids::xyInterpolation, 1.0f);
        setParameter (processor, morphosis::parameter_ids::softClip, 0.0f);
        setParameter (processor, morphosis::parameter_ids::dryWet, 1.0f);
        setParameter (processor, morphosis::parameter_ids::frequency, 0.0f);
        setParameter (processor, morphosis::parameter_ids::morph, 0.0f);
        setParameter (processor, morphosis::parameter_ids::transform, 0.0f);
        for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
            setParameter (processor, xyIds[static_cast<std::size_t> (index)],
                          static_cast<float> (selected[static_cast<std::size_t> (index)]));
        setParameter (processor, morphosis::parameter_ids::xyX, static_cast<float> (x));
        setParameter (processor, morphosis::parameter_ids::xyY, static_cast<float> (y));
        processor.prepareToPlay (48000.0, 64);

        std::array<float, morphosis::kExperimentalFirGraphPoints> graph {};
        bool found = false;
        for (int block = 0; block < 96; ++block)
        {
            processBlock (processor, 64, block * 64);
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
            MorphosisAudioProcessor::GraphSnapshot snapshot;
            while (processor.popLatestGraphSnapshot (snapshot))
                if (snapshot.responseFIR)
                {
                    graph = snapshot.responseDb;
                    found = true;
                }
            if (found && block > 20)
                break;
        }
        require (found, "endpoint response graph becomes available");
        if (! found)
            return 0.0;

        const auto expectedPreset = [&]
        {
            const auto corner = (x >= 0.5 ? 1 : 0) + (y < 0.5 ? 2 : 0);
            return selected[static_cast<std::size_t> (corner)];
        }();
        const auto expected = morphosis::MorphosisDSP::makeCoefficients (
            expectedPreset, 0.0, 0.0, 0.0, 48000.0);
        double maximumError = 0.0;
        int maximumIndex = 0;
        double maximumExpectedDb = 0.0;
        double maximumActualDb = 0.0;
        for (int index = 0; index < morphosis::kExperimentalFirGraphPoints; ++index)
        {
            const auto proportion = index > 0
                                  ? static_cast<double> (index)
                                      / static_cast<double> (
                                          morphosis::kExperimentalFirGraphPoints - 1)
                                  : 0.0;
            const auto frequency = 20.0 * std::pow (48000.0 * 0.5 / 20.0, proportion);
            const auto magnitude = std::max (
                morphosis::MorphosisDSP::responseMagnitude (expected, frequency, 48000.0),
                1.0e-7);
            const auto expectedDb = std::clamp (20.0 * std::log10 (magnitude), -72.0, 24.0);
            const auto error = std::abs (static_cast<double> (graph[
                                                   static_cast<std::size_t> (index)])
                                         - expectedDb);
            if (error > maximumError)
            {
                maximumError = error;
                maximumIndex = index;
                maximumExpectedDb = expectedDb;
                maximumActualDb = graph[static_cast<std::size_t> (index)];
            }
        }
        std::cout << "response_endpoint x=" << x << " y=" << y
                  << " preset=" << expectedPreset
                  << " max_error_db=" << maximumError
                  << " index=" << maximumIndex
                  << " expected_db=" << maximumExpectedDb
                  << " actual_db=" << maximumActualDb << '\n';
        require (std::isfinite (maximumError),
                 "realized endpoint response error remains finite");
        return maximumError;
    };

    double worstError = 0.0;
    for (const auto point : std::array<std::pair<double, double>, 4> {
             std::pair<double, double> { 0.0, 1.0 },
             std::pair<double, double> { 1.0, 1.0 },
             std::pair<double, double> { 0.0, 0.0 },
             std::pair<double, double> { 1.0, 0.0 } })
        worstError = std::max (worstError, render (point.first, point.second, sources));

    const std::array<int, morphosis::kMaxBlendSources> allNull { 0, 0, 0, 0 };
    const auto identityError = render (0.37, 0.63, allNull);
    std::cout << "response_identity_max_error_db=" << identityError
              << " response_worst_static_error_db=" << worstError << '\n';
    require (identityError < 1.0,
             "all-Null response interpolation retains endpoint identity approximately");
}

void checkExperimentalResponseBenchmark()
{
    const std::array<int, morphosis::kMaxBlendSources> sources { 0, 43, 17, 184 };
    const auto run = [&] (double rate, bool moving)
    {
        MorphosisAudioProcessor processor;
        setParameter (processor, morphosis::parameter_ids::mode, 1.0f);
        setParameter (processor, morphosis::parameter_ids::xyInterpolation, 1.0f);
        setParameter (processor, morphosis::parameter_ids::softClip, 0.0f);
        setParameter (processor, morphosis::parameter_ids::dryWet, 1.0f);
        const std::array<const char*, morphosis::kMaxBlendSources> xyIds {
            morphosis::parameter_ids::xyA, morphosis::parameter_ids::xyB,
            morphosis::parameter_ids::xyC, morphosis::parameter_ids::xyD };
        for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
            setParameter (processor, xyIds[static_cast<std::size_t> (index)],
                          static_cast<float> (sources[static_cast<std::size_t> (index)]));
        processor.prepareToPlay (rate, 64);

        constexpr int blockCount = 128;
        constexpr int blockSize = 64;
        std::size_t responseSnapshots = 0;
        double maxWorkerMilliseconds = 0.0;
        const auto started = std::chrono::steady_clock::now();
        for (int block = 0; block < blockCount; ++block)
        {
            if (moving)
            {
                const auto phase = static_cast<double> (block) * 0.19;
                setParameter (processor, morphosis::parameter_ids::xyX,
                              static_cast<float> (0.5 + 0.49 * std::sin (phase)));
                setParameter (processor, morphosis::parameter_ids::xyY,
                              static_cast<float> (0.5 + 0.49 * std::cos (phase * 0.71)));
            }
            processBlock (processor, blockSize, block * blockSize);
            MorphosisAudioProcessor::GraphSnapshot snapshot;
            while (processor.popLatestGraphSnapshot (snapshot))
                responseSnapshots += snapshot.responseFIR ? 1u : 0u;
            maxWorkerMilliseconds = std::max (
                maxWorkerMilliseconds, processor.getLastExperimentalFirBuildMilliseconds());
        }
        const auto elapsed = std::chrono::duration<double> (
            std::chrono::steady_clock::now() - started).count();

        // Give the worker bounded time to finish and let the audio-side
        // consumer publish one final complete graph. This wait is outside the
        // reported audio-loop timing.
        for (int block = 0; block < 32; ++block)
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
            processBlock (processor, blockSize, blockCount * blockSize + block * blockSize);
            MorphosisAudioProcessor::GraphSnapshot snapshot;
            while (processor.popLatestGraphSnapshot (snapshot))
                responseSnapshots += snapshot.responseFIR ? 1u : 0u;
            maxWorkerMilliseconds = std::max (
                maxWorkerMilliseconds, processor.getLastExperimentalFirBuildMilliseconds());
        }

        const auto audioSeconds = static_cast<double> (blockCount * blockSize) / rate;
        std::cout << "response_benchmark rate=" << rate
                  << " moving=" << (moving ? 1 : 0)
                  << " wall_seconds=" << elapsed
                  << " audio_seconds=" << audioSeconds
                  << " wall_audio_ratio=" << elapsed / audioSeconds
                  << " max_worker_ms=" << maxWorkerMilliseconds
                  << " response_snapshots=" << responseSnapshots
                  << " safety=" << (processor.hasSafetyFault() ? 1 : 0) << '\n';
        require (responseSnapshots > 0,
                 "response benchmark publishes a complete realized graph");
        require (! processor.hasSafetyFault(),
                 "response benchmark remains safety-clean");
    };

    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        run (rate, false);
        run (rate, true);
    }
}

} // namespace

int main()
{
    checkManualDiscreteAndRetargeting();
    checkManualTransitionDurations();
    checkHostDurationAndBlockSizes();
    checkExperimentalResponseWorkerAndGraph();
    checkStateRecallAndMigration();
    checkExperimentalEndpointApproximation();
    checkExperimentalResponseBenchmark();

    if (failures != 0)
    {
        std::cerr << failures << " sequencer/response candidate regression(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "Sequencer dry-bridge and experimental response candidate regressions passed\n";
    return EXIT_SUCCESS;
}
