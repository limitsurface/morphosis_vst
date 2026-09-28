#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

// White-box access is confined to this translation unit. The production class
// is compiled with the same fields and types as the plugin target.
#define private public
#include "MorphosisDSP.cpp"
#undef private

using morphosis::MorphosisDSP;
using Clock = std::chrono::steady_clock;

namespace
{

int failures = 0;
int printedFailures = 0;

void expect (bool condition, const std::string& message)
{
    if (condition)
        return;
    ++failures;
    if (printedFailures++ < 24)
        std::cerr << "FAIL: " << message << '\n';
}

using Impulse = std::array<float, morphosis::kExperimentalFirTaps>;
using Partitions = std::array<std::complex<float>,
                              morphosis::kExperimentalFirPartitionSpectrumCount>;

struct Kernel
{
    Impulse impulse {};
    std::array<std::pair<int, float>, 8> nonzero {};
    int count = 0;
};

Kernel makeKernel (std::initializer_list<std::pair<int, float>> taps)
{
    Kernel result;
    for (const auto& [index, value] : taps)
    {
        result.impulse[static_cast<std::size_t> (index)] = value;
        result.nonzero[static_cast<std::size_t> (result.count++)] = { index, value };
    }
    return result;
}

Partitions makePartitions (const Impulse& impulse)
{
    Partitions result {};
    std::array<std::complex<float>, morphosis::kExperimentalFirRuntimeFftSize> work {};
    for (int partition = 0; partition < morphosis::kExperimentalFirPartitionCount; ++partition)
    {
        work.fill ({ 0.0f, 0.0f });
        for (int index = 0; index < morphosis::kExperimentalFirPartitionSize; ++index)
        {
            const auto tap = partition * morphosis::kExperimentalFirPartitionSize + index;
            work[static_cast<std::size_t> (index)] = impulse[static_cast<std::size_t> (tap)];
        }
        morphosis::fftFixed (work, false);
        const auto offset = partition * morphosis::kExperimentalFirRuntimeFftSize;
        for (int bin = 0; bin < morphosis::kExperimentalFirRuntimeFftSize; ++bin)
            result[static_cast<std::size_t> (offset + bin)] = work[static_cast<std::size_t> (bin)];
    }
    return result;
}

MorphosisDSP::FirBuildResult makeResult (std::uint64_t generation, const Kernel& kernel)
{
    MorphosisDSP::FirBuildResult result;
    result.generation = generation;
    result.valid = true;
    result.impulse = kernel.impulse;
    result.partitions = makePartitions (kernel.impulse);
    return result;
}

const Kernel& kernelA()
{
    static const auto value = makeKernel ({ { 0, 0.35f }, { 63, 0.15f }, { 64, 0.25f },
                                             { 511, -0.05f }, { 2047, 0.30f } });
    return value;
}

const Kernel& kernelB()
{
    static const auto value = makeKernel ({ { 0, 0.6f }, { 63, 0.4f } });
    return value;
}

const Kernel& kernelC()
{
    static const auto value = makeKernel ({ { 0, 0.2f }, { 63, 0.2f }, { 64, 0.3f },
                                             { 1024, -0.1f }, { 2047, 0.4f } });
    return value;
}

void prepareDsp (MorphosisDSP& dsp, double sampleRate, int blockSize,
                 const Kernel& initialKernel)
{
    dsp.prepare (sampleRate, blockSize);
    dsp.experimentalFirWorker->stop();
    dsp.responseModeRequested = true;
    dsp.firReady = true;
    dsp.firPartitions = makePartitions (initialKernel.impulse);
    dsp.firTargetPartitions = dsp.firPartitions;
    dsp.firImpulse = initialKernel.impulse;
    dsp.firRequestCountdown = 1000000;
}

void stageResult (MorphosisDSP& dsp, std::uint64_t generation, const Kernel& kernel)
{
    dsp.firRequestGeneration = generation;
    dsp.firOutstandingGeneration = generation;
    dsp.firBuildOutstanding = true;
    expect (dsp.experimentalFirWorker->storeResult (makeResult (generation, kernel)),
            "synthetic FIR result enters the worker mailbox");
}

float signalAt (int channel, std::size_t sample)
{
    auto value = static_cast<std::uint32_t> (sample + 1)
               * (channel == 0 ? 747796405u : 2891336453u);
    value += channel == 0 ? 2891336453u : 1181783497u;
    value ^= value >> 16;
    value *= 2246822519u;
    value ^= value >> 13;
    value *= 3266489917u;
    value ^= value >> 16;
    const auto signedUnit = static_cast<double> (static_cast<std::int32_t> (value))
                          / 2147483648.0;
    return static_cast<float> (signedUnit * 0.12);
}

double directConvolution (const std::vector<float>& input,
                          const Kernel& kernel,
                          std::int64_t outputIndex)
{
    if (outputIndex < 0)
        return 0.0;
    double result = 0.0;
    for (int tap = 0; tap < kernel.count; ++tap)
    {
        const auto [offset, coefficient] = kernel.nonzero[static_cast<std::size_t> (tap)];
        const auto inputIndex = outputIndex - offset;
        if (inputIndex >= 0 && static_cast<std::size_t> (inputIndex) < input.size())
            result += static_cast<double> (coefficient)
                    * static_cast<double> (input[static_cast<std::size_t> (inputIndex)]);
    }
    return result;
}

void warmChannel (MorphosisDSP& dsp, int channel, int samples,
                  std::vector<float>& history, int hostBlockSize)
{
    int done = 0;
    while (done < samples)
    {
        const auto blockRemaining = hostBlockSize - (done % hostBlockSize);
        const auto count = std::min ({ blockRemaining, samples - done, 512 });
        for (int index = 0; index < count; ++index)
        {
            dsp.advanceSample();
            const auto input = signalAt (channel, history.size());
            (void) dsp.processExperimentalFir (input, channel);
            history.push_back (input);
            ++done;
        }
    }
}

void warmStereo (MorphosisDSP& dsp,
                 int samples,
                 std::array<std::vector<float>, morphosis::kChannelCount>& history,
                 int hostBlockSize)
{
    int done = 0;
    while (done < samples)
    {
        const auto blockRemaining = hostBlockSize - (done % hostBlockSize);
        const auto count = std::min ({ blockRemaining, samples - done, 512 });
        for (int index = 0; index < count; ++index)
        {
            dsp.advanceSample();
            const auto left = signalAt (0, history[0].size());
            const auto right = signalAt (1, history[1].size());
            (void) dsp.processExperimentalFir (left, 0);
            (void) dsp.processExperimentalFir (right, 1);
            history[0].push_back (left);
            history[1].push_back (right);
            ++done;
        }
    }
}

void runCheckedFrame (MorphosisDSP& dsp,
                      std::array<std::vector<float>, morphosis::kChannelCount>& history,
                      std::uint8_t channelMask,
                      bool reverseOrder,
                      const Kernel& initial,
                      const Kernel& destination,
                      const std::string& label)
{
    dsp.advanceSample();

    const auto accepted = dsp.firAcceptedGeneration;
    const auto fading = dsp.firTransitionActive;
    const auto& current = accepted == 0 || (accepted == 1 && fading)
                            ? initial : destination;
    const auto& target = accepted == 1 && fading ? destination : current;
    const auto mix = fading ? std::clamp (dsp.firTransitionMix, 0.0, 1.0) : 1.0;

    for (int order = 0; order < morphosis::kChannelCount; ++order)
    {
        const auto channel = reverseOrder ? morphosis::kChannelCount - 1 - order : order;
        if ((channelMask & static_cast<std::uint8_t> (1u << channel)) == 0)
            continue;

        const auto input = signalAt (channel, history[static_cast<std::size_t> (channel)].size());
        const auto outputIndex = static_cast<std::int64_t> (
            history[static_cast<std::size_t> (channel)].size()) - morphosis::kExperimentalFirPartitionSize;
        const auto currentOutput = directConvolution (
            history[static_cast<std::size_t> (channel)], current, outputIndex);
        const auto targetOutput = directConvolution (
            history[static_cast<std::size_t> (channel)], target, outputIndex);
        const auto expected = currentOutput + (targetOutput - currentOutput) * mix;
        const auto actual = dsp.processExperimentalFir (input, channel);
        expect (std::isfinite (actual), label + ": FIR output remains finite");
        expect (std::abs (actual - expected) < 7.5e-5,
                label + ": output matches direct convolution and transition envelope (ch "
                    + std::to_string (channel) + ", sample "
                    + std::to_string (history[static_cast<std::size_t> (channel)].size())
                    + ", expected " + std::to_string (expected) + ", got "
                    + std::to_string (actual) + ")");
        history[static_cast<std::size_t> (channel)].push_back (input);
    }
}

void checkUnityDcOverlap()
{
    MorphosisDSP dsp;
    dsp.prepare (48000.0, 64);
    dsp.experimentalFirWorker->stop();
    dsp.controlSmoothingSamples = 100000;

    const auto identity = makeKernel ({ { 0, 1.0f } });
    dsp.firPartitions = makePartitions (identity.impulse);
    dsp.firTargetPartitions = dsp.firPartitions;
    dsp.firImpulse = identity.impulse;
    dsp.firReady = true;

    for (int sample = 0; sample < 256; ++sample)
        (void) dsp.processExperimentalFir (1.0, 0);

    const auto unityDcTarget = makeKernel ({ { 0, 2.0f / 3.0f }, { 63, 1.0f / 3.0f } });
    dsp.responseModeRequested = true;
    stageResult (dsp, 1, unityDcTarget);
    dsp.consumeExperimentalFirResult();

    for (int sample = 0; sample < morphosis::kExperimentalFirPartitionSize; ++sample)
        (void) dsp.processExperimentalFir (1.0, 0);

    const auto observedTargetDc = dsp.firChannels[0].targetOutputBlock[0];
    expect (std::abs (observedTargetDc - 1.0f) < 1.0e-5f,
            "unity-DC target overlap is rebuilt from its own kernel and input history; got "
                + std::to_string (observedTargetDc));
}

void checkAllAdoptionPhases()
{
    constexpr std::array<double, 3> rates { 44100.0, 48000.0, 96000.0 };
    constexpr std::array<int, 3> blockSizes { 1, 64, 512 };
    constexpr auto partition = morphosis::kExperimentalFirPartitionSize;
    const auto& initial = kernelA();
    const auto& destination = kernelB();

    for (const auto rate : rates)
    {
        MorphosisDSP dsp;
        dsp.prepare (rate, 64);
        dsp.experimentalFirWorker->stop();
        for (int phase = 0; phase < partition; ++phase)
        {
            dsp.reset();
            dsp.responseModeRequested = true;
            dsp.firReady = true;
            dsp.firPartitions = makePartitions (initial.impulse);
            dsp.firTargetPartitions = dsp.firPartitions;
            dsp.firImpulse = initial.impulse;
            dsp.firRequestCountdown = 1000000;
            const auto blockSize = blockSizes[static_cast<std::size_t> ((phase / 3) % 3)];
            std::array<std::vector<float>, morphosis::kChannelCount> history;
            const auto leftWarm = morphosis::kExperimentalFirTaps + phase;
            const auto rightWarm = morphosis::kExperimentalFirTaps + ((phase + 19) % partition);

            if (phase % 3 == 0)
            {
                warmChannel (dsp, 0, leftWarm, history[0], blockSize);
            }
            else if (phase % 3 == 1)
            {
                warmStereo (dsp, leftWarm, history, blockSize);
            }
            else
            {
                warmChannel (dsp, 0, leftWarm, history[0], blockSize);
                warmChannel (dsp, 1, rightWarm, history[1], blockSize);
            }

            stageResult (dsp, 1, destination);
            const auto postFrames = dsp.controlSmoothingSamples + 8;
            for (int frame = 0; frame < postFrames; ++frame)
            {
                std::uint8_t channelMask = 1;
                if (phase % 3 == 1)
                    channelMask = 3;
                else if (phase % 3 == 2)
                    channelMask = static_cast<std::uint8_t> (frame % 4 == 0 ? 2 : frame % 4 == 1 ? 1 : 3);
                runCheckedFrame (dsp, history, channelMask,
                                 (phase & 1) != 0, initial, destination,
                                 "rate " + std::to_string (static_cast<int> (rate))
                                     + " phase " + std::to_string (phase)
                                     + " block " + std::to_string (blockSize));
            }

            expect (dsp.firAcceptedGeneration == 1,
                    "phase matrix accepts exactly the requested target");
            expect (! dsp.firTransitionActive,
                    "phase matrix reaches exact target settlement");
            expect (dsp.firPartitions == makePartitions (destination.impulse),
                    "phase matrix promotes the exact target kernel");
        }
    }
}

void checkImpulseTransition()
{
    MorphosisDSP dsp;
    const auto initial = makeKernel ({ { 0, 1.0f } });
    prepareDsp (dsp, 48000.0, 1, initial);
    std::vector<float> history;
    const auto& destination = kernelB();
    bool submitted = false;

    for (int frame = 0; frame < 256; ++frame)
    {
        if (! submitted && frame == 23)
        {
            stageResult (dsp, 1, destination);
            submitted = true;
        }

        dsp.advanceSample();
        const auto accepted = dsp.firAcceptedGeneration;
        const auto fading = dsp.firTransitionActive;
        const auto& current = accepted == 0 || (accepted == 1 && fading)
                                ? initial : destination;
        const auto& target = accepted == 1 && fading ? destination : current;
        const auto mix = fading ? std::clamp (dsp.firTransitionMix, 0.0, 1.0) : 1.0;
        const auto input = frame == 0 ? 1.0f : frame == 9 ? -0.5f : 0.0f;
        const auto outputIndex = static_cast<std::int64_t> (history.size())
                               - morphosis::kExperimentalFirPartitionSize;
        const auto a = directConvolution (history, current, outputIndex);
        const auto b = directConvolution (history, target, outputIndex);
        const auto expected = a + (b - a) * mix;
        const auto actual = dsp.processExperimentalFir (input, 0);
        expect (std::isfinite (actual), "impulse response remains finite across adoption");
        expect (std::abs (actual - expected) < 5.0e-5,
                "impulse response matches direct current/target convolution and fade");
        history.push_back (input);
    }
    expect (submitted && dsp.firAcceptedGeneration == 1,
            "impulse fixture adopts its target kernel");
}

void checkQueuedRetarget()
{
    MorphosisDSP dsp;
    prepareDsp (dsp, 48000.0, 1, kernelA());
    std::array<std::vector<float>, morphosis::kChannelCount> history;
    for (int sample = 0; sample < morphosis::kExperimentalFirTaps + 17; ++sample)
    {
        dsp.advanceSample();
        const auto left = signalAt (0, history[0].size());
        const auto right = signalAt (1, history[1].size());
        (void) dsp.processExperimentalFir (left, 0);
        (void) dsp.processExperimentalFir (right, 1);
        history[0].push_back (left);
        history[1].push_back (right);
    }

    stageResult (dsp, 1, kernelB());
    bool queuedC = false;
    int ageInFirstFade = 0;
    bool sawSecondAdoption = false;
    const auto maxFrames = dsp.controlSmoothingSamples * 2 + 24;
    for (int frame = 0; frame < maxFrames; ++frame)
    {
        dsp.advanceSample();
        if (dsp.firAcceptedGeneration == 1 && dsp.firTransitionActive)
            ++ageInFirstFade;

        if (! queuedC && ageInFirstFade == 12)
        {
            stageResult (dsp, 2, kernelC());
            queuedC = true;
        }

        const auto accepted = dsp.firAcceptedGeneration;
        const auto fading = dsp.firTransitionActive;
        const Kernel* current = &kernelA();
        const Kernel* target = &kernelA();
        if (accepted == 1)
        {
            current = fading ? &kernelA() : &kernelB();
            target = fading ? &kernelB() : current;
        }
        else if (accepted >= 2)
        {
            current = fading ? &kernelB() : &kernelC();
            target = fading ? &kernelC() : current;
            if (! sawSecondAdoption)
            {
                sawSecondAdoption = true;
                expect (dsp.firPartitions == makePartitions (kernelB().impulse),
                        "queued retarget starts from the settled first target");
                expect (dsp.firTargetPartitions == makePartitions (kernelC().impulse),
                        "queued retarget installs the latest result as its new target");
                for (int channel : { 0, 1 })
                {
                    const auto& state = dsp.firChannels[static_cast<std::size_t> (channel)];
                    const auto outputIndex = static_cast<std::int64_t> (
                        history[static_cast<std::size_t> (channel)].size())
                        - morphosis::kExperimentalFirPartitionSize;
                    const auto read = static_cast<std::size_t> (state.outputRead);
                    const auto expectedCurrent = directConvolution (
                        history[static_cast<std::size_t> (channel)], kernelB(), outputIndex);
                    const auto expectedTarget = directConvolution (
                        history[static_cast<std::size_t> (channel)], kernelC(), outputIndex);
                    expect (std::abs (state.outputBlock[read] - expectedCurrent) < 7.5e-5,
                            "queued retarget preserves current-kernel sample alignment");
                    expect (std::abs (state.targetOutputBlock[read] - expectedTarget) < 7.5e-5,
                            "queued retarget primes target output from channel history (ch "
                                + std::to_string (channel) + ", out index "
                                + std::to_string (outputIndex) + ", read "
                                + std::to_string (read) + ", expected "
                                + std::to_string (expectedTarget) + ", got "
                                + std::to_string (state.targetOutputBlock[read]) + ")");
                }
            }
        }

        const auto mix = fading ? std::clamp (dsp.firTransitionMix, 0.0, 1.0) : 1.0;
        for (int channel : { 1, 0 })
        {
            const auto input = signalAt (channel, history[static_cast<std::size_t> (channel)].size());
            const auto outIndex = static_cast<std::int64_t> (
                history[static_cast<std::size_t> (channel)].size())
                - morphosis::kExperimentalFirPartitionSize;
            const auto a = directConvolution (history[static_cast<std::size_t> (channel)],
                                              *current, outIndex);
            const auto b = directConvolution (history[static_cast<std::size_t> (channel)],
                                              *target, outIndex);
            const auto expected = a + (b - a) * mix;
            const auto actual = dsp.processExperimentalFir (input, channel);
            expect (std::isfinite (actual), "queued retarget output remains finite");
            expect (std::abs (actual - expected) < 7.5e-5,
                    "queued retarget output matches direct envelope at frame "
                        + std::to_string (frame) + ", channel " + std::to_string (channel)
                        + ", accepted " + std::to_string (accepted) + ", mix "
                        + std::to_string (mix) + ", expected " + std::to_string (expected)
                        + ", got " + std::to_string (actual));
            history[static_cast<std::size_t> (channel)].push_back (input);
        }

        if (queuedC && dsp.firAcceptedGeneration == 1 && dsp.firTransitionActive)
        {
            expect (dsp.firTargetPartitions == makePartitions (kernelB().impulse),
                    "new result cannot replace spectra during the first kernel fade");
            expect (dsp.firBuildOutstanding,
                    "latest result remains queued until the current fade completes");
        }
        if (sawSecondAdoption && accepted >= 2 && ! dsp.firTransitionActive)
            break;
    }

    expect (queuedC, "a newer result was submitted during the first fade");
    expect (sawSecondAdoption, "latest queued result is adopted after the first fade");
    expect (dsp.firAcceptedGeneration == 2 && ! dsp.firTransitionActive,
            "queued retarget completes and settles exactly on the newest result");
    expect (dsp.firPartitions == makePartitions (kernelC().impulse),
            "newest target kernel is active after the second fade");
}

void checkAdoptionTiming()
{
    constexpr int trials = 64;
    MorphosisDSP dsp;
    prepareDsp (dsp, 48000.0, 1, kernelA());
    for (int sample = 0; sample < morphosis::kExperimentalFirTaps + 64; ++sample)
    {
        dsp.advanceSample();
        (void) dsp.processExperimentalFir (signalAt (0, static_cast<std::size_t> (sample)), 0);
        (void) dsp.processExperimentalFir (signalAt (1, static_cast<std::size_t> (sample)), 1);
    }

    std::array<double, trials> timesUs {};
    for (int trial = 0; trial < trials; ++trial)
    {
        dsp.firTransitionActive = false;
        dsp.firTransitionMix = 1.0;
        dsp.firTargetPartitions = dsp.firPartitions;
        stageResult (dsp, static_cast<std::uint64_t> (trial + 1), kernelB());
        const auto start = Clock::now();
        dsp.advanceSample(); // A one-frame host callback with a ready result.
        const auto elapsed = std::chrono::duration<double, std::micro> (Clock::now() - start).count();
        timesUs[static_cast<std::size_t> (trial)] = elapsed;
    }
    std::sort (timesUs.begin(), timesUs.end());
    const auto sum = std::accumulate (timesUs.begin(), timesUs.end(), 0.0);
    std::cout << "Stereo ready-result adoption, 1-frame boundary, " << trials
              << " trials: mean=" << (sum / trials)
              << " us, p50=" << timesUs[trials / 2]
              << " us, p95=" << timesUs[static_cast<std::size_t> (trials * 95 / 100)]
              << " us, max=" << timesUs.back() << " us.\n";
}

void checkExitDuringQueuedRetarget()
{
    MorphosisDSP dsp;
    prepareDsp (dsp, 48000.0, 1, kernelA());
    stageResult (dsp, 1, kernelB());
    dsp.advanceSample();
    expect (dsp.firTransitionActive, "first kernel fade begins before Response exit");
    stageResult (dsp, 2, kernelC());

    dsp.responseModeRequested = false;
    dsp.experimentalAlgorithmMix.current = 0.0;
    dsp.experimentalAlgorithmMix.target = 0.0;
    dsp.experimentalAlgorithmMix.remaining = 0;
    for (int sample = 0; sample < dsp.controlSmoothingSamples + 3; ++sample)
        dsp.advanceSample();

    expect (! dsp.firTransitionActive,
            "inaudible completed kernel fade settles without FIR channel processing");
    expect (! dsp.firBuildOutstanding,
            "queued result is released after Response exits during kernel fade");
}

} // namespace

int main()
{
    checkUnityDcOverlap();
    checkImpulseTransition();
    checkAllAdoptionPhases();
    checkQueuedRetarget();
    checkExitDuringQueuedRetarget();
    checkAdoptionTiming();

    if (failures != 0)
    {
        std::cerr << failures << " Stage 3 assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Stage 3 FIR overlap and transition checks passed.\n";
    return EXIT_SUCCESS;
}
