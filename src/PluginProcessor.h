#pragma once

#include "MorphosisDSP.h"
#include "MorphosisSequencer.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace morphosis::parameter_ids
{
inline constexpr char preset[] = "preset";
inline constexpr char frequency[] = "frequency";
inline constexpr char morph[] = "morph";
inline constexpr char transform[] = "transform";
inline constexpr char inputGainDb[] = "inputGainDb";
inline constexpr char preClipGainDb[] = "preClipGainDb";
inline constexpr char postClipGainDb[] = "postClipGainDb";
inline constexpr char dryWet[] = "dryWet";
inline constexpr char manualTransitionMs[] = "manualTransitionMs";
inline constexpr char xyInterpolation[] = "xyInterpolation";
inline constexpr char xyEncodedDomain[] = "xyEncodedDomain";
inline constexpr char softClip[] = "softClip";
inline constexpr char internalDistortion[] = "internalDistortion";
inline constexpr char internalThresholdSetting[] = "internalThresholdSetting";
inline constexpr char mode[] = "mode";
inline constexpr char xyX[] = "xyX";
inline constexpr char xyY[] = "xyY";
inline constexpr char xyA[] = "xyA";
inline constexpr char xyB[] = "xyB";
inline constexpr char xyC[] = "xyC";
inline constexpr char xyD[] = "xyD";
inline constexpr char sequenceLength[] = "sequenceLength";
inline constexpr char sequenceDivision[] = "sequenceDivision";
inline constexpr char sequenceSync[] = "sequenceSync";
inline constexpr char sequenceSource[] = "sequenceSource";
inline constexpr char sequencePosition[] = "sequencePosition";
inline constexpr char sequenceGlide[] = "sequenceGlide";
inline constexpr std::array<const char*, morphosis::kSequenceSlotCount> sequenceSlots {
    "sequenceSlot01", "sequenceSlot02", "sequenceSlot03", "sequenceSlot04",
    "sequenceSlot05", "sequenceSlot06", "sequenceSlot07", "sequenceSlot08",
    "sequenceSlot09", "sequenceSlot10", "sequenceSlot11", "sequenceSlot12",
    "sequenceSlot13", "sequenceSlot14", "sequenceSlot15", "sequenceSlot16"
};
} // namespace morphosis::parameter_ids

class MorphosisAudioProcessor final : public juce::AudioProcessor
{
public:
    struct GraphSnapshot
    {
        morphosis::CoefficientSet coefficients {};
        double sampleRate = 48000.0;
        bool responseFIR = false;
        std::array<float, morphosis::kExperimentalFirGraphPoints> responseDb {};
        std::uint64_t generation = 0;
    };

    MorphosisAudioProcessor();
    ~MorphosisAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }
    const juce::AudioProcessorValueTreeState& getParameters() const noexcept { return parameters; }
    morphosis::ParameterSnapshot getParameterSnapshot() const noexcept;

    void selectPreviousPreset() noexcept;
    void selectNextPreset() noexcept;
    void setProcessingMode (morphosis::ProcessingMode mode) noexcept;
    void activateSequencer() noexcept;
    void activateXY() noexcept;
    void returnToPresetMode() noexcept;
    void setXYSlot (int slot, int preset) noexcept;
    void setSequenceSlot (int slot, int preset) noexcept;
    using SequencePresetArray = std::array<int, morphosis::kSequenceSlotCount>;
    using XYPresetArray = std::array<int, morphosis::kMaxBlendSources>;
    void setSequenceSlotsBatch (const SequencePresetArray& presets) noexcept;
    void setXYSlotsBatch (const XYPresetArray& presets) noexcept;
    void randomizePreset() noexcept;
    void randomizeSequencePresets() noexcept;
    void randomizeXYPresets() noexcept;
    morphosis::ProcessingMode getProcessingMode() const noexcept;
    int getPresetIndex() const noexcept;
    const char* getPresetName (int index) const noexcept;
    int getSequencerStep() const noexcept
    {
        return sequenceDisplayStep.load (std::memory_order_relaxed);
    }
    double getSampleRate() const noexcept { return currentSampleRate; }
    double getLastExperimentalFirBuildMilliseconds() const noexcept
    {
        return dsp.getLastExperimentalFirBuildMilliseconds();
    }

    float consumePeak (int channel) noexcept;
    bool hasSafetyFault() const noexcept { return safetyFault.load (std::memory_order_relaxed); }

    // The audio thread publishes complete, immutable-for-the-consumer graph
    // snapshots. The editor drains only these copies; it never reads dsp.
    bool popLatestGraphSnapshot (GraphSnapshot& destination) noexcept;
    std::uint64_t getGraphSnapshotGeneration() const noexcept
    {
        return graphSnapshotGeneration.load (std::memory_order_acquire);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    static constexpr int kGraphSnapshotQueueSize = 8;

    void processBlockInternal (juce::AudioBuffer<float>&,
                               juce::MidiBuffer&,
                               bool bypassed);
    void resetSequenceScheduler() noexcept;
    void invalidateGraphSnapshots() noexcept;
    void publishGraphSnapshot() noexcept;

    juce::AudioProcessorValueTreeState parameters;
    morphosis::MorphosisDSP dsp;
    std::atomic<float> peakLeft { 0.0f };
    std::atomic<float> peakRight { 0.0f };
    std::atomic<bool> safetyFault { false };
    std::array<GraphSnapshot, kGraphSnapshotQueueSize> graphSnapshotQueue {};
    juce::AbstractFifo graphSnapshotFifo { kGraphSnapshotQueueSize };
    std::atomic<std::uint64_t> graphSnapshotGeneration { 1 };
    double currentSampleRate = 48000.0;
    double fallbackSequencePpq = 0.0;
    double lastHostPpq = 0.0;
    double heldSequencePhase = 0.0;
    int heldSequenceStep = 0;
    bool sequenceSchedulerInitialized = false;
    bool sequenceWasPlaying = false;
    std::atomic<int> sequenceDisplayStep { 0 };
    int scheduledDivision = -1;
    int scheduledSync = -1;
    int scheduledLength = -1;
    double scheduledGlide = -1.0;
    bool sequenceInitialized = false;
    bool xyInitialized = false;
    std::uint64_t lastPublishedGraphRevision = 0;
    bool lastPublishedResponseMode = false;

    SequencePresetArray readSequenceSlotsFromParameters() const noexcept;
    XYPresetArray readXYSlotsFromParameters() const noexcept;
    void commitSlotArrays (const SequencePresetArray& sequence,
                           const XYPresetArray& xy) noexcept;

    std::array<std::array<std::atomic<int>, morphosis::kSequenceSlotCount>, 2>
        committedSequenceSlots {};
    std::array<std::array<std::atomic<int>, morphosis::kMaxBlendSources>, 2>
        committedXYSlots {};
    std::atomic<int> activeSlotBatchIndex { 0 };
    std::atomic<bool> slotBatchUpdate { false };
    std::atomic<std::uint64_t> slotBatchEpoch { 0 };

    static bool stateIsValid (const juce::ValueTree& state,
                              const juce::AudioProcessorValueTreeState& parameters) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphosisAudioProcessor)
};
