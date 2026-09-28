#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#define private public
#include "MorphosisDSP.h"
#undef private

#include "PresetRouting.h"

namespace
{

using morphosis::MorphosisDSP;

constexpr std::array<double, 5> kControlGrid { -5.0, -2.5, 0.0, 2.5, 5.0 };
constexpr std::array<double, 3> kSampleRates { 44100.0, 48000.0, 96000.0 };
constexpr std::size_t kBaseCandidatesPerRate = 6;
constexpr std::size_t kDistCandidatesPerRate = 3;
constexpr std::size_t kResponseCandidatesPerRate = 6;
constexpr double kMinus90Dbfs = 0.00003162277660168379;
constexpr double kMinus100Dbfs = 0.00001;
constexpr double kLinearRenderLimitSeconds = 120.0;
constexpr double kDistRenderLimitSeconds = 30.0;

struct Candidate
{
    int preset = 0;
    double frequency = 0.0;
    double morph = 0.0;
    double transform = 0.0;
    double sampleRate = 48000.0;
    double maxPoleRadius = 0.0;
    double maxDistPoleRadius = 0.0;
    double maxResponseMagnitude = 0.0;
    double maxResponseFrequency = 0.0;
    int maxPoleStage = 0;
    bool dotFour = false;
};

struct ScanSummary
{
    std::vector<Candidate> candidates;
    std::vector<Candidate> responseCandidates;
    std::vector<Candidate> dotFourResponseCandidates;
    std::vector<double> maximumRadii;
    std::uint64_t coefficientSets = 0;
    std::uint64_t invalidSets = 0;
    std::uint64_t fallbackSets = 0;
    double maximumPoleRadius = 0.0;
    Candidate maximumPoleCase {};
    double maximumDistPoleRadius = 0.0;
    Candidate maximumDistPoleCase {};
    double maximumResponseMagnitude = 0.0;
    Candidate maximumResponseCase {};
};

void require (bool condition, const std::string& message)
{
    if (condition)
        return;

    std::cerr << "FAIL: " << message << '\n';
    std::exit (EXIT_FAILURE);
}

std::string csvQuote (const char* value)
{
    std::string result = "\"";
    for (const auto* ch = value; *ch != '\0'; ++ch)
    {
        if (*ch == '\"')
            result += '\"';
        result += *ch;
    }
    result += '\"';
    return result;
}

void insertTopCandidate (std::vector<Candidate>& top,
                         const Candidate& candidate,
                         bool useDistRadius,
                         bool useResponseMagnitude,
                         std::size_t limit)
{
    const auto score = [useDistRadius, useResponseMagnitude] (const Candidate& item)
    {
        if (useResponseMagnitude)
            return item.maxResponseMagnitude;
        return useDistRadius ? item.maxDistPoleRadius : item.maxPoleRadius;
    };

    const auto duplicate = std::find_if (top.begin(), top.end(), [&] (const Candidate& item)
    {
        return item.preset == candidate.preset
            && item.frequency == candidate.frequency
            && item.morph == candidate.morph
            && item.transform == candidate.transform;
    });
    if (duplicate != top.end())
    {
        if (score (candidate) > score (*duplicate))
            *duplicate = candidate;
    }
    else
    {
        top.push_back (candidate);
    }

    std::sort (top.begin(), top.end(), [&] (const Candidate& left, const Candidate& right)
    {
        return score (left) > score (right);
    });
    if (top.size() > limit)
        top.resize (limit);
}

ScanSummary scanCoefficientGrid (const std::string& path)
{
    std::ofstream csv (path, std::ios::binary | std::ios::trunc);
    require (csv.good(), "coefficient census CSV opens in the build output directory");

    csv << "sample_rate,preset,name,frequency,morph,transform";
    for (int stage = 0; stage < morphosis::kStageCount; ++stage)
        csv << ",stage" << stage << "_pole_radius";
    csv << ",dominant_stage,max_pole_radius,max_dist_pole_radius,max_response_db,max_response_hz,dot_four\n";
    csv << std::setprecision (17);

    ScanSummary summary;
    for (const auto sampleRate : kSampleRates)
    {
        std::vector<Candidate> rateBaseTop;
        std::vector<Candidate> rateDistTop;
        std::vector<Candidate> rateResponseTop;
        std::vector<Candidate> rateDotFourResponseTop;

        for (int preset = 0; preset < morphosis::cube_data::kRecordCount; ++preset)
        {
            const auto dotFour = morphosis::usesDotFourDistortionLayout (preset);
            for (const auto frequency : kControlGrid)
            {
                for (const auto morph : kControlGrid)
                {
                    for (const auto transform : kControlGrid)
                    {
                        const auto coefficients = MorphosisDSP::makeCoefficients (
                            preset, frequency, morph, transform, sampleRate);
                        ++summary.coefficientSets;
                        if (! coefficients.valid)
                        {
                            ++summary.invalidSets;
                            continue;
                        }
                        if (coefficients.usedSafeFallback)
                            ++summary.fallbackSets;

                        Candidate candidate;
                        candidate.preset = preset;
                        candidate.frequency = frequency;
                        candidate.morph = morph;
                        candidate.transform = transform;
                        candidate.sampleRate = sampleRate;
                        candidate.dotFour = dotFour;

                        std::array<double, morphosis::kStageCount> radii {};
                        for (int stageIndex = 0; stageIndex < morphosis::kStageCount; ++stageIndex)
                        {
                            const auto& stage = coefficients.stages[
                                static_cast<std::size_t> (stageIndex)];
                            const auto baseRadius = std::hypot (stage.a, stage.b);
                            const auto distRadius = stage.radius > 1.0e-12
                                ? baseRadius * (2.0 - stage.radius)
                                : baseRadius;
                            radii[static_cast<std::size_t> (stageIndex)] = baseRadius;
                            if (baseRadius > candidate.maxPoleRadius)
                            {
                                candidate.maxPoleRadius = baseRadius;
                                candidate.maxPoleStage = stageIndex;
                            }
                            candidate.maxDistPoleRadius = std::max (
                                candidate.maxDistPoleRadius, distRadius);
                        }

                        const auto nyquist = sampleRate * 0.5;
                        const auto scoreResponseAt = [&] (double frequency)
                        {
                            const auto magnitude = MorphosisDSP::responseMagnitude (
                                coefficients, frequency, sampleRate);
                            if (std::isfinite (magnitude)
                                && magnitude > candidate.maxResponseMagnitude)
                            {
                                candidate.maxResponseMagnitude = magnitude;
                                candidate.maxResponseFrequency = frequency;
                            }
                        };
                        scoreResponseAt (0.0);
                        scoreResponseAt (nyquist);
                        for (const auto& stage : coefficients.stages)
                        {
                            const auto frequency = std::clamp (
                                std::abs (stage.angle) * sampleRate
                                    / (2.0 * 3.14159265358979323846),
                                0.0, nyquist);
                            scoreResponseAt (frequency);
                        }

                        csv << sampleRate << ',' << preset << ','
                            << csvQuote (morphosis::cube_data::kCubes[
                                   static_cast<std::size_t> (preset)].name)
                            << ',' << frequency << ',' << morph << ',' << transform;
                        for (const auto radius : radii)
                            csv << ',' << radius;
                        csv << ',' << candidate.maxPoleStage << ','
                            << candidate.maxPoleRadius << ',' << candidate.maxDistPoleRadius
                            << ',' << (20.0 * std::log10 (
                                std::max (candidate.maxResponseMagnitude, 1.0e-300)))
                            << ',' << candidate.maxResponseFrequency
                            << ',' << (dotFour ? 1 : 0) << '\n';

                        summary.maximumRadii.push_back (candidate.maxPoleRadius);
                        if (candidate.maxPoleRadius > summary.maximumPoleRadius)
                        {
                            summary.maximumPoleRadius = candidate.maxPoleRadius;
                            summary.maximumPoleCase = candidate;
                        }
                        if (dotFour
                            && candidate.maxDistPoleRadius > summary.maximumDistPoleRadius)
                        {
                            summary.maximumDistPoleRadius = candidate.maxDistPoleRadius;
                            summary.maximumDistPoleCase = candidate;
                        }
                        if (candidate.maxResponseMagnitude > summary.maximumResponseMagnitude)
                        {
                            summary.maximumResponseMagnitude = candidate.maxResponseMagnitude;
                            summary.maximumResponseCase = candidate;
                        }

                        insertTopCandidate (rateBaseTop, candidate, false,
                                            false,
                                            kBaseCandidatesPerRate);
                        if (dotFour)
                            insertTopCandidate (rateDistTop, candidate, true,
                                                false,
                                                kDistCandidatesPerRate);
                        insertTopCandidate (rateResponseTop, candidate, false,
                                            true,
                                            kResponseCandidatesPerRate);
                        if (dotFour)
                            insertTopCandidate (rateDotFourResponseTop, candidate, false,
                                                true,
                                                kDistCandidatesPerRate);
                    }
                }
            }
        }

        summary.candidates.insert (summary.candidates.end(),
                                   rateBaseTop.begin(), rateBaseTop.end());
        summary.candidates.insert (summary.candidates.end(),
                                   rateDistTop.begin(), rateDistTop.end());
        summary.candidates.insert (summary.candidates.end(),
                                   rateResponseTop.begin(), rateResponseTop.end());
        summary.responseCandidates.insert (summary.responseCandidates.end(),
                                           rateResponseTop.begin(), rateResponseTop.end());
        summary.dotFourResponseCandidates.insert (summary.dotFourResponseCandidates.end(),
                                                  rateDotFourResponseTop.begin(),
                                                  rateDotFourResponseTop.end());
    }

    std::sort (summary.maximumRadii.begin(), summary.maximumRadii.end());
    csv.flush();
    require (csv.good(), "coefficient census CSV flushes successfully");
    return summary;
}

void printCandidate (const char* label, const Candidate& candidate, double radius)
{
    std::cout << label << " score=" << std::setprecision (12) << radius
              << " preset=" << candidate.preset
              << " name=" << morphosis::cube_data::kCubes[
                     static_cast<std::size_t> (candidate.preset)].name
              << " F=" << candidate.frequency
              << " M=" << candidate.morph
              << " X=" << candidate.transform
              << " rate=" << candidate.sampleRate
              << " stage=" << candidate.maxPoleStage
              << " response_db=" << 20.0 * std::log10 (
                     std::max (candidate.maxResponseMagnitude, 1.0e-300))
              << " response_hz=" << candidate.maxResponseFrequency
              << " dist_source=" << (candidate.dotFour ? "yes" : "no") << '\n';
}

enum class Excitation
{
    impulse,
    burst
};

const char* excitationName (Excitation excitation) noexcept
{
    return excitation == Excitation::impulse ? "impulse" : "burst";
}

struct TailResult
{
    double peak = 0.0;
    double peakAfterInput = 0.0;
    std::array<std::int64_t, morphosis::kChannelCount> lastAbove90 { -1, -1 };
    std::array<std::int64_t, morphosis::kChannelCount> lastAbove100 { -1, -1 };
    std::int64_t lastProcessed = 0;
    int lastInputSample = 0;
    bool censored = false;
};

double burstSample (int sample, int channel) noexcept
{
    auto state = static_cast<std::uint32_t> (sample + 1)
               * (channel == 0 ? 0x9e3779b9u : 0x85ebca6bu);
    state ^= state >> 16;
    state *= 0x7feb352du;
    state ^= state >> 15;
    state *= 0x846ca68bu;
    state ^= state >> 16;
    const auto signedNoise = static_cast<double> (static_cast<std::int32_t> (state))
                           / 2147483648.0;
    return 0.2 * signedNoise;
}

TailResult renderIirTail (const Candidate& candidate,
                          bool distortionOn,
                          Excitation excitation,
                          int channels,
                          std::ofstream& summaryCsv,
                          std::ofstream& envelopeCsv)
{
    require (channels == 1 || channels == 2, "tail render channel count is mono or stereo");
    require (! distortionOn || candidate.dotFour,
             "internal DIST render is limited to documented .4 source presets");

    MorphosisDSP dsp;
    dsp.currentCoefficients = MorphosisDSP::makeCoefficients (
        candidate.preset, candidate.frequency, candidate.morph,
        candidate.transform, candidate.sampleRate);
    require (dsp.currentCoefficients.valid && ! dsp.currentCoefficients.usedSafeFallback,
             "selected tail render has a valid recovered coefficient set");
    dsp.filterState.clear();
    dsp.nonlinearMix.current = distortionOn ? 1.0 : 0.0;
    dsp.nonlinearThreshold.current = MorphosisDSP::settledThresholdForSetting (60);

    constexpr int burstLength = 32;
    const auto inputLength = excitation == Excitation::impulse ? 1 : burstLength;
    const auto lastInput = inputLength - 1;
    const auto rate = static_cast<std::int64_t> (candidate.sampleRate);
    const auto maxSeconds = distortionOn ? kDistRenderLimitSeconds
                                         : kLinearRenderLimitSeconds;
    const auto maxSamples = static_cast<std::int64_t> (std::ceil (candidate.sampleRate * maxSeconds));
    const auto quietWindow = rate;
    const auto envelopeBucket = std::max<std::int64_t> (1, rate / 20);
    const auto name = morphosis::cube_data::kCubes[
        static_cast<std::size_t> (candidate.preset)].name;
    const auto label = std::string ("IIR_") + std::to_string (candidate.preset)
                     + "_" + (distortionOn ? "DIST" : "linear")
                     + "_" + excitationName (excitation)
                     + "_" + (channels == 1 ? "mono" : "stereo");

    TailResult result;
    result.lastInputSample = lastInput;
    std::array<double, morphosis::kChannelCount> bucketPeak {};
    std::int64_t previousAbove100 = -1;
    std::int64_t currentBucket = 0;

    envelopeCsv << std::setprecision (17);
    for (std::int64_t sample = 0; sample < maxSamples; ++sample)
    {
        const auto bucket = sample / envelopeBucket;
        if (bucket != currentBucket)
        {
            envelopeCsv << csvQuote (label.c_str()) << ',' << candidate.sampleRate << ','
                        << candidate.preset << ',' << csvQuote (name) << ','
                        << candidate.frequency << ',' << candidate.morph << ','
                        << candidate.transform << ',' << (distortionOn ? 1 : 0) << ','
                        << excitationName (excitation) << ',' << channels << ','
                        << ((currentBucket * envelopeBucket + envelopeBucket / 2
                             - lastInput + morphosis::kMorphosisLatencySamples)
                            * 1000.0 / candidate.sampleRate);
            for (int channel = 0; channel < morphosis::kChannelCount; ++channel)
                envelopeCsv << ',' << bucketPeak[static_cast<std::size_t> (channel)];
            envelopeCsv << '\n';
            bucketPeak = {};
            currentBucket = bucket;
        }

        bool above100 = false;
        for (int channel = 0; channel < channels; ++channel)
        {
            double input = 0.0;
            if (excitation == Excitation::impulse && sample == 0)
                input = channel == 0 ? 0.5 : -0.37;
            else if (excitation == Excitation::burst && sample < burstLength)
                input = burstSample (static_cast<int> (sample), channel);

            const auto output = dsp.processFilter (input, channel);
            require (std::isfinite (output), "IIR tail render remains finite");
            const auto magnitude = std::abs (output);
            result.peak = std::max (result.peak, magnitude);
            if (sample > lastInput)
                result.peakAfterInput = std::max (result.peakAfterInput, magnitude);
            bucketPeak[static_cast<std::size_t> (channel)] = std::max (
                bucketPeak[static_cast<std::size_t> (channel)], magnitude);

            if (magnitude > kMinus90Dbfs)
                result.lastAbove90[static_cast<std::size_t> (channel)] = sample;
            if (magnitude > kMinus100Dbfs)
            {
                result.lastAbove100[static_cast<std::size_t> (channel)] = sample;
                above100 = true;
            }
        }
        if (above100)
            previousAbove100 = sample;

        result.lastProcessed = sample;
        if (sample >= lastInput && sample - previousAbove100 > quietWindow)
            break;
    }

    result.censored = result.lastAbove100[0] >= result.lastProcessed - rate / 10
                   || (channels == 2
                       && result.lastAbove100[1] >= result.lastProcessed - rate / 10);
    require (! result.censored, "selected IIR tail cases fall below -100 dBFS within their render horizon");

    envelopeCsv << csvQuote (label.c_str()) << ',' << candidate.sampleRate << ','
                << candidate.preset << ',' << csvQuote (name) << ','
                << candidate.frequency << ',' << candidate.morph << ','
                << candidate.transform << ',' << (distortionOn ? 1 : 0) << ','
                << excitationName (excitation) << ',' << channels << ','
                << ((result.lastProcessed - lastInput
                     + morphosis::kMorphosisLatencySamples)
                    * 1000.0 / candidate.sampleRate);
    for (int channel = 0; channel < morphosis::kMorphosisLatencySamples && channel < 2; ++channel)
    {
        if (channel < channels)
            envelopeCsv << ',' << bucketPeak[static_cast<std::size_t> (channel)];
        else
            envelopeCsv << ',' << 0.0;
    }
    envelopeCsv << '\n';

    summaryCsv << std::setprecision (17) << csvQuote (label.c_str()) << ','
               << candidate.sampleRate << ',' << candidate.preset << ',' << csvQuote (name)
               << ',' << candidate.frequency << ',' << candidate.morph << ','
               << candidate.transform << ',' << (distortionOn ? 1 : 0) << ','
               << excitationName (excitation) << ',' << channels << ',' << result.peak
               << ',' << result.peakAfterInput;
    for (int channel = 0; channel < morphosis::kChannelCount; ++channel)
    {
        const auto index = static_cast<std::size_t> (channel);
        const auto tail90 = result.lastAbove90[index] < 0 ? -1
            : result.lastAbove90[index] - lastInput + morphosis::kMorphosisLatencySamples;
        const auto tail100 = result.lastAbove100[index] < 0 ? -1
            : result.lastAbove100[index] - lastInput + morphosis::kMorphosisLatencySamples;
        summaryCsv << ',' << tail90 << ',' << tail100;
    }
    summaryCsv << ',' << (result.lastProcessed + 1) << ',' << (result.censored ? 1 : 0) << '\n';

    std::cout << label << " rate=" << candidate.sampleRate
              << " max=" << result.peak << " postInput=" << result.peakAfterInput
              << " -90dBFS_samples="
              << result.lastAbove90[0] - lastInput + morphosis::kMorphosisLatencySamples
              << " -100dBFS_samples="
              << result.lastAbove100[0] - lastInput + morphosis::kMorphosisLatencySamples
              << " processed=" << result.lastProcessed + 1
              << " censored=" << (result.censored ? "yes" : "no") << '\n';
    return result;
}

const Candidate& candidateFor (const ScanSummary& summary, int preset, double rate)
{
    const auto found = std::find_if (summary.candidates.begin(), summary.candidates.end(),
        [=] (const Candidate& candidate)
        {
            return candidate.preset == preset && candidate.sampleRate == rate;
        });
    require (found != summary.candidates.end(), "selected candidate is present in the coefficient census");
    return *found;
}

const Candidate& responseCandidateFor (const std::vector<Candidate>& candidates,
                                       double rate)
{
    const auto found = std::find_if (candidates.begin(), candidates.end(),
        [=] (const Candidate& candidate)
        {
            return candidate.sampleRate == rate;
        });
    require (found != candidates.end(), "response candidate exists for each sample rate");
    return *found;
}

void characterizeIirTails (const ScanSummary& summary)
{
    std::ofstream summaryCsv ("stage6_iir_tail_renders.csv", std::ios::binary | std::ios::trunc);
    std::ofstream envelopeCsv ("stage6_iir_decay_envelopes.csv", std::ios::binary | std::ios::trunc);
    require (summaryCsv.good() && envelopeCsv.good(), "IIR tail CSV outputs open successfully");
    summaryCsv << "case,sample_rate,preset,name,frequency,morph,transform,distortion,excitation,channels,peak,peak_after_input,ch0_last_minus90_samples,ch0_last_minus100_samples,ch1_last_minus90_samples,ch1_last_minus100_samples,processed_samples,censored_at_horizon\n";
    envelopeCsv << "case,sample_rate,preset,name,frequency,morph,transform,distortion,excitation,channels,time_ms,ch0_bucket_peak,ch1_bucket_peak\n";

    for (const auto rate : kSampleRates)
    {
        const auto& worst = candidateFor (summary, 205, rate);
        renderIirTail (worst, false, Excitation::impulse, 2, summaryCsv, envelopeCsv);
        renderIirTail (worst, false, Excitation::burst, 2, summaryCsv, envelopeCsv);

        const auto& highResponse = responseCandidateFor (summary.responseCandidates, rate);
        renderIirTail (highResponse, false, Excitation::impulse, 2, summaryCsv, envelopeCsv);
        renderIirTail (highResponse, false, Excitation::burst, 2, summaryCsv, envelopeCsv);

        const auto& distortion = candidateFor (summary, 159, rate);
        renderIirTail (distortion, true, Excitation::burst, 2, summaryCsv, envelopeCsv);
        if (rate == 48000.0)
            renderIirTail (distortion, false, Excitation::burst, 2, summaryCsv, envelopeCsv);

        const auto& highDistResponse = responseCandidateFor (
            summary.dotFourResponseCandidates, rate);
        renderIirTail (highDistResponse, true, Excitation::burst, 2,
                       summaryCsv, envelopeCsv);
        if (rate == 48000.0)
            renderIirTail (highDistResponse, false, Excitation::burst, 2,
                           summaryCsv, envelopeCsv);
    }

    const auto& worst48k = candidateFor (summary, 205, 48000.0);
    renderIirTail (worst48k, false, Excitation::impulse, 1, summaryCsv, envelopeCsv);
    renderIirTail (worst48k, false, Excitation::burst, 1, summaryCsv, envelopeCsv);
    const auto& tracker48k = candidateFor (summary, 211, 48000.0);
    renderIirTail (tracker48k, false, Excitation::impulse, 2, summaryCsv, envelopeCsv);
    renderIirTail (tracker48k, false, Excitation::burst, 2, summaryCsv, envelopeCsv);
    const auto& harmonic48k = candidateFor (summary, 224, 48000.0);
    renderIirTail (harmonic48k, false, Excitation::impulse, 2, summaryCsv, envelopeCsv);
    renderIirTail (harmonic48k, false, Excitation::burst, 2, summaryCsv, envelopeCsv);
    const auto& distortion48k = candidateFor (summary, 159, 48000.0);
    renderIirTail (distortion48k, true, Excitation::burst, 1, summaryCsv, envelopeCsv);
    const auto& highResponse48k = responseCandidateFor (summary.responseCandidates, 48000.0);
    renderIirTail (highResponse48k, false, Excitation::impulse, 1,
                   summaryCsv, envelopeCsv);
    renderIirTail (highResponse48k, false, Excitation::burst, 1,
                   summaryCsv, envelopeCsv);
    const auto& highDistResponse48k = responseCandidateFor (
        summary.dotFourResponseCandidates, 48000.0);
    renderIirTail (highDistResponse48k, true, Excitation::burst, 1,
                   summaryCsv, envelopeCsv);

    summaryCsv.flush();
    envelopeCsv.flush();
    require (summaryCsv.good() && envelopeCsv.good(), "IIR tail CSV outputs complete successfully");
}

void characterizeFirSupport (const ScanSummary& summary)
{
    std::ofstream csv ("stage6_fir_tail_support.csv", std::ios::binary | std::ios::trunc);
    require (csv.good(), "FIR support CSV opens successfully");
    csv << "sample_rate,preset,name,frequency,morph,transform,valid,nonzero_taps,last_nonzero_tap,last_minus90_tap,last_minus100_tap,latency_samples,tail_support_samples,build_ms\n";
    csv << std::setprecision (17);

    for (const auto rate : kSampleRates)
    {
        const auto& candidate = candidateFor (summary, 205, rate);
        MorphosisDSP dsp;
        dsp.prepare (rate, morphosis::kExperimentalFirPartitionSize);
        morphosis::ParameterSnapshot parameters;
        parameters.mode = morphosis::ProcessingMode::xy;
        parameters.xyInterpolation = morphosis::XYInterpolationMode::response;
        parameters.xyPresets.fill (candidate.preset);
        parameters.frequency = candidate.frequency;
        parameters.morph = candidate.morph;
        parameters.transform = candidate.transform;
        parameters.xyX = 0.5;
        parameters.xyY = 0.5;
        parameters.dryWet = 1.0;
        dsp.beginBlock (parameters, true);
        for (int sample = 0; sample < 64 && ! dsp.firReady; ++sample)
            dsp.advanceSample();
        require (dsp.firReady, "synchronous Response design yields a ready 2048-tap kernel");

        int nonzeroTaps = 0;
        int lastNonzero = -1;
        int lastMinus90 = -1;
        int lastMinus100 = -1;
        double peak = 0.0;
        for (int tap = 0; tap < morphosis::kExperimentalFirTaps; ++tap)
        {
            const auto magnitude = std::abs (dsp.firImpulse[static_cast<std::size_t> (tap)]);
            peak = std::max (peak, static_cast<double> (magnitude));
            if (magnitude != 0.0f)
            {
                ++nonzeroTaps;
                lastNonzero = tap;
            }
            if (magnitude > kMinus90Dbfs)
                lastMinus90 = tap;
            if (magnitude > kMinus100Dbfs)
                lastMinus100 = tap;
        }
        const auto tailSamples = lastNonzero < 0 ? 0
            : morphosis::kMorphosisLatencySamples + lastNonzero + 1;
        require (nonzeroTaps == morphosis::kExperimentalFirTaps
                     && lastNonzero == morphosis::kExperimentalFirTaps - 1,
                 "selected Response kernel uses all 2048 finite taps");
        require (tailSamples == morphosis::kMorphosisLatencySamples
                     + morphosis::kExperimentalFirTaps,
                 "Response support includes exactly the declared 64-sample latency");
        csv << rate << ',' << candidate.preset << ','
            << csvQuote (morphosis::cube_data::kCubes[
                   static_cast<std::size_t> (candidate.preset)].name)
            << ',' << candidate.frequency << ',' << candidate.morph << ','
            << candidate.transform << ",1," << nonzeroTaps << ',' << lastNonzero
            << ',' << lastMinus90 << ',' << lastMinus100 << ','
            << morphosis::kMorphosisLatencySamples << ',' << tailSamples << ','
            << dsp.getLastExperimentalFirBuildMilliseconds() << '\n';
        std::cout << "FIR rate=" << rate << " ready=yes taps=" << nonzeroTaps
                  << " last_nonzero=" << lastNonzero << " peak=" << peak
                  << " build_ms=" << dsp.getLastExperimentalFirBuildMilliseconds() << '\n';
    }
    csv.flush();
    require (csv.good(), "FIR support CSV completes successfully");
}

} // namespace

int main (int argc, char** argv)
{
    const auto summary = scanCoefficientGrid ("stage6_coefficient_radii.csv");
    require (summary.coefficientSets == static_cast<std::uint64_t> (
                 morphosis::cube_data::kRecordCount * kControlGrid.size()
                 * kControlGrid.size() * kControlGrid.size() * kSampleRates.size()),
             "coefficient census covers all cubes, grid points, and host rates");
    require (summary.invalidSets == 0 && summary.fallbackSets == 0,
             "sampled recovered coefficient sets are valid without fallback");
    require (summary.maximumPoleRadius < 1.0 && summary.maximumDistPoleRadius < 1.0,
             "sampled base and maximum-amplitude DIST pole radii remain strictly stable");
    std::cout << "Coefficient sets=" << summary.coefficientSets
              << " invalid=" << summary.invalidSets
              << " fallback=" << summary.fallbackSets
              << " sampled_points_per_cube_rate=" << kControlGrid.size() * kControlGrid.size()
                     * kControlGrid.size()
              << '\n';
    if (! summary.maximumRadii.empty())
    {
        const auto percentile = [&] (double fraction)
        {
            const auto index = static_cast<std::size_t> (std::floor (
                fraction * static_cast<double> (summary.maximumRadii.size() - 1)));
            return summary.maximumRadii[index];
        };
        std::cout << "Max-radius percentiles p50=" << percentile (0.50)
                  << " p90=" << percentile (0.90)
                  << " p99=" << percentile (0.99) << '\n';
    }
    printCandidate ("maximum-base", summary.maximumPoleCase, summary.maximumPoleRadius);
    printCandidate ("maximum-dot4-dist-bound", summary.maximumDistPoleCase,
                    summary.maximumDistPoleRadius);
    printCandidate ("maximum-response", summary.maximumResponseCase,
                    summary.maximumResponseMagnitude);
    for (std::size_t index = 0; index < summary.candidates.size(); ++index)
        printCandidate ("selected", summary.candidates[index],
                        summary.candidates[index].dotFour
                            ? summary.candidates[index].maxDistPoleRadius
                            : summary.candidates[index].maxPoleRadius);
    for (const auto& candidate : summary.responseCandidates)
        printCandidate ("selected-response", candidate, candidate.maxResponseMagnitude);
    for (const auto& candidate : summary.dotFourResponseCandidates)
        printCandidate ("selected-dot4-response", candidate,
                        candidate.maxResponseMagnitude);

    if (argc > 1 && std::string (argv[1]) == "--coeff-only")
        return EXIT_SUCCESS;

    characterizeIirTails (summary);
    characterizeFirSupport (summary);
    return EXIT_SUCCESS;
}

#include "MorphosisDSP.cpp"
