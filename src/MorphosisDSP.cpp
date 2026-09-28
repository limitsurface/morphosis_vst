#include "MorphosisDSP.h"
#include "MorphosisSequencer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace morphosis
{
namespace
{

constexpr std::uint32_t kMask = 0xffffffffu;
constexpr double kThresholdFloor = 1361505.0 / 2147483648.0;
constexpr double kSettledThresholdAtSettingZero = 2.0627455711364746;
#if defined(MORPHOSIS_TESTING)
std::uint64_t synchronousFirDesignCountForTesting = 0;
std::uint64_t getSynchronousFirDesignCountForTesting() noexcept
{
    return synchronousFirDesignCountForTesting;
}
#endif

float floatFromBits (std::uint32_t bits) noexcept
{
    float value = 0.0f;
    std::memcpy (&value, &bits, sizeof (value));
    return value;
}

float toFloat (double value) noexcept
{
    return static_cast<float> (value);
}

std::int64_t signedValue (std::uint32_t value, int bits) noexcept
{
    const auto mask = (std::uint64_t (1) << bits) - 1u;
    const auto clipped = static_cast<std::uint64_t> (value) & mask;
    const auto sign = std::uint64_t (1) << (bits - 1);
    return static_cast<std::int64_t> ((clipped & sign) != 0 ? clipped - (std::uint64_t (1) << bits)
                                                            : clipped);
}

std::uint32_t interpolationWeights (int coordinate) noexcept
{
    const auto q = std::clamp (coordinate, 0, 32768);
    return ((static_cast<std::uint32_t> (-q) & 0xffffu) << 16)
         | (static_cast<std::uint32_t> (q - 32768) & 0xffffu);
}

std::uint32_t dot (std::uint32_t a, std::uint32_t b) noexcept
{
    const auto low = signedValue (a, 16) * signedValue (b, 16);
    const auto high = signedValue (a >> 16, 16) * signedValue (b >> 16, 16);
    return static_cast<std::uint32_t> (static_cast<std::uint64_t> (low + high) & kMask);
}

std::uint32_t packStage (std::uint32_t a, std::uint32_t b) noexcept
{
    const auto signedA = signedValue (a, 32);
    const auto shifted = static_cast<std::int32_t> (signedA >> 15);
    return ((b << 1) & 0xffff0000u) | (static_cast<std::uint32_t> (shifted) & 0xffffu);
}

std::uint32_t interpolate (const std::array<std::uint32_t, 4>& words,
                           const std::array<int, 3>& coordinates) noexcept
{
    const auto w1 = interpolationWeights (coordinates[0]);
    const auto w4 = interpolationWeights (coordinates[1]);
    const auto w5 = interpolationWeights (coordinates[2]);
    const std::array<std::uint32_t, 4> d {
        dot (w1, words[0]), dot (w1, words[1]), dot (w1, words[2]), dot (w1, words[3])
    };
    const auto a = dot (w4, packStage (d[0], d[1]));
    const auto b = dot (w4, packStage (d[2], d[3]));
    return static_cast<std::uint32_t> (0u - dot (w5, packStage (a, b)));
}

std::uint32_t expandInterpolated (std::uint32_t value, bool angle) noexcept
{
    const auto exponent = (value >> 26) & 15u;
    const auto mantissa = (value >> 15) & 2047u;
    if (angle)
        return (mantissa | 2048u) << exponent;

    const auto nonzero = exponent != 0u ? 1u : 0u;
    return (mantissa | (nonzero << 11)) << (exponent - nonzero);
}

double numeric (const std::array<std::uint32_t, 4>& words,
               const std::array<int, 3>& coordinates,
               bool angle) noexcept
{
    const auto expanded = static_cast<float> (expandInterpolated (interpolate (words, coordinates), angle));
    return static_cast<double> (toFloat (static_cast<double> (expanded)
                                         * static_cast<double> (floatFromBits (angle ? 0x32c90fdbu
                                                                                       : 0x32800800u))));
}

double encodedCoordinate (const std::array<std::uint32_t, 4>& words,
                          const std::array<int, 3>& coordinates) noexcept
{
    // The firmware expansion consumes the upper 15 bits of this 30-bit
    // payload. Keep the lower 15 bits as the fractional part while blending
    // source cubes, then floor only after the weighted sum.
    return static_cast<double> (interpolate (words, coordinates) & 0x3fffffffU)
         / 32768.0;
}

double decodeEncodedCoordinate (double value,
                                bool angle,
                                std::uint32_t scaleBits) noexcept
{
    if (! std::isfinite (value))
        return 0.0;

    const auto code = std::clamp (static_cast<long long> (std::floor (value)),
                                  0ll, 32767ll);
    const auto reconstructed = static_cast<std::uint32_t> (code) << 15;
    const auto expanded = static_cast<float> (expandInterpolated (reconstructed, angle));
    return static_cast<double> (toFloat (
        static_cast<double> (expanded) * static_cast<double> (floatFromBits (scaleBits))));
}

std::array<std::uint32_t, 4> readField (const cube_data::CubeRecord& record,
                                        int offset) noexcept
{
    std::array<std::uint32_t, 4> words {};
    for (int i = 0; i < 4; ++i)
        words[static_cast<std::size_t> (i)] = record.words[static_cast<std::size_t> (offset + i)]
                                             & (i == 0 ? 0x7fffffffu : 0xffffffffu);
    return words;
}

std::array<int, 3> namedCoordinates (double frequency,
                                     double morph,
                                     double transform) noexcept
{
    const auto coordinate = [] (double value) noexcept
    {
        if (!std::isfinite (value))
            value = 0.0;
        value = std::clamp (value, -5.0, 5.0);
        return static_cast<int> (std::lround ((value + 5.0) * 32768.0 / 10.0));
    };

    // Register order is Transform, Frequency, Morph. These are effective CVs,
    // not claims about physical-pot calibration.
    return { coordinate (transform), coordinate (frequency), coordinate (morph) };
}

double firmwareCosine (double angle) noexcept
{
    const auto threshold = static_cast<double> (floatFromBits (0x3ec90fd8u));
    if (angle <= threshold)
        return 1.0 - angle * angle / 2.0;

    const auto x = static_cast<double> (floatFromBits (0x3fc90fdau));
    const auto distance = x - angle;
    return distance - distance * distance * distance * static_cast<double> (floatFromBits (0x3e1fd312u));
}

double firmwareSine (double angle) noexcept
{
    const auto threshold = static_cast<double> (floatFromBits (0x3ec90fd8u));
    if (angle <= threshold)
        return angle - angle * angle * angle
                     * (angle * static_cast<double> (floatFromBits (0x3b6e54ffu))
                        + static_cast<double> (floatFromBits (0x3dff9724u)));

    const auto cosine = firmwareCosine (angle);
    return std::sqrt (std::max (0.0, 1.0 - cosine * cosine));
}

void adaptPole (StageCoefficients& stage, double hostRate) noexcept
{
    const auto ratio = kFirmwareNominalRate / std::max (hostRate, 1.0);
    stage.radius = std::clamp (std::pow (std::max (stage.radius, 1.0e-12), ratio), 1.0e-9, 0.999999);
    stage.angle *= ratio;
    const auto cosine = std::cos (stage.angle);
    const auto sine = std::sin (stage.angle);
    stage.a = stage.radius * cosine;
    stage.b = stage.radius * sine;
    const auto dcTerm = 1.0 + stage.radius * stage.radius - 2.0 * stage.a;
    stage.inputGain = stage.normalizationExponent <= 0.0
                        ? 1.0
                        : stage.normalizationExponent >= 1.0
                            ? dcTerm
                            : std::pow (std::max (dcTerm, 1.0e-12),
                                        stage.normalizationExponent);

    if (stage.zeroRadius > 0.0)
    {
        stage.zeroRadius = std::clamp (std::pow (std::max (stage.zeroRadius, 1.0e-12), ratio), 0.0, 0.999999);
        stage.zeroAngle *= ratio;
        const auto zeroCosine = std::cos (stage.zeroAngle);
        stage.z1 = -2.0 * stage.zeroRadius * zeroCosine;
        stage.z2 = stage.zeroRadius * stage.zeroRadius;
    }
}

double normalizedInputGain (double radius, double a, double exponent) noexcept
{
    const auto dcTerm = 1.0 + radius * radius - 2.0 * a;
    if (exponent <= 0.0)
        return 1.0;
    if (exponent >= 1.0)
        return dcTerm;
    return std::pow (std::max (dcTerm, 1.0e-12), exponent);
}

double complexMagnitude (double real, double imaginary) noexcept
{
    return std::hypot (real, imaginary);
}

template <typename Value, std::size_t Size>
void fftFixed (std::array<std::complex<Value>, Size>& data, bool inverse) noexcept
{
    static_assert ((Size & (Size - 1)) == 0, "FFT size must be a power of two");
    for (std::size_t j = 1, i = 0; j < Size; ++j)
    {
        std::size_t bit = Size >> 1;
        for (; (i & bit) != 0; bit >>= 1)
            i ^= bit;
        i ^= bit;
        if (j < i)
            std::swap (data[j], data[i]);
    }

    constexpr Value pi = static_cast<Value> (3.1415926535897932384626433832795);
    for (std::size_t length = 2; length <= Size; length <<= 1)
    {
        const auto sign = inverse ? Value (1) : Value (-1);
        const auto angle = sign * Value (2) * pi / static_cast<Value> (length);
        const std::complex<Value> root (std::cos (angle), std::sin (angle));
        for (std::size_t start = 0; start < Size; start += length)
        {
            std::complex<Value> twiddle (1, 0);
            const auto half = length >> 1;
            for (std::size_t offset = 0; offset < half; ++offset)
            {
                const auto even = data[start + offset];
                const auto odd = twiddle * data[start + offset + half];
                data[start + offset] = even + odd;
                data[start + offset + half] = even - odd;
                twiddle *= root;
            }
        }
    }

    if (inverse)
        for (auto& value : data)
            value /= static_cast<Value> (Size);
}

double firResponseMagnitude (const std::array<float, kExperimentalFirTaps>& impulse,
                             double frequencyHz,
                             double hostRate) noexcept
{
    if (!std::isfinite (frequencyHz) || !std::isfinite (hostRate) || hostRate <= 0.0)
        return 0.0;

    const auto omega = 2.0 * 3.14159265358979323846
                     * std::clamp (frequencyHz, 0.0, hostRate * 0.5) / hostRate;
    const auto stepReal = std::cos (omega);
    const auto stepImaginary = -std::sin (omega);
    double phaseReal = 1.0;
    double phaseImaginary = 0.0;
    double real = 0.0;
    double imaginary = 0.0;
    for (int index = 0; index < kExperimentalFirTaps; ++index)
    {
        const auto coefficient = static_cast<double> (impulse[static_cast<std::size_t> (index)]);
        real += coefficient * phaseReal;
        imaginary += coefficient * phaseImaginary;
        const auto nextReal = phaseReal * stepReal - phaseImaginary * stepImaginary;
        phaseImaginary = phaseReal * stepImaginary + phaseImaginary * stepReal;
        phaseReal = nextReal;
    }
    return std::isfinite (real) && std::isfinite (imaginary)
             ? std::hypot (real, imaginary) : 0.0;
}

} // namespace

class MorphosisDSP::ExperimentalFirWorker final
{
public:
    ExperimentalFirWorker() = default;

    ~ExperimentalFirWorker()
    {
        stop();
    }

    void start() noexcept
    {
        if (running.load (std::memory_order_acquire) || worker.joinable())
            return;

        clearMailboxes();
        stopping.store (false, std::memory_order_release);
        try
        {
            worker = std::thread ([this] { run(); });
            running.store (true, std::memory_order_release);
        }
        catch (...)
        {
            stopping.store (true, std::memory_order_release);
            running.store (false, std::memory_order_release);
        }
    }

    void stop() noexcept
    {
        running.store (false, std::memory_order_release);
        stopping.store (true, std::memory_order_release);
        wake.notify_all();
        if (worker.joinable())
            worker.join();
        clearMailboxes();
    }

    bool submit (const FirBuildRequest& request) noexcept
    {
        if (! running.load (std::memory_order_acquire))
            return false;

        for (auto& slot : requests)
        {
            int expected = 0;
            if (! slot.state.compare_exchange_strong (expected, 1,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_relaxed))
                continue;

            slot.value = request;
            slot.state.store (2, std::memory_order_release);
            wake.notify_one();
            return true;
        }
        return false;
    }

    bool popLatest (FirBuildResult& destination) noexcept
    {
        int chosen = -1;
        std::uint64_t newest = 0;
        for (auto& slot : results)
        {
            if (slot.state.load (std::memory_order_acquire) != 2)
                continue;
            if (chosen < 0 || slot.value.generation >= newest)
            {
                chosen = static_cast<int> (&slot - results.data());
                newest = slot.value.generation;
            }
        }

        if (chosen < 0)
            return false;

        auto& selected = results[static_cast<std::size_t> (chosen)];
        int expected = 2;
        if (! selected.state.compare_exchange_strong (expected, 3,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_relaxed))
            return false;

        destination = selected.value;
        selected.state.store (0, std::memory_order_release);

        // A newer result is the only useful result. Drop older completed
        // designs so the bounded mailbox cannot turn into a stale backlog.
        for (auto& slot : results)
        {
            if (slot.state.load (std::memory_order_acquire) == 2
                && slot.value.generation <= destination.generation)
            {
                int ready = 2;
                slot.state.compare_exchange_strong (ready, 0,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed);
            }
        }
        return true;
    }

private:
    template <typename Payload>
    struct Slot
    {
        std::atomic<int> state { 0 }; // free, writing, ready, processing
        Payload value {};
    };

    static constexpr int kSlotCount = 3;

    std::array<Slot<FirBuildRequest>, kSlotCount> requests {};
    std::array<Slot<FirBuildResult>, kSlotCount> results {};
    std::atomic<bool> stopping { false };
    std::atomic<bool> running { false };
    std::condition_variable wake;
    std::mutex wakeMutex;
    std::thread worker;

    void clearMailboxes() noexcept
    {
        for (auto& slot : requests)
            slot.state.store (0, std::memory_order_release);
        for (auto& slot : results)
            slot.state.store (0, std::memory_order_release);
    }

    static bool claimLatestRequest (std::array<Slot<FirBuildRequest>, kSlotCount>& slots,
                                    FirBuildRequest& destination) noexcept
    {
        int chosen = -1;
        std::uint64_t newest = 0;
        for (auto& slot : slots)
        {
            if (slot.state.load (std::memory_order_acquire) != 2)
                continue;
            if (chosen < 0 || slot.value.generation >= newest)
            {
                chosen = static_cast<int> (&slot - slots.data());
                newest = slot.value.generation;
            }
        }

        if (chosen < 0)
            return false;
        auto& selected = slots[static_cast<std::size_t> (chosen)];
        int expected = 2;
        if (! selected.state.compare_exchange_strong (expected, 3,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_relaxed))
            return false;
        destination = selected.value;
        selected.state.store (0, std::memory_order_release);

        for (auto& slot : slots)
        {
            if (slot.state.load (std::memory_order_acquire) == 2
                && slot.value.generation <= destination.generation)
            {
                int ready = 2;
                slot.state.compare_exchange_strong (ready, 0,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed);
            }
        }
        return true;
    }

    bool storeResult (const FirBuildResult& result) noexcept
    {
        Slot<FirBuildResult>* selected = nullptr;
        for (auto& slot : results)
        {
            int expected = 0;
            if (slot.state.compare_exchange_strong (expected, 1,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed))
            {
                selected = &slot;
                break;
            }
        }

        if (selected == nullptr)
        {
            // Prefer replacing an older ready result. Processing slots are
            // never touched, so the audio consumer remains race-free.
            for (auto& slot : results)
            {
                int expected = 2;
                if (slot.state.compare_exchange_strong (expected, 1,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_relaxed))
                {
                    selected = &slot;
                    break;
                }
            }
        }

        if (selected == nullptr)
            return false;
        selected->value = result;
        selected->state.store (2, std::memory_order_release);
        return true;
    }

public:
    static FirBuildResult build (const FirBuildRequest& request) noexcept
    {
        FirBuildResult result;
        result.generation = request.generation;
        result.sampleRate = request.sampleRate;

#if defined (MORPHOSIS_STAGE8_PROFILE)
        auto stageStarted = std::chrono::steady_clock::now();
#endif
        const auto blend = MorphosisDSP::makeBilinearBlend (
            request.presets, request.nonlinearSources, request.x, request.y);
        std::array<CoefficientSet, kMaxBlendSources> sources {};
        for (int source = 0; source < blend.count; ++source)
        {
            sources[static_cast<std::size_t> (source)] = MorphosisDSP::makeCoefficients (
                blend.presets[static_cast<std::size_t> (source)],
                request.frequency, request.morph, request.transform, request.sampleRate);
        }
#if defined (MORPHOSIS_STAGE8_PROFILE)
        result.stage8Timing.coefficientMs = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - stageStarted).count();
        stageStarted = std::chrono::steady_clock::now();
#endif
        std::array<double, kExperimentalFirFftSize> logMagnitude {};
        // The lab's documented -140 dB floor is retained in the target.
        constexpr auto targetFloor = 1.0e-7;

        for (int bin = 0; bin < kExperimentalFirFftSize; ++bin)
        {
            const auto folded = bin <= kExperimentalFirFftSize / 2
                                  ? bin : kExperimentalFirFftSize - bin;
            double value = 0.0;
            for (int source = 0; source < blend.count; ++source)
            {
                const auto weight = blend.weights[static_cast<std::size_t> (source)];
                if (weight <= 0.0)
                    continue;
                const auto& coefficients = sources[static_cast<std::size_t> (source)];
                if (! coefficients.valid || coefficients.usedSafeFallback)
                {
                    result.usedSafeFallback = true;
                    result.valid = false;
                    return result;
                }
                const auto magnitude = MorphosisDSP::responseMagnitude (
                    coefficients,
                    request.sampleRate * static_cast<double> (folded)
                        / static_cast<double> (kExperimentalFirFftSize),
                    request.sampleRate);
                if (! std::isfinite (magnitude))
                {
                    result.usedSafeFallback = true;
                    return result;
                }
                value += weight * std::log (std::max (magnitude, targetFloor));
            }
            if (! std::isfinite (value))
            {
                result.usedSafeFallback = true;
                return result;
            }
            logMagnitude[static_cast<std::size_t> (bin)] = value;
        }

#if defined (MORPHOSIS_STAGE8_PROFILE)
        result.stage8Timing.targetGridMs = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - stageStarted).count();
        stageStarted = std::chrono::steady_clock::now();
#endif
        std::array<std::complex<double>, kExperimentalFirFftSize> logSpectrum {};
        std::array<std::complex<double>, kExperimentalFirFftSize> cepstrum {};
        for (int index = 0; index < kExperimentalFirFftSize; ++index)
            logSpectrum[static_cast<std::size_t> (index)] =
                { logMagnitude[static_cast<std::size_t> (index)], 0.0 };
        fftFixed (logSpectrum, true);
        cepstrum[0] = logSpectrum[0];
        for (int index = 1; index < kExperimentalFirFftSize / 2; ++index)
            cepstrum[static_cast<std::size_t> (index)] =
                2.0 * logSpectrum[static_cast<std::size_t> (index)];
        cepstrum[kExperimentalFirFftSize / 2] = logSpectrum[kExperimentalFirFftSize / 2];
        fftFixed (cepstrum, false);
        for (auto& value : cepstrum)
            value = std::exp (value);
        fftFixed (cepstrum, true);

        for (int index = 0; index < kExperimentalFirTaps; ++index)
        {
            const auto value = cepstrum[static_cast<std::size_t> (index)].real();
            if (! std::isfinite (value))
            {
                result.usedSafeFallback = true;
                return result;
            }
            result.impulse[static_cast<std::size_t> (index)] = static_cast<float> (value);
        }

#if defined (MORPHOSIS_STAGE8_PROFILE)
        result.stage8Timing.minimumPhaseMs = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - stageStarted).count();
        stageStarted = std::chrono::steady_clock::now();
#endif
        std::array<std::complex<double>, kExperimentalFirRuntimeFftSize> partitionFft {};
        for (int partition = 0; partition < kExperimentalFirPartitionCount; ++partition)
        {
            partitionFft.fill ({ 0.0, 0.0 });
            const auto offset = partition * kExperimentalFirPartitionSize;
            for (int index = 0; index < kExperimentalFirPartitionSize; ++index)
                partitionFft[static_cast<std::size_t> (index)] =
                    result.impulse[static_cast<std::size_t> (offset + index)];
            fftFixed (partitionFft, false);
            const auto destinationOffset = partition * kExperimentalFirRuntimeFftSize;
            for (int index = 0; index < kExperimentalFirRuntimeFftSize; ++index)
                result.partitions[static_cast<std::size_t> (destinationOffset + index)] =
                    { static_cast<float> (partitionFft[static_cast<std::size_t> (index)].real()),
                      static_cast<float> (partitionFft[static_cast<std::size_t> (index)].imag()) };
        }

#if defined (MORPHOSIS_STAGE8_PROFILE)
        result.stage8Timing.partitionMs = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - stageStarted).count();
        stageStarted = std::chrono::steady_clock::now();
#endif
        for (int index = 0; index < kExperimentalFirGraphPoints; ++index)
        {
            const auto proportion = index > 0
                                  ? static_cast<double> (index)
                                      / static_cast<double> (kExperimentalFirGraphPoints - 1)
                                  : 0.0;
            const auto frequency = 20.0 * std::pow (request.sampleRate * 0.5 / 20.0, proportion);
            const auto magnitude = std::max (firResponseMagnitude (result.impulse, frequency,
                                                                    request.sampleRate),
                                             targetFloor);
            result.graphResponseDb[static_cast<std::size_t> (index)] = static_cast<float> (
                std::clamp (20.0 * std::log10 (magnitude), -72.0, 24.0));
        }
#if defined (MORPHOSIS_STAGE8_PROFILE)
        result.stage8Timing.graphMs = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - stageStarted).count();
#endif
        result.valid = true;
        return result;
    }

private:
#if defined(MORPHOSIS_TESTING)
    std::atomic<std::uint64_t> buildCountForTesting { 0 };
#endif

    void run() noexcept
    {
        while (! stopping.load (std::memory_order_acquire))
        {
            FirBuildRequest request;
            if (! claimLatestRequest (requests, request))
            {
                std::unique_lock<std::mutex> lock (wakeMutex);
                wake.wait_for (lock, std::chrono::milliseconds (2), [this]
                {
                    return stopping.load (std::memory_order_acquire)
                        || std::any_of (requests.begin(), requests.end(), [] (const auto& slot)
                        {
                            return slot.state.load (std::memory_order_acquire) == 2;
                        });
                });
                continue;
            }
            if (stopping.load (std::memory_order_acquire))
                break;
            const auto started = std::chrono::steady_clock::now();
#if defined(MORPHOSIS_TESTING)
            buildCountForTesting.fetch_add (1, std::memory_order_relaxed);
#endif
            auto result = build (request);
            result.buildMilliseconds = std::chrono::duration<double, std::milli> (
                std::chrono::steady_clock::now() - started).count();
            if (! stopping.load (std::memory_order_acquire))
                storeResult (result);
        }
    }
};

#if defined (MORPHOSIS_STAGE8_PROFILE)
FirDesignStage8Profile MorphosisDSP::buildFirForStage8Profiling (
    const std::array<int, kMaxBlendSources>& presets,
    const std::array<bool, kMaxBlendSources>& nonlinearSources,
    double x,
    double y,
    double frequency,
    double morph,
    double transform,
    double hostRate) noexcept
{
    FirBuildRequest request;
    request.presets = presets;
    request.nonlinearSources = nonlinearSources;
    request.x = x;
    request.y = y;
    request.frequency = frequency;
    request.morph = morph;
    request.transform = transform;
    request.sampleRate = hostRate;

    const auto started = std::chrono::steady_clock::now();
    const auto result = ExperimentalFirWorker::build (request);
    FirDesignStage8Profile profile;
    profile.totalMs = std::chrono::duration<double, std::milli> (
        std::chrono::steady_clock::now() - started).count();
    profile.valid = result.valid;
    profile.usedSafeFallback = result.usedSafeFallback;
    profile.timing = result.stage8Timing;
    profile.impulse = result.impulse;
    profile.graphResponseDb = result.graphResponseDb;

    auto hash = std::uint64_t { 14695981039346656037ull };
    const auto mixWord = [&hash] (std::uint32_t word)
    {
        for (int byte = 0; byte < 4; ++byte)
        {
            hash ^= static_cast<std::uint8_t> (word >> (byte * 8));
            hash *= 1099511628211ull;
        }
    };
    for (const auto& value : result.partitions)
    {
        std::uint32_t realBits = 0;
        std::uint32_t imaginaryBits = 0;
        const auto real = value.real();
        const auto imaginary = value.imag();
        std::memcpy (&realBits, &real, sizeof (realBits));
        std::memcpy (&imaginaryBits, &imaginary, sizeof (imaginaryBits));
        mixWord (realBits);
        mixWord (imaginaryBits);
    }
    profile.partitionHash = hash;
    return profile;
}
#endif

MorphosisDSP::MorphosisDSP()
    : firResultScratch (std::make_unique<FirBuildResult>()),
      experimentalFirWorker (std::make_unique<ExperimentalFirWorker>())
{
}

MorphosisDSP::~MorphosisDSP() = default;

void FilterState::clear() noexcept
{
    for (auto& channel : values)
        for (auto& stage : channel)
            stage.fill (0.0);
}

void MorphosisDSP::Smoother::setTarget (double value, int samples) noexcept
{
    if (!std::isfinite (value))
        value = current;
    if (target == value)
        return;
    target = value;
    remaining = std::max (1, samples);
}

void MorphosisDSP::CoordinateSmoother::setImmediate (double value) noexcept
{
    current = std::isfinite (value) ? value : 0.0;
    target = current;
}

void MorphosisDSP::CoordinateSmoother::setTarget (double value) noexcept
{
    target = std::isfinite (value) ? value : current;
}

double MorphosisDSP::CoordinateSmoother::next (double amount, bool snapToTarget) noexcept
{
    const auto delta = target - current;
    current += delta * amount;
    if (snapToTarget && std::abs (target - current) <= std::max (amount, 1.0e-12))
        current = target;
    return current;
}

std::array<int, 3> MorphosisDSP::quantizedCoordinateKey (double frequency,
                                                          double morph,
                                                          double transform) noexcept
{
    return namedCoordinates (frequency, morph, transform);
}

double MorphosisDSP::Smoother::next() noexcept
{
    if (remaining > 0)
    {
        current += (target - current) / static_cast<double> (remaining);
        --remaining;
    }
    else
    {
        current = target;
    }
    return current;
}

void MorphosisDSP::prepare (double newSampleRate, int samplesPerBlock) noexcept
{
    if (experimentalFirWorker != nullptr)
        experimentalFirWorker->stop();

    sampleRate = std::isfinite (newSampleRate) && newSampleRate > 1000.0
                   ? newSampleRate : 48000.0;
    preparedBlockSize = std::max (0, samplesPerBlock);
    smoothingSamples = std::max (1, static_cast<int> (std::lround (sampleRate * kSmoothingMilliseconds / 1000.0)));
    controlSmoothingSamples = std::max (1, static_cast<int> (
        std::lround (sampleRate * kControlSmoothingMilliseconds / 1000.0)));
    reset();

    if (experimentalFirWorker != nullptr)
        experimentalFirWorker->start();
}

void MorphosisDSP::releaseResources() noexcept
{
    if (experimentalFirWorker != nullptr)
        experimentalFirWorker->stop();
    reset();
}

void MorphosisDSP::reset() noexcept
{
    filterState.clear();
    initialized = false;
    pendingReady = false;
    transitionPhase = TransitionPhase::none;
    filterMix = 1.0;
    sequenceHardwareTransition = false;
    transitionFadeSamples = 1;
    transitionRemaining = 0;
    requestedSequenceTransitionSamples = 1;
    currentCoefficients = {};
    pendingCoefficients = {};
    currentBlend = {};
    pendingBlend = {};
    resolvedBlend = {};
    resolvedBlendValid = false;
    pendingFrequency = 0.0;
    pendingMorph = 0.0;
    pendingTransform = 0.0;
    cachedCoordinateKey = {};
    cachedCoordinateKeyValid = false;
    cachedBlend = {};
    cachedBlendValid = false;
    processingMode = ProcessingMode::preset;
    pendingMode = ProcessingMode::preset;
    activeInternalDistortion = false;
    nonlinearRoutingSuppressed = false;
    transitionResetCount = 0;
    coefficientFallbackCount = 0;
    safetyResetCount = 0;
    frequencyControl.setImmediate (0.0);
    morphControl.setImmediate (0.0);
    transformControl.setImmediate (0.0);
    xyXControl.setImmediate (0.5);
    xyYControl.setImmediate (0.5);
    sequencePositionControl.setImmediate (0.0);
    xyPresets = { 0, 0, 0, 0 };
    xyNonlinearSources = { false, false, false, false };
    sequencePresets.fill (0);
    sequenceNonlinearSources.fill (false);
    sequenceLength = 4;
    sequenceManual = false;
    manualTransitionMs = 20.0;

    responseModeRequested = false;
    responseModeActive = false;
    responseModeWasRequested = false;
    nonRealtimeRendering = false;
    encodedDomainRequested = false;
    encodedSourceCacheCoordinateKey = {};
    encodedSourceCacheCoordinateKeyValid = false;
    for (auto& source : encodedSourceCache)
    {
        source = {};
        source.preset = -1;
    }
    experimentalAlgorithmMix = { 0.0, 0.0, 0 };
    firImpulse.fill (0.0f);
    firPartitions.fill ({ 0.0f, 0.0f });
    firTargetPartitions.fill ({ 0.0f, 0.0f });
    firGraphResponseDb.fill (0.0f);
    firImpulse[0] = 1.0f;
    firLastSubmittedKeyValid = false;
    firReady = false;
    firTransitionActive = false;
    firTransitionMix = 1.0;
    firTransitionRemaining = 0;
    firTransitionTotal = 1;
    firProcessedChannelMask = 0;
    ++firRequestGeneration;
    firAcceptedGeneration = 0;
    firBuildOutstanding = false;
    firOutstandingGeneration = 0;
    lastExperimentalFirBuildMilliseconds = 0.0;
    firRequestCountdown = 0;
    for (auto& channel : firChannels)
        channel = {};
    descriptorDelay = {};
    descriptorDelayWrite = { 0, 0 };
    inputDelay = {};
    inputDelayWrite = { 0, 0 };
    bypassDelay = {};
    bypassDelayWrite = { 0, 0 };
    ++graphRevision;
    inputGain = { 1.0, 1.0, 0 };
    preClipGain = { 1.0, 1.0, 0 };
    postClipGain = { 1.0, 1.0, 0 };
    dryWetMix = { 1.0, 1.0, 0 };
    clipMix = { 1.0, 1.0, 0 };
    nonlinearMix = { 0.0, 0.0, 0 };
    nonlinearThreshold = { settledThresholdForSetting (0), settledThresholdForSetting (0), 0 };
    if (safetyFault != nullptr)
        safetyFault->store (false, std::memory_order_relaxed);
}

void MorphosisDSP::setTelemetry (std::atomic<float>* newLeftPeak,
                                 std::atomic<float>* newRightPeak,
                                 std::atomic<bool>* newSafetyFault) noexcept
{
    leftPeak = newLeftPeak;
    rightPeak = newRightPeak;
    safetyFault = newSafetyFault;
}

CoefficientSet MorphosisDSP::makeCoefficients (int preset,
                                                double frequency,
                                                double morph,
                                                double transform,
                                                double hostRate) noexcept
{
    return adaptNativeDescriptor (makeNativeDescriptor (preset, frequency, morph, transform),
                                  hostRate);
}

#if defined(MORPHOSIS_TESTING)
CoefficientSet MorphosisDSP::adaptNativeDescriptorForTesting (
    const NativeDescriptorSet& descriptor, double hostRate) noexcept
{
    return adaptNativeDescriptor (descriptor, hostRate);
}
#endif

NativeDescriptorSet MorphosisDSP::makeNativeDescriptor (int preset,
                                                         double frequency,
                                                         double morph,
                                                         double transform) noexcept
{
    NativeDescriptorSet result;
    const auto safePreset = std::clamp (preset, 0, cube_data::kRecordCount - 1);
    const auto& record = cube_data::kCubes[static_cast<std::size_t> (safePreset)];
    const auto coordinates = quantizedCoordinateKey (frequency, morph, transform);

    for (int stageIndex = 0; stageIndex < kStageCount; ++stageIndex)
    {
        auto& stage = result.stages[static_cast<std::size_t> (stageIndex)];
        const auto offset = stageIndex * 16;
        const auto poleAngleWords = readField (record, offset);
        const auto poleRadiusWords = readField (record, offset + 4);
        const auto zeroAngleWords = readField (record, offset + 8);
        const auto zeroRadiusWords = readField (record, offset + 12);

        stage.poleAngle = numeric (poleAngleWords, coordinates, true);
        stage.poleRadius = static_cast<double> (
            toFloat (1.0 - numeric (poleRadiusWords, coordinates, false)));
        stage.normalizationExponent = (record.words[static_cast<std::size_t> (offset + 8)]
                                       & 0x80000000u) != 0
                                        ? 1.0 : 0.0;

        if (stageIndex < kStageCount - 1)
        {
            stage.zeroAngle = numeric (zeroAngleWords, coordinates, true);
            stage.zeroRadius = static_cast<double> (
                toFloat (1.0 - numeric (zeroRadiusWords, coordinates, false)));
        }
    }

    const auto gainWords = std::array<std::uint32_t, 4> {
        record.words[112], record.words[113], record.words[114], record.words[115]
    };
    // The global gain uses the expanded interpolation without the radius
    // decay scale, then applies the recovered float32 normalization constant.
    const auto expandedGain = static_cast<float> (expandInterpolated (interpolate (gainWords, coordinates), false));
    result.gain = static_cast<double> (toFloat (static_cast<double> (expandedGain)
                                                * static_cast<double> (floatFromBits (0x338007ffu))));
    result.valid = finiteNativeDescriptor (result);
    if (!result.valid)
    {
        result.gain = 1.0;
        result.usedSafeFallback = true;
    }
    return result;
}

CoefficientSet MorphosisDSP::makeNeutralCoefficients() noexcept
{
    CoefficientSet result;
    result.gain = 1.0;
    result.valid = true;
    result.usedSafeFallback = true;
    result.neutralBypass = true;
    for (auto& stage : result.stages)
    {
        stage.a = 0.0;
        stage.b = 1.0e-6;
        stage.inputGain = 1.0;
        stage.z1 = 0.0;
        stage.z2 = 0.0;
        stage.radius = 0.0;
        stage.angle = 0.0;
        stage.zeroRadius = 0.0;
        stage.zeroAngle = 0.0;
        stage.normalizationExponent = 0.0;
        stage.normalizeDc = false;
    }
    return result;
}

CoefficientSet MorphosisDSP::adaptNativeDescriptor (const NativeDescriptorSet& descriptor,
                                                    double hostRate) noexcept
{
    CoefficientSet result;
    result.gain = descriptor.gain;
    result.usedSafeFallback = descriptor.usedSafeFallback;

    if (! descriptor.valid || descriptor.usedSafeFallback
        || ! std::isfinite (descriptor.gain) || descriptor.gain < 0.0)
        return makeNeutralCoefficients();

    for (std::size_t index = 0; index < result.stages.size(); ++index)
    {
        const auto& native = descriptor.stages[index];
        auto& stage = result.stages[index];
        stage.angle = native.poleAngle;
        stage.radius = std::clamp (native.poleRadius, 1.0e-9, 0.999999);
        stage.normalizationExponent = std::clamp (native.normalizationExponent, 0.0, 1.0);
        stage.normalizeDc = stage.normalizationExponent >= 0.5;
        stage.a = stage.radius * firmwareCosine (stage.angle);
        stage.b = stage.radius * firmwareSine (stage.angle);
        stage.inputGain = normalizedInputGain (stage.radius, stage.a,
                                               stage.normalizationExponent);
        stage.zeroAngle = native.zeroAngle;
        stage.zeroRadius = std::clamp (native.zeroRadius, 0.0, 0.999999);
        const auto zeroCosine = firmwareCosine (stage.zeroAngle);
        stage.z1 = -2.0 * stage.zeroRadius * zeroCosine;
        stage.z2 = stage.zeroRadius * stage.zeroRadius;

        if (std::isfinite (hostRate) && hostRate > 1000.0
            && std::abs (hostRate - kFirmwareNominalRate) > 0.01)
            adaptPole (stage, hostRate);
    }

    result.valid = finiteCoefficientSet (result);
    if (! result.valid)
        return makeNeutralCoefficients();
    return result;
}

CoefficientSet MorphosisDSP::makeBlendedCoefficients (const MorphBlend& rawBlend,
                                                       double frequency,
                                                       double morph,
                                                       double transform,
                                                       double hostRate) noexcept
{
    const auto blend = sanitizeBlend (rawBlend);
    int activeCount = 0;
    int firstPreset = 0;
    bool allSamePreset = true;
    bool firstNonlinear = false;
    for (int index = 0; index < blend.count; ++index)
    {
        if (blend.weights[static_cast<std::size_t> (index)] <= 0.0)
            continue;
        if (activeCount == 0)
        {
            firstPreset = blend.presets[static_cast<std::size_t> (index)];
            firstNonlinear = blend.nonlinearSources[static_cast<std::size_t> (index)];
        }
        else if (blend.presets[static_cast<std::size_t> (index)] != firstPreset
                 || blend.nonlinearSources[static_cast<std::size_t> (index)] != firstNonlinear)
        {
            allSamePreset = false;
        }
        ++activeCount;
    }

    if (activeCount == 1 || allSamePreset)
        return makeCoefficients (firstPreset, frequency, morph, transform, hostRate);

    NativeDescriptorSet blended;
    blended.gain = 0.0;
    blended.valid = false;
    blended.usedSafeFallback = false;
    // NativeStageDescriptor has useful nonzero defaults for standalone
    // descriptor construction. Blend accumulators need additive identities.
    for (auto& stage : blended.stages)
    {
        stage.poleAngle = 0.0;
        stage.poleRadius = 0.0;
        stage.zeroAngle = 0.0;
        stage.zeroRadius = 0.0;
        stage.normalizationExponent = 0.0;
    }
    double logGain = 0.0;
    for (int index = 0; index < blend.count; ++index)
    {
        const auto weight = blend.weights[static_cast<std::size_t> (index)];
        if (weight <= 0.0)
            continue;

        const auto descriptor = makeNativeDescriptor (
            blend.presets[static_cast<std::size_t> (index)], frequency, morph, transform);
        if (! descriptor.valid || descriptor.usedSafeFallback
            || ! std::isfinite (descriptor.gain) || descriptor.gain <= 0.0)
        {
            blended.valid = false;
            blended.usedSafeFallback = true;
            return adaptNativeDescriptor (blended, hostRate);
        }

        logGain += weight * std::log (descriptor.gain);
        for (std::size_t stageIndex = 0; stageIndex < blended.stages.size(); ++stageIndex)
        {
            const auto& source = descriptor.stages[stageIndex];
            auto& destination = blended.stages[stageIndex];
            destination.poleAngle += weight * source.poleAngle;
            destination.poleRadius += weight * source.poleRadius;
            destination.zeroAngle += weight * source.zeroAngle;
            destination.zeroRadius += weight * source.zeroRadius;
            destination.normalizationExponent += weight * source.normalizationExponent;
        }
    }

    // The weights are normalized, but a convex endpoint can still round a
    // bounded field one ulp outside its legal interval. Keep that numerical
    // noise from converting an otherwise valid blend into a fallback.
    for (auto& stage : blended.stages)
    {
        stage.poleRadius = std::clamp (stage.poleRadius, 0.0,
                                       std::nextafter (1.0, 0.0));
        stage.zeroRadius = std::clamp (stage.zeroRadius, 0.0,
                                       std::nextafter (1.0, 0.0));
        stage.normalizationExponent = std::clamp (stage.normalizationExponent, 0.0, 1.0);
    }
    blended.gain = std::exp (logGain);
    blended.valid = finiteNativeDescriptor (blended);
    if (! blended.valid)
        blended.usedSafeFallback = true;
    return adaptNativeDescriptor (blended, hostRate);
}

bool MorphosisDSP::populateEncodedSourceCache (EncodedSourceCache& destination,
                                               int preset,
                                               const std::array<int, 3>& coordinates) noexcept
{
    destination = {};
    destination.preset = std::clamp (preset, 0, cube_data::kRecordCount - 1);
    const auto& record = cube_data::kCubes[static_cast<std::size_t> (destination.preset)];

    for (int stageIndex = 0; stageIndex < kStageCount; ++stageIndex)
    {
        const auto stageOffset = stageIndex * 16;
        const auto safeStage = static_cast<std::size_t> (stageIndex);
        destination.poleAngleQ[safeStage] = encodedCoordinate (
            readField (record, stageOffset), coordinates);
        destination.poleRadiusQ[safeStage] = encodedCoordinate (
            readField (record, stageOffset + 4), coordinates);
        destination.normalizationFlags[safeStage] =
            (record.words[static_cast<std::size_t> (stageOffset + 8)] & 0x80000000u) != 0;

        if (stageIndex < kStageCount - 1)
        {
            destination.zeroAngleQ[safeStage] = encodedCoordinate (
                readField (record, stageOffset + 8), coordinates);
            destination.zeroRadiusQ[safeStage] = encodedCoordinate (
                readField (record, stageOffset + 12), coordinates);
        }
    }

    const auto gainWords = std::array<std::uint32_t, 4> {
        record.words[112], record.words[113], record.words[114], record.words[115]
    };
    destination.gainQ = encodedCoordinate (gainWords, coordinates);
    destination.valid = std::isfinite (destination.gainQ);
    for (int stageIndex = 0; stageIndex < kStageCount && destination.valid; ++stageIndex)
    {
        const auto safeStage = static_cast<std::size_t> (stageIndex);
        destination.valid = std::isfinite (destination.poleAngleQ[safeStage])
                         && std::isfinite (destination.poleRadiusQ[safeStage])
                         && std::isfinite (destination.zeroAngleQ[safeStage])
                         && std::isfinite (destination.zeroRadiusQ[safeStage]);
    }
    return destination.valid;
}

CoefficientSet MorphosisDSP::makeEncodedDomainCoefficientsFromCache (
    const MorphBlend& rawBlend,
    const std::array<EncodedSourceCache, kMaxBlendSources>& sources,
    double frequency,
    double morph,
    double transform,
    double hostRate) noexcept
{
    const auto blend = sanitizeBlend (rawBlend);
    int activeCount = 0;
    int firstPreset = 0;
    bool allSamePreset = true;
    for (int index = 0; index < blend.count; ++index)
    {
        const auto weight = blend.weights[static_cast<std::size_t> (index)];
        if (weight <= 0.0)
            continue;
        const auto preset = blend.presets[static_cast<std::size_t> (index)];
        if (activeCount == 0)
            firstPreset = preset;
        else if (preset != firstPreset)
            allSamePreset = false;
        ++activeCount;
    }

    // One-hot and repeated-source identities deliberately use the ordinary
    // decoder, preserving exact corner behavior and avoiding a fake interior
    // discontinuity at a duplicate source.
    if (activeCount <= 1 || allSamePreset)
        return makeCoefficients (firstPreset, frequency, morph, transform, hostRate);

    std::array<int, kMaxBlendSources> uniquePresets {};
    std::array<int, kMaxBlendSources> uniqueSourceIndices {};
    std::array<double, kMaxBlendSources> uniqueWeights {};
    int uniqueCount = 0;
    for (int index = 0; index < blend.count; ++index)
    {
        const auto weight = blend.weights[static_cast<std::size_t> (index)];
        if (weight <= 0.0)
            continue;

        const auto preset = blend.presets[static_cast<std::size_t> (index)];
        int uniqueIndex = -1;
        for (int candidate = 0; candidate < uniqueCount; ++candidate)
            if (uniquePresets[static_cast<std::size_t> (candidate)] == preset)
                uniqueIndex = candidate;

        if (uniqueIndex < 0)
        {
            uniqueIndex = uniqueCount++;
            uniquePresets[static_cast<std::size_t> (uniqueIndex)] = preset;
            uniqueSourceIndices[static_cast<std::size_t> (uniqueIndex)] = index;
        }
        uniqueWeights[static_cast<std::size_t> (uniqueIndex)] += weight;
    }

    NativeDescriptorSet native;
    native.gain = 0.0;
    native.valid = false;
    native.usedSafeFallback = false;
    for (auto& stage : native.stages)
    {
        stage.poleAngle = 0.0;
        stage.poleRadius = 0.0;
        stage.zeroAngle = 0.0;
        stage.zeroRadius = 0.0;
        stage.normalizationExponent = 0.0;
    }

    for (int uniqueIndex = 0; uniqueIndex < uniqueCount; ++uniqueIndex)
    {
        const auto sourceIndex = static_cast<std::size_t> (
            uniqueSourceIndices[static_cast<std::size_t> (uniqueIndex)]);
        if (! sources[sourceIndex].valid)
        {
            native.usedSafeFallback = true;
            return makeNeutralCoefficients();
        }

        const auto weight = uniqueWeights[static_cast<std::size_t> (uniqueIndex)];
        const auto& source = sources[sourceIndex];
        for (int stageIndex = 0; stageIndex < kStageCount; ++stageIndex)
        {
            const auto safeStage = static_cast<std::size_t> (stageIndex);
            auto& destination = native.stages[safeStage];
            destination.poleAngle += weight * source.poleAngleQ[safeStage];
            destination.poleRadius += weight * source.poleRadiusQ[safeStage];
            destination.zeroAngle += weight * source.zeroAngleQ[safeStage];
            destination.zeroRadius += weight * source.zeroRadiusQ[safeStage];
            destination.normalizationExponent += weight * static_cast<double> (
                source.normalizationFlags[safeStage]);
        }
        native.gain += weight * source.gainQ;
    }

    for (int stageIndex = 0; stageIndex < kStageCount; ++stageIndex)
    {
        auto& stage = native.stages[static_cast<std::size_t> (stageIndex)];
        stage.poleAngle = decodeEncodedCoordinate (stage.poleAngle, true, 0x32c90fdbu);
        stage.poleRadius = 1.0 - decodeEncodedCoordinate (
            stage.poleRadius, false, 0x32800800u);
        if (stageIndex < kStageCount - 1)
        {
            stage.zeroAngle = decodeEncodedCoordinate (stage.zeroAngle, true, 0x32c90fdbu);
            stage.zeroRadius = 1.0 - decodeEncodedCoordinate (
                stage.zeroRadius, false, 0x32800800u);
        }
        else
        {
            stage.zeroAngle = 0.0;
            stage.zeroRadius = 0.0;
        }
        // The weighted flag exponent is a convex quantity. Clamp only
        // floating-point endpoint noise before native validation; do not
        // renormalize or otherwise alter the encoded-domain blend.
        stage.normalizationExponent = std::clamp (
            stage.normalizationExponent, 0.0, 1.0);
    }
    native.gain = decodeEncodedCoordinate (native.gain, false, 0x338007ffu);
    native.valid = finiteNativeDescriptor (native);
    if (! native.valid)
        native.usedSafeFallback = true;
    return adaptNativeDescriptor (native, hostRate);
}

CoefficientSet MorphosisDSP::makeEncodedDomainCoefficients (const MorphBlend& rawBlend,
                                                             double frequency,
                                                             double morph,
                                                             double transform,
                                                             double hostRate) noexcept
{
    const auto blend = sanitizeBlend (rawBlend);
    const auto coordinates = quantizedCoordinateKey (frequency, morph, transform);
    std::array<EncodedSourceCache, kMaxBlendSources> sources {};
    for (int index = 0; index < blend.count; ++index)
        populateEncodedSourceCache (sources[static_cast<std::size_t> (index)],
                                    blend.presets[static_cast<std::size_t> (index)],
                                    coordinates);
    return makeEncodedDomainCoefficientsFromCache (blend, sources, frequency, morph,
                                                   transform, hostRate);
}

CoefficientSet MorphosisDSP::makeEncodedDomainCoefficientsCached (
    const MorphBlend& rawBlend,
    double frequency,
    double morph,
    double transform,
    double hostRate) noexcept
{
    const auto blend = sanitizeBlend (rawBlend);
    const auto coordinates = quantizedCoordinateKey (frequency, morph, transform);
    if (! encodedSourceCacheCoordinateKeyValid
        || coordinates != encodedSourceCacheCoordinateKey)
    {
        encodedSourceCacheCoordinateKey = coordinates;
        encodedSourceCacheCoordinateKeyValid = true;
        for (auto& source : encodedSourceCache)
            source.valid = false;
    }

    for (int index = 0; index < blend.count; ++index)
    {
        auto& source = encodedSourceCache[static_cast<std::size_t> (index)];
        const auto preset = blend.presets[static_cast<std::size_t> (index)];
        if (! source.valid || source.preset != preset)
            populateEncodedSourceCache (source, preset, coordinates);
    }
    return makeEncodedDomainCoefficientsFromCache (blend, encodedSourceCache,
                                                   frequency, morph, transform, hostRate);
}

MorphBlend MorphosisDSP::makeBilinearBlend (
    const std::array<int, kMaxBlendSources>& presets,
    const std::array<bool, kMaxBlendSources>& nonlinearSources,
    double x,
    double y) noexcept
{
    const auto clampedX = std::clamp (std::isfinite (x) ? x : 0.0, 0.0, 1.0);
    const auto clampedY = std::clamp (std::isfinite (y) ? y : 0.0, 0.0, 1.0);
    MorphBlend blend;
    blend.count = 4;
    blend.presets = presets;
    blend.nonlinearSources = nonlinearSources;
    blend.weights = { (1.0 - clampedX) * clampedY,
                      clampedX * clampedY,
                      (1.0 - clampedX) * (1.0 - clampedY),
                      clampedX * (1.0 - clampedY) };
    return sanitizeBlend (blend);
}

MorphBlend MorphosisDSP::makeAdjacentBlend (
    const std::array<int, kSequenceSlotCount>& presets,
    const std::array<bool, kSequenceSlotCount>& nonlinearSources,
    int length,
    double position) noexcept
{
    const auto safeLength = std::clamp (length, 1, kSequenceSlotCount);
    const auto clampedPosition = std::clamp (std::isfinite (position) ? position : 0.0,
                                             0.0, 1.0);
    MorphBlend blend;
    blend.presets[0] = presets[0];
    blend.nonlinearSources[0] = nonlinearSources[0];
    if (safeLength == 1 || clampedPosition >= 1.0)
    {
        const auto index = safeLength - 1;
        blend.presets[0] = presets[static_cast<std::size_t> (index)];
        blend.nonlinearSources[0] = nonlinearSources[static_cast<std::size_t> (index)];
        blend.weights[0] = 1.0;
        blend.count = 1;
        return blend;
    }

    const auto scaled = clampedPosition * static_cast<double> (safeLength - 1);
    const auto first = std::clamp (static_cast<int> (std::floor (scaled)), 0, safeLength - 1);
    const auto second = std::min (first + 1, safeLength - 1);
    const auto fraction = scaled - static_cast<double> (first);
    blend.count = first == second ? 1 : 2;
    blend.presets[0] = presets[static_cast<std::size_t> (first)];
    blend.nonlinearSources[0] = nonlinearSources[static_cast<std::size_t> (first)];
    blend.weights[0] = 1.0 - fraction;
    if (blend.count == 2)
    {
        blend.presets[1] = presets[static_cast<std::size_t> (second)];
        blend.nonlinearSources[1] = nonlinearSources[static_cast<std::size_t> (second)];
        blend.weights[1] = fraction;
    }
    return sanitizeBlend (blend);
}

int MorphosisDSP::discreteSequenceSlot (double position) noexcept
{
    return sequenceStepFromPosition (position);
}

MorphBlend MorphosisDSP::makeDiscreteSequenceBlend (
    const std::array<int, kSequenceSlotCount>& presets,
    const std::array<bool, kSequenceSlotCount>& nonlinearSources,
    double position) noexcept
{
    MorphBlend result;
    const auto slot = discreteSequenceSlot (position);
    result.presets[0] = presets[static_cast<std::size_t> (slot)];
    result.nonlinearSources[0] = nonlinearSources[static_cast<std::size_t> (slot)];
    result.weights[0] = 1.0;
    result.count = 1;
    return result;
}

bool MorphosisDSP::finiteCoefficientSet (const CoefficientSet& coefficients) noexcept
{
    if (!std::isfinite (coefficients.gain) || coefficients.gain < 0.0)
        return false;
    if (coefficients.neutralBypass)
        return true;
    for (const auto& stage : coefficients.stages)
    {
        if (!std::isfinite (stage.a) || !std::isfinite (stage.b)
            || !std::isfinite (stage.inputGain) || !std::isfinite (stage.z1)
            || !std::isfinite (stage.z2) || !std::isfinite (stage.radius)
            || !std::isfinite (stage.normalizationExponent)
            || stage.normalizationExponent < 0.0 || stage.normalizationExponent > 1.0
            || std::abs (stage.b) < 1.0e-12 || std::hypot (stage.a, stage.b) >= 1.0)
            return false;
    }
    return true;
}

bool MorphosisDSP::finiteNativeDescriptor (const NativeDescriptorSet& descriptor) noexcept
{
    if (! std::isfinite (descriptor.gain) || descriptor.gain < 0.0)
        return false;
    for (const auto& stage : descriptor.stages)
    {
        if (! std::isfinite (stage.poleAngle) || ! std::isfinite (stage.poleRadius)
            || ! std::isfinite (stage.zeroAngle) || ! std::isfinite (stage.zeroRadius)
            || ! std::isfinite (stage.normalizationExponent)
            || stage.poleRadius < 0.0 || stage.poleRadius >= 1.0
            || stage.zeroRadius < 0.0 || stage.zeroRadius >= 1.0
            || stage.normalizationExponent < 0.0 || stage.normalizationExponent > 1.0)
            return false;
    }
    return true;
}

MorphBlend MorphosisDSP::sanitizeBlend (const MorphBlend& rawBlend) noexcept
{
    MorphBlend result;
    result.count = std::clamp (rawBlend.count, 1, kMaxBlendSources);
    double total = 0.0;
    int writeIndex = 0;
    for (int index = 0; index < result.count; ++index)
    {
        const auto rawWeight = rawBlend.weights[static_cast<std::size_t> (index)];
        if (! std::isfinite (rawWeight) || rawWeight <= 0.0)
            continue;

        const auto destination = static_cast<std::size_t> (writeIndex++);
        result.presets[destination] = std::clamp (
            rawBlend.presets[static_cast<std::size_t> (index)], 0, cube_data::kRecordCount - 1);
        result.weights[destination] = rawWeight;
        result.nonlinearSources[destination] =
            rawBlend.nonlinearSources[static_cast<std::size_t> (index)];
        total += rawWeight;
    }

    if (writeIndex == 0 || ! std::isfinite (total) || total <= 0.0)
        return result;

    result.count = writeIndex;
    for (int index = 0; index < result.count; ++index)
        result.weights[static_cast<std::size_t> (index)] /= total;
    return result;
}

bool MorphosisDSP::blendsEqual (const MorphBlend& left, const MorphBlend& right) noexcept
{
    if (left.count != right.count)
        return false;
    for (int index = 0; index < left.count; ++index)
    {
        const auto i = static_cast<std::size_t> (index);
        if (left.presets[i] != right.presets[i]
            || left.nonlinearSources[i] != right.nonlinearSources[i]
            || left.weights[i] != right.weights[i])
            return false;
    }
    return true;
}

double MorphosisDSP::nonlinearMixForBlend (const MorphBlend& rawBlend) noexcept
{
    const auto blend = sanitizeBlend (rawBlend);
    double result = 0.0;
    for (int index = 0; index < blend.count; ++index)
        if (blend.nonlinearSources[static_cast<std::size_t> (index)])
            result += blend.weights[static_cast<std::size_t> (index)];
    return std::clamp (std::isfinite (result) ? result : 0.0, 0.0, 1.0);
}

void MorphosisDSP::setResolvedBlend (const MorphBlend& blend) noexcept
{
    resolvedBlend = sanitizeBlend (blend);
    resolvedBlendValid = true;
    if (initialized && processingMode == ProcessingMode::sequencer && ! sequenceManual)
        requestSequenceBlend (resolvedBlend, requestedSequenceTransitionSamples);
}

void MorphosisDSP::setSequenceTransitionDurationSamples (int samples) noexcept
{
    requestedSequenceTransitionSamples = std::max (1, samples);
}

void MorphosisDSP::setSequenceTarget (int slot, bool nonlinear, int transitionSamples) noexcept
{
    MorphBlend target;
    target.presets[0] = std::clamp (slot, 0, cube_data::kRecordCount - 1);
    target.nonlinearSources[0] = nonlinear;
    target.weights[0] = 1.0;
    target.count = 1;
    requestedSequenceTransitionSamples = std::max (1, transitionSamples);
    resolvedBlend = target;
    resolvedBlendValid = true;
    if (initialized && processingMode == ProcessingMode::sequencer)
        requestSequenceBlend (target, requestedSequenceTransitionSamples);
}

void MorphosisDSP::requestSequenceBlend (const MorphBlend& rawTarget,
                                         int transitionSamples) noexcept
{
    const auto target = sanitizeBlend (rawTarget);
    const auto totalSamples = std::max (2, transitionSamples);
    const auto halfSamples = std::max (1, totalSamples / 2);

    if (processingMode == ProcessingMode::sequencer
        && ! pendingReady
        && blendsEqual (currentBlend, target))
        return;

    if (pendingReady && pendingMode == ProcessingMode::sequencer
        && blendsEqual (pendingBlend, target))
        return;

    // A rapid retarget back to the currently active slot cancels the pending
    // destination instead of swapping to an identical filter and clearing its
    // state a second time. Reverse the audible bridge from its current mix.
    if (transitionPhase == TransitionPhase::fadeOut
        && pendingReady && pendingMode == ProcessingMode::sequencer
        && blendsEqual (currentBlend, target))
    {
        pendingReady = false;
        sequenceHardwareTransition = true;
        transitionFadeSamples = halfSamples;
        const auto remainingWet = 1.0 - std::clamp (filterMix, 0.0, 1.0);
        if (remainingWet <= 0.0)
        {
            transitionPhase = TransitionPhase::none;
            sequenceHardwareTransition = false;
            filterMix = 1.0;
            transitionRemaining = 0;
        }
        else
        {
            transitionPhase = TransitionPhase::fadeIn;
            transitionRemaining = std::max (1, static_cast<int> (
                std::ceil (remainingWet * static_cast<double> (halfSamples))));
        }
        return;
    }

    const auto candidate = makeBlendedCoefficients (target,
                                                     frequencyControl.current,
                                                     morphControl.current,
                                                     transformControl.current,
                                                     sampleRate);
    if (! candidate.valid || candidate.usedSafeFallback)
    {
        noteCoefficientFallback();
        return;
    }

    pendingCoefficients = candidate;
    pendingBlend = target;
    pendingFrequency = frequencyControl.current;
    pendingMorph = morphControl.current;
    pendingTransform = transformControl.current;
    pendingPreset = target.presets[0];
    pendingMode = ProcessingMode::sequencer;
    pendingReady = true;
    sequenceHardwareTransition = true;
    transitionFadeSamples = halfSamples;

    if (transitionPhase == TransitionPhase::none)
    {
        filterMix = std::clamp (filterMix, 0.0, 1.0);
        transitionRemaining = std::max (1, halfSamples);
        transitionPhase = TransitionPhase::fadeOut;
    }
    else if (transitionPhase == TransitionPhase::fadeIn)
    {
        // The new destination wins, but the currently audible filter first
        // returns toward dry from its present mix. Do not restart a complete
        // fade-out from one, which is the source of audible envelope jumps.
        transitionRemaining = std::max (1, static_cast<int> (
            std::ceil (std::clamp (filterMix, 0.0, 1.0)
                      * static_cast<double> (halfSamples))));
        transitionPhase = TransitionPhase::fadeOut;
    }
    // During fade-out the remaining countdown is deliberately preserved.
}

void MorphosisDSP::beginBlock (const ParameterSnapshot& rawParameters,
                               bool nonRealtime) noexcept
{
    if (nonRealtimeRendering != nonRealtime)
    {
        nonRealtimeRendering = nonRealtime;
        ++firRequestGeneration;
        firBuildOutstanding = false;
        firOutstandingGeneration = 0;
        firRequestCountdown = 0;
        if (nonRealtimeRendering)
            firLastSubmittedKeyValid = false;
    }

    auto parameters = rawParameters;
    parameters.preset = std::clamp (parameters.preset, 0, cube_data::kRecordCount - 1);
    parameters.frequency = clampFinite (parameters.frequency, 0.0, -5.0, 5.0);
    parameters.morph = clampFinite (parameters.morph, 0.0, -5.0, 5.0);
    parameters.transform = clampFinite (parameters.transform, 0.0, -5.0, 5.0);
    parameters.inputGainDb = clampFinite (parameters.inputGainDb, 0.0, -60.0, 24.0);
    parameters.preClipGainDb = clampFinite (parameters.preClipGainDb, 0.0, -60.0, 24.0);
    parameters.postClipGainDb = clampFinite (parameters.postClipGainDb, 0.0, -60.0, 24.0);
    parameters.dryWet = clampFinite (parameters.dryWet, 1.0, 0.0, 1.0);
    parameters.manualTransitionMs = clampFinite (parameters.manualTransitionMs, 20.0, 5.0, 250.0);
    parameters.internalThresholdSetting = std::clamp (parameters.internalThresholdSetting, -24, 60);
    parameters.xyX = clampFinite (parameters.xyX, 0.5, 0.0, 1.0);
    parameters.xyY = clampFinite (parameters.xyY, 0.5, 0.0, 1.0);
    parameters.sequenceLength = std::clamp (parameters.sequenceLength, 1, kSequenceSlotCount);
    parameters.sequencePosition = clampFinite (parameters.sequencePosition, 0.0, 0.0, 1.0);
    parameters.sequenceGlide = clampFinite (parameters.sequenceGlide, 1.0, 0.0, 1.0);
    if (parameters.xyInterpolation != XYInterpolationMode::descriptor
        && parameters.xyInterpolation != XYInterpolationMode::response)
        parameters.xyInterpolation = XYInterpolationMode::descriptor;

    if (parameters.mode != ProcessingMode::preset
        && parameters.mode != ProcessingMode::xy
        && parameters.mode != ProcessingMode::sequencer)
        parameters.mode = ProcessingMode::preset;

    const auto previousMode = processingMode;
    const auto previousXyPresets = xyPresets;
    const auto previousXyNonlinearSources = xyNonlinearSources;
    const auto previousSequencePresets = sequencePresets;
    const auto previousSequenceNonlinearSources = sequenceNonlinearSources;
    const auto previousSequenceLength = sequenceLength;
    const auto previousSequenceManual = sequenceManual;
    const auto previousEncodedDomain = encodedDomainRequested;
    encodedDomainRequested = parameters.xyEncodedDomain;

    const auto makeSingleBlend = [this] (int preset, bool nonlinear) noexcept
    {
        MorphBlend result;
        result.presets[0] = std::clamp (preset, 0, cube_data::kRecordCount - 1);
        result.weights[0] = 1.0;
        result.nonlinearSources[0] = nonlinear;
        result.count = 1;
        return result;
    };

    auto parameterBlend = sanitizeBlend (parameters.blend);
    if (parameters.mode == ProcessingMode::preset)
        parameterBlend = makeSingleBlend (parameters.preset, parameters.internalDistortion);

    if (parameters.mode == ProcessingMode::xy)
    {
        xyPresets = parameters.xyPresets;
        xyNonlinearSources = parameters.xyNonlinearSources;
        xyXControl.setTarget (parameters.xyX);
        xyYControl.setTarget (parameters.xyY);
        resolvedBlendValid = false;
    }
    else if (parameters.mode == ProcessingMode::sequencer)
    {
        sequencePresets = parameters.sequencePresets;
        sequenceNonlinearSources = parameters.sequenceNonlinearSources;
        // Manual Position always traverses all sixteen destinations. Keep the
        // stored length intact in ParameterSnapshot for HOST mode/state recall.
        sequenceLength = parameters.sequenceManual ? kSequenceSlotCount
                                                   : parameters.sequenceLength;
        sequenceManual = parameters.sequenceManual;
        manualTransitionMs = parameters.manualTransitionMs;
        sequencePositionControl.setTarget (parameters.sequencePosition);
        resolvedBlend = parameterBlend;
        resolvedBlendValid = ! parameters.sequenceManual;
    }
    else
    {
        resolvedBlend = parameterBlend;
        resolvedBlendValid = true;
    }

    if (!initialized)
    {
        frequencyControl.setImmediate (parameters.frequency);
        morphControl.setImmediate (parameters.morph);
        transformControl.setImmediate (parameters.transform);
        xyXControl.setImmediate (parameters.xyX);
        xyYControl.setImmediate (parameters.xyY);
        sequencePositionControl.setImmediate (parameters.sequencePosition);
        processingMode = parameters.mode;
        pendingMode = parameters.mode;
        activeInternalDistortion = parameters.internalDistortion;
        auto initialBlend = parameterBlend;
        if (parameters.mode == ProcessingMode::xy)
            initialBlend = makeBilinearBlend (xyPresets, xyNonlinearSources,
                                              parameters.xyX, parameters.xyY);
        else if (parameters.mode == ProcessingMode::sequencer)
        {
            initialBlend = makeDiscreteSequenceBlend (sequencePresets,
                                                       sequenceNonlinearSources,
                                                       parameters.sequenceManual
                                                           ? parameters.sequencePosition : 0.0);
        }
        currentBlend = initialBlend;
        currentCoefficients = parameters.mode == ProcessingMode::xy
                            && parameters.xyEncodedDomain
                                ? makeEncodedDomainCoefficients (initialBlend,
                                                                  parameters.frequency,
                                                                  parameters.morph,
                                                                  parameters.transform,
                                                                  sampleRate)
                                : makeBlendedCoefficients (initialBlend,
                                                           parameters.frequency,
                                                           parameters.morph,
                                                           parameters.transform,
                                                           sampleRate);
        if (! currentCoefficients.valid || currentCoefficients.usedSafeFallback)
            noteCoefficientFallback();
        cachedCoordinateKey = quantizedCoordinateKey (parameters.frequency,
                                                       parameters.morph,
                                                       parameters.transform);
        cachedCoordinateKeyValid = true;
        cachedBlend = initialBlend;
        cachedBlendValid = true;
        activePreset = parameters.preset;
        filterMix = 1.0;
        responseModeRequested = parameters.mode == ProcessingMode::xy
                             && ! parameters.xyEncodedDomain
                             && parameters.xyInterpolation == XYInterpolationMode::response;
        responseModeWasRequested = responseModeRequested;
        initialized = true;
    }
    else if (parameters.mode == ProcessingMode::preset)
    {
        const auto targetBlend = makeSingleBlend (parameters.preset,
                                                  parameters.internalDistortion);
        const auto needsTransition = processingMode != ProcessingMode::preset
                                  || parameters.preset != activePreset
                                  || (pendingReady
                                      && (pendingMode != ProcessingMode::preset
                                          || ! blendsEqual (pendingBlend, targetBlend)));

        if (needsTransition)
        {
            const auto candidate = makeCoefficients (parameters.preset,
                                                     parameters.frequency,
                                                     parameters.morph,
                                                     parameters.transform,
                                                     sampleRate);
            if (! candidate.valid || candidate.usedSafeFallback)
            {
                noteCoefficientFallback();
            }
            else
            {
                const auto samePendingTarget = pendingReady
                                            && pendingMode == ProcessingMode::preset
                                            && blendsEqual (pendingBlend, targetBlend);
                pendingCoefficients = candidate;
                pendingBlend = targetBlend;
                pendingFrequency = parameters.frequency;
                pendingMorph = parameters.morph;
                pendingTransform = parameters.transform;
                pendingPreset = parameters.preset;
                pendingMode = ProcessingMode::preset;
                pendingReady = true;

                // Keep an in-flight fade alive. A genuinely new target during
                // fade-in reverses from the current mix without resetting the
                // filter state or starting a second fade-out from 1.0.
                if (transitionPhase == TransitionPhase::none
                    || (! samePendingTarget && transitionPhase == TransitionPhase::fadeIn))
                {
                    sequenceHardwareTransition = false;
                    transitionFadeSamples = std::max (1, smoothingSamples);
                    transitionRemaining = transitionFadeSamples;
                    transitionPhase = TransitionPhase::fadeOut;
                }
            }
        }
        else
        {
            activeInternalDistortion = parameters.internalDistortion;
            frequencyControl.setTarget (parameters.frequency);
            morphControl.setTarget (parameters.morph);
            transformControl.setTarget (parameters.transform);
        }
    }
    else if (parameters.mode == ProcessingMode::xy)
    {
        const auto sourceTopologyChanged = parameters.mode != previousMode
            || previousXyPresets != xyPresets
            || previousXyNonlinearSources != xyNonlinearSources
            || parameters.xyEncodedDomain != previousEncodedDomain;
        const auto transitionBlend = makeBilinearBlend (xyPresets, xyNonlinearSources,
                                                        xyXControl.current, xyYControl.current);
        const auto samePendingTarget = pendingReady
                                    && pendingMode == parameters.mode
                                    && blendsEqual (pendingBlend, transitionBlend);
        const auto needsTransitionTarget = sourceTopologyChanged
                                        || (pendingReady && ! samePendingTarget);

        if (needsTransitionTarget)
        {
            const auto candidate = parameters.xyEncodedDomain
                                ? makeEncodedDomainCoefficients (transitionBlend,
                                                                  parameters.frequency,
                                                                  parameters.morph,
                                                                  parameters.transform,
                                                                  sampleRate)
                                : makeBlendedCoefficients (transitionBlend,
                                                           parameters.frequency,
                                                           parameters.morph,
                                                           parameters.transform,
                                                           sampleRate);
            if (! candidate.valid || candidate.usedSafeFallback)
                noteCoefficientFallback();
            else
            {
                pendingCoefficients = candidate;
                pendingBlend = transitionBlend;
                pendingFrequency = parameters.frequency;
                pendingMorph = parameters.morph;
                pendingTransform = parameters.transform;
                pendingPreset = parameters.preset;
                pendingMode = parameters.mode;
                pendingReady = true;

                if (transitionPhase == TransitionPhase::none
                    || (! samePendingTarget && transitionPhase == TransitionPhase::fadeIn))
                {
                    sequenceHardwareTransition = false;
                    transitionFadeSamples = std::max (1, smoothingSamples);
                    transitionRemaining = transitionFadeSamples;
                    transitionPhase = TransitionPhase::fadeOut;
                }
            }
        }
        else
        {
            processingMode = parameters.mode;
            if (transitionPhase == TransitionPhase::none)
                filterMix = 1.0;
        }
        frequencyControl.setTarget (parameters.frequency);
        morphControl.setTarget (parameters.morph);
        transformControl.setTarget (parameters.transform);
    }
    else if (parameters.mode == ProcessingMode::sequencer)
    {
        const auto modeChanged = previousMode != ProcessingMode::sequencer;
        const auto transitionSamples = parameters.sequenceManual
                                     ? std::max (2, static_cast<int> (std::lround (
                                           sampleRate * parameters.manualTransitionMs / 1000.0)))
                                     : requestedSequenceTransitionSamples;
        requestedSequenceTransitionSamples = transitionSamples;
        manualTransitionMs = parameters.manualTransitionMs;
        frequencyControl.setTarget (parameters.frequency);
        morphControl.setTarget (parameters.morph);
        transformControl.setTarget (parameters.transform);

        if (parameters.sequenceManual)
        {
            const auto target = makeDiscreteSequenceBlend (sequencePresets,
                                                           sequenceNonlinearSources,
                                                           sequencePositionControl.current);
            if (modeChanged || previousSequencePresets != sequencePresets
                || previousSequenceNonlinearSources != sequenceNonlinearSources
                || previousSequenceManual != sequenceManual)
                requestSequenceBlend (target, transitionSamples);
            else if (transitionPhase == TransitionPhase::none)
                requestSequenceBlend (target, transitionSamples);
        }
        else if (modeChanged || previousSequencePresets != sequencePresets
                 || previousSequenceNonlinearSources != sequenceNonlinearSources
                 || previousSequenceLength != sequenceLength
                 || previousSequenceManual != sequenceManual)
        {
            MorphBlend target;
            target.presets[0] = sequencePresets[0];
            target.nonlinearSources[0] = sequenceNonlinearSources[0];
            target.weights[0] = 1.0;
            target.count = 1;
            requestSequenceBlend (target, transitionSamples);
        }

        processingMode = parameters.mode;
    }
    else
    {
        frequencyControl.setTarget (parameters.frequency);
        morphControl.setTarget (parameters.morph);
        transformControl.setTarget (parameters.transform);
    }

    inputGain.setTarget (decibelsToGain (parameters.inputGainDb), smoothingSamples);
    preClipGain.setTarget (decibelsToGain (parameters.preClipGainDb), smoothingSamples);
    postClipGain.setTarget (decibelsToGain (parameters.postClipGainDb), smoothingSamples);
    dryWetMix.setTarget (parameters.dryWet, smoothingSamples);
    clipMix.setTarget (parameters.softClip ? 1.0 : 0.0, smoothingSamples);
    nonlinearThreshold.setTarget (settledThresholdForSetting (parameters.internalThresholdSetting), smoothingSamples);

    responseModeRequested = parameters.mode == ProcessingMode::xy
                         && ! parameters.xyEncodedDomain
                         && parameters.xyInterpolation == XYInterpolationMode::response;
    if (responseModeRequested != responseModeWasRequested)
    {
        if (responseModeRequested)
            primeExperimentalFirFromHistory();
        responseModeWasRequested = responseModeRequested;
        ++firRequestGeneration;
        firBuildOutstanding = false;
        firOutstandingGeneration = 0;
        firLastSubmittedKeyValid = false;
        firRequestCountdown = 0;
        ++graphRevision;
    }
    updateNonlinearRouting();
    if (responseModeRequested && ! nonRealtimeRendering)
        requestExperimentalFirBuild();
}

void MorphosisDSP::updateModeBlend() noexcept
{
    // A mode transition owns its pending destination. While the old
    // sequencer is fading out toward a preset (or another non-sequencer mode),
    // the active-mode coordinate updater must not reinterpret that destination
    // as a new sequencer slot and enqueue a second reset.
    if (processingMode == ProcessingMode::sequencer
        && pendingReady && pendingMode != ProcessingMode::sequencer)
    {
        updateNonlinearRouting();
        return;
    }

    MorphBlend nextBlend = currentBlend;
    if (processingMode == ProcessingMode::preset)
    {
        nextBlend = {};
        nextBlend.presets[0] = activePreset;
        nextBlend.weights[0] = 1.0;
        nextBlend.nonlinearSources[0] = activeInternalDistortion;
        nextBlend.count = 1;
    }
    else if (processingMode == ProcessingMode::xy)
    {
        nextBlend = makeBilinearBlend (xyPresets, xyNonlinearSources,
                                       xyXControl.current, xyYControl.current);
    }
    else if (sequenceManual)
    {
        nextBlend = makeDiscreteSequenceBlend (sequencePresets,
                                                sequenceNonlinearSources,
                                                sequencePositionControl.current);
    }
    else if (resolvedBlendValid)
    {
        nextBlend = resolvedBlend;
    }

    const auto coordinateKey = quantizedCoordinateKey (frequencyControl.current,
                                                       morphControl.current,
                                                       transformControl.current);
    if (! cachedCoordinateKeyValid || coordinateKey != cachedCoordinateKey
        || ! cachedBlendValid || ! blendsEqual (nextBlend, cachedBlend))
    {
        const auto candidate = processingMode == ProcessingMode::xy
                            && encodedDomainRequested
                                ? makeEncodedDomainCoefficientsCached (
                                      nextBlend, frequencyControl.current,
                                      morphControl.current, transformControl.current,
                                      sampleRate)
                                : makeBlendedCoefficients (nextBlend,
                                                           frequencyControl.current,
                                                           morphControl.current,
                                                           transformControl.current,
                                                           sampleRate);
        cachedCoordinateKey = coordinateKey;
        cachedCoordinateKeyValid = true;
        cachedBlend = nextBlend;
        cachedBlendValid = true;

        if (processingMode == ProcessingMode::sequencer)
        {
            // Sequencer destinations use the hardware dry bridge. Coordinate
            // motion inside the active cube still rebuilds the active filter,
            // but a new discrete slot is requested through the state machine.
            if (! blendsEqual (nextBlend, currentBlend))
                requestSequenceBlend (nextBlend,
                                       sequenceManual
                                           ? std::max (2, static_cast<int> (std::lround (
                                               sampleRate * manualTransitionMs / 1000.0)))
                                           : requestedSequenceTransitionSamples);
            else
            {
                acceptRuntimeCoefficients (candidate);
                if (pendingReady && pendingMode == ProcessingMode::sequencer)
                {
                    pendingCoefficients = makeBlendedCoefficients (
                        pendingBlend, frequencyControl.current, morphControl.current,
                        transformControl.current, sampleRate);
                }
            }
        }
        else if (acceptRuntimeCoefficients (candidate))
            currentBlend = nextBlend;
    }

    updateNonlinearRouting();
}

void MorphosisDSP::updateNonlinearRouting() noexcept
{
    const auto responseRoutingSuppressed = responseModeRequested
                                        || experimentalAlgorithmMix.current > 0.0;
    if (responseRoutingSuppressed)
    {
        // Response disables the descriptor IIR's nonlinear path even before a
        // worker result exists. The threshold smoother remains independent.
        nonlinearMix.current = 0.0;
        nonlinearMix.target = 0.0;
        nonlinearMix.remaining = 0;
        nonlinearRoutingSuppressed = true;
        return;
    }

    const auto desired = nonlinearMixForBlend (currentBlend);
    if (nonlinearMix.remaining > 0 && nonlinearMix.target == desired)
    {
        nonlinearRoutingSuppressed = false;
        return;
    }
    const auto samples = nonlinearRoutingSuppressed
                       ? smoothingSamples
                       : processingMode == ProcessingMode::preset ? smoothingSamples : 1;
    if (samples <= 1)
    {
        nonlinearMix.current = desired;
        nonlinearMix.target = desired;
        nonlinearMix.remaining = 0;
    }
    else
    {
        nonlinearMix.setTarget (desired, samples);
    }
    nonlinearRoutingSuppressed = false;
}

bool MorphosisDSP::acceptRuntimeCoefficients (const CoefficientSet& candidate) noexcept
{
    if (candidate.valid && ! candidate.usedSafeFallback)
    {
        currentCoefficients = candidate;
        return true;
    }

    noteCoefficientFallback();
    if (! currentCoefficients.valid || currentCoefficients.usedSafeFallback)
        currentCoefficients = candidate;
    return false;
}

void MorphosisDSP::advanceTransition() noexcept
{
    if (transitionPhase == TransitionPhase::fadeOut)
    {
        const auto fadeSamples = std::max (1, transitionFadeSamples);
        filterMix = std::max (0.0, filterMix - 1.0 / static_cast<double> (fadeSamples));
        if (transitionRemaining > 0)
            --transitionRemaining;
        if (transitionRemaining <= 0 || filterMix <= 0.0)
        {
            filterMix = 0.0;
            if (pendingReady)
            {
                const auto sequenceHandover = sequenceHardwareTransition;
                const auto handoverCoefficients = sequenceHandover
                    ? makeBlendedCoefficients (pendingBlend,
                                               frequencyControl.current,
                                               morphControl.current,
                                               transformControl.current,
                                               sampleRate)
                    : pendingCoefficients;

                if (! handoverCoefficients.valid || handoverCoefficients.usedSafeFallback)
                {
                    noteCoefficientFallback();
                    pendingReady = false;
                    transitionPhase = TransitionPhase::none;
                    filterMix = 1.0;
                    sequenceHardwareTransition = false;
                }
                else
                {
                    filterState.clear();
                    ++transitionResetCount;
                    currentCoefficients = handoverCoefficients;
                    currentBlend = pendingBlend;
                    cachedBlend = pendingBlend;
                    cachedBlendValid = true;

                    if (sequenceHandover)
                    {
                        // F/M/X are global trajectories. Keep their current
                        // values and latest targets across the slot handover.
                        cachedCoordinateKey = quantizedCoordinateKey (
                            frequencyControl.current, morphControl.current,
                            transformControl.current);
                    }
                    else
                    {
                        frequencyControl.setImmediate (pendingFrequency);
                        morphControl.setImmediate (pendingMorph);
                        transformControl.setImmediate (pendingTransform);
                        cachedCoordinateKey = quantizedCoordinateKey (pendingFrequency,
                                                                       pendingMorph,
                                                                       pendingTransform);
                    }
                    cachedCoordinateKeyValid = true;
                    activePreset = pendingPreset;
                    processingMode = pendingMode;
                    activeInternalDistortion = pendingBlend.nonlinearSources[0];
                    pendingReady = false;
                    transitionPhase = TransitionPhase::fadeIn;
                    transitionFadeSamples = sequenceHandover
                                          ? std::max (1, transitionFadeSamples)
                                          : std::max (1, smoothingSamples);
                    transitionRemaining = transitionFadeSamples;
                }
            }
            else
            {
                transitionPhase = TransitionPhase::none;
                filterMix = 1.0;
                sequenceHardwareTransition = false;
            }
        }
    }
    else if (transitionPhase == TransitionPhase::fadeIn)
    {
        const auto fadeSamples = std::max (1, transitionFadeSamples);
        filterMix = std::min (1.0, filterMix + 1.0 / static_cast<double> (fadeSamples));
        if (transitionRemaining > 0)
            --transitionRemaining;
        if (transitionRemaining <= 0 || filterMix >= 1.0)
        {
            filterMix = 1.0;
            transitionPhase = TransitionPhase::none;
            sequenceHardwareTransition = false;
        }
    }

    updateNonlinearRouting();
}

double MorphosisDSP::decibelsToGain (double decibels) noexcept
{
    return std::pow (10.0, std::clamp (decibels, -60.0, 24.0) / 20.0);
}

double MorphosisDSP::clampFinite (double value, double fallback, double low, double high) noexcept
{
    return std::isfinite (value) ? std::clamp (value, low, high) : fallback;
}

double MorphosisDSP::processFilter (double input, int channel) noexcept
{
    if (currentCoefficients.neutralBypass)
        return input;

    auto& states = filterState.values[static_cast<std::size_t> (std::clamp (channel, 0, kChannelCount - 1))];
    double x = input;
    for (int i = 0; i < kStageCount; ++i)
    {
        auto& state = states[static_cast<std::size_t> (i)];
        const auto& coefficient = currentCoefficients.stages[static_cast<std::size_t> (i)];
        const auto radius = coefficient.radius;
        const auto previous = std::abs (state[2]);
        const auto threshold = nonlinearThreshold.current;
        const auto excess = (nonlinearMix.current > 0.0 && previous > threshold)
                              ? 1.0 - threshold / previous : 0.0;
        const auto effectiveRadius = radius + radius * (1.0 - radius) * excess * nonlinearMix.current;
        const auto scale = radius > 1.0e-12 ? effectiveRadius / radius : 1.0;
        const auto a = coefficient.a * scale;
        const auto b = coefficient.b * scale;
        const auto g = coefficient.normalizationExponent <= 0.0
                         ? 1.0
                         : coefficient.normalizationExponent >= 1.0
                             ? 1.0 + effectiveRadius * effectiveRadius - 2.0 * a
                             : normalizedInputGain (effectiveRadius, a,
                                                    coefficient.normalizationExponent);
        auto u = g * x + a * state[0] - b * state[1];
        auto v = b * state[0] + a * state[1];
        if (nonlinearMix.current > 0.0)
        {
            const auto cap = 1000.0 * threshold;
            u = std::clamp (u, -cap, cap);
            // The recovered right-channel final-stage v-state clamp is absent
            // in firmware. Preserve that verified asymmetry when enabled.
            if (! (channel == 1 && i == kStageCount - 1))
                v = std::clamp (v, -cap, cap);
        }
        const auto w = u + coefficient.a / coefficient.b * v;
        x = w + coefficient.z1 * state[2] + coefficient.z2 * state[3];
        state[0] = u;
        state[1] = v;
        state[3] = state[2];
        state[2] = w;
    }
    return x * currentCoefficients.gain;
}

void MorphosisDSP::copyExperimentalGraphResponse (
    std::array<float, kExperimentalFirGraphPoints>& destination) const noexcept
{
    destination = firGraphResponseDb;
}

double MorphosisDSP::responseMagnitudeFromFir (
    const std::array<float, kExperimentalFirTaps>& impulse,
    double frequencyHz,
    double hostRate) noexcept
{
    return firResponseMagnitude (impulse, frequencyHz, hostRate);
}

bool MorphosisDSP::firBuildRequestsEqual (const FirBuildRequest& left,
                                          const FirBuildRequest& right) noexcept
{
    return left.presets == right.presets
        && left.nonlinearSources == right.nonlinearSources
        && left.x == right.x
        && left.y == right.y
        && left.frequency == right.frequency
        && left.morph == right.morph
        && left.transform == right.transform
        && left.sampleRate == right.sampleRate;
}

void MorphosisDSP::requestExperimentalFirBuild() noexcept
{
    if (! responseModeRequested || firRequestCountdown > 0)
        return;
    if (nonRealtimeRendering)
    {
        // Do not build a target that cannot yet be adopted. The current
        // coherent fade settles first, then the latest sample-boundary key is
        // designed and adopted synchronously in the same boundary.
        if (firTransitionActive)
            return;
    }
    else if (experimentalFirWorker == nullptr || firBuildOutstanding)
        return;

    FirBuildRequest request;
    request.presets = xyPresets;
    request.nonlinearSources = xyNonlinearSources;
    request.x = xyXControl.current;
    request.y = xyYControl.current;
    request.frequency = frequencyControl.current;
    request.morph = morphControl.current;
    request.transform = transformControl.current;
    request.sampleRate = sampleRate;
    if (firLastSubmittedKeyValid && firBuildRequestsEqual (request, firLastSubmittedKey))
    {
        firRequestCountdown = firRequestCadenceSamples;
        return;
    }

    if (nonRealtimeRendering)
    {
        request.generation = ++firRequestGeneration;
        const auto started = std::chrono::steady_clock::now();
        auto result = ExperimentalFirWorker::build (request);
        result.buildMilliseconds = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - started).count();
        lastExperimentalFirBuildMilliseconds = result.buildMilliseconds;
        firLastSubmittedKey = request;
        firLastSubmittedKeyValid = true;
        firRequestCountdown = firRequestCadenceSamples;
#if defined(MORPHOSIS_TESTING)
        ++synchronousFirDesignCountForTesting;
#endif
        adoptExperimentalFirResult (result);
        return;
    }

    request.generation = firRequestGeneration + 1;
    if (experimentalFirWorker->submit (request))
    {
        // Keep at most one build in flight. The worker result is tied to this
        // generation; allowing the audio thread to enqueue newer generations
        // before it can consume the result would make every completion stale.
        firRequestGeneration = request.generation;
        firLastSubmittedKey = request;
        firLastSubmittedKeyValid = true;
        firBuildOutstanding = true;
        firOutstandingGeneration = request.generation;
        firRequestCountdown = firRequestCadenceSamples;
    }
}

void MorphosisDSP::consumeExperimentalFirResult() noexcept
{
    // Preserve the mailbox result until the active target's OLA state is settled.
    if (experimentalFirWorker == nullptr || firTransitionActive)
        return;

    // popLatest claims a ready mailbox slot before copying its payload. The
    // persistent destination keeps an empty poll from constructing/clearing
    // the large result object on the audio thread.
    if (firResultScratch == nullptr || ! experimentalFirWorker->popLatest (*firResultScratch))
        return;
    const auto& result = *firResultScratch;

    if (firBuildOutstanding && result.generation == firOutstandingGeneration)
        firBuildOutstanding = false;
    lastExperimentalFirBuildMilliseconds = result.buildMilliseconds;

    adoptExperimentalFirResult (result);
}

void MorphosisDSP::adoptExperimentalFirResult (const FirBuildResult& result) noexcept
{
    // A worker completion may belong to a coordinate/state request that was
    // superseded while the FFT was running. Never let it overwrite a newer
    // destination.
    if (! responseModeRequested || result.generation != firRequestGeneration)
        return;
    if (! result.valid || result.usedSafeFallback)
    {
        noteCoefficientFallback();
        return;
    }

    firAcceptedGeneration = result.generation;
    firGraphResponseDb = result.graphResponseDb;
    firImpulse = result.impulse;
    if (! firReady)
    {
        firPartitions = result.partitions;
        firTargetPartitions = result.partitions;
        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            auto& state = firChannels[static_cast<std::size_t> (channel)];
            primeExperimentalFirChannel (channel, firPartitions,
                                         state.outputBlock, state.overlap);
            state.targetOutputBlock = state.outputBlock;
            state.targetOverlap = state.overlap;
        }
        firReady = true;
        firTransitionActive = false;
        firTransitionRemaining = 0;
        firProcessedChannelMask = 0;
        responseModeActive = true;
        ++graphRevision;
        return;
    }

    firTargetPartitions = result.partitions;
    for (int channel = 0; channel < kChannelCount; ++channel)
    {
        auto& state = firChannels[static_cast<std::size_t> (channel)];
        primeExperimentalFirChannel (channel, firTargetPartitions,
                                     state.targetOutputBlock, state.targetOverlap);
    }
    firTransitionActive = true;
    firTransitionMix = 0.0;
    firTransitionTotal = std::max (1, controlSmoothingSamples);
    firTransitionRemaining = firTransitionTotal;
    firProcessedChannelMask = 0;
    responseModeActive = true;
    ++graphRevision;
}

void MorphosisDSP::updateExperimentalAlgorithmMix() noexcept
{
    const auto target = responseModeRequested && firReady ? 1.0 : 0.0;
    experimentalAlgorithmMix.setTarget (target, controlSmoothingSamples);
    if (! responseModeRequested && experimentalAlgorithmMix.current <= 0.0)
        responseModeActive = false;
}

void MorphosisDSP::recordExperimentalFirHistory (int channel) noexcept
{
    auto& state = firChannels[static_cast<std::size_t> (std::clamp (channel, 0, kChannelCount - 1))];
    firFftWork.fill ({ 0.0f, 0.0f });
    for (int index = 0; index < kExperimentalFirPartitionSize; ++index)
        firFftWork[static_cast<std::size_t> (index)] =
            state.inputBlock[static_cast<std::size_t> (index)];
    fftFixed (firFftWork, false);

    const auto write = state.historyWrite;
    const auto historyOffset = write * kExperimentalFirRuntimeFftSize;
    for (int index = 0; index < kExperimentalFirRuntimeFftSize; ++index)
        state.history[static_cast<std::size_t> (historyOffset + index)] =
            firFftWork[static_cast<std::size_t> (index)];

    state.historyWrite = (write + 1) % kExperimentalFirHistoryBlockCount;
    ++state.historyBlockCount;
}

void MorphosisDSP::processExperimentalFirBlock (int channel) noexcept
{
    auto& state = firChannels[static_cast<std::size_t> (std::clamp (channel, 0, kChannelCount - 1))];
    recordExperimentalFirHistory (channel);
    const auto newestHistory = (state.historyWrite - 1 + kExperimentalFirHistoryBlockCount)
                             % kExperimentalFirHistoryBlockCount;

    firSumWork.fill ({ 0.0f, 0.0f });
    firTargetSumWork.fill ({ 0.0f, 0.0f });
    for (int partition = 0; partition < kExperimentalFirPartitionCount; ++partition)
    {
        const auto historyIndex = (newestHistory - partition + kExperimentalFirHistoryBlockCount)
                                 % kExperimentalFirHistoryBlockCount;
        const auto inputOffset = historyIndex * kExperimentalFirRuntimeFftSize;
        const auto coefficientOffset = partition * kExperimentalFirRuntimeFftSize;
        for (int bin = 0; bin < kExperimentalFirRuntimeFftSize; ++bin)
        {
            const auto inputSpectrum = state.history[static_cast<std::size_t> (inputOffset + bin)];
            firSumWork[static_cast<std::size_t> (bin)] +=
                inputSpectrum * firPartitions[static_cast<std::size_t> (coefficientOffset + bin)];
            if (firTransitionActive)
                firTargetSumWork[static_cast<std::size_t> (bin)] +=
                    inputSpectrum * firTargetPartitions[
                        static_cast<std::size_t> (coefficientOffset + bin)];
        }
    }

    fftFixed (firSumWork, true);
    if (firTransitionActive)
        fftFixed (firTargetSumWork, true);

    for (int index = 0; index < kExperimentalFirPartitionSize; ++index)
    {
        const auto first = static_cast<std::size_t> (index);
        const auto second = static_cast<std::size_t> (index + kExperimentalFirPartitionSize);
        state.outputBlock[first] = firSumWork[first].real() + state.overlap[first];
        state.overlap[first] = firSumWork[second].real();
        if (firTransitionActive)
        {
            state.targetOutputBlock[first] = firTargetSumWork[first].real()
                                           + state.targetOverlap[first];
            state.targetOverlap[first] = firTargetSumWork[second].real();
        }
        else
        {
            state.targetOutputBlock[first] = state.outputBlock[first];
            state.targetOverlap[first] = state.overlap[first];
        }
    }
}

void MorphosisDSP::processExperimentalFirHistoryOnly (double input, int channel) noexcept
{
    auto& state = firChannels[static_cast<std::size_t> (std::clamp (channel, 0, kChannelCount - 1))];
    if (state.outputRead < kExperimentalFirPartitionSize)
        ++state.outputRead;

    state.inputBlock[static_cast<std::size_t> (state.inputFill++)] = static_cast<float> (input);
    if (state.inputFill >= kExperimentalFirPartitionSize)
    {
        state.inputFill = 0;
        recordExperimentalFirHistory (channel);
        state.outputRead = 0;
    }
}

void MorphosisDSP::primeExperimentalFirFromHistory() noexcept
{
    for (int channel = 0; channel < kChannelCount; ++channel)
    {
        auto& state = firChannels[static_cast<std::size_t> (channel)];
        primeExperimentalFirChannel (channel, firPartitions,
                                     state.outputBlock, state.overlap);
        if (firTransitionActive)
        {
            primeExperimentalFirChannel (channel, firTargetPartitions,
                                         state.targetOutputBlock, state.targetOverlap);
        }
        else
        {
            state.targetOutputBlock = state.outputBlock;
            state.targetOverlap = state.overlap;
        }
    }
}

void MorphosisDSP::sumExperimentalFirHistory (
    const FirChannelState& state,
    const std::array<std::complex<float>, kExperimentalFirPartitionSpectrumCount>& partitions,
    int newestHistoryIndex,
    std::array<std::complex<float>, kExperimentalFirRuntimeFftSize>& work) noexcept
{
    work.fill ({ 0.0f, 0.0f });
    for (int partition = 0; partition < kExperimentalFirPartitionCount; ++partition)
    {
        const auto historyIndex = (newestHistoryIndex - partition + kExperimentalFirHistoryBlockCount)
                                % kExperimentalFirHistoryBlockCount;
        const auto inputOffset = historyIndex * kExperimentalFirRuntimeFftSize;
        const auto coefficientOffset = partition * kExperimentalFirRuntimeFftSize;
        for (int bin = 0; bin < kExperimentalFirRuntimeFftSize; ++bin)
        {
            const auto inputSpectrum = state.history[static_cast<std::size_t> (inputOffset + bin)];
            work[static_cast<std::size_t> (bin)] +=
                inputSpectrum * partitions[static_cast<std::size_t> (coefficientOffset + bin)];
        }
    }
    fftFixed (work, true);
}

void MorphosisDSP::primeExperimentalFirChannel (
    int channel,
    const std::array<std::complex<float>, kExperimentalFirPartitionSpectrumCount>& partitions,
    std::array<float, kExperimentalFirPartitionSize>& outputBlock,
    std::array<float, kExperimentalFirPartitionSize>& overlap) noexcept
{
    const auto& state = firChannels[static_cast<std::size_t> (
        std::clamp (channel, 0, kChannelCount - 1))];
    if (state.historyBlockCount == 0)
    {
        outputBlock.fill (0.0f);
        overlap.fill (0.0f);
        return;
    }

    const auto newest = (state.historyWrite - 1 + kExperimentalFirHistoryBlockCount)
                      % kExperimentalFirHistoryBlockCount;
    // Rebuild both the pending block head and the preceding block's tail under
    // this kernel; the stored overlap belongs to whichever kernel ran before.
    sumExperimentalFirHistory (state, partitions, newest, firTargetSumWork);

    if (state.historyBlockCount > 1)
    {
        const auto previous = (newest - 1 + kExperimentalFirHistoryBlockCount)
                           % kExperimentalFirHistoryBlockCount;
        sumExperimentalFirHistory (state, partitions, previous, firSumWork);
    }
    else
    {
        firSumWork.fill ({ 0.0f, 0.0f });
    }

    for (int index = 0; index < kExperimentalFirPartitionSize; ++index)
    {
        const auto first = static_cast<std::size_t> (index);
        const auto second = static_cast<std::size_t> (index + kExperimentalFirPartitionSize);
        outputBlock[first] = firTargetSumWork[first].real() + firSumWork[second].real();
        overlap[first] = firTargetSumWork[second].real();
    }
}

double MorphosisDSP::processExperimentalFir (double input, int channel) noexcept
{
    auto safeChannel = std::clamp (channel, 0, kChannelCount - 1);
    auto& state = firChannels[static_cast<std::size_t> (safeChannel)];
    double output = 0.0;
    if (state.outputRead < kExperimentalFirPartitionSize)
    {
        const auto index = static_cast<std::size_t> (state.outputRead);
        const auto mix = firTransitionActive ? std::clamp (firTransitionMix, 0.0, 1.0) : 1.0;
        output = static_cast<double> (state.outputBlock[index])
               + (static_cast<double> (state.targetOutputBlock[index])
                  - static_cast<double> (state.outputBlock[index])) * mix;
        ++state.outputRead;
    }

    state.inputBlock[static_cast<std::size_t> (state.inputFill++)] = static_cast<float> (input);
    if (state.inputFill >= kExperimentalFirPartitionSize)
    {
        state.inputFill = 0;
        processExperimentalFirBlock (safeChannel);
        state.outputRead = 0;
    }

    // Transition progress is advanced once per audio sample in advanceSample,
    // not once per channel. Keep the transition audible at mix=1 until the
    // next sample boundary so both stereo channels finish on the same vector.
    if (firTransitionActive && firTransitionMix >= 1.0)
        firProcessedChannelMask |= static_cast<std::uint8_t> (1u << safeChannel);
    return std::isfinite (output) ? output : 0.0;
}

void MorphosisDSP::advanceSample() noexcept
{
    const auto firInaudible = ! responseModeRequested
                            && experimentalAlgorithmMix.current <= 0.0;
    if (firTransitionActive && firTransitionMix >= 1.0
        && (firProcessedChannelMask != 0 || firInaudible))
    {
        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            auto& state = firChannels[static_cast<std::size_t> (channel)];
            state.outputBlock = state.targetOutputBlock;
            state.overlap = state.targetOverlap;
            state.targetOutputBlock = state.outputBlock;
            state.targetOverlap = state.overlap;
        }
        firPartitions = firTargetPartitions;
        firTransitionActive = false;
        firTransitionRemaining = 0;
        firTransitionMix = 1.0;
        firProcessedChannelMask = 0;
    }

    consumeExperimentalFirResult();
    for (auto* smoother : { &inputGain, &preClipGain, &postClipGain, &dryWetMix, &clipMix,
                            &nonlinearMix, &nonlinearThreshold, &experimentalAlgorithmMix })
        smoother->next();

    updateExperimentalAlgorithmMix();

    if (initialized && (transitionPhase != TransitionPhase::fadeOut
                        || processingMode == ProcessingMode::sequencer))
    {
        const auto amount = 1.0 / static_cast<double> (controlSmoothingSamples);
        frequencyControl.next (amount);
        morphControl.next (amount);
        transformControl.next (amount);
        if (processingMode == ProcessingMode::xy)
        {
            xyXControl.next (amount);
            xyYControl.next (amount);
        }
        else if (processingMode == ProcessingMode::sequencer && sequenceManual)
        {
            sequencePositionControl.next (amount, true);
        }
        // The source blend is rebuilt from native descriptors as one complete
        // coefficient set. Identical quantized coordinates and blend weights
        // remain cached, preserving the exact ordinary path when static.
        updateModeBlend();
    }

    if (responseModeRequested)
    {
        if (firRequestCountdown > 0)
            --firRequestCountdown;
        if (firRequestCountdown <= 0)
            requestExperimentalFirBuild();
    }

    if (firTransitionActive)
    {
        firTransitionMix = std::min (1.0,
                                     firTransitionMix
                                         + 1.0 / static_cast<double> (
                                             std::max (1, firTransitionTotal)));
        if (firTransitionRemaining > 0)
            --firTransitionRemaining;
    }

    advanceTransition();
}

float MorphosisDSP::processSample (float input, int channel) noexcept
{
    advanceSample();
    return processSampleNoAdvance (input, channel);
}

float MorphosisDSP::processSampleNoAdvance (float input, int channel, bool bypassed) noexcept
{
    if (!initialized || channel < 0 || channel >= kChannelCount || !std::isfinite (input))
        return failSafe (input);

    const auto channelIndex = static_cast<std::size_t> (channel);
    const auto wetSource = static_cast<double> (input) * inputGain.current;
    auto& inputHistory = inputDelay[channelIndex];
    auto& inputWrite = inputDelayWrite[channelIndex];
    const auto alignedInput = inputHistory[static_cast<std::size_t> (inputWrite)];
    inputHistory[static_cast<std::size_t> (inputWrite)] = wetSource;
    inputWrite = (inputWrite + 1) % kMorphosisLatencySamples;

    auto& bypassHistory = bypassDelay[channelIndex];
    auto& bypassWrite = bypassDelayWrite[channelIndex];
    const auto alignedBypass = static_cast<double> (
        bypassHistory[static_cast<std::size_t> (bypassWrite)]);
    bypassHistory[static_cast<std::size_t> (bypassWrite)] = input;
    bypassWrite = (bypassWrite + 1) % kMorphosisLatencySamples;

    const auto algorithmMix = std::clamp (experimentalAlgorithmMix.current, 0.0, 1.0);
    const auto responsePath = responseModeRequested || algorithmMix > 0.0;
    const auto descriptorFiltered = processFilter (wetSource, channel);
    auto& descriptorHistory = descriptorDelay[channelIndex];
    auto& descriptorWrite = descriptorDelayWrite[channelIndex];
    const auto delayedDescriptor = descriptorHistory[static_cast<std::size_t> (descriptorWrite)];
    descriptorHistory[static_cast<std::size_t> (descriptorWrite)] = descriptorFiltered;
    descriptorWrite = (descriptorWrite + 1) % kMorphosisLatencySamples;

    double firFiltered = 0.0;
    if (responsePath)
        firFiltered = processExperimentalFir (wetSource, channel);
    else
        processExperimentalFirHistoryOnly (wetSource, channel);
    const auto filteredCore = delayedDescriptor + (firFiltered - delayedDescriptor) * algorithmMix;
    const auto filtered = filteredCore * preClipGain.current;
    const auto filterDry = alignedInput * preClipGain.current;
    const auto filterOutput = filterDry + (filtered - filterDry) * filterMix;
    const auto clipped = filterOutput
                       + (firmwareOutputSoftClip (filterOutput) - filterOutput) * clipMix.current;
    const auto wet = clipped;
    const auto mix = std::clamp (dryWetMix.current, 0.0, 1.0);
    const auto dry = alignedInput;
    const auto mixed = mix >= 1.0 ? wet
                     : mix <= 0.0 ? dry
                                  : dry + (wet - dry) * mix;
    const auto output = mixed * postClipGain.current;
    if (!std::isfinite (output) || std::abs (output) > 1000000.0)
    {
        const auto safeOutput = failSafe (input);
        return bypassed ? static_cast<float> (alignedBypass) : safeOutput;
    }

    const auto finalOutput = bypassed ? alignedBypass : output;
    storePeak (channel, static_cast<float> (std::abs (finalOutput)));
    return static_cast<float> (finalOutput);
}

float MorphosisDSP::failSafe (float input) noexcept
{
    (void) input;
    filterState.clear();
    ++safetyResetCount;
    if (safetyFault != nullptr)
        safetyFault->store (true, std::memory_order_relaxed);
    return 0.0f;
}

void MorphosisDSP::noteCoefficientFallback() noexcept
{
    ++coefficientFallbackCount;
    if (safetyFault != nullptr)
        safetyFault->store (true, std::memory_order_relaxed);
}

void MorphosisDSP::storePeak (int channel, float value) noexcept
{
    auto* destination = channel == 0 ? leftPeak : rightPeak;
    if (destination == nullptr || !std::isfinite (value))
        return;

    auto old = destination->load (std::memory_order_relaxed);
    while (old < value && !destination->compare_exchange_weak (old, value,
                                                                std::memory_order_relaxed,
                                                                std::memory_order_relaxed))
    {
    }
}

double MorphosisDSP::firmwareOutputSoftClip (double input) noexcept
{
    if (!std::isfinite (input))
        return 0.0;
    const auto a = std::min (std::abs (input), 1.5);
    const auto magnitude = a <= 0.5 ? a : 1.0 - 0.5 * (1.5 - a) * (1.5 - a);
    return std::copysign (magnitude, input);
}

double MorphosisDSP::settledThresholdForSetting (int setting) noexcept
{
    const auto quantized = std::clamp (static_cast<int> (std::lround (setting / 6.0)) * 6, -24, 60);
    return kSettledThresholdAtSettingZero * std::ldexp (1.0, -quantized / 6);
}

double MorphosisDSP::transformThreshold (double base, int index) noexcept
{
    if (!std::isfinite (base) || base <= 0.0)
        return kThresholdFloor;
    const auto safeIndex = std::clamp (index, 0, 255);
    const auto scale = static_cast<double> (((16 | (safeIndex & 15)) << (safeIndex >> 4))) / 4096.0;
    return std::max (kThresholdFloor, base * scale);
}

double MorphosisDSP::responseMagnitude (const CoefficientSet& coefficients,
                                        double frequencyHz,
                                        double hostRate) noexcept
{
    if (!coefficients.valid || !std::isfinite (frequencyHz) || !std::isfinite (hostRate)
        || hostRate <= 0.0)
        return 0.0;
    if (coefficients.neutralBypass)
        return 1.0;
    const auto omega = 2.0 * 3.14159265358979323846
                     * std::clamp (frequencyHz, 0.0, hostRate * 0.5) / hostRate;
    const auto zr = std::cos (-omega);
    const auto zi = std::sin (-omega);
    double hr = coefficients.gain;
    double hi = 0.0;
    for (const auto& stage : coefficients.stages)
    {
        const auto denR = 1.0 - 2.0 * stage.a * zr
                        + (stage.a * stage.a + stage.b * stage.b) * (zr * zr - zi * zi);
        const auto denI = -2.0 * stage.a * zi
                        + (stage.a * stage.a + stage.b * stage.b) * (2.0 * zr * zi);
        const auto numR = 1.0 + stage.z1 * zr + stage.z2 * (zr * zr - zi * zi);
        const auto numI = stage.z1 * zi + stage.z2 * (2.0 * zr * zi);
        const auto factorR = stage.inputGain * (numR * denR + numI * denI)
                           / std::max (denR * denR + denI * denI, 1.0e-24);
        const auto factorI = stage.inputGain * (numI * denR - numR * denI)
                           / std::max (denR * denR + denI * denI, 1.0e-24);
        const auto nextR = hr * factorR - hi * factorI;
        const auto nextI = hr * factorI + hi * factorR;
        hr = nextR;
        hi = nextI;
    }
    return std::isfinite (hr) && std::isfinite (hi) ? complexMagnitude (hr, hi) : 0.0;
}

} // namespace morphosis
