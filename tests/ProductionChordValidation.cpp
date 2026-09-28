#include "MorphosisDSP.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

using morphosis::CoefficientSet;
using morphosis::MorphosisDSP;
using morphosis::ParameterSnapshot;

constexpr double kControlSmoothingMs = morphosis::kControlSmoothingMilliseconds;
constexpr double kPreRollMs = 250.0;
constexpr double kHoldMs = 500.0;
constexpr double kTailMs = 250.0;

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
    std::uint64_t frames = 0;
    std::uint64_t overHalf = 0;
    std::uint64_t overOne = 0;
    double peak = 0.0;
    double sumSquares = 0.0;
    bool finite = true;

    void add (double left, double right) noexcept
    {
        if (! std::isfinite (left) || ! std::isfinite (right))
        {
            finite = false;
            return;
        }
        const auto framePeak = std::max (std::abs (left), std::abs (right));
        peak = std::max (peak, framePeak);
        overHalf += framePeak > 0.5 ? 1u : 0u;
        overOne += framePeak > 1.0 ? 1u : 0u;
        sumSquares += left * left + right * right;
        ++frames;
    }

    double rms() const noexcept
    {
        return frames == 0 ? 0.0 : std::sqrt (
            sumSquares / (2.0 * static_cast<double> (frames)));
    }

    double overOnePercent() const noexcept
    {
        return frames == 0 ? 0.0 : 100.0 * static_cast<double> (overOne)
            / static_cast<double> (frames);
    }

    double overHalfPercent() const noexcept
    {
        return frames == 0 ? 0.0 : 100.0 * static_cast<double> (overHalf)
            / static_cast<double> (frames);
    }
};

std::uint16_t readLe16 (const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t> (bytes[offset])
         | static_cast<std::uint16_t> (
             static_cast<std::uint16_t> (bytes[offset + 1]) << 8);
}

std::uint32_t readLe32 (const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t> (bytes[offset])
         | (static_cast<std::uint32_t> (bytes[offset + 1]) << 8)
         | (static_cast<std::uint32_t> (bytes[offset + 2]) << 16)
         | (static_cast<std::uint32_t> (bytes[offset + 3]) << 24);
}

class WavSource
{
public:
    bool load (const std::filesystem::path& path, std::string& error)
    {
        std::ifstream file (path, std::ios::binary);
        if (! file)
        {
            error = "unable to open WAV";
            return false;
        }
        const std::vector<std::uint8_t> bytes (
            (std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 12
            || std::string (reinterpret_cast<const char*> (bytes.data()), 4) != "RIFF"
            || std::string (reinterpret_cast<const char*> (bytes.data() + 8), 4) != "WAVE")
        {
            error = "not a RIFF/WAVE file";
            return false;
        }

        std::size_t position = 12;
        std::size_t dataOffset = 0;
        std::size_t dataBytes = 0;
        std::uint16_t format = 0;
        std::uint16_t channels = 0;
        std::uint16_t bits = 0;
        std::uint16_t blockAlign = 0;
        std::uint32_t rate = 0;
        while (position + 8 <= bytes.size())
        {
            const std::string tag (reinterpret_cast<const char*> (
                bytes.data() + position), 4);
            const auto size = static_cast<std::size_t> (readLe32 (bytes, position + 4));
            const auto payload = position + 8;
            if (payload + size > bytes.size())
            {
                error = "truncated WAV chunk";
                return false;
            }
            if (tag == "fmt " && size >= 16)
            {
                format = readLe16 (bytes, payload);
                channels = readLe16 (bytes, payload + 2);
                rate = readLe32 (bytes, payload + 4);
                blockAlign = readLe16 (bytes, payload + 12);
                bits = readLe16 (bytes, payload + 14);
            }
            else if (tag == "data")
            {
                dataOffset = payload;
                dataBytes = size;
            }
            position = payload + size + (size & 1u);
        }

        if (format != 1 || (channels != 1 && channels != 2) || rate == 0
            || blockAlign == 0 || dataOffset == 0
            || (bits != 16 && bits != 24 && bits != 32))
        {
            error = "unsupported WAV; expected PCM mono/stereo 16/24/32-bit";
            return false;
        }

        const auto frames = dataBytes / blockAlign;
        left.resize (frames);
        right.resize (frames);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto offset = dataOffset + frame * blockAlign;
            left[frame] = decode (bytes, offset, bits);
            right[frame] = channels == 2
                         ? decode (bytes, offset + bits / 8, bits)
                         : left[frame];
        }
        sourceRate = static_cast<double> (rate);
        sourceChannels = channels;
        sourceBits = bits;
        return true;
    }

    Frame sample (std::int64_t hostSample, double hostRate) const noexcept
    {
        const auto position = std::max (0.0, static_cast<double> (hostSample))
                             * sourceRate / hostRate;
        const auto base = static_cast<std::size_t> (std::floor (position));
        const auto fraction = position - static_cast<double> (base);
        const auto first = std::min (base, left.size() - 1);
        const auto second = std::min (first + 1, left.size() - 1);
        return {
            left[first] + (left[second] - left[first]) * fraction,
            right[first] + (right[second] - right[first]) * fraction
        };
    }

    double rate() const noexcept { return sourceRate; }
    std::size_t frames() const noexcept { return left.size(); }
    std::uint16_t channels() const noexcept { return sourceChannels; }
    std::uint16_t bits() const noexcept { return sourceBits; }

private:
    static double decode (const std::vector<std::uint8_t>& bytes,
                          std::size_t offset,
                          std::uint16_t bits) noexcept
    {
        if (bits == 16)
            return static_cast<double> (
                static_cast<std::int16_t> (readLe16 (bytes, offset))) / 32768.0;
        if (bits == 24)
        {
            auto value = static_cast<std::int32_t> (bytes[offset])
                       | (static_cast<std::int32_t> (bytes[offset + 1]) << 8)
                       | (static_cast<std::int32_t> (bytes[offset + 2]) << 16);
            if ((value & 0x00800000) != 0)
                value |= static_cast<std::int32_t> (0xff000000);
            return static_cast<double> (value) / 8388608.0;
        }
        return static_cast<double> (
            static_cast<std::int32_t> (readLe32 (bytes, offset))) / 2147483648.0;
    }

    std::vector<double> left;
    std::vector<double> right;
    double sourceRate = 0.0;
    std::uint16_t sourceChannels = 0;
    std::uint16_t sourceBits = 0;
};

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
            const auto& stage = coefficients.stages[static_cast<std::size_t> (index)];
            const auto gain = stage.normalizeDc
                            ? 1.0 + stage.radius * stage.radius - 2.0 * stage.a
                            : stage.inputGain;
            const auto u = gain * x + stage.a * stageState[0] - stage.b * stageState[1];
            const auto v = stage.b * stageState[0] + stage.a * stageState[1];
            const auto w = u + stage.a / stage.b * v;
            x = w + stage.z1 * stageState[2] + stage.z2 * stageState[3];
            stageState[0] = u;
            stageState[1] = v;
            stageState[3] = stageState[2];
            stageState[2] = w;
        }
        return x * coefficients.gain;
    }

    morphosis::FilterState state;
};

struct Oracle
{
    Oracle (int newPreset, double newRate, const Controls& initial)
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
    double result = std::abs (left.gain - right.gain);
    for (std::size_t index = 0; index < left.stages.size(); ++index)
    {
        const auto& a = left.stages[index];
        const auto& b = right.stages[index];
        result = std::max ({
            result, std::abs (a.a - b.a), std::abs (a.b - b.b),
            std::abs (a.inputGain - b.inputGain), std::abs (a.z1 - b.z1),
            std::abs (a.z2 - b.z2), std::abs (a.radius - b.radius),
            std::abs (a.angle - b.angle), std::abs (a.zeroRadius - b.zeroRadius),
            std::abs (a.zeroAngle - b.zeroAngle)
        });
    }
    return result;
}

Controls lerp (const Controls& from, const Controls& to, double amount) noexcept
{
    return {
        from.frequency + (to.frequency - from.frequency) * amount,
        from.morph + (to.morph - from.morph) * amount,
        from.transform + (to.transform - from.transform) * amount
    };
}

ParameterSnapshot makeParameters (int preset, const Controls& controls)
{
    ParameterSnapshot result;
    result.preset = preset;
    result.frequency = controls.frequency;
    result.morph = controls.morph;
    result.transform = controls.transform;
    result.softClip = false;
    result.internalDistortion = false;
    return result;
}

struct Scenario
{
    std::string name;
    double range = 0.0;
    bool upwards = true;
    double durationMs = 0.0;
    int blockSize = 64;
};

enum class Phase
{
    preRoll,
    ramp,
    hold,
    tail,
    staticWindow
};

struct Result
{
    Scenario scenario;
    Stats inputMeasured;
    Stats productionRamp;
    Stats productionHold;
    Stats productionTail;
    Stats productionStatic;
    Stats oracleRamp;
    Stats oracleHold;
    Stats oracleTail;
    Stats oracleStatic;
    double maximumRelativeDifference = 0.0;
    double maximumCoefficientDifference = 0.0;
    bool safetyFault = false;
};

Phase phaseFor (bool isStatic,
                std::int64_t sample,
                std::int64_t preRoll,
                std::int64_t ramp,
                std::int64_t hold,
                std::int64_t duration)
{
    if (sample < preRoll)
        return Phase::preRoll;
    if (isStatic)
        return sample < preRoll + duration ? Phase::staticWindow : Phase::tail;
    if (sample < preRoll + ramp)
        return Phase::ramp;
    if (sample < preRoll + ramp + hold)
        return Phase::hold;
    return Phase::tail;
}

Result run (const std::filesystem::path& wavPath,
            const WavSource& wav,
            const Scenario& scenario,
            bool isStatic)
{
    Result result;
    result.scenario = scenario;
    const Controls start { -scenario.range, -1.87, 2.37 };
    const Controls finish { scenario.range, -1.87, 2.37 };
    const auto from = scenario.upwards ? start : finish;
    const auto to = scenario.upwards ? finish : start;
    const auto preRoll = static_cast<std::int64_t> (
        std::llround (scenario.durationMs * 0.0 + kPreRollMs * 48.0));
    const auto ramp = static_cast<std::int64_t> (
        std::llround (scenario.durationMs * 48.0));
    const auto hold = static_cast<std::int64_t> (kHoldMs * 48.0);
    const auto tail = static_cast<std::int64_t> (kTailMs * 48.0);
    const auto staticDuration = ramp;
    const auto total = isStatic ? preRoll + staticDuration + tail
                                : preRoll + ramp + hold + tail;

    std::atomic<bool> safetyFault { false };
    MorphosisDSP processor;
    processor.setTelemetry (nullptr, nullptr, &safetyFault);
    processor.prepare (48000.0, scenario.blockSize);
    Oracle oracle (184, 48000.0, from);
    ReferenceRecurrence reference;
    std::array<std::array<double, morphosis::kMorphosisLatencySamples>, 2> oracleDelay {};
    auto oracleDelayWrite = 0;

    for (std::int64_t processed = 0; processed < total; processed += scenario.blockSize)
    {
        Controls target = from;
        if (isStatic)
            target = from;
        else if (processed >= preRoll)
        {
            const auto amount = std::min (
                1.0, static_cast<double> (processed - preRoll + scenario.blockSize)
                    / static_cast<double> (std::max<std::int64_t> (1, ramp)));
            target = lerp (from, to, amount);
        }
        processor.beginBlock (makeParameters (184, target));
        oracle.setTarget (target);
        const auto count = std::min<std::int64_t> (
            scenario.blockSize, total - processed);

        for (std::int64_t offset = 0; offset < count; ++offset)
        {
            const auto sample = processed + offset;
            const auto sourceSample = sample - morphosis::kMorphosisLatencySamples;
            processor.advanceSample();
            oracle.advance();
            result.maximumCoefficientDifference = std::max (
                result.maximumCoefficientDifference,
                coefficientDifference (processor.getCurrentCoefficients(),
                                        oracle.coefficient));
            const auto phase = phaseFor (isStatic, sourceSample, preRoll, ramp, hold,
                                         staticDuration);
            const auto inputPhase = phaseFor (isStatic, sample, preRoll, ramp, hold,
                                              staticDuration);
            const auto input = inputPhase == Phase::tail
                             ? Frame {}
                             : wav.sample (sample, 48000.0);
            const auto leftInput = static_cast<float> (input.left);
            const auto rightInput = static_cast<float> (input.right);
            const auto expectedLeft = reference.process (
                static_cast<double> (leftInput), 0, oracle.coefficient);
            const auto expectedRight = reference.process (
                static_cast<double> (rightInput), 1, oracle.coefficient);
            const auto delayedExpectedLeft = oracleDelay[0][static_cast<std::size_t> (
                oracleDelayWrite)];
            const auto delayedExpectedRight = oracleDelay[1][static_cast<std::size_t> (
                oracleDelayWrite)];
            oracleDelay[0][static_cast<std::size_t> (oracleDelayWrite)] = expectedLeft;
            oracleDelay[1][static_cast<std::size_t> (oracleDelayWrite)] = expectedRight;
            oracleDelayWrite = (oracleDelayWrite + 1) % morphosis::kMorphosisLatencySamples;
            const auto actualLeft = static_cast<double> (
                processor.processSampleNoAdvance (leftInput, 0));
            const auto actualRight = static_cast<double> (
                processor.processSampleNoAdvance (rightInput, 1));
            if (sourceSample >= preRoll)
            {
                result.maximumRelativeDifference = std::max (
                    result.maximumRelativeDifference,
                    std::max (
                        std::abs (actualLeft - delayedExpectedLeft)
                            / std::max (1.0, std::abs (delayedExpectedLeft)),
                        std::abs (actualRight - delayedExpectedRight)
                            / std::max (1.0, std::abs (delayedExpectedRight))));
            }
            switch (phase)
            {
                case Phase::ramp:
                {
                    const auto matchedInput = wav.sample (sourceSample, 48000.0);
                    result.inputMeasured.add (matchedInput.left, matchedInput.right);
                    result.productionRamp.add (actualLeft, actualRight);
                    result.oracleRamp.add (delayedExpectedLeft, delayedExpectedRight);
                    break;
                }
                case Phase::hold:
                {
                    const auto matchedInput = wav.sample (sourceSample, 48000.0);
                    result.inputMeasured.add (matchedInput.left, matchedInput.right);
                    result.productionHold.add (actualLeft, actualRight);
                    result.oracleHold.add (delayedExpectedLeft, delayedExpectedRight);
                    break;
                }
                case Phase::staticWindow:
                {
                    const auto matchedInput = wav.sample (sourceSample, 48000.0);
                    result.inputMeasured.add (matchedInput.left, matchedInput.right);
                    result.productionStatic.add (actualLeft, actualRight);
                    result.oracleStatic.add (delayedExpectedLeft, delayedExpectedRight);
                    break;
                }
                case Phase::tail:
                    result.productionTail.add (actualLeft, actualRight);
                    result.oracleTail.add (delayedExpectedLeft, delayedExpectedRight);
                    break;
                case Phase::preRoll:
                    break;
            }
        }
    }
    result.safetyFault = safetyFault.load (std::memory_order_relaxed);
    static_cast<void> (wavPath);
    return result;
}

void writeCsv (const std::filesystem::path& path, const std::vector<Result>& results)
{
    std::ofstream output (path);
    output << "mode,range,direction,duration_ms,block_size,input_peak,input_rms,input_gt05_pct,"
              "input_gt1_pct,ramp_peak,ramp_rms,ramp_gt05_pct,ramp_gt1_pct,hold_peak,hold_rms,"
              "hold_gt05_pct,hold_gt1_pct,tail_peak,static_peak,oracle_ramp_peak,oracle_hold_peak,"
              "oracle_tail_peak,oracle_static_peak,"
              "max_relative_difference,max_coefficient_difference,safety_fault\n";
    output << std::setprecision (17);
    for (const auto& result : results)
        output << (result.scenario.name == "static" ? "static" : "sweep") << ','
               << result.scenario.range << ','
               << (result.scenario.upwards ? "minus_to_plus" : "plus_to_minus") << ','
               << result.scenario.durationMs << ',' << result.scenario.blockSize << ','
               << result.inputMeasured.peak << ',' << result.inputMeasured.rms() << ','
               << result.inputMeasured.overHalfPercent() << ','
               << result.inputMeasured.overOnePercent() << ','
               << result.productionRamp.peak << ',' << result.productionRamp.rms() << ','
               << result.productionRamp.overHalfPercent() << ','
               << result.productionRamp.overOnePercent() << ','
               << result.productionHold.peak << ',' << result.productionHold.rms() << ','
               << result.productionHold.overHalfPercent() << ','
               << result.productionHold.overOnePercent() << ','
               << result.productionTail.peak << ',' << result.productionStatic.peak << ','
               << result.oracleRamp.peak << ',' << result.oracleHold.peak << ','
               << result.oracleTail.peak << ',' << result.oracleStatic.peak << ','
               << result.maximumRelativeDifference << ','
               << result.maximumCoefficientDifference << ','
               << (result.safetyFault ? 1 : 0) << '\n';
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: ProductionChordValidation.exe <chord.wav> <output-dir>\n";
        return 2;
    }

    WavSource wav;
    std::string error;
    if (! wav.load (argv[1], error))
    {
        std::cerr << "WAV load failed: " << error << '\n';
        return 3;
    }
    if (wav.rate() != 48000.0 || wav.channels() != 2 || wav.bits() != 24)
    {
        std::cerr << "Expected the verified stereo 24-bit 48 kHz chord source\n";
        return 4;
    }

    std::error_code filesystemError;
    const std::filesystem::path outputDirectory (argv[2]);
    std::filesystem::create_directories (outputDirectory, filesystemError);
    if (filesystemError)
    {
        std::cerr << "Unable to create output directory: "
                  << filesystemError.message() << '\n';
        return 5;
    }

    const auto ranges = std::array<double, 3> { 5.0, 3.0, 1.0 };
    const auto durations = std::array<double, 5> { 20.0, 50.0, 100.0, 500.0, 2000.0 };
    std::vector<Result> results;
    for (const auto range : ranges)
        for (const auto duration : durations)
            for (const auto blockSize : { 64, 512 })
            {
                for (const auto upwards : { true, false })
                {
                    results.push_back (run (
                        argv[1], wav,
                        { "sweep", range, upwards, duration, blockSize }, false));
                    const auto& result = results.back();
                    if (result.safetyFault || result.maximumCoefficientDifference > 1.0e-12
                        || result.maximumRelativeDifference > 2.0e-5
                        || result.productionRamp.peak > 2.0
                        || result.productionHold.peak > 2.0)
                    {
                        std::cerr << "FAIL: production chord sweep validation at range "
                                  << range << ", duration " << duration
                                  << ", block " << blockSize << '\n';
                        return 6;
                    }
                }
            }

    for (const auto range : ranges)
        for (const auto duration : durations)
            for (const auto fraction : { -1.0, -0.75, -0.5, -0.25, 0.0,
                                         0.25, 0.5, 0.75, 1.0 })
            {
                const auto result = run (
                    argv[1], wav,
                    { "static", range * fraction, true, duration, 64 }, true);
                if (result.safetyFault || result.maximumCoefficientDifference > 1.0e-12
                    || result.maximumRelativeDifference > 2.0e-5)
                {
                    std::cerr << "FAIL: production chord static validation at frequency "
                              << range * fraction << ", duration " << duration << '\n';
                    return 7;
                }
                results.push_back (result);
            }

    writeCsv (outputDirectory / "production_chord_validation.csv", results);
    double maximumInput = 0.0;
    double maximumInputRms = 0.0;
    double maximumInputOverHalf = 0.0;
    double maximumInputOverOne = 0.0;
    double maximumRamp = 0.0;
    double maximumRampRms = 0.0;
    double maximumRampOverHalf = 0.0;
    double maximumHold = 0.0;
    double maximumHoldRms = 0.0;
    double maximumHoldOverHalf = 0.0;
    double maximumTail = 0.0;
    double maximumStatic = 0.0;
    double maximumRelativeDifference = 0.0;
    double maximumCoefficientDifference = 0.0;
    double maximumRampOverOne = 0.0;
    double maximumHoldOverOne = 0.0;
    for (const auto& result : results)
    {
        maximumInput = std::max (maximumInput, result.inputMeasured.peak);
        maximumInputRms = std::max (maximumInputRms, result.inputMeasured.rms());
        maximumInputOverHalf = std::max (
            maximumInputOverHalf, result.inputMeasured.overHalfPercent());
        maximumInputOverOne = std::max (
            maximumInputOverOne, result.inputMeasured.overOnePercent());
        maximumRamp = std::max (maximumRamp, result.productionRamp.peak);
        maximumRampRms = std::max (maximumRampRms, result.productionRamp.rms());
        maximumRampOverHalf = std::max (
            maximumRampOverHalf, result.productionRamp.overHalfPercent());
        maximumHold = std::max (maximumHold, result.productionHold.peak);
        maximumHoldRms = std::max (maximumHoldRms, result.productionHold.rms());
        maximumHoldOverHalf = std::max (
            maximumHoldOverHalf, result.productionHold.overHalfPercent());
        maximumTail = std::max (maximumTail, result.productionTail.peak);
        maximumStatic = std::max (maximumStatic, result.productionStatic.peak);
        maximumRelativeDifference = std::max (
            maximumRelativeDifference, result.maximumRelativeDifference);
        maximumCoefficientDifference = std::max (
            maximumCoefficientDifference, result.maximumCoefficientDifference);
        maximumRampOverOne = std::max (
            maximumRampOverOne, result.productionRamp.overOnePercent());
        maximumHoldOverOne = std::max (
            maximumHoldOverOne, result.productionHold.overOnePercent());
    }

    std::ofstream summary (outputDirectory / "production_chord_validation_summary.txt");
    summary << std::fixed << std::setprecision (12)
            << "Production chord validation\n"
               "Source: " << argv[1] << "\n"
            << "Preset: UI 185 -> zero-based 184 OddHrm+Rez\n"
               "Morph: -1.87, Transform: 2.37, host rate: 48000 Hz\n"
               "Assumptions: input/pre/post gains 0 dB, soft clip OFF, internal distortion OFF\n"
               "Pre-roll: 250 ms; endpoint continued-chord hold: 500 ms; silence tail: 250 ms\n"
               "Static rows: 135; dynamic rows: 60\n"
            << "Measured production output is pre-clip/output here because all gains are 0 dB and both nonlinear stages are OFF\n"
            << "Maximum measured input peak: " << maximumInput << "\n"
            << "Maximum measured input RMS: " << maximumInputRms << "\n"
            << "Maximum measured input percentage >0.5: " << maximumInputOverHalf << "%\n"
            << "Maximum measured input percentage >1.0: " << maximumInputOverOne << "%\n"
            << "Maximum production ramp peak: " << maximumRamp << "\n"
            << "Maximum production ramp RMS: " << maximumRampRms << "\n"
            << "Maximum production ramp percentage >0.5: " << maximumRampOverHalf << "%\n"
            << "Maximum production endpoint-hold peak: " << maximumHold << "\n"
            << "Maximum production endpoint-hold RMS: " << maximumHoldRms << "\n"
            << "Maximum production endpoint-hold percentage >0.5: " << maximumHoldOverHalf << "%\n"
            << "Maximum production silence-tail peak: " << maximumTail << "\n"
            << "Maximum production static peak: " << maximumStatic << "\n"
            << "Maximum ramp percentage >1.0: " << maximumRampOverOne << "%\n"
            << "Maximum hold percentage >1.0: " << maximumHoldOverOne << "%\n"
            << "Maximum relative production/oracle difference: "
            << maximumRelativeDifference << "\n"
            << "Maximum coefficient difference: " << maximumCoefficientDifference << "\n"
            << "Safety flags: 0\n";

    std::cout << "Production chord validation passed\n"
              << "Dynamic rows: 60, static rows: 135\n"
              << "Maximum ramp peak: " << maximumRamp << "\n"
              << "Maximum endpoint-hold peak: " << maximumHold << "\n"
              << "Maximum relative oracle difference: "
              << maximumRelativeDifference << "\n"
              << "Output directory: " << outputDirectory.string() << '\n';
    return 0;
}
