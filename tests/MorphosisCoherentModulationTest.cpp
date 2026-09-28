#include "MorphosisDSP.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

using morphosis::CoefficientSet;
using morphosis::MorphosisDSP;
using morphosis::ParameterSnapshot;

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kControlSmoothingMs = morphosis::kControlSmoothingMilliseconds;
constexpr double kGainSmoothingMs = morphosis::kSmoothingMilliseconds;

struct Controls
{
    double frequency = 0.0;
    double morph = 0.0;
    double transform = 0.0;
};

struct Frame
{
    double left = 0.0;
    double right = 0.0;
};

struct Stats
{
    std::uint64_t samples = 0;
    double peak = 0.0;
    double sumSquares = 0.0;

    void add (double left, double right) noexcept
    {
        if (! std::isfinite (left) || ! std::isfinite (right))
            return;
        peak = std::max (peak, std::max (std::abs (left), std::abs (right)));
        sumSquares += left * left + right * right;
        ++samples;
    }

    double rms() const noexcept
    {
        return samples == 0 ? 0.0
                            : std::sqrt (sumSquares / (2.0 * static_cast<double> (samples)));
    }
};

void require (bool condition, const std::string& message)
{
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit (1);
    }
}

Controls lerp (const Controls& from, const Controls& to, double amount) noexcept
{
    return {
        from.frequency + (to.frequency - from.frequency) * amount,
        from.morph + (to.morph - from.morph) * amount,
        from.transform + (to.transform - from.transform) * amount
    };
}

ParameterSnapshot parameters (int preset, const Controls& controls,
                              bool softClip = false, bool internalDistortion = false)
{
    ParameterSnapshot result;
    result.preset = preset;
    result.frequency = controls.frequency;
    result.morph = controls.morph;
    result.transform = controls.transform;
    result.inputGainDb = 0.0;
    result.preClipGainDb = 0.0;
    result.postClipGainDb = 0.0;
    result.softClip = softClip;
    result.internalDistortion = internalDistortion;
    return result;
}

std::uint32_t hashValue (std::int64_t sample, int channel) noexcept
{
    auto value = static_cast<std::uint32_t> (sample);
    value ^= 0x9e3779b9u + static_cast<std::uint32_t> (channel) * 0x85ebca6bu;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

Frame deterministicInput (std::int64_t sample, double rate) noexcept
{
    const auto makeSine = [sample, rate] (int channel) {
        const auto phase = 2.0 * kPi * (997.0 + 173.0 * channel)
                         * static_cast<double> (sample) / rate;
        return 0.42 * std::sin (phase + 0.11 * channel);
    };
    const auto leftNoise = static_cast<double> (hashValue (sample, 0)) / 2147483647.5 - 1.0;
    const auto rightNoise = static_cast<double> (hashValue (sample, 1)) / 2147483647.5 - 1.0;
    return {
        makeSine (0) + 0.08 * leftNoise,
        0.37 * makeSine (1) + 0.06 * rightNoise
    };
}

struct ReferenceRecurrence
{
    double process (double input, int channel, const CoefficientSet& coefficients) noexcept
    {
        auto& states = state.values[static_cast<std::size_t> (
            std::clamp (channel, 0, morphosis::kChannelCount - 1))];
        double x = input;
        for (int index = 0; index < morphosis::kStageCount; ++index)
        {
            auto& stageState = states[static_cast<std::size_t> (index)];
            const auto& coefficient = coefficients.stages[static_cast<std::size_t> (index)];
            const auto gain = coefficient.normalizeDc
                            ? 1.0 + coefficient.radius * coefficient.radius
                                - 2.0 * coefficient.a
                            : coefficient.inputGain;
            const auto u = gain * x + coefficient.a * stageState[0]
                         - coefficient.b * stageState[1];
            const auto v = coefficient.b * stageState[0] + coefficient.a * stageState[1];
            const auto w = u + coefficient.a / coefficient.b * v;
            x = w + coefficient.z1 * stageState[2] + coefficient.z2 * stageState[3];
            stageState[0] = u;
            stageState[1] = v;
            stageState[3] = stageState[2];
            stageState[2] = w;
        }
        return x * coefficients.gain;
    }

    morphosis::FilterState state;
};

struct PhysicalOracle
{
    PhysicalOracle (int newPreset, double newRate, const Controls& initial)
        : preset (newPreset), rate (newRate), current (initial), target (initial),
          coefficient (MorphosisDSP::makeCoefficients (
              preset, current.frequency, current.morph, current.transform, rate))
    {
        amount = 1.0 / static_cast<double> (std::max (
            1, static_cast<int> (std::lround (
                rate * kControlSmoothingMs / 1000.0))));
    }

    void setTarget (const Controls& value) noexcept { target = value; }

    void advance() noexcept
    {
        current.frequency += (target.frequency - current.frequency) * amount;
        current.morph += (target.morph - current.morph) * amount;
        current.transform += (target.transform - current.transform) * amount;
        coefficient = MorphosisDSP::makeCoefficients (
            preset, current.frequency, current.morph, current.transform, rate);
    }

    int preset = 0;
    double rate = 48000.0;
    double amount = 1.0;
    Controls current;
    Controls target;
    CoefficientSet coefficient;
};

double coefficientDifference (const CoefficientSet& left,
                              const CoefficientSet& right) noexcept
{
    double difference = std::abs (left.gain - right.gain);
    for (std::size_t index = 0; index < left.stages.size(); ++index)
    {
        const auto& a = left.stages[index];
        const auto& b = right.stages[index];
        difference = std::max ({
            difference,
            std::abs (a.a - b.a),
            std::abs (a.b - b.b),
            std::abs (a.inputGain - b.inputGain),
            std::abs (a.z1 - b.z1),
            std::abs (a.z2 - b.z2),
            std::abs (a.radius - b.radius),
            std::abs (a.angle - b.angle),
            std::abs (a.zeroRadius - b.zeroRadius),
            std::abs (a.zeroAngle - b.zeroAngle)
        });
    }
    return difference;
}

enum class Envelope
{
    step,
    repeated
};

struct Scenario
{
    int preset = 184;
    double rate = 48000.0;
    int blockSize = 64;
    Controls from;
    Controls to;
    Envelope envelope = Envelope::step;
    const char* name = "";
};

Controls targetForBlock (const Scenario& scenario,
                         std::int64_t blockStart,
                         std::int64_t preRoll,
                         std::int64_t rampSamples)
{
    if (blockStart < preRoll)
        return scenario.from;

    if (scenario.envelope == Envelope::step)
        return scenario.to;

    const auto local = blockStart - preRoll;
    const auto segment = std::max<std::int64_t> (1, rampSamples);
    const auto cycle = local / segment;
    if (cycle >= 8)
        return scenario.from;

    const auto position = static_cast<double> (local % segment)
                        / static_cast<double> (segment);
    const auto amount = (cycle % 2 == 0) ? position : 1.0 - position;
    return lerp (scenario.from, scenario.to, amount);
}

void runOracleScenario (const Scenario& scenario)
{
    std::atomic<bool> safetyFault { false };
    MorphosisDSP processor;
    processor.setTelemetry (nullptr, nullptr, &safetyFault);
    processor.prepare (scenario.rate, scenario.blockSize);
    PhysicalOracle oracle (scenario.preset, scenario.rate, scenario.from);
    ReferenceRecurrence reference;

    const auto preRoll = std::max<std::int64_t> (
        1, static_cast<std::int64_t> (std::llround (scenario.rate * 0.250)));
    const auto ramp = std::max<std::int64_t> (
        1, static_cast<std::int64_t> (std::llround (scenario.rate * 0.020)));
    const auto envelopeSamples = scenario.envelope == Envelope::step ? ramp : ramp * 8;
    const auto hold = std::max<std::int64_t> (
        1, static_cast<std::int64_t> (std::llround (scenario.rate * 0.500)));
    const auto tail = std::max<std::int64_t> (
        1, static_cast<std::int64_t> (std::llround (scenario.rate * 0.100)));
    const auto total = preRoll + envelopeSamples + hold + tail;
    Stats output;
    Stats holdOutput;
    double maximumCoefficientDifference = 0.0;
    double maximumRelativeOutputDifference = 0.0;
    std::vector<Frame> referenceOutput (static_cast<std::size_t> (total));

    for (std::int64_t processed = 0; processed < total; processed += scenario.blockSize)
    {
        const auto target = targetForBlock (scenario, processed, preRoll, ramp);
        processor.beginBlock (parameters (scenario.preset, target));
        oracle.setTarget (target);
        const auto count = std::min<std::int64_t> (scenario.blockSize, total - processed);

        for (std::int64_t offset = 0; offset < count; ++offset)
        {
            const auto sample = processed + offset;
            processor.advanceSample();
            oracle.advance();
            const auto& actualCoefficients = processor.getCurrentCoefficients();
            maximumCoefficientDifference = std::max (
                maximumCoefficientDifference,
                coefficientDifference (actualCoefficients, oracle.coefficient));

            const auto input = deterministicInput (sample, scenario.rate);
            const auto inputLeft = static_cast<float> (input.left);
            const auto inputRight = static_cast<float> (input.right);
            const auto expectedLeft = reference.process (
                static_cast<double> (inputLeft), 0, oracle.coefficient);
            const auto expectedRight = reference.process (
                static_cast<double> (inputRight), 1, oracle.coefficient);
            referenceOutput[static_cast<std::size_t> (sample)] = { expectedLeft, expectedRight };
            const auto actualLeft = static_cast<double> (
                processor.processSampleNoAdvance (inputLeft, 0));
            const auto actualRight = static_cast<double> (
                processor.processSampleNoAdvance (inputRight, 1));

            require (std::isfinite (actualLeft) && std::isfinite (actualRight),
                     std::string ("finite coherent output: ") + scenario.name);
            constexpr auto latency = morphosis::kMorphosisLatencySamples;
            if (sample >= preRoll + latency)
            {
                const auto& delayedReference = referenceOutput[
                    static_cast<std::size_t> (sample - latency)];
                const auto relativeLeft = std::abs (actualLeft - delayedReference.left)
                    / std::max (1.0, std::abs (delayedReference.left));
                const auto relativeRight = std::abs (actualRight - delayedReference.right)
                    / std::max (1.0, std::abs (delayedReference.right));
                maximumRelativeOutputDifference = std::max (
                    maximumRelativeOutputDifference, std::max (relativeLeft, relativeRight));
                output.add (actualLeft, actualRight);
                if (sample >= preRoll + envelopeSamples + latency
                    && sample < preRoll + envelopeSamples + hold + latency)
                    holdOutput.add (actualLeft, actualRight);
            }
        }
    }

    require (! safetyFault.load (std::memory_order_relaxed),
             std::string ("no production safety flag: ") + scenario.name);
    require (maximumCoefficientDifference < 1.0e-12,
             std::string ("production coefficients match independent physical oracle: ")
                 + scenario.name);
    require (maximumRelativeOutputDifference < 2.0e-5,
             std::string ("production audio matches independent physical oracle: ")
                 + scenario.name);
    require (holdOutput.peak < 10.0,
             std::string ("coherent endpoint hold remains bounded: ") + scenario.name);
    require (output.peak < 10.0,
             std::string ("coherent modulation remains bounded: ") + scenario.name);
}

void checkCoherentOracleMatrix()
{
    const auto axes = std::array<Scenario, 4> {
        Scenario { 184, 48000.0, 64, { -5.0, -1.87, 2.37 },
                   { 5.0, -1.87, 2.37 }, Envelope::step, "F step" },
        Scenario { 184, 48000.0, 64, { -1.0, -5.0, 0.0 },
                   { -1.0, 5.0, 0.0 }, Envelope::step, "M step" },
        Scenario { 184, 48000.0, 64, { -1.0, 0.0, -5.0 },
                   { -1.0, 0.0, 5.0 }, Envelope::step, "T step" },
        Scenario { 184, 48000.0, 64, { -4.0, -3.0, -2.0 },
                   { 4.0, 3.0, 2.0 }, Envelope::step, "F/M/T step" }
    };

    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto blockSize : { 1, 64, 512 })
            for (const auto axis : axes)
                for (const auto envelope : { Envelope::step, Envelope::repeated })
                {
                    auto scenario = axis;
                    scenario.rate = rate;
                    scenario.blockSize = blockSize;
                    scenario.envelope = envelope;
                    runOracleScenario (scenario);
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
            const Controls controls { 0.0, 0.0, 0.0 };
            processor.beginBlock (parameters (184, controls));
            const auto total = static_cast<std::int64_t> (std::llround (rate * 0.100));
            for (std::int64_t processed = 0; processed < total; processed += blockSize)
            {
                processor.beginBlock (parameters (184, controls));
                const auto count = std::min<std::int64_t> (blockSize, total - processed);
                for (std::int64_t sample = 0; sample < count; ++sample)
                {
                    processor.advanceSample();
                    const auto input = deterministicInput (processed + sample, rate);
                    static_cast<void> (processor.processSampleNoAdvance (
                        static_cast<float> (input.left), 0));
                    const auto right = processor.processSampleNoAdvance (0.0f, 1);
                    require (right == 0.0f, "silent stereo channel remains isolated");
                }
            }
            require (! safetyFault.load (std::memory_order_relaxed),
                     "stereo isolation remains safety-clean");
        }
}

void checkAllPresetFiniteSweep()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (int preset = 0; preset < morphosis::cube_data::kRecordCount; ++preset)
        {
            std::atomic<bool> safetyFault { false };
            MorphosisDSP processor;
            processor.setTelemetry (nullptr, nullptr, &safetyFault);
            processor.prepare (rate, 64);
            const Controls controls {
                static_cast<double> ((preset % 11) - 5),
                static_cast<double> ((preset % 7) - 3),
                static_cast<double> ((preset % 9) - 4)
            };
            processor.beginBlock (parameters (preset, controls));
            for (int sample = 0; sample < 64; ++sample)
            {
                processor.advanceSample();
                const auto input = deterministicInput (sample, rate);
                const auto left = processor.processSampleNoAdvance (
                    static_cast<float> (input.left), 0);
                const auto right = processor.processSampleNoAdvance (
                    static_cast<float> (input.right), 1);
                require (std::isfinite (left) && std::isfinite (right),
                         "all 289 presets remain finite at supported rates");
            }
            require (! safetyFault.load (std::memory_order_relaxed),
                     "all 289 preset sweep remains safety-clean");
        }
}

void checkNonlinearSelected()
{
    for (const auto preset : { 43, 162, 184, 197, 288 })
        for (const auto internal : { false, true })
            for (const auto softClip : { false, true })
            {
                std::atomic<bool> safetyFault { false };
                MorphosisDSP processor;
                processor.setTelemetry (nullptr, nullptr, &safetyFault);
                processor.prepare (48000.0, 64);
                const Controls controls { 1.0, -1.0, 2.0 };
                for (int block = 0; block < 40; ++block)
                {
                    processor.beginBlock (parameters (preset, controls, softClip, internal));
                    for (int sample = 0; sample < 64; ++sample)
                    {
                        processor.advanceSample();
                        const auto input = deterministicInput (block * 64 + sample, 48000.0);
                        const auto left = processor.processSampleNoAdvance (
                            static_cast<float> (input.left * 1.5), 0);
                        const auto right = processor.processSampleNoAdvance (
                            static_cast<float> (input.right * 1.5), 1);
                        require (std::isfinite (left) && std::isfinite (right),
                                 "selected nonlinear modes remain finite");
                    }
                }
                require (! safetyFault.load (std::memory_order_relaxed),
                         "selected nonlinear modes remain safety-clean");
            }
}

void checkPresetTransitions()
{
    for (const auto blockSize : { 1, 64, 512 })
    {
        std::atomic<bool> safetyFault { false };
        MorphosisDSP processor;
        processor.setTelemetry (nullptr, nullptr, &safetyFault);
        processor.prepare (48000.0, blockSize);
        const Controls first { 2.5, -1.5, 3.0 };
        const Controls second { -2.5, 1.5, -3.0 };
        for (int block = 0; block < 4; ++block)
        {
            processor.beginBlock (parameters (46, first));
            for (int sample = 0; sample < blockSize; ++sample)
            {
                processor.advanceSample();
                const auto input = deterministicInput (block * blockSize + sample, 48000.0);
                static_cast<void> (processor.processSampleNoAdvance (
                    static_cast<float> (input.left), 0));
                static_cast<void> (processor.processSampleNoAdvance (
                    static_cast<float> (input.right), 1));
            }
        }

        const auto transitionSamples = static_cast<int> (std::lround (
            48000.0 * (2.0 * kGainSmoothingMs / 1000.0 + 0.050)));
        int processed = 0;
        while (processed < transitionSamples)
        {
            processor.beginBlock (parameters (184, second));
            const auto count = std::min (blockSize, transitionSamples - processed);
            for (int sample = 0; sample < count; ++sample)
            {
                processor.advanceSample();
                const auto input = deterministicInput (1000 + processed + sample,
                                                        48000.0);
                const auto left = processor.processSampleNoAdvance (
                    static_cast<float> (input.left), 0);
                const auto right = processor.processSampleNoAdvance (
                    static_cast<float> (input.right), 1);
                require (std::isfinite (left) && std::isfinite (right),
                         "preset transition output remains finite");
            }
            processed += count;
        }
        require (processor.getFilterMix() > 0.99f,
                 "preset transition fade reaches the new preset");
        require (coefficientDifference (
                     processor.getCurrentCoefficients(),
                     MorphosisDSP::makeCoefficients (184, second.frequency,
                                                      second.morph, second.transform, 48000.0))
                 < 1.0e-12,
                 "preset swap initializes the physical control path at requested controls");
        require (! safetyFault.load (std::memory_order_relaxed),
                 "preset transition remains safety-clean");
    }
}

void checkStaticCoefficientReference()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (int preset = 0; preset < morphosis::cube_data::kRecordCount; ++preset)
        {
            MorphosisDSP processor;
            processor.prepare (rate, 1);
            const Controls controls { -1.25, 0.75, 2.5 };
            processor.beginBlock (parameters (preset, controls));
            processor.advanceSample();
            require (coefficientDifference (
                         processor.getCurrentCoefficients(),
                         MorphosisDSP::makeCoefficients (
                             preset, controls.frequency, controls.morph,
                             controls.transform, rate)) < 1.0e-12,
                     "static production coefficients retain exact graph/reference values");
        }
}

void runBenchmark()
{
    std::cout << "Release coherent-path benchmark (stereo, 2 seconds per case)\n";
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto continuouslyModulated : { false, true })
        {
            constexpr int blockSize = 512;
            const auto total = static_cast<std::int64_t> (std::llround (rate * 2.0));
            std::atomic<bool> safetyFault { false };
            MorphosisDSP processor;
            processor.setTelemetry (nullptr, nullptr, &safetyFault);
            processor.prepare (rate, blockSize);
            const Controls initial { -4.0, -3.0, -2.0 };
            const Controls endpoint { 4.0, 3.0, 2.0 };
            volatile double checksum = 0.0;
            auto start = std::chrono::steady_clock::now();
            for (std::int64_t processed = 0; processed < total; processed += blockSize)
            {
                const auto amount = continuouslyModulated
                                  ? 0.5 + 0.5 * std::sin (
                                        2.0 * kPi * static_cast<double> (processed) / rate)
                                  : 0.0;
                processor.beginBlock (parameters (
                    184, lerp (initial, endpoint, amount)));
                const auto count = std::min<std::int64_t> (blockSize, total - processed);
                for (std::int64_t sample = 0; sample < count; ++sample)
                {
                    processor.advanceSample();
                    const auto input = deterministicInput (processed + sample, rate);
                    checksum += processor.processSampleNoAdvance (
                        static_cast<float> (input.left), 0);
                    checksum += processor.processSampleNoAdvance (
                        static_cast<float> (input.right), 1);
                }
            }
            const auto elapsed = std::chrono::duration<double> (
                std::chrono::steady_clock::now() - start).count();
            const auto stereoSamples = static_cast<double> (total);
            std::cout << rate << " Hz "
                      << (continuouslyModulated ? "continuous" : "static")
                      << " seconds=" << elapsed
                      << " ns/stereo-sample=" << elapsed * 1.0e9 / stereoSamples
                      << " checksum=" << checksum
                      << " safety=" << (safetyFault.load() ? 1 : 0) << '\n';
        }

    std::cout << "Release four-source XY benchmark (stereo, 2 seconds per case)\n";
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto continuouslyModulated : { false, true })
        {
            constexpr int blockSize = 512;
            const auto total = static_cast<std::int64_t> (std::llround (rate * 2.0));
            std::atomic<bool> safetyFault { false };
            MorphosisDSP processor;
            processor.setTelemetry (nullptr, nullptr, &safetyFault);
            processor.prepare (rate, blockSize);

            ParameterSnapshot snapshot;
            snapshot.mode = morphosis::ProcessingMode::xy;
            snapshot.frequency = -2.0;
            snapshot.morph = 1.0;
            snapshot.transform = -1.0;
            snapshot.softClip = false;
            snapshot.internalDistortion = false;
            snapshot.xyPresets = { 0, 17, 184, 288 };
            snapshot.xyNonlinearSources = { false, false, false, false };
            snapshot.xyX = 0.5;
            snapshot.xyY = 0.5;
            volatile double checksum = 0.0;
            auto start = std::chrono::steady_clock::now();
            for (std::int64_t processed = 0; processed < total; processed += blockSize)
            {
                const auto phase = 2.0 * kPi * static_cast<double> (processed) / rate;
                snapshot.xyX = continuouslyModulated ? 0.5 + 0.5 * std::sin (phase) : 0.5;
                snapshot.xyY = continuouslyModulated ? 0.5 + 0.5 * std::cos (phase * 0.71) : 0.5;
                snapshot.frequency = continuouslyModulated ? 3.0 * std::sin (phase * 0.37) : -2.0;
                snapshot.morph = continuouslyModulated ? 4.0 * std::cos (phase * 0.23) : 1.0;
                snapshot.transform = continuouslyModulated ? 2.5 * std::sin (phase * 0.19) : -1.0;
                processor.beginBlock (snapshot);
                const auto count = std::min<std::int64_t> (blockSize, total - processed);
                for (std::int64_t sample = 0; sample < count; ++sample)
                {
                    processor.advanceSample();
                    const auto input = deterministicInput (processed + sample, rate);
                    checksum += processor.processSampleNoAdvance (
                        static_cast<float> (input.left), 0);
                    checksum += processor.processSampleNoAdvance (
                        static_cast<float> (input.right), 1);
                }
            }
            const auto elapsed = std::chrono::duration<double> (
                std::chrono::steady_clock::now() - start).count();
            const auto stereoSamples = static_cast<double> (total);
            std::cout << rate << " Hz four-source "
                      << (continuouslyModulated ? "continuous" : "static")
                      << " seconds=" << elapsed
                      << " ns/stereo-sample=" << elapsed * 1.0e9 / stereoSamples
                      << " checksum=" << checksum
                      << " safety=" << (safetyFault.load() ? 1 : 0) << '\n';
        }
}

} // namespace

int main (int argc, char** argv)
{
    if (argc > 1 && std::string (argv[1]) == "--benchmark")
    {
        runBenchmark();
        return 0;
    }

    checkStaticCoefficientReference();
    checkCoherentOracleMatrix();
    checkStereoIsolation();
    checkAllPresetFiniteSweep();
    checkNonlinearSelected();
    checkPresetTransitions();
    std::cout << "Morphosis coherent modulation test passed: physical-coordinate oracle, "
                 "post-ramp holds, stereo isolation, all rates/block sizes, 289-preset finite "
                 "sweep, nonlinear modes, and preset transitions.\n";
    return 0;
}
