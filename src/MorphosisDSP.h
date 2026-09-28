#pragma once

#include "CubeData.h"

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>

namespace morphosis
{

inline constexpr int kStageCount = 7;
inline constexpr int kChannelCount = 2;
inline constexpr int kMaxBlendSources = 4;
inline constexpr int kSequenceSlotCount = 16;
inline constexpr double kFirmwareNominalRate = 1250000.0 / 27.0;
inline constexpr double kSmoothingMilliseconds = 20.0;
inline constexpr double kControlSmoothingMilliseconds = 2.0;
inline constexpr int kExperimentalFirTaps = 2048;
inline constexpr int kExperimentalFirFftSize = 4096;
inline constexpr int kExperimentalFirPartitionSize = 64;
inline constexpr int kMorphosisLatencySamples = kExperimentalFirPartitionSize;
inline constexpr int kExperimentalFirRuntimeFftSize = kExperimentalFirPartitionSize * 2;
inline constexpr int kExperimentalFirPartitionCount =
    kExperimentalFirTaps / kExperimentalFirPartitionSize;
// Re-priming also needs the prior block's tail at the oldest kernel partition.
inline constexpr int kExperimentalFirHistoryBlockCount = kExperimentalFirPartitionCount + 1;
inline constexpr int kExperimentalFirPartitionSpectrumCount =
    kExperimentalFirPartitionCount * kExperimentalFirRuntimeFftSize;
inline constexpr int kExperimentalFirHistorySpectrumCount =
    kExperimentalFirHistoryBlockCount * kExperimentalFirRuntimeFftSize;
inline constexpr int kExperimentalFirGraphPoints = 1024;

#if defined (MORPHOSIS_STAGE8_PROFILE)
struct FirDesignStage8TimingProfile
{
    double coefficientMs = 0.0;
    double targetGridMs = 0.0;
    double minimumPhaseMs = 0.0;
    double partitionMs = 0.0;
    double graphMs = 0.0;
};

struct FirDesignStage8Profile
{
    bool valid = false;
    bool usedSafeFallback = false;
    double totalMs = 0.0;
    FirDesignStage8TimingProfile timing;
    std::uint64_t partitionHash = 0;
    std::array<float, kExperimentalFirTaps> impulse {};
    std::array<float, kExperimentalFirGraphPoints> graphResponseDb {};
};
#endif

enum class XYInterpolationMode : std::uint8_t
{
    descriptor = 0,
    response = 1
};

enum class ProcessingMode : std::uint8_t
{
    preset = 0,
    xy = 1,
    sequencer = 2
};

struct MorphBlend
{
    std::array<int, kMaxBlendSources> presets { 0, 0, 0, 0 };
    std::array<double, kMaxBlendSources> weights { 1.0, 0.0, 0.0, 0.0 };
    std::array<bool, kMaxBlendSources> nonlinearSources { false, false, false, false };
    int count = 1;
};

struct NativeStageDescriptor
{
    double poleAngle = 0.0;
    double poleRadius = 0.5;
    double zeroAngle = 0.0;
    double zeroRadius = 0.0;
    double normalizationExponent = 0.0;
};

struct NativeDescriptorSet
{
    std::array<NativeStageDescriptor, kStageCount> stages {};
    double gain = 1.0;
    bool valid = false;
    bool usedSafeFallback = false;
};

struct ParameterSnapshot
{
    int preset = 0;
    double frequency = 0.0;
    double morph = 0.0;
    double transform = 0.0;
    double inputGainDb = 0.0;
    double preClipGainDb = 0.0;
    double postClipGainDb = 0.0;
    bool softClip = true;
    bool internalDistortion = false;
    int internalThresholdSetting = 0;
    double dryWet = 1.0;
    double manualTransitionMs = 20.0;
    XYInterpolationMode xyInterpolation = XYInterpolationMode::descriptor;
    bool xyEncodedDomain = false;

    ProcessingMode mode = ProcessingMode::preset;
    MorphBlend blend {};
    std::array<int, kMaxBlendSources> xyPresets { 0, 0, 0, 0 };
    std::array<bool, kMaxBlendSources> xyNonlinearSources { false, false, false, false };
    double xyX = 0.5;
    double xyY = 0.5;
    std::array<int, kSequenceSlotCount> sequencePresets {};
    std::array<bool, kSequenceSlotCount> sequenceNonlinearSources {};
    int sequenceLength = 4;
    int sequenceDivision = 4;
    int sequenceSync = 0;
    bool sequenceManual = false;
    double sequencePosition = 0.0;
    double sequenceGlide = 0.0;
};

struct StageCoefficients
{
    double a = 0.0;
    double b = 1.0e-6;
    double inputGain = 1.0;
    double z1 = 0.0;
    double z2 = 0.0;
    double radius = 0.5;
    double angle = 0.0;
    double zeroRadius = 0.0;
    double zeroAngle = 0.0;
    double normalizationExponent = 0.0;
    bool normalizeDc = false;
};

struct CoefficientSet
{
    std::array<StageCoefficients, kStageCount> stages {};
    double gain = 1.0;
    bool valid = false;
    bool usedSafeFallback = false;
    bool neutralBypass = false;
};

struct FilterState
{
    std::array<std::array<std::array<double, 4>, kStageCount>, kChannelCount> values {};

    void clear() noexcept;
};

class MorphosisDSP final
{
public:
    MorphosisDSP();
    ~MorphosisDSP();

    MorphosisDSP (const MorphosisDSP&) = delete;
    MorphosisDSP& operator= (const MorphosisDSP&) = delete;

    void prepare(double sampleRate, int samplesPerBlock) noexcept;
    void releaseResources() noexcept;
    void reset() noexcept;
    void beginBlock(const ParameterSnapshot& parameters,
                    bool nonRealtime = false) noexcept;
    void setSequenceTarget(int slot, bool nonlinear, int transitionSamples) noexcept;
    void setSequenceTransitionDurationSamples(int samples) noexcept;
    void advanceSample() noexcept;
    float processSample(float input, int channel) noexcept;
    float processSampleNoAdvance(float input, int channel, bool bypassed = false) noexcept;

    void setTelemetry(std::atomic<float>* leftPeak,
                      std::atomic<float>* rightPeak,
                      std::atomic<bool>* safetyFault) noexcept;

    double getSampleRate() const noexcept { return sampleRate; }
    float getFilterMix() const noexcept { return static_cast<float>(filterMix); }
    float getSequenceTransitionMix() const noexcept { return static_cast<float>(filterMix); }
    const CoefficientSet& getCurrentCoefficients() const noexcept { return currentCoefficients; }
    bool isReady() const noexcept { return initialized; }
    bool isExperimentalResponseActive() const noexcept { return responseModeActive; }
    bool isExperimentalResponseRequested() const noexcept { return responseModeRequested; }
    double getLastExperimentalFirBuildMilliseconds() const noexcept
    {
        return lastExperimentalFirBuildMilliseconds;
    }
    std::uint64_t getGraphRevision() const noexcept { return graphRevision; }
    void copyExperimentalGraphResponse (
        std::array<float, kExperimentalFirGraphPoints>& destination) const noexcept;

    static CoefficientSet makeCoefficients(int preset,
                                           double frequency,
                                           double morph,
                                           double transform,
                                           double hostRate) noexcept;

    static NativeDescriptorSet makeNativeDescriptor(int preset,
                                                     double frequency,
                                                     double morph,
                                                     double transform) noexcept;

    static CoefficientSet makeBlendedCoefficients(const MorphBlend& blend,
                                                   double frequency,
                                                   double morph,
                                                   double transform,
                                                   double hostRate) noexcept;

    static CoefficientSet makeEncodedDomainCoefficients(const MorphBlend& blend,
                                                         double frequency,
                                                         double morph,
                                                         double transform,
                                                         double hostRate) noexcept;

    static MorphBlend makeBilinearBlend(const std::array<int, kMaxBlendSources>& presets,
                                        const std::array<bool, kMaxBlendSources>& nonlinearSources,
                                        double x,
                                        double y) noexcept;

    static MorphBlend makeAdjacentBlend(const std::array<int, kSequenceSlotCount>& presets,
                                        const std::array<bool, kSequenceSlotCount>& nonlinearSources,
                                        int length,
                                        double position) noexcept;

    void setResolvedBlend(const MorphBlend& blend) noexcept;
    const MorphBlend& getCurrentBlend() const noexcept { return currentBlend; }

    static double responseMagnitude(const CoefficientSet& coefficients,
                                    double frequencyHz,
                                    double hostRate) noexcept;
    static double responseMagnitudeFromFir (
        const std::array<float, kExperimentalFirTaps>& impulse,
        double frequencyHz,
        double hostRate) noexcept;

#if defined (MORPHOSIS_STAGE8_PROFILE)
    static FirDesignStage8Profile buildFirForStage8Profiling (
        const std::array<int, kMaxBlendSources>& presets,
        const std::array<bool, kMaxBlendSources>& nonlinearSources,
        double x,
        double y,
        double frequency,
        double morph,
        double transform,
        double hostRate) noexcept;
#endif

    static double firmwareOutputSoftClip(double input) noexcept;
    static double settledThresholdForSetting(int setting) noexcept;
    static double transformThreshold(double base, int index) noexcept;

#if defined(MORPHOSIS_TESTING)
    static CoefficientSet adaptNativeDescriptorForTesting(const NativeDescriptorSet& descriptor,
                                                          double hostRate) noexcept;
    std::uint64_t getTransitionResetCountForTesting() const noexcept
    {
        return transitionResetCount;
    }
    std::uint64_t getCoefficientFallbackCountForTesting() const noexcept
    {
        return coefficientFallbackCount;
    }
    std::uint64_t getSafetyResetCountForTesting() const noexcept
    {
        return safetyResetCount;
    }
    bool acceptRuntimeCoefficientsForTesting(const CoefficientSet& candidate) noexcept
    {
        return acceptRuntimeCoefficients (candidate);
    }
#endif

private:
    enum class TransitionPhase : std::uint8_t
    {
        none,
        fadeOut,
        fadeIn
    };

    struct Smoother
    {
        double current = 0.0;
        double target = 0.0;
        int remaining = 0;

        void setTarget(double value, int samples) noexcept;
        double next() noexcept;
    };

    struct CoordinateSmoother
    {
        double current = 0.0;
        double target = 0.0;

        void setImmediate(double value) noexcept;
        void setTarget(double value) noexcept;
        double next(double amount, bool snapToTarget = false) noexcept;
    };

    static std::array<int, 3> quantizedCoordinateKey(double frequency,
                                                       double morph,
                                                       double transform) noexcept;
    static double decibelsToGain(double decibels) noexcept;
    static double clampFinite(double value, double fallback, double low, double high) noexcept;
    static bool finiteCoefficientSet(const CoefficientSet& coefficients) noexcept;
    static bool finiteNativeDescriptor(const NativeDescriptorSet& descriptor) noexcept;
    static CoefficientSet makeNeutralCoefficients() noexcept;
    static CoefficientSet adaptNativeDescriptor(const NativeDescriptorSet& descriptor,
                                                double hostRate) noexcept;
    static MorphBlend sanitizeBlend(const MorphBlend& blend) noexcept;
    static bool blendsEqual(const MorphBlend& left, const MorphBlend& right) noexcept;
    static double nonlinearMixForBlend(const MorphBlend& blend) noexcept;

    struct EncodedSourceCache
    {
        int preset = -1;
        bool valid = false;
        std::array<double, kStageCount> poleAngleQ {};
        std::array<double, kStageCount> poleRadiusQ {};
        std::array<double, kStageCount> zeroAngleQ {};
        std::array<double, kStageCount> zeroRadiusQ {};
        std::array<bool, kStageCount> normalizationFlags {};
        double gainQ = 0.0;
    };

    static bool populateEncodedSourceCache(EncodedSourceCache& destination,
                                           int preset,
                                           const std::array<int, 3>& coordinates) noexcept;
    static CoefficientSet makeEncodedDomainCoefficientsFromCache(
        const MorphBlend& blend,
        const std::array<EncodedSourceCache, kMaxBlendSources>& sources,
        double frequency,
        double morph,
        double transform,
        double hostRate) noexcept;
    CoefficientSet makeEncodedDomainCoefficientsCached(const MorphBlend& blend,
                                                        double frequency,
                                                        double morph,
                                                        double transform,
                                                        double hostRate) noexcept;

    double processFilter(double input, int channel) noexcept;
    void advanceTransition() noexcept;
    void updateModeBlend() noexcept;
    void updateNonlinearRouting() noexcept;
    bool acceptRuntimeCoefficients(const CoefficientSet& candidate) noexcept;
    void noteCoefficientFallback() noexcept;
    void storePeak(int channel, float value) noexcept;
    float failSafe(float input) noexcept;
    void requestExperimentalFirBuild() noexcept;
    void consumeExperimentalFirResult() noexcept;
    void updateExperimentalAlgorithmMix() noexcept;
    double processExperimentalFir (double input, int channel) noexcept;
    void processExperimentalFirHistoryOnly (double input, int channel) noexcept;
    void recordExperimentalFirHistory (int channel) noexcept;
    void processExperimentalFirBlock (int channel) noexcept;
    void primeExperimentalFirFromHistory() noexcept;
    void requestSequenceBlend (const MorphBlend& target,
                               int transitionSamples) noexcept;
    static MorphBlend makeDiscreteSequenceBlend (
        const std::array<int, kSequenceSlotCount>& presets,
        const std::array<bool, kSequenceSlotCount>& nonlinearSources,
        double position) noexcept;
    static int discreteSequenceSlot (double position) noexcept;

    struct FirBuildRequest
    {
        std::array<int, kMaxBlendSources> presets { 0, 0, 0, 0 };
        std::array<bool, kMaxBlendSources> nonlinearSources { false, false, false, false };
        double x = 0.5;
        double y = 0.5;
        double frequency = 0.0;
        double morph = 0.0;
        double transform = 0.0;
        double sampleRate = 48000.0;
        std::uint64_t generation = 0;
    };

    struct FirBuildResult
    {
        std::uint64_t generation = 0;
        double sampleRate = 48000.0;
        bool valid = false;
        bool usedSafeFallback = false;
        double buildMilliseconds = 0.0;
#if defined (MORPHOSIS_STAGE8_PROFILE)
        FirDesignStage8TimingProfile stage8Timing;
#endif
        std::array<float, kExperimentalFirTaps> impulse {};
        std::array<std::complex<float>,
                   kExperimentalFirPartitionSpectrumCount> partitions {};
        std::array<float, kExperimentalFirGraphPoints> graphResponseDb {};
    };

    static bool firBuildRequestsEqual (const FirBuildRequest& left,
                                       const FirBuildRequest& right) noexcept;
    void adoptExperimentalFirResult (const FirBuildResult& result) noexcept;

    class ExperimentalFirWorker;

    double sampleRate = kFirmwareNominalRate;
    int smoothingSamples = 1;
    int controlSmoothingSamples = 1;
    int preparedBlockSize = 0;

    FilterState filterState;
    CoefficientSet currentCoefficients;
    CoefficientSet pendingCoefficients;
    MorphBlend currentBlend {};
    MorphBlend pendingBlend {};
    MorphBlend resolvedBlend {};
    bool resolvedBlendValid = false;
    int activePreset = 0;
    int pendingPreset = 0;
    double pendingFrequency = 0.0;
    double pendingMorph = 0.0;
    double pendingTransform = 0.0;
    std::array<int, 3> cachedCoordinateKey {};
    bool cachedCoordinateKeyValid = false;
    bool initialized = false;
    bool pendingReady = false;
    TransitionPhase transitionPhase = TransitionPhase::none;
    double filterMix = 1.0;
    bool sequenceHardwareTransition = false;
    int transitionFadeSamples = 1;
    int transitionRemaining = 0;
    int requestedSequenceTransitionSamples = 1;
    ProcessingMode processingMode = ProcessingMode::preset;
    ProcessingMode pendingMode = ProcessingMode::preset;
    bool activeInternalDistortion = false;
    bool nonlinearRoutingSuppressed = false;
    MorphBlend cachedBlend {};
    bool cachedBlendValid = false;
    std::uint64_t transitionResetCount = 0;
    std::uint64_t coefficientFallbackCount = 0;
    std::uint64_t safetyResetCount = 0;

    CoordinateSmoother frequencyControl;
    CoordinateSmoother morphControl;
    CoordinateSmoother transformControl;
    CoordinateSmoother xyXControl;
    CoordinateSmoother xyYControl;
    CoordinateSmoother sequencePositionControl;
    std::array<int, kMaxBlendSources> xyPresets { 0, 0, 0, 0 };
    std::array<bool, kMaxBlendSources> xyNonlinearSources { false, false, false, false };
    std::array<int, kSequenceSlotCount> sequencePresets {};
    std::array<bool, kSequenceSlotCount> sequenceNonlinearSources {};
    int sequenceLength = 4;
    bool sequenceManual = false;
    double manualTransitionMs = 20.0;

    bool responseModeRequested = false;
    bool responseModeActive = false;
    bool responseModeWasRequested = false;
    bool nonRealtimeRendering = false;
    bool encodedDomainRequested = false;
    std::array<EncodedSourceCache, kMaxBlendSources> encodedSourceCache {};
    std::array<int, 3> encodedSourceCacheCoordinateKey {};
    bool encodedSourceCacheCoordinateKeyValid = false;
    Smoother experimentalAlgorithmMix { 0.0, 0.0, 0 };
    std::array<float, kExperimentalFirTaps> firImpulse {};
    std::array<std::complex<float>,
               kExperimentalFirPartitionSpectrumCount> firPartitions {};
    std::array<std::complex<float>,
               kExperimentalFirPartitionSpectrumCount> firTargetPartitions {};
    std::array<float, kExperimentalFirGraphPoints> firGraphResponseDb {};
    std::unique_ptr<FirBuildResult> firResultScratch;
    FirBuildRequest firLastSubmittedKey {};
    bool firLastSubmittedKeyValid = false;
    bool firReady = false;
    bool firTransitionActive = false;
    double firTransitionMix = 1.0;
    int firTransitionRemaining = 0;
    int firTransitionTotal = 1;
    std::uint8_t firProcessedChannelMask = 0;
    std::uint64_t firRequestGeneration = 1;
    std::uint64_t firAcceptedGeneration = 0;
    std::uint64_t firOutstandingGeneration = 0;
    bool firBuildOutstanding = false;
    double lastExperimentalFirBuildMilliseconds = 0.0;
    int firRequestCountdown = 0;
    static constexpr int firRequestCadenceSamples = 32;

    struct FirChannelState
    {
        std::array<float, kExperimentalFirPartitionSize> inputBlock {};
        std::array<float, kExperimentalFirPartitionSize> outputBlock {};
        std::array<float, kExperimentalFirPartitionSize> targetOutputBlock {};
        std::array<float, kExperimentalFirPartitionSize> overlap {};
        std::array<float, kExperimentalFirPartitionSize> targetOverlap {};
        std::array<std::complex<float>,
                   kExperimentalFirHistorySpectrumCount> history {};
        int inputFill = 0;
        int outputRead = kExperimentalFirPartitionSize;
        int historyWrite = 0;
        std::uint64_t historyBlockCount = 0;
    };
    void sumExperimentalFirHistory (
        const FirChannelState& state,
        const std::array<std::complex<float>, kExperimentalFirPartitionSpectrumCount>& partitions,
        int newestHistoryIndex,
        std::array<std::complex<float>, kExperimentalFirRuntimeFftSize>& work) noexcept;
    void primeExperimentalFirChannel (
        int channel,
        const std::array<std::complex<float>, kExperimentalFirPartitionSpectrumCount>& partitions,
        std::array<float, kExperimentalFirPartitionSize>& outputBlock,
        std::array<float, kExperimentalFirPartitionSize>& overlap) noexcept;
    std::array<FirChannelState, kChannelCount> firChannels {};
    std::array<std::complex<float>, kExperimentalFirRuntimeFftSize> firFftWork {};
    std::array<std::complex<float>, kExperimentalFirRuntimeFftSize> firSumWork {};
    std::array<std::complex<float>, kExperimentalFirRuntimeFftSize> firTargetSumWork {};
    std::array<std::array<double, kMorphosisLatencySamples>, kChannelCount> descriptorDelay {};
    std::array<int, kChannelCount> descriptorDelayWrite { 0, 0 };
    std::array<std::array<double, kMorphosisLatencySamples>, kChannelCount> inputDelay {};
    std::array<int, kChannelCount> inputDelayWrite { 0, 0 };
    std::array<std::array<float, kMorphosisLatencySamples>, kChannelCount> bypassDelay {};
    std::array<int, kChannelCount> bypassDelayWrite { 0, 0 };
    std::unique_ptr<ExperimentalFirWorker> experimentalFirWorker;
    std::uint64_t graphRevision = 1;

    Smoother inputGain { 1.0, 1.0, 0 };
    Smoother preClipGain { 1.0, 1.0, 0 };
    Smoother postClipGain { 1.0, 1.0, 0 };
    Smoother dryWetMix { 1.0, 1.0, 0 };
    Smoother clipMix { 1.0, 1.0, 0 };
    Smoother nonlinearMix { 0.0, 0.0, 0 };
    Smoother nonlinearThreshold { settledThresholdForSetting (0),
                                  settledThresholdForSetting (0), 0 };

    std::atomic<float>* leftPeak = nullptr;
    std::atomic<float>* rightPeak = nullptr;
    std::atomic<bool>* safetyFault = nullptr;

};

} // namespace morphosis
