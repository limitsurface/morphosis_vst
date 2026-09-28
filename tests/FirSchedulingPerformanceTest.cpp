#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

// White-box access is confined to this translation unit.  The production
// class is compiled with the same fields and types as the plugin target.
#define private public
#include "MorphosisDSP.cpp"
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
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit (EXIT_FAILURE);
    }
}

ParameterSnapshot responseParameters()
{
    ParameterSnapshot parameters;
    parameters.mode = ProcessingMode::xy;
    parameters.xyInterpolation = XYInterpolationMode::response;
    parameters.xyPresets = { 0, 43, 17, 184 };
    parameters.xyX = 0.37;
    parameters.xyY = 0.62;
    parameters.frequency = 0.4;
    parameters.morph = -0.7;
    parameters.transform = 0.9;
    parameters.softClip = false;
    return parameters;
}

void processBlock (MorphosisDSP& dsp, const ParameterSnapshot& parameters, int channels)
{
    dsp.beginBlock (parameters);
    for (int sample = 0; sample < 64; ++sample)
    {
        dsp.advanceSample();
        const auto input = static_cast<float> (0.001 * std::sin (sample * 0.07));
        (void) dsp.processSampleNoAdvance (input, 0);
        if (channels == 2)
            (void) dsp.processSampleNoAdvance (input * 0.73f, 1);
    }
}

template <typename Predicate>
void waitFor (MorphosisDSP& dsp, const ParameterSnapshot& parameters, int channels,
              Predicate predicate, const char* message)
{
    const auto deadline = Clock::now() + std::chrono::seconds (5);
    while (! predicate())
    {
        processBlock (dsp, parameters, channels);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
        require (Clock::now() < deadline, message);
    }
}

void waitForAccepted (MorphosisDSP& dsp, const ParameterSnapshot& parameters,
                      int channels, std::uint64_t previousGeneration,
                      const char* message)
{
    waitFor (dsp, parameters, channels, [&]
    {
        return dsp.firAcceptedGeneration > previousGeneration
            && dsp.firAcceptedGeneration == dsp.firRequestGeneration
            && ! dsp.firBuildOutstanding;
    }, message);
}

bool sameResult (const MorphosisDSP::FirBuildResult& left,
                 const MorphosisDSP::FirBuildResult& right)
{
    return left.generation == right.generation
        && left.sampleRate == right.sampleRate
        && left.valid == right.valid
        && left.usedSafeFallback == right.usedSafeFallback
        && left.buildMilliseconds == right.buildMilliseconds
        && left.impulse == right.impulse
        && left.partitions == right.partitions
        && left.graphResponseDb == right.graphResponseDb;
}

void checkStaticBuildDeterminism()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        MorphosisDSP::FirBuildRequest request;
        request.presets = { 0, 43, 17, 184 };
        request.x = 0.37;
        request.y = 0.62;
        request.frequency = 0.4;
        request.morph = -0.7;
        request.transform = 0.9;
        request.sampleRate = rate;
        request.generation = 1;
        const auto first = MorphosisDSP::ExperimentalFirWorker::build (request);
        request.generation = 2;
        const auto second = MorphosisDSP::ExperimentalFirWorker::build (request);
        require (first.valid == second.valid && first.usedSafeFallback == second.usedSafeFallback,
                 "identical FIR requests preserve validity");
        require (first.impulse == second.impulse && first.partitions == second.partitions
                     && first.graphResponseDb == second.graphResponseDb,
                 "identical FIR requests preserve taps and response");
    }
}

void checkEmptyPoll()
{
    MorphosisDSP dsp;
    dsp.experimentalFirWorker->stop();
    dsp.firResultScratch->generation = 17;
    dsp.firResultScratch->sampleRate = 96000.0;
    dsp.firResultScratch->valid = true;
    dsp.firResultScratch->usedSafeFallback = true;
    dsp.firResultScratch->buildMilliseconds = 3.5;
    dsp.firResultScratch->impulse.fill (0.25f);
    dsp.firResultScratch->partitions.fill ({ 0.25f, -0.5f });
    dsp.firResultScratch->graphResponseDb.fill (-12.0f);
    const auto before = *dsp.firResultScratch;
    for (int poll = 0; poll < 10000; ++poll)
        dsp.consumeExperimentalFirResult();
    require (sameResult (before, *dsp.firResultScratch),
             "empty FIR polling leaves persistent scratch untouched");
}

void checkStationaryAndChanges()
{
    MorphosisDSP stationary;
    stationary.prepare (48000.0, 64);
    const auto stationaryParameters = responseParameters();
    waitForAccepted (stationary, stationaryParameters, 1, 0,
                     "stationary Response build completes");
    const auto generation = stationary.firRequestGeneration;
    for (int block = 0; block < 16; ++block)
        processBlock (stationary, stationaryParameters, 1);
    require (stationary.firRequestGeneration == generation
                 && stationary.firAcceptedGeneration == generation,
             "stationary Response does not resubmit unchanged input");

    const auto checkChange = [] (auto mutate, const char* message)
    {
        MorphosisDSP dsp;
        dsp.prepare (48000.0, 64);
        auto parameters = responseParameters();
        waitForAccepted (dsp, parameters, 2, 0, "initial changed-input build completes");
        const auto beforeGeneration = dsp.firAcceptedGeneration;
        const auto beforeKey = dsp.firLastSubmittedKey;
        mutate (parameters);
        waitForAccepted (dsp, parameters, 2, beforeGeneration, message);
        require (! MorphosisDSP::firBuildRequestsEqual (beforeKey, dsp.firLastSubmittedKey),
                 "changed input produces a different submitted key");
    };

    checkChange ([] (auto& p) { p.xyPresets[0] = 1; p.xyNonlinearSources[0] = true; },
                 "source change produces a new accepted FIR");
    checkChange ([] (auto& p) { p.frequency = 2.2; },
                 "frequency change produces a new accepted FIR");
    checkChange ([] (auto& p) { p.morph = 2.1; },
                 "morph change produces a new accepted FIR");
    checkChange ([] (auto& p) { p.transform = -2.4; },
                 "transform change produces a new accepted FIR");
    checkChange ([] (auto& p) { p.xyX = 0.82; },
                 "XY X change produces a new accepted FIR");
    checkChange ([] (auto& p) { p.xyY = 0.18; },
                 "XY Y change produces a new accepted FIR");
}

void checkOutstandingFailureAndLifecycle()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    auto parameters = responseParameters();
    dsp.beginBlock (parameters);
    require (dsp.firBuildOutstanding, "initial request is outstanding");
    const auto firstGeneration = dsp.firOutstandingGeneration;
    auto latest = parameters;
    latest.xyPresets[0] = 1;
    latest.frequency = -2.0;
    dsp.beginBlock (latest);
    waitFor (dsp, latest, 2, [&]
    {
        return dsp.firLastSubmittedKeyValid
            && dsp.firLastSubmittedKey.presets == latest.xyPresets
            && dsp.firRequestGeneration > firstGeneration
            && dsp.firAcceptedGeneration == dsp.firRequestGeneration
            && ! dsp.firBuildOutstanding;
    }, "latest input submitted after outstanding build");

    MorphosisDSP failed;
    failed.prepare (48000.0, 64);
    failed.experimentalFirWorker->stop();
    failed.beginBlock (parameters);
    require (! failed.firLastSubmittedKeyValid && ! failed.firBuildOutstanding,
             "failed submission does not poison the request key");
    failed.experimentalFirWorker->start();
    waitForAccepted (failed, parameters, 2, 0,
                     "same input submits after worker restart");

    const auto acceptedBeforeLeave = failed.firAcceptedGeneration;
    auto descriptor = parameters;
    descriptor.xyInterpolation = XYInterpolationMode::descriptor;
    failed.beginBlock (descriptor);
    require (! failed.firLastSubmittedKeyValid, "Response leave invalidates deduplication");
    failed.beginBlock (parameters);
    waitForAccepted (failed, parameters, 2, acceptedBeforeLeave,
                     "Response re-entry rebuilds unchanged input");

    failed.reset();
    waitForAccepted (failed, parameters, 2, 0, "reset permits a fresh Response build");
    failed.prepare (96000.0, 64);
    waitForAccepted (failed, parameters, 2, 0, "reprepare permits a fresh Response build");
}

} // namespace

int main()
{
    checkStaticBuildDeterminism();
    checkEmptyPoll();
    checkStationaryAndChanges();
    checkOutstandingFailureAndLifecycle();
    std::cout << "Stage 1 FIR scheduling checks passed.\n";
    return EXIT_SUCCESS;
}
