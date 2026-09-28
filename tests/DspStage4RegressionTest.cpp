#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#ifndef MORPHOSIS_STAGE4_DSP_IMPLEMENTATION
 #define MORPHOSIS_STAGE4_DSP_IMPLEMENTATION "MorphosisDSP.cpp"
#endif

#define private public
#include MORPHOSIS_STAGE4_DSP_IMPLEMENTATION
#undef private

using morphosis::MorphosisDSP;
using morphosis::ParameterSnapshot;
using morphosis::ProcessingMode;
using morphosis::XYInterpolationMode;
using Clock = std::chrono::steady_clock;

namespace
{

void require (bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit (EXIT_FAILURE);
}

ParameterSnapshot responseParameters()
{
    ParameterSnapshot parameters;
    parameters.mode = ProcessingMode::xy;
    parameters.xyInterpolation = XYInterpolationMode::response;
    parameters.xyPresets = { 0, 43, 17, 184 };
    parameters.xyX = 0.37;
    parameters.xyY = 0.62;
    parameters.frequency = -0.8;
    parameters.morph = 0.45;
    parameters.transform = -0.65;
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

#if ! defined(MORPHOSIS_STAGE4_PREEDIT_ONLY)

enum class Scenario
{
    staticResponse,
    movingCoordinates,
    responseExitReentry
};

const char* scenarioName (Scenario scenario)
{
    switch (scenario)
    {
        case Scenario::staticResponse: return "static";
        case Scenario::movingCoordinates: return "moving";
        case Scenario::responseExitReentry: return "exit-reentry";
    }
    return "unknown";
}

ParameterSnapshot parametersAt (Scenario scenario, int sample)
{
    auto parameters = responseParameters();
    if (scenario == Scenario::movingCoordinates)
    {
        if (sample >= 512)
        {
            parameters.xyX = 0.87;
            parameters.frequency = 2.1;
        }
        if (sample >= 1024)
        {
            parameters.xyY = 0.18;
            parameters.morph = 2.3;
            parameters.transform = -1.4;
        }
        if (sample >= 1536)
        {
            parameters.xyX = 0.14;
            parameters.xyY = 0.74;
            parameters.frequency = -1.6;
        }
    }
    else if (scenario == Scenario::responseExitReentry)
    {
        if (sample >= 512)
            parameters.xyInterpolation = XYInterpolationMode::descriptor;
        if (sample >= 1024)
        {
            parameters.xyInterpolation = XYInterpolationMode::response;
            parameters.xyX = 0.71;
            parameters.frequency = 1.4;
        }
    }
    return parameters;
}

struct RenderResult
{
    std::uint64_t hash = 14695981039346656037ull;
    std::uint64_t designCount = 0;
    std::uint64_t acceptedGeneration = 0;
    double lastBuildMilliseconds = 0.0;
};

RenderResult renderOffline (double sampleRate, int blockSize, int channels,
                            Scenario scenario, bool workerContention = false)
{
    constexpr int totalSamples = 2048;
    const auto designsBefore = morphosis::getSynchronousFirDesignCountForTesting();
    MorphosisDSP dsp;
    dsp.prepare (sampleRate, blockSize);

    if (workerContention)
    {
        const auto realtimeRequest = responseParameters();
        dsp.beginBlock (realtimeRequest, false);
        require (dsp.firBuildOutstanding,
                 "realtime request is outstanding before switching to offline");
    }
    else
    {
        dsp.experimentalFirWorker->stop();
    }

    RenderResult result;
    for (int blockStart = 0; blockStart < totalSamples; blockStart += blockSize)
    {
        const auto parameters = parametersAt (scenario, blockStart);
        dsp.beginBlock (parameters, true);
        for (int offset = 0; offset < blockSize; ++offset)
        {
            const auto sample = blockStart + offset;
            dsp.advanceSample();
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto phase = static_cast<double> (sample) * (0.031 + 0.009 * channel);
                const auto input = static_cast<float> (0.08 * std::sin (phase)
                                                       + (sample == 7 + channel ? 0.2 : 0.0));
                const auto output = dsp.processSampleNoAdvance (input, channel);
                result.hash = appendFloat (result.hash, output);
            }
        }
    }

    if (workerContention)
        dsp.experimentalFirWorker->stop();

    result.designCount = morphosis::getSynchronousFirDesignCountForTesting() - designsBefore;
    result.acceptedGeneration = dsp.firAcceptedGeneration;
    result.lastBuildMilliseconds = dsp.getLastExperimentalFirBuildMilliseconds();
    require (dsp.firReady, "offline Response produces a FIR without worker completion");
    require (! dsp.firBuildOutstanding, "offline mode clears the obsolete worker request");
    return result;
}

void checkOfflineDeterminismMatrix()
{
    constexpr std::array<double, 3> sampleRates { 44100.0, 48000.0, 96000.0 };
    constexpr std::array<int, 3> blockSizes { 1, 64, 512 };
    constexpr std::array<Scenario, 3> scenarios {
        Scenario::staticResponse, Scenario::movingCoordinates,
        Scenario::responseExitReentry
    };

    for (const auto scenario : scenarios)
    {
        for (const auto sampleRate : sampleRates)
        {
            for (const auto channels : { 1, 2 })
            {
                RenderResult reference;
                for (const auto blockSize : blockSizes)
                {
                    const auto render = renderOffline (sampleRate, blockSize, channels, scenario);
                    if (blockSize == blockSizes.front())
                    {
                        reference = render;
                        std::cout << "offline_hash scenario=" << scenarioName (scenario)
                                  << " rate=" << sampleRate
                                  << " channels=" << channels
                                  << " hash=" << std::hex << render.hash << std::dec << '\n';
                    }
                    else
                        require (render.hash == reference.hash,
                                 "offline output hash is exact across host block sizes");

                    if (scenario == Scenario::staticResponse)
                        require (render.designCount == 1,
                                 "unchanged offline controls design one FIR only");
                    else if (scenario == Scenario::movingCoordinates)
                        require (render.designCount > 1,
                                 "moving XY and FMX controls schedule changed FIR designs");
                    else
                        require (render.designCount >= 2,
                                 "Response re-entry schedules a fresh deterministic FIR");
                }
            }
        }
    }
}

void checkDefinedAdoptionBoundaries()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 1);
    dsp.experimentalFirWorker->stop();
    auto parameters = responseParameters();
    std::uint64_t firstGeneration = 0;
    std::uint64_t firstRetargetGeneration = 0;
    int firstAdoptionSample = -1;
    int retargetAdoptionSample = -1;
    int queuedRetargetAdoptionSample = -1;

    for (int sample = 0; sample <= 256; ++sample)
    {
        if (sample == 64)
            parameters.xyX = 0.9;
        if (sample == 96)
            parameters.frequency = 1.8;
        dsp.beginBlock (parameters, true);
        dsp.advanceSample();

        if (firstAdoptionSample < 0 && dsp.firReady)
        {
            firstAdoptionSample = sample;
            firstGeneration = dsp.firAcceptedGeneration;
        }
        else if (firstGeneration != 0 && retargetAdoptionSample < 0
                 && dsp.firAcceptedGeneration > firstGeneration)
        {
            retargetAdoptionSample = sample;
            firstRetargetGeneration = dsp.firAcceptedGeneration;
        }
        else if (firstRetargetGeneration != 0 && queuedRetargetAdoptionSample < 0
                 && dsp.firAcceptedGeneration > firstRetargetGeneration)
        {
            queuedRetargetAdoptionSample = sample;
        }

        const auto input = static_cast<float> (0.03 * std::sin (sample * 0.021));
        (void) dsp.processSampleNoAdvance (input, 0);
        (void) dsp.processSampleNoAdvance (input * 0.61f, 1);
    }

    require (firstAdoptionSample == 0,
             "first offline FIR is designed and adopted at sample boundary zero");
    require (retargetAdoptionSample == 64,
             "first changed FIR is adopted at its deterministic request boundary");
    require (queuedRetargetAdoptionSample == 161,
             "queued retarget adopts after the existing FIR fade reaches and settles its endpoint");
    require (dsp.firAcceptedGeneration == dsp.firRequestGeneration,
             "the retargeted generation remains current after adoption");
}

void checkWorkerContentionAndStaleResult()
{
    constexpr auto sampleRate = 48000.0;
    const auto baseline = renderOffline (sampleRate, 64, 2, Scenario::movingCoordinates);
    const auto contended = renderOffline (sampleRate, 64, 2, Scenario::movingCoordinates, true);
    require (baseline.hash == contended.hash,
             "offline output is independent of an outstanding worker build");

    MorphosisDSP dsp;
    dsp.prepare (sampleRate, 64);
    const auto parameters = responseParameters();
    dsp.beginBlock (parameters, false);
    require (dsp.firBuildOutstanding, "worker request is recorded before offline handover");
    auto staleRequest = dsp.firLastSubmittedKey;
    staleRequest.generation = dsp.firOutstandingGeneration;
    dsp.experimentalFirWorker->stop();
    auto staleResult = MorphosisDSP::ExperimentalFirWorker::build (staleRequest);
    require (staleResult.valid, "stale worker fixture is a valid FIR result");

    dsp.beginBlock (parameters, true);
    const auto beforeOfflineBuilds = morphosis::getSynchronousFirDesignCountForTesting();
    dsp.advanceSample();
    require (dsp.firReady && dsp.firAcceptedGeneration == dsp.firRequestGeneration,
             "offline design is accepted on its first audio-sample boundary");
    const auto acceptedOfflineGeneration = dsp.firAcceptedGeneration;
    require (dsp.experimentalFirWorker->storeResult (staleResult),
             "late old-generation result enters the normal worker mailbox");
    dsp.advanceSample();
    require (dsp.firAcceptedGeneration == acceptedOfflineGeneration,
             "late stale result cannot replace the offline kernel");
    require (! dsp.firBuildOutstanding,
             "stale completion does not leave the outstanding-request gate set");
    require (morphosis::getSynchronousFirDesignCountForTesting() == beforeOfflineBuilds + 1,
             "mode switch triggers exactly one synchronous first design");

    const auto synchronousBuildsBeforeRealtime =
        morphosis::getSynchronousFirDesignCountForTesting();
    dsp.experimentalFirWorker->start();
    const auto workerBuildsBeforeRealtime = dsp.experimentalFirWorker->buildCountForTesting.load (
        std::memory_order_relaxed);
    auto realtimeParameters = parameters;
    realtimeParameters.frequency = 1.6;
    dsp.beginBlock (realtimeParameters, false);
    for (int sample = 0; sample < 512
         && dsp.experimentalFirWorker->buildCountForTesting.load (std::memory_order_relaxed)
                == workerBuildsBeforeRealtime; ++sample)
    {
        dsp.beginBlock (realtimeParameters, false);
        dsp.advanceSample();
        (void) dsp.processSampleNoAdvance (0.02f, 0);
        (void) dsp.processSampleNoAdvance (0.013f, 1);
        std::this_thread::yield();
    }
    require (dsp.experimentalFirWorker->buildCountForTesting.load (std::memory_order_relaxed)
                 > workerBuildsBeforeRealtime,
             "offline-to-realtime mode switch resumes asynchronous worker designs");
    require (morphosis::getSynchronousFirDesignCountForTesting()
                 == synchronousBuildsBeforeRealtime,
             "offline-to-realtime switch does not design synchronously");
}

void checkRealtimeRemainsAsynchronous()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    const auto parameters = responseParameters();
    const auto designsBefore = morphosis::getSynchronousFirDesignCountForTesting();
    dsp.experimentalFirWorker->stop();
    dsp.beginBlock (parameters, false);
    for (int sample = 0; sample < 96; ++sample)
    {
        dsp.advanceSample();
        (void) dsp.processSampleNoAdvance (0.01f, 0);
    }
    require (morphosis::getSynchronousFirDesignCountForTesting() == designsBefore,
             "realtime processing never calls the synchronous designer");
    require (! dsp.firReady,
             "stopped realtime worker does not silently fall back to synchronous design");

    dsp.experimentalFirWorker->start();
    dsp.beginBlock (parameters, false);
    require (dsp.firBuildOutstanding,
             "realtime retry submits work through the asynchronous worker");
    const auto deadline = Clock::now() + std::chrono::seconds (10);
    while (! dsp.firReady && Clock::now() < deadline)
    {
        dsp.advanceSample();
        (void) dsp.processSampleNoAdvance (0.01f, 0);
        std::this_thread::yield();
    }
    require (dsp.firReady, "asynchronous worker result remains consumable");
    require (dsp.experimentalFirWorker->buildCountForTesting.load (
                 std::memory_order_relaxed) > 0,
             "realtime FIR construction runs on the worker thread");
    require (morphosis::getSynchronousFirDesignCountForTesting() == designsBefore,
             "realtime worker completion did not invoke synchronous design");
}

void checkInvalidResultFallback()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    dsp.experimentalFirWorker->stop();
    const auto parameters = responseParameters();
    dsp.beginBlock (parameters, true);
    dsp.advanceSample();
    require (dsp.firReady, "initial offline FIR is ready before invalid-result test");

    const auto previousGeneration = dsp.firAcceptedGeneration;
    const auto previousImpulse = dsp.firImpulse;
    const auto previousFallbacks = dsp.coefficientFallbackCount;
    MorphosisDSP::FirBuildResult invalid;
    invalid.generation = ++dsp.firRequestGeneration;
    invalid.valid = false;
    invalid.usedSafeFallback = true;
    require (dsp.experimentalFirWorker->storeResult (invalid),
             "invalid result is delivered through the worker mailbox");
    dsp.advanceSample();
    require (dsp.firAcceptedGeneration == previousGeneration,
             "invalid result leaves the accepted FIR generation unchanged");
    require (dsp.firImpulse == previousImpulse,
             "invalid result preserves the established FIR fallback");
    require (dsp.coefficientFallbackCount == previousFallbacks + 1,
             "invalid result increments existing fallback telemetry");
}

void measureSynchronousBuildCost()
{
    std::array<double, 5> elapsedMs {};
    for (std::size_t trial = 0; trial < elapsedMs.size(); ++trial)
    {
        MorphosisDSP::FirBuildRequest request;
        request.presets = { 0, 43, 17, 184 };
        request.x = 0.37;
        request.y = 0.62;
        request.frequency = -0.8;
        request.morph = 0.45;
        request.transform = -0.65;
        request.sampleRate = 48000.0;
        request.generation = trial + 1;
        const auto started = Clock::now();
        const auto result = MorphosisDSP::ExperimentalFirWorker::build (request);
        elapsedMs[trial] = std::chrono::duration<double, std::milli> (
            Clock::now() - started).count();
        require (result.valid, "synchronous performance sample designs a valid FIR");
    }
    std::sort (elapsedMs.begin(), elapsedMs.end());
    std::cout << "Shared pure FIR builder, 48 kHz: median=" << elapsedMs[2]
              << " ms, max=" << elapsedMs.back() << " ms across 5 builds.\n";
}

#endif

#if defined(MORPHOSIS_STAGE4_PREEDIT_ONLY)

std::uint64_t renderWithInjectedCompletion (int completionSample, int& activationSample)
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    const auto parameters = responseParameters();
    dsp.beginBlock (parameters);
    require (dsp.firBuildOutstanding, "pre-edit async request starts outstanding");
    auto request = dsp.firLastSubmittedKey;
    request.generation = dsp.firOutstandingGeneration;
    dsp.experimentalFirWorker->stop();
    auto result = MorphosisDSP::ExperimentalFirWorker::build (request);
    require (result.valid, "pre-edit controlled worker result is valid");

    activationSample = -1;
    auto hash = 14695981039346656037ull;
    for (int sample = 0; sample < 2048; ++sample)
    {
        if (sample == completionSample)
            require (dsp.experimentalFirWorker->storeResult (result),
                     "pre-edit controlled completion enters worker mailbox");
        dsp.advanceSample();
        if (activationSample < 0 && dsp.firReady)
            activationSample = sample;
        const auto input = static_cast<float> (0.1 * std::sin (sample * 0.037));
        hash = appendFloat (hash, dsp.processSampleNoAdvance (input, 0));
    }
    return hash;
}

void checkPreEditNondeterminism()
{
    int earlyActivation = -1;
    int delayedActivation = -1;
    const auto early = renderWithInjectedCompletion (0, earlyActivation);
    const auto delayed = renderWithInjectedCompletion (256, delayedActivation);
    require (earlyActivation == 0 && delayedActivation == 256,
             "pre-edit FIR activation follows injected worker completion timing");
    require (early != delayed,
             "pre-edit identical renders diverge under deterministic worker pacing");
    std::cout << "Pre-edit controlled completion: FIR activation samples "
              << earlyActivation << " and " << delayedActivation
              << "; hashes 0x" << std::hex << early << " and 0x" << delayed
              << std::dec << " differ as expected.\n";
}

#endif

} // namespace

int main()
{
#if defined(MORPHOSIS_STAGE4_PREEDIT_ONLY)
    checkPreEditNondeterminism();
#else
    checkOfflineDeterminismMatrix();
    checkDefinedAdoptionBoundaries();
    checkWorkerContentionAndStaleResult();
    checkRealtimeRemainsAsynchronous();
    checkInvalidResultFallback();
    measureSynchronousBuildCost();
    std::cout << "Stage 4 deterministic offline rendering checks passed.\n";
#endif
    return EXIT_SUCCESS;
}
