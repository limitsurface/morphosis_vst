#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PresetRouting.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>

namespace
{

juce::NormalisableRange<float> cvRange()
{
    return { -5.0f, 5.0f, 0.001f };
}

juce::NormalisableRange<float> gainRange()
{
    return { -60.0f, 24.0f, 0.01f };
}

} // namespace

MorphosisAudioProcessor::MorphosisAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "Parameters", createParameterLayout())
{
    setLatencySamples (morphosis::kMorphosisLatencySamples);
    dsp.setTelemetry (&peakLeft, &peakRight, &safetyFault);
    for (auto& buffer : committedSequenceSlots)
        for (auto& value : buffer)
            value.store (0, std::memory_order_relaxed);
    for (auto& buffer : committedXYSlots)
        for (auto& value : buffer)
            value.store (0, std::memory_order_relaxed);
}

MorphosisAudioProcessor::~MorphosisAudioProcessor()
{
    releaseResources();
}

juce::AudioProcessorValueTreeState::ParameterLayout MorphosisAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::preset, 2 },
        "Preset", 0, morphosis::cube_data::kRecordCount - 1, 0));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::frequency, 2 },
        "Frequency", cvRange(), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::morph, 2 },
        "Morph", cvRange(), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::transform, 2 },
        "Transform", cvRange(), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::inputGainDb, 2 },
        "Input Gain", gainRange(), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::preClipGainDb, 2 },
        "Pre-Clip Gain", gainRange(), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::postClipGainDb, 2 },
        "Post-Clip Gain", gainRange(), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::dryWet, 4 },
        "Dry/Wet", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 1.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::manualTransitionMs, 1 },
        "Manual Transition", juce::NormalisableRange<float> (5.0f, 250.0f, 0.01f), 20.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { morphosis::parameter_ids::xyInterpolation, 1 },
        "XY Interpolation", juce::StringArray { "Descriptor", "Response (Experimental)" }, 1));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { morphosis::parameter_ids::xyEncodedDomain, 1 },
        "Encoded Domain XY (Experimental)", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { morphosis::parameter_ids::softClip, 2 },
        "Output Soft Clip", true));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { morphosis::parameter_ids::internalDistortion, 2 },
        "Internal Distortion", false));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::internalThresholdSetting, 2 },
        "Internal Threshold Setting", -24, 60, 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::mode, 3 },
        "Processing Mode", 0, 2, 0));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::xyX, 3 },
        "XY X", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::xyY, 3 },
        "XY Y", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::xyA, 3 },
        "XY A", 0, morphosis::cube_data::kRecordCount - 1, 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::xyB, 3 },
        "XY B", 0, morphosis::cube_data::kRecordCount - 1, 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::xyC, 3 },
        "XY C", 0, morphosis::cube_data::kRecordCount - 1, 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::xyD, 3 },
        "XY D", 0, morphosis::cube_data::kRecordCount - 1, 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::sequenceLength, 3 },
        "Sequence Length", 1, morphosis::kSequenceSlotCount, 4));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::sequenceDivision, 3 },
        "Sequence Division", 0, 7, static_cast<int> (morphosis::SequenceDivision::quarter)));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::sequenceSync, 3 },
        "Sequence Sync", 0, 2, static_cast<int> (morphosis::SequenceSync::straight)));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { morphosis::parameter_ids::sequenceSource, 3 },
        "Sequence Source", 0, 1, 0));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::sequencePosition, 3 },
        "Sequence Position", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { morphosis::parameter_ids::sequenceGlide, 3 },
        "Sequence Glide", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { morphosis::parameter_ids::sequenceSlots[
                                    static_cast<std::size_t> (index)], 3 },
            "Sequence Slot " + juce::String (index + 1),
            0, morphosis::cube_data::kRecordCount - 1, 0));
    return layout;
}

const juce::String MorphosisAudioProcessor::getName() const
{
    return "MORPHOSIS";
}

bool MorphosisAudioProcessor::acceptsMidi() const { return false; }
bool MorphosisAudioProcessor::producesMidi() const { return false; }
bool MorphosisAudioProcessor::isMidiEffect() const { return false; }
double MorphosisAudioProcessor::getTailLengthSeconds() const
{
    return std::numeric_limits<double>::infinity();
}

int MorphosisAudioProcessor::getNumPrograms()
{
    return morphosis::cube_data::kRecordCount;
}

int MorphosisAudioProcessor::getPresetIndex() const noexcept
{
    const auto* raw = parameters.getRawParameterValue (morphosis::parameter_ids::preset);
    const auto* parameter = parameters.getParameter (morphosis::parameter_ids::preset);
    if (raw == nullptr || parameter == nullptr)
        return 0;
    juce::ignoreUnused (parameter);
    const auto value = raw->load (std::memory_order_relaxed);
    return juce::jlimit (0, morphosis::cube_data::kRecordCount - 1, juce::roundToInt (value));
}

int MorphosisAudioProcessor::getCurrentProgram()
{
    return getPresetIndex();
}

void MorphosisAudioProcessor::setCurrentProgram (int index)
{
    if (getProcessingMode() != morphosis::ProcessingMode::preset)
        return;

    const auto safeIndex = juce::jlimit (0, morphosis::cube_data::kRecordCount - 1, index);
    if (auto* parameter = parameters.getParameter (morphosis::parameter_ids::preset))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (static_cast<float> (safeIndex)));

    if (auto* parameter = parameters.getParameter (morphosis::parameter_ids::internalDistortion))
        parameter->setValue (morphosis::usesDotFourDistortionLayout (safeIndex) ? 1.0f : 0.0f);
}

const juce::String MorphosisAudioProcessor::getProgramName (int index)
{
    return getPresetName (index);
}

void MorphosisAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

const char* MorphosisAudioProcessor::getPresetName (int index) const noexcept
{
    const auto safeIndex = juce::jlimit (0, morphosis::cube_data::kRecordCount - 1, index);
    return morphosis::cube_data::kCubes[static_cast<std::size_t> (safeIndex)].name;
}

void MorphosisAudioProcessor::selectPreviousPreset() noexcept
{
    setCurrentProgram ((getPresetIndex() + morphosis::cube_data::kRecordCount - 1)
                       % morphosis::cube_data::kRecordCount);
}

void MorphosisAudioProcessor::selectNextPreset() noexcept
{
    setCurrentProgram ((getPresetIndex() + 1) % morphosis::cube_data::kRecordCount);
}

void MorphosisAudioProcessor::setProcessingMode (morphosis::ProcessingMode newMode) noexcept
{
    if (newMode == morphosis::ProcessingMode::sequencer)
    {
        activateSequencer();
        return;
    }
    if (newMode == morphosis::ProcessingMode::xy)
    {
        activateXY();
        return;
    }
    returnToPresetMode();
}

void MorphosisAudioProcessor::activateSequencer() noexcept
{
    if (! sequenceInitialized)
    {
        const auto preset = getPresetIndex();
        SequencePresetArray slots {};
        slots.fill (preset);
        setSequenceSlotsBatch (slots);
        sequenceInitialized = true;
    }

    if (auto* parameter = parameters.getParameter (morphosis::parameter_ids::mode))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (
            static_cast<float> (morphosis::ProcessingMode::sequencer)));
}

void MorphosisAudioProcessor::activateXY() noexcept
{
    if (! xyInitialized)
    {
        const auto preset = getPresetIndex();
        XYPresetArray slots {};
        slots.fill (preset);
        setXYSlotsBatch (slots);
        xyInitialized = true;
    }

    if (auto* parameter = parameters.getParameter (morphosis::parameter_ids::mode))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (
            static_cast<float> (morphosis::ProcessingMode::xy)));
}

void MorphosisAudioProcessor::returnToPresetMode() noexcept
{
    if (auto* parameter = parameters.getParameter (morphosis::parameter_ids::mode))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (0.0f));
}

void MorphosisAudioProcessor::setXYSlot (int slot, int preset) noexcept
{
    const auto safeSlot = juce::jlimit (0, morphosis::kMaxBlendSources - 1, slot);
    auto slots = readXYSlotsFromParameters();
    slots[static_cast<std::size_t> (safeSlot)] = juce::jlimit (
        0, morphosis::cube_data::kRecordCount - 1, preset);
    setXYSlotsBatch (slots);
    xyInitialized = true;
}

void MorphosisAudioProcessor::setSequenceSlot (int slot, int preset) noexcept
{
    const auto safeSlot = juce::jlimit (0, morphosis::kSequenceSlotCount - 1, slot);
    auto slots = readSequenceSlotsFromParameters();
    slots[static_cast<std::size_t> (safeSlot)] = juce::jlimit (
        0, morphosis::cube_data::kRecordCount - 1, preset);
    setSequenceSlotsBatch (slots);
    sequenceInitialized = true;
}

MorphosisAudioProcessor::SequencePresetArray
    MorphosisAudioProcessor::readSequenceSlotsFromParameters() const noexcept
{
    SequencePresetArray slots {};
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
    {
        const auto* raw = parameters.getRawParameterValue (
            morphosis::parameter_ids::sequenceSlots[static_cast<std::size_t> (index)]);
        const auto value = raw != nullptr ? raw->load (std::memory_order_relaxed) : 0.0f;
        slots[static_cast<std::size_t> (index)] = juce::jlimit (
            0, morphosis::cube_data::kRecordCount - 1,
            std::isfinite (value) ? juce::roundToInt (value) : 0);
    }
    return slots;
}

MorphosisAudioProcessor::XYPresetArray
    MorphosisAudioProcessor::readXYSlotsFromParameters() const noexcept
{
    XYPresetArray slots {};
    const auto ids = std::array<const char*, morphosis::kMaxBlendSources> {
        morphosis::parameter_ids::xyA, morphosis::parameter_ids::xyB,
        morphosis::parameter_ids::xyC, morphosis::parameter_ids::xyD };
    for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
    {
        const auto* raw = parameters.getRawParameterValue (ids[static_cast<std::size_t> (index)]);
        const auto value = raw != nullptr ? raw->load (std::memory_order_relaxed) : 0.0f;
        slots[static_cast<std::size_t> (index)] = juce::jlimit (
            0, morphosis::cube_data::kRecordCount - 1,
            std::isfinite (value) ? juce::roundToInt (value) : 0);
    }
    return slots;
}

void MorphosisAudioProcessor::commitSlotArrays (const SequencePresetArray& sequence,
                                                const XYPresetArray& xy) noexcept
{
    const auto active = activeSlotBatchIndex.load (std::memory_order_acquire);
    const auto destination = 1 - juce::jlimit (0, 1, active);
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        committedSequenceSlots[static_cast<std::size_t> (destination)]
                              [static_cast<std::size_t> (index)]
            .store (sequence[static_cast<std::size_t> (index)], std::memory_order_relaxed);
    for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
        committedXYSlots[static_cast<std::size_t> (destination)]
                        [static_cast<std::size_t> (index)]
            .store (xy[static_cast<std::size_t> (index)], std::memory_order_relaxed);
    activeSlotBatchIndex.store (destination, std::memory_order_release);
}

void MorphosisAudioProcessor::setSequenceSlotsBatch (const SequencePresetArray& requested) noexcept
{
    SequencePresetArray safe = requested;
    for (auto& preset : safe)
        preset = juce::jlimit (0, morphosis::cube_data::kRecordCount - 1, preset);

    const auto currentXY = readXYSlotsFromParameters();
    slotBatchEpoch.fetch_add (1, std::memory_order_acq_rel);
    slotBatchUpdate.store (true, std::memory_order_release);
    const auto active = activeSlotBatchIndex.load (std::memory_order_acquire);
    const auto destination = 1 - juce::jlimit (0, 1, active);
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        if (auto* parameter = parameters.getParameter (
                morphosis::parameter_ids::sequenceSlots[static_cast<std::size_t> (index)]))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (
                static_cast<float> (safe[static_cast<std::size_t> (index)])));
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        committedSequenceSlots[static_cast<std::size_t> (destination)]
                              [static_cast<std::size_t> (index)]
            .store (safe[static_cast<std::size_t> (index)], std::memory_order_relaxed);
    for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
        committedXYSlots[static_cast<std::size_t> (destination)]
                        [static_cast<std::size_t> (index)]
            .store (currentXY[static_cast<std::size_t> (index)], std::memory_order_relaxed);
    activeSlotBatchIndex.store (destination, std::memory_order_release);
    slotBatchUpdate.store (false, std::memory_order_release);
    slotBatchEpoch.fetch_add (1, std::memory_order_release);
}

void MorphosisAudioProcessor::setXYSlotsBatch (const XYPresetArray& requested) noexcept
{
    XYPresetArray safe = requested;
    for (auto& preset : safe)
        preset = juce::jlimit (0, morphosis::cube_data::kRecordCount - 1, preset);

    const auto currentSequence = readSequenceSlotsFromParameters();
    const auto ids = std::array<const char*, morphosis::kMaxBlendSources> {
        morphosis::parameter_ids::xyA, morphosis::parameter_ids::xyB,
        morphosis::parameter_ids::xyC, morphosis::parameter_ids::xyD };
    slotBatchEpoch.fetch_add (1, std::memory_order_acq_rel);
    slotBatchUpdate.store (true, std::memory_order_release);
    const auto active = activeSlotBatchIndex.load (std::memory_order_acquire);
    const auto destination = 1 - juce::jlimit (0, 1, active);
    for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
        if (auto* parameter = parameters.getParameter (ids[static_cast<std::size_t> (index)]))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (
                static_cast<float> (safe[static_cast<std::size_t> (index)])));
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        committedSequenceSlots[static_cast<std::size_t> (destination)]
                              [static_cast<std::size_t> (index)]
            .store (currentSequence[static_cast<std::size_t> (index)], std::memory_order_relaxed);
    for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
        committedXYSlots[static_cast<std::size_t> (destination)]
                        [static_cast<std::size_t> (index)]
            .store (safe[static_cast<std::size_t> (index)], std::memory_order_relaxed);
    activeSlotBatchIndex.store (destination, std::memory_order_release);
    slotBatchUpdate.store (false, std::memory_order_release);
    slotBatchEpoch.fetch_add (1, std::memory_order_release);
}

void MorphosisAudioProcessor::randomizePreset() noexcept
{
    if (getProcessingMode() == morphosis::ProcessingMode::preset)
        setCurrentProgram (juce::Random::getSystemRandom().nextInt (morphosis::cube_data::kRecordCount));
}

void MorphosisAudioProcessor::randomizeSequencePresets() noexcept
{
    SequencePresetArray slots {};
    auto& random = juce::Random::getSystemRandom();
    for (auto& preset : slots)
        preset = random.nextInt (morphosis::cube_data::kRecordCount);
    setSequenceSlotsBatch (slots);
    sequenceInitialized = true;
}

void MorphosisAudioProcessor::randomizeXYPresets() noexcept
{
    XYPresetArray slots {};
    auto& random = juce::Random::getSystemRandom();
    for (auto& preset : slots)
        preset = random.nextInt (morphosis::cube_data::kRecordCount);
    setXYSlotsBatch (slots);
    xyInitialized = true;
}

morphosis::ProcessingMode MorphosisAudioProcessor::getProcessingMode() const noexcept
{
    const auto* raw = parameters.getRawParameterValue (morphosis::parameter_ids::mode);
    const auto value = raw != nullptr ? juce::roundToInt (raw->load (std::memory_order_relaxed)) : 0;
    return static_cast<morphosis::ProcessingMode> (juce::jlimit (0, 2, value));
}

void MorphosisAudioProcessor::resetSequenceScheduler() noexcept
{
    fallbackSequencePpq = 0.0;
    lastHostPpq = 0.0;
    heldSequencePhase = 0.0;
    heldSequenceStep = 0;
    sequenceDisplayStep.store (0, std::memory_order_relaxed);
    sequenceSchedulerInitialized = false;
    sequenceWasPlaying = false;
    scheduledDivision = -1;
    scheduledSync = -1;
    scheduledLength = -1;
    scheduledGlide = -1.0;
}

void MorphosisAudioProcessor::invalidateGraphSnapshots() noexcept
{
    // Do not reset AbstractFifo from the message thread while the audio thread
    // may be publishing. Generation invalidation makes all queued entries
    // stale without touching the SPSC indices from the wrong side.
    graphSnapshotGeneration.fetch_add (1, std::memory_order_acq_rel);
}

void MorphosisAudioProcessor::publishGraphSnapshot() noexcept
{
    const auto responseMode = dsp.isExperimentalResponseActive();
    const auto graphRevision = dsp.getGraphRevision();
    // A realized FIR graph is copied only when the worker publishes a new
    // complete design. Re-copying its 1024-point response every audio block
    // would needlessly add memory traffic to the real-time path.
    if (responseMode && responseMode == lastPublishedResponseMode
        && graphRevision == lastPublishedGraphRevision)
        return;

    int start1 = 0;
    int size1 = 0;
    int start2 = 0;
    int size2 = 0;
    graphSnapshotFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 + size2 <= 0)
        return;

    const auto slot = size1 > 0 ? start1 : start2;
    auto& snapshot = graphSnapshotQueue[static_cast<std::size_t> (slot)];
    snapshot.coefficients = dsp.getCurrentCoefficients();
    snapshot.sampleRate = dsp.getSampleRate();
    snapshot.responseFIR = responseMode;
    if (responseMode)
        dsp.copyExperimentalGraphResponse (snapshot.responseDb);
    snapshot.generation = graphSnapshotGeneration.load (std::memory_order_acquire);
    graphSnapshotFifo.finishedWrite (1);
    lastPublishedResponseMode = responseMode;
    lastPublishedGraphRevision = graphRevision;
}

bool MorphosisAudioProcessor::popLatestGraphSnapshot (GraphSnapshot& destination) noexcept
{
    bool found = false;
    GraphSnapshot candidate;

    for (;;)
    {
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        graphSnapshotFifo.prepareToRead (1, start1, size1, start2, size2);
        if (size1 + size2 <= 0)
            break;

        const auto slot = size1 > 0 ? start1 : start2;
        candidate = graphSnapshotQueue[static_cast<std::size_t> (slot)];
        graphSnapshotFifo.finishedRead (1);

        const auto generationBefore = graphSnapshotGeneration.load (std::memory_order_acquire);
        const auto generationAfter = graphSnapshotGeneration.load (std::memory_order_acquire);
        if (candidate.generation == generationBefore
            && candidate.generation == generationAfter)
        {
            destination = candidate;
            found = true;
        }
    }

    return found;
}

morphosis::ParameterSnapshot MorphosisAudioProcessor::getParameterSnapshot() const noexcept
{
    morphosis::ParameterSnapshot snapshot;
    const auto get = [this] (const char* id, float fallback) noexcept
    {
        const auto* raw = parameters.getRawParameterValue (id);
        const auto* parameter = parameters.getParameter (id);
        if (raw == nullptr || parameter == nullptr)
            return fallback;
        const auto denormalized = raw->load (std::memory_order_relaxed);
        return std::isfinite (denormalized) ? denormalized : fallback;
    };

    snapshot.preset = juce::roundToInt (get (morphosis::parameter_ids::preset, 0.0f));
    snapshot.frequency = get (morphosis::parameter_ids::frequency, 0.0f);
    snapshot.morph = get (morphosis::parameter_ids::morph, 0.0f);
    snapshot.transform = get (morphosis::parameter_ids::transform, 0.0f);
    snapshot.inputGainDb = get (morphosis::parameter_ids::inputGainDb, 0.0f);
    snapshot.preClipGainDb = get (morphosis::parameter_ids::preClipGainDb, 0.0f);
    snapshot.postClipGainDb = get (morphosis::parameter_ids::postClipGainDb, 0.0f);
    snapshot.dryWet = get (morphosis::parameter_ids::dryWet, 1.0f);
    snapshot.manualTransitionMs = std::clamp (
        static_cast<double> (get (morphosis::parameter_ids::manualTransitionMs, 20.0f)),
        5.0, 250.0);
    snapshot.xyInterpolation = static_cast<morphosis::XYInterpolationMode> (
        juce::jlimit (0, 1, juce::roundToInt (
            get (morphosis::parameter_ids::xyInterpolation, 0.0f))));
    snapshot.xyEncodedDomain = get (morphosis::parameter_ids::xyEncodedDomain, 0.0f) >= 0.5f;
    snapshot.softClip = get (morphosis::parameter_ids::softClip, 1.0f) >= 0.5f;
    // The `.4` catalog family owns this routing decision for the current UI
    // pass. The retained parameter remains part of the versioned state model,
    // but cannot bypass the product rule from a stale state or automation lane.
    snapshot.internalDistortion = morphosis::usesDotFourDistortionLayout (snapshot.preset);
    snapshot.internalThresholdSetting = juce::roundToInt (
        get (morphosis::parameter_ids::internalThresholdSetting, 0.0f));

    const auto modeValue = juce::roundToInt (get (morphosis::parameter_ids::mode, 0.0f));
    snapshot.mode = static_cast<morphosis::ProcessingMode> (juce::jlimit (0, 2, modeValue));
    snapshot.xyX = get (morphosis::parameter_ids::xyX, 0.5f);
    snapshot.xyY = get (morphosis::parameter_ids::xyY, 0.5f);
    const auto xyIds = std::array<const char*, morphosis::kMaxBlendSources> {
        morphosis::parameter_ids::xyA, morphosis::parameter_ids::xyB,
        morphosis::parameter_ids::xyC, morphosis::parameter_ids::xyD };
    const auto slotEpochBefore = slotBatchEpoch.load (std::memory_order_acquire);
    const auto useCommittedSlots = (slotEpochBefore & 1u) != 0u;
    const auto committedIndex = useCommittedSlots
                                    ? activeSlotBatchIndex.load (std::memory_order_acquire)
                                    : 0;
    for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
    {
        const auto safeIndex = static_cast<std::size_t> (index);
        snapshot.xyPresets[safeIndex] = useCommittedSlots
            ? committedXYSlots[static_cast<std::size_t> (committedIndex)][safeIndex]
                  .load (std::memory_order_relaxed)
            : juce::jlimit (0, morphosis::cube_data::kRecordCount - 1,
                            juce::roundToInt (get (xyIds[safeIndex], 0.0f)));
        snapshot.xyNonlinearSources[safeIndex] = morphosis::usesDotFourDistortionLayout (
            snapshot.xyPresets[safeIndex]);
        snapshot.blend.presets[safeIndex] = snapshot.xyPresets[safeIndex];
        snapshot.blend.nonlinearSources[safeIndex] = snapshot.xyNonlinearSources[safeIndex];
    }
    snapshot.blend.count = snapshot.mode == morphosis::ProcessingMode::xy ? 4 : 1;
    snapshot.blend.presets[0] = snapshot.preset;
    snapshot.blend.weights = { 1.0, 0.0, 0.0, 0.0 };
    snapshot.blend.nonlinearSources[0] = snapshot.internalDistortion;

    snapshot.sequenceLength = juce::jlimit (1, morphosis::kSequenceSlotCount,
        juce::roundToInt (get (morphosis::parameter_ids::sequenceLength, 4.0f)));
    snapshot.sequenceDivision = juce::jlimit (0, 7,
        juce::roundToInt (get (morphosis::parameter_ids::sequenceDivision,
                               static_cast<float> (morphosis::SequenceDivision::quarter))));
    snapshot.sequenceSync = juce::jlimit (0, 2,
        juce::roundToInt (get (morphosis::parameter_ids::sequenceSync, 0.0f)));
    snapshot.sequenceManual = get (morphosis::parameter_ids::sequenceSource, 0.0f) >= 0.5f;
    snapshot.sequencePosition = get (morphosis::parameter_ids::sequencePosition, 0.0f);
    snapshot.sequenceGlide = get (morphosis::parameter_ids::sequenceGlide, 0.0f);
    snapshot.blend.count = 1;
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
    {
        const auto safeIndex = static_cast<std::size_t> (index);
        snapshot.sequencePresets[safeIndex] = useCommittedSlots
            ? committedSequenceSlots[static_cast<std::size_t> (committedIndex)][safeIndex]
                  .load (std::memory_order_relaxed)
            : juce::jlimit (
                  0, morphosis::cube_data::kRecordCount - 1,
                  juce::roundToInt (get (morphosis::parameter_ids::sequenceSlots[safeIndex], 0.0f)));
        snapshot.sequenceNonlinearSources[safeIndex] =
            morphosis::usesDotFourDistortionLayout (snapshot.sequencePresets[safeIndex]);
    }

    // A batch is bracketed by odd/even epochs. If the audio read crossed the
    // message-thread write window, use the last fully committed slot arrays
    // rather than a partially observed APVTS list.
    if (! useCommittedSlots
        && slotBatchEpoch.load (std::memory_order_acquire) != slotEpochBefore)
    {
        const auto stableIndex = activeSlotBatchIndex.load (std::memory_order_acquire);
        for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
        {
            const auto safeIndex = static_cast<std::size_t> (index);
            snapshot.xyPresets[safeIndex] = committedXYSlots[
                static_cast<std::size_t> (stableIndex)][safeIndex].load (std::memory_order_relaxed);
            snapshot.xyNonlinearSources[safeIndex] = morphosis::usesDotFourDistortionLayout (
                snapshot.xyPresets[safeIndex]);
        }
        for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        {
            const auto safeIndex = static_cast<std::size_t> (index);
            snapshot.sequencePresets[safeIndex] = committedSequenceSlots[
                static_cast<std::size_t> (stableIndex)][safeIndex].load (std::memory_order_relaxed);
            snapshot.sequenceNonlinearSources[safeIndex] =
                morphosis::usesDotFourDistortionLayout (snapshot.sequencePresets[safeIndex]);
        }
    }

    for (int index = 0; index < morphosis::kMaxBlendSources; ++index)
    {
        const auto safeIndex = static_cast<std::size_t> (index);
        snapshot.blend.presets[safeIndex] = snapshot.xyPresets[safeIndex];
        snapshot.blend.nonlinearSources[safeIndex] = snapshot.xyNonlinearSources[safeIndex];
    }
    snapshot.blend.presets[0] = snapshot.sequencePresets[0];
    snapshot.blend.nonlinearSources[0] = snapshot.sequenceNonlinearSources[0];
    return snapshot;
}

void MorphosisAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    invalidateGraphSnapshots();
    setLatencySamples (morphosis::kMorphosisLatencySamples);
    currentSampleRate = std::isfinite (sampleRate) && sampleRate > 1000.0 ? sampleRate : 48000.0;
    dsp.prepare (currentSampleRate, samplesPerBlock);
    lastPublishedGraphRevision = 0;
    lastPublishedResponseMode = false;
    resetSequenceScheduler();
    peakLeft.store (0.0f, std::memory_order_relaxed);
    peakRight.store (0.0f, std::memory_order_relaxed);
    safetyFault.store (false, std::memory_order_relaxed);
}

void MorphosisAudioProcessor::releaseResources()
{
    invalidateGraphSnapshots();
    dsp.releaseResources();
    lastPublishedGraphRevision = 0;
    lastPublishedResponseMode = false;
    resetSequenceScheduler();
}

bool MorphosisAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.inputBuses.size() != 1 || layouts.outputBuses.size() != 1)
        return false;

    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    const auto isMonoOrStereo = [] (const juce::AudioChannelSet& set)
    {
        return set == juce::AudioChannelSet::mono()
            || set == juce::AudioChannelSet::stereo();
    };

    return isMonoOrStereo (input) && input == output;
}

void MorphosisAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midiMessages)
{
    processBlockInternal (buffer, midiMessages, false);
}

void MorphosisAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer& midiMessages)
{
    processBlockInternal (buffer, midiMessages, true);
}

void MorphosisAudioProcessor::processBlockInternal (juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer& midiMessages,
                                                    bool bypassed)
{
    midiMessages.clear();
    const auto snapshot = getParameterSnapshot();
    dsp.beginBlock (snapshot, isNonRealtime());

    const auto isHostSequencer = snapshot.mode == morphosis::ProcessingMode::sequencer
                              && ! snapshot.sequenceManual;
    bool hostPositionAvailable = false;
    bool hostPlaying = isHostSequencer;
    bool hostPpqAvailable = false;
    double blockPpq = fallbackSequencePpq;
    double bpm = 120.0;

    if (isHostSequencer)
    {
        if (auto* hostPlayHead = getPlayHead())
        {
            if (const auto position = hostPlayHead->getPosition())
            {
                hostPositionAvailable = true;
                hostPlaying = position->getIsPlaying();
                if (const auto ppq = position->getPpqPosition())
                {
                    blockPpq = *ppq;
                    hostPpqAvailable = std::isfinite (blockPpq);
                }
                if (const auto hostBpm = position->getBpm())
                    if (std::isfinite (*hostBpm) && *hostBpm > 0.0)
                        bpm = *hostBpm;
            }
        }

        if (hostPositionAvailable && ! hostPpqAvailable)
            blockPpq = fallbackSequencePpq;

        if (scheduledDivision != snapshot.sequenceDivision
            || scheduledSync != snapshot.sequenceSync
            || scheduledLength != snapshot.sequenceLength
            || scheduledGlide != snapshot.sequenceGlide)
        {
            scheduledDivision = snapshot.sequenceDivision;
            scheduledSync = snapshot.sequenceSync;
            scheduledLength = snapshot.sequenceLength;
            scheduledGlide = snapshot.sequenceGlide;
            heldSequenceStep = std::clamp (heldSequenceStep, 0, snapshot.sequenceLength - 1);
        }
    }

    const auto inputChannels = getTotalNumInputChannels();
    const auto outputChannels = getTotalNumOutputChannels();
    for (int channel = inputChannels; channel < outputChannels; ++channel)
        if (channel < buffer.getNumChannels())
            buffer.clear (channel, 0, buffer.getNumSamples());

    const auto channelsToProcess = std::min ({ inputChannels, outputChannels, buffer.getNumChannels(),
                                               morphosis::kChannelCount });
    const auto periodBeats = isHostSequencer
                               ? morphosis::sequencePeriodQuarterBeats (snapshot.sequenceDivision,
                                                                         snapshot.sequenceSync)
                               : 1.0;
    const auto safeBpm = std::max (1.0, bpm);
    const auto ppqPerSample = safeBpm / (60.0 * std::max (1000.0, currentSampleRate));
    const auto periodSeconds = periodBeats * 60.0 / safeBpm;
    const auto safePeriodSeconds = std::max (periodSeconds, 1.0e-9);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        if (isHostSequencer)
        {
            double phase = heldSequencePhase;
            int step = heldSequenceStep;
            if (hostPlaying)
            {
                const auto currentPpq = hostPpqAvailable
                                           ? blockPpq + static_cast<double> (sample) * ppqPerSample
                                           : fallbackSequencePpq;
                const auto cycle = currentPpq / std::max (periodBeats, 1.0e-9);
                const auto cycleFloor = std::floor (cycle);
                const auto cycleIndex = static_cast<std::int64_t> (cycleFloor);
                step = static_cast<int> ((cycleIndex % snapshot.sequenceLength
                                          + snapshot.sequenceLength) % snapshot.sequenceLength);
                phase = std::clamp (cycle - cycleFloor, 0.0, 1.0);
                heldSequenceStep = step;
                heldSequencePhase = phase;
                if (! hostPpqAvailable)
                    fallbackSequencePpq += ppqPerSample;
            }

            // Glide is the total old-filter exit plus new-filter entry time.
            // Glide zero still gets a short anti-click bridge, capped to the
            // actual step period when a host produces very short steps.
            const auto totalTransitionSeconds = std::min (
                safePeriodSeconds,
                std::max (0.002, snapshot.sequenceGlide * safePeriodSeconds));
            const auto totalTransitionSamples = std::max (2, static_cast<int> (
                std::lround (totalTransitionSeconds * currentSampleRate)));
            juce::ignoreUnused (phase);
            dsp.setSequenceTarget (snapshot.sequencePresets[static_cast<std::size_t> (step)],
                                   snapshot.sequenceNonlinearSources[
                                       static_cast<std::size_t> (step)],
                                   totalTransitionSamples);
        }

        dsp.advanceSample();
        for (int channel = 0; channel < channelsToProcess; ++channel)
            buffer.setSample (channel, sample,
                              dsp.processSampleNoAdvance (buffer.getSample (channel, sample),
                                                          channel, bypassed));
    }

    if (isHostSequencer)
    {
        sequenceWasPlaying = hostPlaying;
        sequenceDisplayStep.store (heldSequenceStep, std::memory_order_relaxed);
        if (hostPpqAvailable)
            lastHostPpq = blockPpq + static_cast<double> (buffer.getNumSamples()) * ppqPerSample;
    }

    publishGraphSnapshot();
}

bool MorphosisAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* MorphosisAudioProcessor::createEditor()
{
    return new MorphosisAudioProcessorEditor (*this);
}

void MorphosisAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("stateVersion", 6, nullptr);
    state.setProperty ("product", "MORPHOSIS", nullptr);
    state.setProperty ("sequenceInitialized", sequenceInitialized, nullptr);
    state.setProperty ("xyInitialized", xyInitialized, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

bool MorphosisAudioProcessor::stateIsValid (
    const juce::ValueTree& state,
    const juce::AudioProcessorValueTreeState& stateParameters) noexcept
{
    if (!state.isValid() || !state.hasType ("Parameters"))
        return false;

    const auto version = static_cast<int> (state.getProperty ("stateVersion", 0));
    if (version < 1 || version > 6)
        return false;

    juce::StringArray seenIds;
    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        const auto child = state.getChild (i);
        if (!child.hasType ("PARAM"))
            return false;
        const auto id = child.getProperty ("id").toString();
        if (id.isEmpty() || seenIds.contains (id))
            return false;
        seenIds.add (id);
        const auto* parameter = stateParameters.getParameter (id);
        const auto value = child.getProperty ("value");
        bool numeric = value.isDouble() || value.isInt();
        double numericValue = numeric ? static_cast<double> (value) : 0.0;
        if (!numeric && value.isString())
        {
            const auto valueText = value.toString().trim();
            const auto* utf8 = valueText.toRawUTF8();
            char* end = nullptr;
            numericValue = std::strtod (utf8, &end);
            numeric = utf8 != nullptr && end != utf8 && end != nullptr && *end == '\0';
        }
        const auto normalisedValue = parameter != nullptr
                                       ? parameter->convertTo0to1 (static_cast<float> (numericValue))
                                       : -1.0f;
        if (parameter == nullptr || value.isVoid() || !numeric
            || !std::isfinite (numericValue) || !std::isfinite (normalisedValue)
            || normalisedValue < 0.0f || normalisedValue > 1.0f)
            return false;
    }
    return true;
}

void MorphosisAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr)
        return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (!stateIsValid (state, parameters))
        return;

    // State written before the dry/wet parameter existed is intentionally
    // migrated to 100% wet so the old signal path remains the default.
    auto stateToApply = state.createCopy();
    if (! stateToApply.getChildWithProperty ("id", morphosis::parameter_ids::dryWet).isValid())
    {
        juce::ValueTree dryWetState ("PARAM");
        dryWetState.setProperty ("id", morphosis::parameter_ids::dryWet, nullptr);
        dryWetState.setProperty ("value", 1.0, nullptr);
        stateToApply.addChild (dryWetState, -1, nullptr);
        stateToApply.setProperty ("stateVersion", 6, nullptr);
    }

    if (! stateToApply.getChildWithProperty (
            "id", morphosis::parameter_ids::xyEncodedDomain).isValid())
    {
        juce::ValueTree encodedState ("PARAM");
        encodedState.setProperty ("id", morphosis::parameter_ids::xyEncodedDomain, nullptr);
        encodedState.setProperty ("value", 0.0, nullptr);
        stateToApply.addChild (encodedState, -1, nullptr);
    }

    // New instances use the Response experimental mode, but a legacy state
    // without this selector must retain the historical Descriptor behavior.
    // Do not overwrite an explicitly saved selector value.
    if (! state.getChildWithProperty (
            "id", morphosis::parameter_ids::xyInterpolation).isValid())
    {
        juce::ValueTree interpolationState ("PARAM");
        interpolationState.setProperty ("id", morphosis::parameter_ids::xyInterpolation, nullptr);
        interpolationState.setProperty ("value", 0.0, nullptr);
        stateToApply.addChild (interpolationState, -1, nullptr);
    }
    stateToApply.setProperty ("stateVersion", 6, nullptr);

    if (! stateIsValid (stateToApply, parameters))
        return;

    slotBatchEpoch.fetch_add (1, std::memory_order_acq_rel);
    slotBatchUpdate.store (true, std::memory_order_release);
    parameters.replaceState (stateToApply);
    const auto sequenceSlots = readSequenceSlotsFromParameters();
    const auto xySlots = readXYSlotsFromParameters();
    commitSlotArrays (sequenceSlots, xySlots);
    slotBatchUpdate.store (false, std::memory_order_release);
    slotBatchEpoch.fetch_add (1, std::memory_order_release);

    invalidateGraphSnapshots();

    sequenceInitialized = static_cast<bool> (stateToApply.getProperty ("sequenceInitialized", false));
    xyInitialized = static_cast<bool> (stateToApply.getProperty ("xyInitialized", false));

    if (auto* parameter = parameters.getParameter (morphosis::parameter_ids::internalDistortion))
        parameter->setValue (morphosis::usesDotFourDistortionLayout (getPresetIndex()) ? 1.0f : 0.0f);

    dsp.reset();
    lastPublishedGraphRevision = 0;
    lastPublishedResponseMode = false;
    resetSequenceScheduler();
    peakLeft.store (0.0f, std::memory_order_relaxed);
    peakRight.store (0.0f, std::memory_order_relaxed);
    safetyFault.store (false, std::memory_order_relaxed);
}

float MorphosisAudioProcessor::consumePeak (int channel) noexcept
{
    auto& peak = channel == 0 ? peakLeft : peakRight;
    return peak.exchange (0.0f, std::memory_order_relaxed);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MorphosisAudioProcessor();
}
