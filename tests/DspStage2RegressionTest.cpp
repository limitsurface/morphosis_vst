#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// White-box access is confined to this translation unit. The production class
// is compiled with the same fields and types as the plugin target.
#define private public
#include "MorphosisDSP.cpp"
#undef private

using morphosis::CoefficientSet;
using morphosis::MorphBlend;
using morphosis::MorphosisDSP;
using morphosis::ParameterSnapshot;
using morphosis::ProcessingMode;
using morphosis::XYInterpolationMode;

namespace
{

int failures = 0;

void expect (bool condition, const std::string& message)
{
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool closeEnough (double left, double right, double tolerance = 1.0e-10)
{
    return std::isfinite (left) && std::isfinite (right)
        && std::abs (left - right) <= tolerance;
}

bool sameBlend (const MorphBlend& left, const MorphBlend& right)
{
    if (left.count != right.count)
        return false;
    for (int index = 0; index < left.count; ++index)
    {
        const auto i = static_cast<std::size_t> (index);
        if (left.presets[i] != right.presets[i]
            || left.nonlinearSources[i] != right.nonlinearSources[i]
            || ! closeEnough (left.weights[i], right.weights[i]))
            return false;
    }
    return true;
}

bool sameCoefficients (const CoefficientSet& left, const CoefficientSet& right,
                       double tolerance = 1.0e-10)
{
    if (left.valid != right.valid || left.usedSafeFallback != right.usedSafeFallback
        || left.neutralBypass != right.neutralBypass
        || ! closeEnough (left.gain, right.gain, tolerance))
        return false;

    for (int index = 0; index < morphosis::kStageCount; ++index)
    {
        const auto& a = left.stages[static_cast<std::size_t> (index)];
        const auto& b = right.stages[static_cast<std::size_t> (index)];
        if (! closeEnough (a.a, b.a, tolerance)
            || ! closeEnough (a.b, b.b, tolerance)
            || ! closeEnough (a.inputGain, b.inputGain, tolerance)
            || ! closeEnough (a.z1, b.z1, tolerance)
            || ! closeEnough (a.z2, b.z2, tolerance)
            || ! closeEnough (a.radius, b.radius, tolerance)
            || ! closeEnough (a.angle, b.angle, tolerance)
            || ! closeEnough (a.zeroRadius, b.zeroRadius, tolerance)
            || ! closeEnough (a.zeroAngle, b.zeroAngle, tolerance)
            || ! closeEnough (a.normalizationExponent, b.normalizationExponent, tolerance)
            || a.normalizeDc != b.normalizeDc)
            return false;
    }
    return true;
}

ParameterSnapshot sequenceParameters (int firstPreset, bool firstNonlinear,
                                      bool manual = false)
{
    ParameterSnapshot parameters;
    parameters.mode = ProcessingMode::sequencer;
    parameters.sequencePresets.fill (0);
    parameters.sequenceNonlinearSources.fill (false);
    parameters.sequencePresets[0] = firstPreset;
    parameters.sequenceNonlinearSources[0] = firstNonlinear;
    parameters.sequenceLength = 4;
    parameters.sequenceManual = manual;
    parameters.sequencePosition = 0.0;
    parameters.blend.count = 1;
    parameters.blend.presets[0] = firstPreset;
    parameters.blend.weights[0] = 1.0;
    parameters.blend.nonlinearSources[0] = firstNonlinear;
    parameters.softClip = false;
    return parameters;
}

ParameterSnapshot responseParameters()
{
    ParameterSnapshot parameters;
    parameters.mode = ProcessingMode::xy;
    parameters.xyInterpolation = XYInterpolationMode::response;
    parameters.xyPresets = { 0, 43, 17, 184 };
    parameters.xyNonlinearSources = { false, false, false, true };
    parameters.xyX = 0.4;
    parameters.xyY = 0.0;
    parameters.frequency = 0.4;
    parameters.morph = -0.7;
    parameters.transform = 0.9;
    parameters.internalThresholdSetting = 18;
    parameters.softClip = false;
    return parameters;
}

void advanceSamples (MorphosisDSP& dsp, int count, int channels = 1,
                     int sampleOffset = 0)
{
    for (int index = 0; index < count; ++index)
    {
        dsp.advanceSample();
        const auto sample = static_cast<float> (
            0.001 * std::sin (0.07 * static_cast<double> (sampleOffset + index)));
        const auto left = dsp.processSampleNoAdvance (sample, 0);
        expect (std::isfinite (left), "transition output remains finite");
        if (channels == 2)
        {
            const auto right = dsp.processSampleNoAdvance (sample * 0.73f, 1);
            expect (std::isfinite (right), "stereo transition output remains finite");
        }
    }
}

void processBlock (MorphosisDSP& dsp, const ParameterSnapshot& parameters,
                   int blockSize, int channels, int sampleOffset)
{
    dsp.beginBlock (parameters);
    advanceSamples (dsp, blockSize, channels, sampleOffset);
}

void primeResponseState (MorphosisDSP& dsp)
{
    dsp.prepare (48000.0, 64);
    dsp.experimentalFirWorker->stop();
    const auto parameters = responseParameters();
    dsp.beginBlock (parameters);
    dsp.firReady = true;
    dsp.responseModeActive = true;
    dsp.experimentalAlgorithmMix = { 1.0, 1.0, 0 };
    dsp.nonlinearMix = { 0.0, 0.0, 0 };
}

void checkSequenceCoordinateContinuity()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);

    auto initial = sequenceParameters (0, false);
    dsp.beginBlock (initial);
    dsp.advanceSample();

    auto moving = initial;
    moving.frequency = 5.0;
    moving.morph = -4.0;
    moving.transform = 3.0;
    dsp.beginBlock (moving);
    dsp.setSequenceTarget (1, false, 200);

    const auto amount = 1.0 / static_cast<double> (dsp.controlSmoothingSamples);
    double expectedFrequency = 0.0;
    double expectedMorph = 0.0;
    double expectedTransform = 0.0;
    bool checkedHandover = false;

    for (int sample = 0; sample < 500; ++sample)
    {
        dsp.advanceSample();
        expectedFrequency += (moving.frequency - expectedFrequency) * amount;
        expectedMorph += (moving.morph - expectedMorph) * amount;
        expectedTransform += (moving.transform - expectedTransform) * amount;

        if (! checkedHandover && dsp.transitionPhase == MorphosisDSP::TransitionPhase::fadeIn)
        {
            checkedHandover = true;
            expect (closeEnough (dsp.frequencyControl.current, expectedFrequency),
                    "sequence handover preserves Frequency trajectory");
            expect (closeEnough (dsp.morphControl.current, expectedMorph),
                    "sequence handover preserves Morph trajectory");
            expect (closeEnough (dsp.transformControl.current, expectedTransform),
                    "sequence handover preserves Transform trajectory");
            expect (closeEnough (dsp.frequencyControl.target, moving.frequency),
                    "sequence handover preserves latest Frequency target");
            expect (closeEnough (dsp.morphControl.target, moving.morph),
                    "sequence handover preserves latest Morph target");
            expect (closeEnough (dsp.transformControl.target, moving.transform),
                    "sequence handover preserves latest Transform target");

            const auto expected = MorphosisDSP::makeCoefficients (
                1, expectedFrequency, expectedMorph, expectedTransform, 48000.0);
            expect (sameCoefficients (dsp.currentCoefficients, expected),
                    "sequence handover installs destination coefficients at current coordinates");
            expect (sameBlend (dsp.currentBlend, dsp.cachedBlend),
                    "sequence handover blend cache describes installed coefficients");
            expect (dsp.cachedCoordinateKey
                        == MorphosisDSP::quantizedCoordinateKey (expectedFrequency,
                                                                 expectedMorph,
                                                                 expectedTransform),
                    "sequence handover coordinate cache describes installed coefficients");
        }
    }

    expect (checkedHandover, "sequence transition reaches dry handover");
    expect (dsp.frequencyControl.target == moving.frequency
                && dsp.morphControl.target == moving.morph
                && dsp.transformControl.target == moving.transform,
            "held controls retain their latest settled targets after handover");
}

void checkInvalidHandoverRetention()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    auto parameters = sequenceParameters (0, false);
    dsp.beginBlock (parameters);
    dsp.advanceSample();
    dsp.setSequenceTarget (2, true, 2);

    const auto oldCoefficients = dsp.currentCoefficients;
    const auto oldBlend = dsp.currentBlend;
    const auto oldCache = dsp.cachedBlend;
    const auto oldKey = dsp.cachedCoordinateKey;
    const auto oldFallbacks = dsp.coefficientFallbackCount;

    dsp.pendingCoefficients.valid = false;
    dsp.pendingCoefficients.usedSafeFallback = true;
    dsp.sequenceHardwareTransition = false;
    dsp.filterMix = 0.001;
    dsp.transitionRemaining = 1;
    dsp.advanceTransition();

    expect (! dsp.pendingReady && dsp.transitionPhase == MorphosisDSP::TransitionPhase::none,
            "invalid sequence handover retains the existing fade-back policy");
    expect (closeEnough (dsp.filterMix, 1.0),
            "invalid sequence handover fades back to the retained source");
    expect (sameCoefficients (dsp.currentCoefficients, oldCoefficients),
            "invalid sequence handover does not install invalid coefficients");
    expect (sameBlend (dsp.currentBlend, oldBlend) && sameBlend (dsp.cachedBlend, oldCache),
            "invalid sequence handover does not publish stale blend caches");
    expect (dsp.cachedCoordinateKey == oldKey,
            "invalid sequence handover does not publish a stale coordinate cache");
    expect (dsp.coefficientFallbackCount > oldFallbacks,
            "invalid sequence handover reports the coefficient fault");
}

void checkRetargetsAndLifecycleMatrix()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto blockSize : { 1, 64, 512 })
        {
            for (const auto channels : { 1, 2 })
            {
                MorphosisDSP dsp;
                dsp.prepare (rate, blockSize);
                auto parameters = sequenceParameters (0, false);
                int sample = 0;
                processBlock (dsp, parameters, blockSize, channels, sample);
                sample += blockSize;

                // A host-style update arrives through the block snapshot.
                parameters.sequencePresets[0] = 1;
                parameters.sequenceNonlinearSources[0] = true;
                processBlock (dsp, parameters, blockSize, channels, sample);
                sample += blockSize;

                // A direct/manual update can replace it on the same timeline.
                dsp.setSequenceTarget (2, false, 20);
                advanceSamples (dsp, blockSize, channels, sample);
                sample += blockSize;
                advanceSamples (dsp, 80, channels, sample);
                sample += 80;
                expect (dsp.currentBlend.presets[0] == 2,
                        "latest rapid A-to-B-to-C sequence target wins");

                const auto resetsBeforeReverse = dsp.transitionResetCount;
                dsp.setSequenceTarget (3, true, 20);
                advanceSamples (dsp, 2, channels, sample);
                sample += 2;
                dsp.setSequenceTarget (2, false, 20);
                advanceSamples (dsp, 80, channels, sample);
                sample += 80;
                expect (dsp.currentBlend.presets[0] == 2
                            && dsp.transitionResetCount == resetsBeforeReverse,
                        "rapid A-to-B-to-A cancellation returns to the committed source");

                const auto resetsBeforeNoOp = dsp.transitionResetCount;
                dsp.setSequenceTarget (2, false, 20);
                advanceSamples (dsp, 20, channels, sample);
                sample += 20;
                expect (dsp.currentBlend.presets[0] == 2
                            && dsp.transitionResetCount == resetsBeforeNoOp,
                        "identical sequence targets remain a no-op");

                auto manual = sequenceParameters (0, false, true);
                manual.sequencePresets[4] = 4;
                manual.sequenceNonlinearSources[4] = true;
                manual.sequencePosition = 4.0 / 15.0;
                processBlock (dsp, manual, blockSize, channels, sample);
                sample += blockSize;
                const auto manualTransitionSamples = std::max (
                    200, static_cast<int> (std::lround (rate * 0.03)));
                advanceSamples (dsp, manualTransitionSamples, channels, sample);
                sample += manualTransitionSamples;
                expect (dsp.currentBlend.presets[0] == 4
                            && dsp.currentBlend.nonlinearSources[0],
                        "manual sequence target calls reach the requested destination");
            }
        }
    }
}

void checkSequenceRouting()
{
    const auto checkDirection = [] (bool sourceNonlinear, bool destinationNonlinear,
                                    const std::string& label)
    {
        MorphosisDSP dsp;
        dsp.prepare (48000.0, 64);
        auto parameters = sequenceParameters (3, sourceNonlinear);
        dsp.beginBlock (parameters);
        advanceSamples (dsp, 1200);
        expect (closeEnough (dsp.nonlinearMix.current, sourceNonlinear ? 1.0 : 0.0),
                label + ": source routing is settled");

        dsp.setSequenceTarget (4, destinationNonlinear, 20);
        bool sawFadeOut = false;
        bool sawFadeIn = false;
        for (int sample = 0; sample < 100; ++sample)
        {
            dsp.advanceSample();
            if (dsp.transitionPhase == MorphosisDSP::TransitionPhase::fadeOut)
            {
                sawFadeOut = true;
                expect (closeEnough (dsp.nonlinearMix.current,
                                     sourceNonlinear ? 1.0 : 0.0),
                        label + ": outgoing routing remains during fade-out");
            }
            if (! sawFadeIn && dsp.transitionPhase == MorphosisDSP::TransitionPhase::fadeIn)
            {
                sawFadeIn = true;
                expect (dsp.currentBlend.presets[0] == 4
                            && dsp.currentBlend.nonlinearSources[0] == destinationNonlinear,
                        label + ": destination blend is committed before fade-in");
                expect (closeEnough (dsp.nonlinearMix.current,
                                     destinationNonlinear ? 1.0 : 0.0),
                        label + ": destination routing starts at the dry handover");
            }
        }
        expect (sawFadeOut && sawFadeIn, label + ": transition has both bridge phases");
    };

    checkDirection (true, false, "nonlinear-to-linear sequence");
    checkDirection (false, true, "linear-to-nonlinear sequence");
}

void checkOrdinaryPresetRouting()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    ParameterSnapshot source;
    source.mode = ProcessingMode::preset;
    source.preset = 3;
    source.internalDistortion = true;
    source.softClip = false;
    dsp.beginBlock (source);
    advanceSamples (dsp, 1200);
    expect (closeEnough (dsp.nonlinearMix.current, 1.0),
            "ordinary preset source routing is initially active");

    auto destination = source;
    destination.preset = 4;
    destination.internalDistortion = false;
    dsp.beginBlock (destination);
    bool sawFadeOut = false;
    bool sawFadeIn = false;
    for (int sample = 0; sample < 2200; ++sample)
    {
        dsp.advanceSample();
        if (dsp.transitionPhase == MorphosisDSP::TransitionPhase::fadeOut)
        {
            sawFadeOut = true;
            expect (closeEnough (dsp.nonlinearMix.current, 1.0),
                    "ordinary preset keeps committed routing during fade-out");
        }
        if (! sawFadeIn && dsp.transitionPhase == MorphosisDSP::TransitionPhase::fadeIn)
        {
            sawFadeIn = true;
            expect (dsp.currentBlend.presets[0] == destination.preset
                        && ! dsp.currentBlend.nonlinearSources[0],
                    "ordinary preset commits destination source before fade-in");
            expect (dsp.nonlinearMix.target <= 1.0e-12,
                    "ordinary preset adopts destination routing at handover");
        }
    }
    expect (sawFadeOut && sawFadeIn, "ordinary preset transition has both bridge phases");
}

void checkResponseRouting()
{
    auto response = responseParameters();
    const auto expectedResponseMix = MorphosisDSP::nonlinearMixForBlend (
        MorphosisDSP::makeBilinearBlend (response.xyPresets,
                                          response.xyNonlinearSources,
                                          response.xyX, response.xyY));
    expect (closeEnough (expectedResponseMix, 0.4),
            "Response fixture has the intended weighted nonlinear source");

    MorphosisDSP startup;
    startup.prepare (48000.0, 64);
    startup.beginBlock (response);
    expect (closeEnough (startup.nonlinearMix.current, 0.0)
                && closeEnough (startup.nonlinearMix.target, 0.0),
            "Response startup suppresses nonlinear IIR routing before FIR readiness");
    for (int sample = 0; sample < 128; ++sample)
    {
        startup.advanceSample();
        expect (closeEnough (startup.nonlinearMix.current, 0.0),
                "Response startup keeps nonlinear IIR routing at zero");
    }
    expect (closeEnough (startup.nonlinearThreshold.target,
                         MorphosisDSP::settledThresholdForSetting (18)),
            "Response suppression preserves the DIST threshold setting");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (! startup.firReady && std::chrono::steady_clock::now() < deadline)
    {
        startup.advanceSample();
        (void) startup.processSampleNoAdvance (0.001f, 0);
        if ((startup.firRequestGeneration & 63u) == 0u)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    expect (startup.firReady, "Response fixture reaches FIR activation");
    for (int sample = 0; sample < 128; ++sample)
    {
        startup.advanceSample();
        expect (closeEnough (startup.nonlinearMix.current, 0.0),
                "Response FIR activation keeps nonlinear IIR routing at zero");
    }

    {
        MorphosisDSP dsp;
        primeResponseState (dsp);
        auto descriptor = response;
        descriptor.xyInterpolation = XYInterpolationMode::descriptor;
        dsp.beginBlock (descriptor);
        for (int sample = 0; sample < 1200 && dsp.nonlinearRoutingSuppressed; ++sample)
            dsp.advanceSample();
        expect (! dsp.nonlinearRoutingSuppressed && dsp.nonlinearMix.remaining > 1,
                "leaving Response starts a nonlinear routing ramp");
        dsp.advanceSample();
        expect (dsp.nonlinearMix.current > 0.0
                    && dsp.nonlinearMix.current < expectedResponseMix * 0.1,
                "descriptor XY preserves the Response-exit routing ramp");
        for (int sample = 0; sample < 1200; ++sample)
            dsp.advanceSample();
        expect (closeEnough (dsp.nonlinearMix.current, expectedResponseMix),
                "leaving Response for descriptor XY restores weighted source routing");
    }

    {
        MorphosisDSP dsp;
        primeResponseState (dsp);
        auto preset = response;
        preset.mode = ProcessingMode::preset;
        preset.preset = 7;
        preset.internalDistortion = true;
        preset.xyInterpolation = XYInterpolationMode::descriptor;
        dsp.beginBlock (preset);
        for (int sample = 0; sample < 2200; ++sample)
            dsp.advanceSample();
        expect (dsp.currentBlend.presets[0] == 7
                    && closeEnough (dsp.nonlinearMix.current, 1.0),
                "leaving Response for nonlinear preset restores routing");
    }

    {
        MorphosisDSP dsp;
        primeResponseState (dsp);
        auto sequence = sequenceParameters (7, true);
        dsp.beginBlock (sequence);
        dsp.setSequenceTarget (7, true, 200);
        for (int sample = 0; sample < 1600; ++sample)
            dsp.advanceSample();
        expect (dsp.currentBlend.presets[0] == 7
                    && dsp.currentBlend.nonlinearSources[0]
                    && closeEnough (dsp.nonlinearMix.current, 1.0),
                "leaving Response for nonlinear sequence restores routing");
    }
}

} // namespace

int main()
{
    checkSequenceCoordinateContinuity();
    checkInvalidHandoverRetention();
    checkRetargetsAndLifecycleMatrix();
    checkSequenceRouting();
    checkOrdinaryPresetRouting();
    checkResponseRouting();

    if (failures != 0)
    {
        std::cerr << failures << " Stage 2 regression assertion(s) failed.\n";
        return EXIT_FAILURE;
    }

    std::cout << "Stage 2 DSP regression checks passed.\n";
    return EXIT_SUCCESS;
}
