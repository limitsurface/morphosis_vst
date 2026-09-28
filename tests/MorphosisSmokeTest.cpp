#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PresetRouting.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <vector>

#include "PresetTaxonomy.h"

struct MorphosisEditorTestAccess
{
    static void refresh (MorphosisAudioProcessorEditor& editor)
    {
        editor.timerCallback();
    }

    static bool sequencePositionVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequencePosition.isVisible();
    }

    static bool sequenceSyncVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceSyncBox.isVisible();
    }

    static bool sequenceLengthVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceLengthBox.isVisible();
    }

    static bool sequenceGridVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGrid.isVisible();
    }

    static bool sequenceGlideVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGlide.isVisible();
    }

    static int sequenceVisibleLength (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGrid.getVisibleLength();
    }
};

namespace
{

void expect (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit (EXIT_FAILURE);
    }
}

void expectNear (double actual, double expected, double tolerance, const char* message)
{
    if (!std::isfinite (actual) || !std::isfinite (expected)
        || std::abs (actual - expected) > tolerance * std::max (1.0, std::abs (expected)))
    {
        std::cerr << "FAIL: " << message << " actual=" << actual << " expected=" << expected << '\n';
        std::exit (EXIT_FAILURE);
    }
}

void expectVisibleGraphCurve (const juce::Image& image)
{
    expect (image.isValid(), "editor snapshot is valid");
    const auto scale = std::min (static_cast<float> (image.getWidth()) / 900.0f,
                                 static_cast<float> (image.getHeight()) / 1670.0f);
    const auto graphX = juce::roundToInt (50.0f * scale);
    const auto graphY = juce::roundToInt (294.0f * scale);
    const auto graphWidth = juce::roundToInt (800.0f * scale);
    const auto graphHeight = juce::roundToInt (309.0f * scale);
    const juce::Rectangle<int> plot (graphX + 28, graphY + 16,
                                     graphWidth - 52, graphHeight - 24);
    expect (image.getBounds().contains (plot), "graph plot is contained in the snapshot");

    std::array<bool, 4096> curveRows {};
    int curvePixels = 0;
    for (auto y = plot.getY(); y < plot.getBottom(); ++y)
        for (auto x = plot.getX(); x < plot.getRight(); ++x)
        {
            const auto colour = image.getPixelAt (x, y);
            if (std::abs (static_cast<int> (colour.getRed()) - 141) <= 8
                && std::abs (static_cast<int> (colour.getGreen()) - 141) <= 8
                && std::abs (static_cast<int> (colour.getBlue()) - 141) <= 8)
            {
                ++curvePixels;
                curveRows[static_cast<std::size_t> (y - plot.getY())] = true;
            }
        }

    const auto variedRows = std::count (curveRows.begin(),
                                        curveRows.begin() + plot.getHeight(), true);
    expect (curvePixels > 100 && variedRows >= 4,
            "snapshot contains a visible, non-flat response curve");
}

void expectJuceSplashHasFaded (const juce::Image& image)
{
    const auto left = juce::jmax (0, image.getWidth() - 145);
    const auto top = juce::jmax (0, image.getHeight() - 85);
    int blueSplashPixels = 0;
    for (auto y = top; y < image.getHeight(); ++y)
        for (auto x = left; x < image.getWidth(); ++x)
        {
            const auto colour = image.getPixelAt (x, y);
            const auto red = static_cast<int> (colour.getRed());
            const auto green = static_cast<int> (colour.getGreen());
            const auto blue = static_cast<int> (colour.getBlue());
            if (blue > 80 && blue > red + 30 && blue > green + 20)
                ++blueSplashPixels;
        }

    expect (blueSplashPixels == 0, "JUCE splash logo has naturally faded before capture");
}

void processSnapshotAudioBlock (MorphosisAudioProcessor& processor)
{
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    buffer.setSample (0, 0, 0.25f);
    buffer.setSample (1, 0, 0.25f);
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
}

class TimedMessageLoopStopper final : private juce::Timer
{
public:
    void runFor (int milliseconds)
    {
        startTimer (milliseconds);
        juce::MessageManager::getInstance()->runDispatchLoop();
    }

private:
    void timerCallback() override
    {
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
};

void pumpEditorMessageLoop (int milliseconds)
{
    expect (juce::MessageManager::getInstance() != nullptr,
            "JUCE message manager exists for editor timers");
    TimedMessageLoopStopper stopper;
    stopper.runFor (milliseconds);
}

juce::AudioProcessor::BusesLayout makeLayout (int channels)
{
    juce::AudioProcessor::BusesLayout layout;
    const auto channelSet = channels == 1 ? juce::AudioChannelSet::mono()
                                          : juce::AudioChannelSet::stereo();
    layout.inputBuses.add (channelSet);
    layout.outputBuses.add (channelSet);
    return layout;
}

void setParameter (MorphosisAudioProcessor& processor, const char* id, float value)
{
    auto& state = processor.getParameters();
    auto* parameter = state.getParameter (id);
    expect (parameter != nullptr, "parameter exists");
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

juce::var property (const juce::var& object, const char* name)
{
    auto* dynamic = object.getDynamicObject();
    expect (dynamic != nullptr, "fixture row is an object");
    return dynamic->getProperty (name);
}

double number (const juce::var& object, const char* name)
{
    return static_cast<double> (property (object, name));
}

void checkLayoutsAndMetadata()
{
    MorphosisAudioProcessor processor;

    expect (processor.getName() == "MORPHOSIS", "product name is MORPHOSIS");
    expect (! processor.acceptsMidi(), "MIDI input is disabled");
    expect (! processor.producesMidi(), "MIDI output is disabled");
    expect (! processor.isMidiEffect(), "plugin is not a MIDI effect");
    expect (processor.getNumPrograms() == 289, "all 289 embedded cube names are programs");
    expect (juce::String (processor.getPresetName (0)) == "Null Cube",
            "first embedded preset name retains all glyphs");
    expect (juce::String (processor.getPresetName (1)) == "LPFlange.4",
            "dot-four embedded preset name retains punctuation");
    expect (juce::String (processor.getPresetName (43)) == "VowelSpace",
            "representative alphabetic preset name retains all glyphs");
    expect (processor.getBusCount (true) == 1, "one input bus");
    expect (processor.getBusCount (false) == 1, "one output bus");
    expect (morphosis::usesDotFourDistortionLayout (1),
            "dot-four preset selects alternate layout");
    expect (morphosis::usesDotFourDistortionLayout (38),
            "all dot-four names use the alternate layout");
    expect (morphosis::usesDotFourDistortionLayout (66),
            "later dot-four preset selects alternate layout");
    expect (! morphosis::usesDotFourDistortionLayout (0),
            "null cube keeps the regular layout");
    expect (! morphosis::usesDotFourDistortionLayout (4),
            "non-dot-four preset keeps the regular layout");

    MorphosisAudioProcessor routed;
    routed.setCurrentProgram (1);
    expect (routed.getParameterSnapshot().internalDistortion,
            "dot-four preset forces internal distortion on");
    setParameter (routed, morphosis::parameter_ids::internalDistortion, 0.0f);
    expect (routed.getParameterSnapshot().internalDistortion,
            "dot-four routing cannot be bypassed by the retained parameter");
    routed.setCurrentProgram (4);
    expect (! routed.getParameterSnapshot().internalDistortion,
            "non-dot-four preset forces internal distortion off");

    for (int index = 0; index < processor.getNumPrograms(); ++index)
    {
        expect (processor.getPresetName (index) != nullptr, "cube name is present");
        expect (juce::String (processor.getProgramName (index)).isNotEmpty(), "program name is nonempty");
    }

    expect (processor.checkBusesLayoutSupported (makeLayout (1)),
            "mono input/output layout is supported");
    expect (processor.checkBusesLayoutSupported (makeLayout (2)),
            "stereo input/output layout is supported");

    juce::AudioProcessor::BusesLayout inputMonoOutputStereo;
    inputMonoOutputStereo.inputBuses.add (juce::AudioChannelSet::mono());
    inputMonoOutputStereo.outputBuses.add (juce::AudioChannelSet::stereo());
    expect (! processor.checkBusesLayoutSupported (inputMonoOutputStereo),
            "mismatched mono/stereo layout is rejected");

    juce::AudioProcessor::BusesLayout quad;
    quad.inputBuses.add (juce::AudioChannelSet::quadraphonic());
    quad.outputBuses.add (juce::AudioChannelSet::quadraphonic());
    expect (! processor.checkBusesLayoutSupported (quad),
            "quadraphonic layout is rejected");
}

juce::var loadFixture()
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile ("MorphosisReference.json");
    expect (file.existsAsFile(), "frozen reference fixture is available");
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    expect (parsed.isObject(), "frozen reference is JSON object");
    expect (property (parsed, "schema").toString() == "morphosis-linear-reference/v1",
            "reference schema is current");
    expect (property (parsed, "cube_body_sha256").toString()
                == morphosis::cube_data::kCubeBodySha256,
            "reference cube provenance matches embedded table");
    return parsed;
}

void checkCoefficientsAndReferenceAudio()
{
    const auto fixture = loadFixture();
    const auto* cases = property (fixture, "cases").getArray();
    expect (cases != nullptr && cases->size() == 4, "four representative reference cases exist");

    for (const auto& row : *cases)
    {
        const auto preset = static_cast<int> (number (row, "preset"));
        const auto rate = number (row, "sample_rate");
        const auto snapshot = morphosis::ParameterSnapshot {
            preset,
            number (row, "frequency"),
            number (row, "morph"),
            number (row, "transform"),
            0.0, 0.0, 0.0, true, false, 0
        };
        const auto coefficients = morphosis::MorphosisDSP::makeCoefficients (
            snapshot.preset, snapshot.frequency, snapshot.morph, snapshot.transform, rate);
        expect (coefficients.valid, "reference coefficient set is valid");
        expectNear (coefficients.gain, number (row, "gain"), 4.0e-6, "cube gain matches reference");

        const auto* stages = property (row, "stages").getArray();
        expect (stages != nullptr && stages->size() == morphosis::kStageCount,
                "reference contains seven stages");
        for (int i = 0; i < morphosis::kStageCount; ++i)
        {
            const auto& stage = coefficients.stages[static_cast<std::size_t> (i)];
            const auto& expected = (*stages)[static_cast<std::size_t> (i)];
            const auto* values = expected.getArray();
            expect (values != nullptr && values->size() == 10, "reference stage has ten values");
            expectNear (stage.a, static_cast<double> ((*values)[0]), 5.0e-6, "pole a matches reference");
            expectNear (stage.b, static_cast<double> ((*values)[1]), 5.0e-6, "pole b matches reference");
            expectNear (stage.inputGain, static_cast<double> ((*values)[2]), 5.0e-6,
                        "pole input gain matches reference");
            expectNear (stage.z1, static_cast<double> ((*values)[3]), 5.0e-6, "zero z1 matches reference");
            expectNear (stage.z2, static_cast<double> ((*values)[4]), 5.0e-6, "zero z2 matches reference");
            expectNear (stage.radius, static_cast<double> ((*values)[5]), 5.0e-6,
                        "adapted radius matches reference");
            expectNear (stage.angle, static_cast<double> ((*values)[6]), 5.0e-6,
                        "adapted angle matches reference");
        }

        const auto* left = property (row, "left").getArray();
        const auto* right = property (row, "right").getArray();
        expect (left != nullptr && right != nullptr && left->size() == right->size(),
                "reference contains stereo recurrence samples");
        morphosis::MorphosisDSP dsp;
        dsp.prepare (rate, static_cast<int> (left->size()));
        dsp.beginBlock (snapshot);
        constexpr auto latency = morphosis::kMorphosisLatencySamples;
        for (int i = 0; i < static_cast<int> (left->size()) + latency; ++i)
        {
            dsp.advanceSample();
            const auto fixtureSample = i < static_cast<int> (left->size()) ? i : -1;
            const auto actualLeft = dsp.processSampleNoAdvance (static_cast<float> (
                0.1 * (fixtureSample == 0) - 0.05 * (fixtureSample == 1)
                + 0.025 * (fixtureSample == 2) - 0.0125 * (fixtureSample == 3)), 0);
            const auto actualRight = dsp.processSampleNoAdvance (static_cast<float> (
                (0.1 * (fixtureSample == 0) - 0.05 * (fixtureSample == 1)
                 + 0.025 * (fixtureSample == 2) - 0.0125 * (fixtureSample == 3)) * 0.37), 1);
            if (i < latency)
            {
                expectNear (actualLeft, 0.0, 1.0e-7, "reference impulse respects declared latency");
                expectNear (actualRight, 0.0, 1.0e-7, "stereo reference respects declared latency");
            }
            else
            {
                const auto referenceIndex = i - latency;
                if (referenceIndex < static_cast<int> (left->size()))
                {
                    expectNear (actualLeft, static_cast<double> ((*left)[referenceIndex]),
                                3.0e-5, "left recurrence audio matches reference after 64-sample latency");
                    expectNear (actualRight, static_cast<double> ((*right)[referenceIndex]),
                                3.0e-5, "right recurrence audio matches reference after 64-sample latency");
                }
            }
        }
    }
}

void checkResponseAndRateAdaptation()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (int preset = 0; preset < morphosis::cube_data::kRecordCount; ++preset)
            for (const auto controls : { std::array<double, 3> { -5.0, -5.0, -5.0 },
                                         std::array<double, 3> { 0.0, 0.0, 0.0 },
                                         std::array<double, 3> { 5.0, 5.0, 5.0 } })
            {
                const auto coefficients = morphosis::MorphosisDSP::makeCoefficients (
                    preset, controls[0], controls[1], controls[2], rate);
                expect (coefficients.valid && !coefficients.usedSafeFallback,
                        "every embedded cube has a recovered host-rate coefficient set");
                for (const auto& stage : coefficients.stages)
                    expect (std::isfinite (stage.a) && std::isfinite (stage.b)
                                && std::hypot (stage.a, stage.b) < 1.0,
                            "every embedded cube pole is finite and decaying");
            }

    expectNear (morphosis::MorphosisDSP::firmwareOutputSoftClip (0.5), 0.5, 1.0e-12,
                "soft clip matches at the lower knee");
    expectNear (morphosis::MorphosisDSP::firmwareOutputSoftClip (1.0), 0.875, 1.0e-12,
                "soft clip matches at unity");
    expectNear (morphosis::MorphosisDSP::firmwareOutputSoftClip (1.5), 1.0, 1.0e-12,
                "soft clip matches at the upper knee");
    expectNear (morphosis::MorphosisDSP::settledThresholdForSetting (6),
                morphosis::MorphosisDSP::settledThresholdForSetting (0) * 0.5, 1.0e-12,
                "settled nonlinear threshold halves every six verified setting steps");
    expectNear (morphosis::MorphosisDSP::transformThreshold (2.0, 0), 2.0 / 256.0,
                1.0e-12, "quantized Transform threshold map matches recovered formula");

    const auto atNative = morphosis::MorphosisDSP::makeCoefficients (48, 0.0, 0.0, 0.0,
                                                                       morphosis::kFirmwareNominalRate);
    const auto atHost = morphosis::MorphosisDSP::makeCoefficients (48, 0.0, 0.0, 0.0, 48000.0);
    expect (atNative.valid && atHost.valid, "native and host-rate coefficient sets are valid");
    expect (std::abs (atNative.stages[0].angle - atHost.stages[0].angle) > 1.0e-8
                || std::abs (atNative.stages[0].radius - atHost.stages[0].radius) > 1.0e-8,
            "host-rate adaptation does not silently reuse nominal coefficients");
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto coefficients = morphosis::MorphosisDSP::makeCoefficients (197, -2.0, 1.0, 3.0, rate);
        for (const auto frequency : { 20.0, 1000.0, rate * 0.49 })
            expect (std::isfinite (morphosis::MorphosisDSP::responseMagnitude (coefficients, frequency, rate)),
                    "computed response is finite at supported rate");
    }
}

void checkAudioSafetyAndStereoIsolation()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto channels : { 1, 2 })
            for (const auto blockSize : { 1, 7, 64, 127, 512 })
            {
                MorphosisAudioProcessor processor;
                expect (processor.setBusesLayout (makeLayout (channels)), "supported layout can be applied");
                processor.prepareToPlay (rate, blockSize);
                juce::AudioBuffer<float> buffer (channels, blockSize);
                buffer.clear();
                for (int sample = 0; sample < blockSize; ++sample)
                    buffer.setSample (0, sample, sample == 0 ? 0.25f : 0.0f);
                if (channels == 2)
                    buffer.clear (1, 0, blockSize);
                juce::MidiBuffer midi;
                processor.processBlock (buffer, midi);
                expect (midi.isEmpty(), "MIDI remains disabled and cleared");
                for (int channel = 0; channel < channels; ++channel)
                    for (int sample = 0; sample < blockSize; ++sample)
                        expect (std::isfinite (buffer.getSample (channel, sample)), "audio output is finite");
                if (channels == 2)
                    for (int sample = 0; sample < blockSize; ++sample)
                        expect (buffer.getSample (1, sample) == 0.0f, "stereo state does not cross-feed");
            }

    MorphosisAudioProcessor extreme;
    expect (extreme.setBusesLayout (makeLayout (2)), "stereo layout for extreme test");
    extreme.prepareToPlay (48000.0, 64);
    setParameter (extreme, morphosis::parameter_ids::inputGainDb, 24.0f);
    setParameter (extreme, morphosis::parameter_ids::preClipGainDb, 24.0f);
    setParameter (extreme, morphosis::parameter_ids::postClipGainDb, 24.0f);
    setParameter (extreme, morphosis::parameter_ids::softClip, 0.0f);
    juce::AudioBuffer<float> extremeBuffer (2, 64);
    extremeBuffer.clear();
    extremeBuffer.addFrom (0, 0, std::vector<float> (64, 4.0f).data(), 64, 1.0f);
    juce::MidiBuffer midi;
    extreme.processBlock (extremeBuffer, midi);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 64; ++sample)
            expect (std::isfinite (extremeBuffer.getSample (channel, sample)), "extreme output recovers finite");
}

void checkPresetTransitionsAndState()
{
    MorphosisAudioProcessor processor;
    expect (processor.setBusesLayout (makeLayout (2)), "stereo state test layout");
    processor.prepareToPlay (48000.0, 64);
    setParameter (processor, morphosis::parameter_ids::frequency, 2.5f);
    setParameter (processor, morphosis::parameter_ids::morph, -1.5f);
    setParameter (processor, morphosis::parameter_ids::transform, 3.0f);
    setParameter (processor, morphosis::parameter_ids::inputGainDb, -7.0f);
    setParameter (processor, morphosis::parameter_ids::preClipGainDb, 4.0f);
    setParameter (processor, morphosis::parameter_ids::postClipGainDb, 5.0f);
    setParameter (processor, morphosis::parameter_ids::softClip, 1.0f);
    setParameter (processor, morphosis::parameter_ids::internalDistortion, 1.0f);
    setParameter (processor, morphosis::parameter_ids::internalThresholdSetting, 6.0f);
    processor.setCurrentProgram (197);

    for (int block = 0; block < 6; ++block)
    {
        juce::AudioBuffer<float> buffer (2, 64);
        buffer.clear();
        buffer.setSample (0, 0, 0.4f);
        buffer.setSample (1, 0, -0.3f);
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 64; ++sample)
                expect (std::isfinite (buffer.getSample (channel, sample)), "preset transition is finite");
        if (block == 2)
            processor.setCurrentProgram (288);
    }

    const auto before = processor.getParameterSnapshot();
    juce::MemoryBlock state;
    processor.getStateInformation (state);
    setParameter (processor, morphosis::parameter_ids::frequency, -5.0f);
    processor.setCurrentProgram (0);
    processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    const auto after = processor.getParameterSnapshot();
    expect (after.preset == before.preset, "state restores selected preset");
    expectNear (after.frequency, before.frequency, 1.0e-5, "state restores frequency");
    expectNear (after.morph, before.morph, 1.0e-5, "state restores morph");
    expectNear (after.transform, before.transform, 1.0e-5, "state restores transform");
    expectNear (after.inputGainDb, before.inputGainDb, 1.0e-5, "state restores input gain");
    expect (after.softClip == before.softClip && after.internalDistortion == before.internalDistortion,
            "state restores clip switches");

    const std::unique_ptr<juce::XmlElement> savedXml (
        juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize())));
    expect (savedXml != nullptr, "state has a parseable XML envelope");
    auto xmlText = savedXml->toString();
    xmlText = xmlText.replace ("id=\"preset\"", "id=\"unknown\"");
    const std::unique_ptr<juce::XmlElement> invalidXml (juce::XmlDocument::parse (xmlText));
    expect (invalidXml != nullptr, "modified state remains parseable XML");
    juce::MemoryBlock invalid;
    juce::AudioProcessor::copyXmlToBinary (*invalidXml, invalid);
    const auto stablePreset = processor.getPresetIndex();
    processor.setStateInformation (invalid.getData(), static_cast<int> (invalid.getSize()));
    expect (processor.getPresetIndex() == stablePreset, "unknown state parameter is rejected");
}

void checkPresetTaxonomies()
{
    for (const auto grouping : { morphosis::preset_taxonomy::Grouping::recommended,
                                 morphosis::preset_taxonomy::Grouping::manual })
    {
        const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
        std::array<int, 289> occurrences {};

        for (std::size_t categoryIndex = 0; categoryIndex < taxonomy.categoryCount; ++categoryIndex)
        {
            const auto& category = taxonomy.categories[categoryIndex];
            for (std::size_t subcategoryIndex = 0;
                 subcategoryIndex < category.subcategoryCount;
                 ++subcategoryIndex)
            {
                const auto& subcategory = category.subcategories[subcategoryIndex];
                for (std::size_t presetIndex = 0;
                     presetIndex < subcategory.presetCount;
                     ++presetIndex)
                {
                    const auto id = subcategory.presetIds[presetIndex];
                    expect (id >= 0 && id < static_cast<int> (occurrences.size()),
                            "taxonomy preset ID is in the embedded program range");
                    ++occurrences[static_cast<std::size_t> (id)];
                }
            }
        }

        for (const auto occurrence : occurrences)
            expect (occurrence == 1, "each taxonomy contains every preset exactly once");
    }
}

void checkModeModelAndState()
{
    expectNear (morphosis::sequenceDivisionQuarterBeats (
                    static_cast<int> (morphosis::SequenceDivision::quarter)),
                1.0, 1.0e-12, "quarter sequence division is one beat");
    expectNear (morphosis::sequenceDivisionQuarterBeats (
                    static_cast<int> (morphosis::SequenceDivision::thirtySecond)),
                0.125, 1.0e-12, "thirty-second sequence division is one eighth beat");
    expectNear (morphosis::sequencePeriodQuarterBeats (
                    static_cast<int> (morphosis::SequenceDivision::quarter),
                    static_cast<int> (morphosis::SequenceSync::dotted)),
                1.5, 1.0e-12, "dotted timing multiplies the period");

    MorphosisAudioProcessor processor;
    auto initial = processor.getParameterSnapshot();
    expect (initial.sequenceDivision == static_cast<int> (morphosis::SequenceDivision::quarter),
            "sequencer defaults to 1/4");
    expectNear (initial.sequenceGlide, 0.0, 1.0e-12,
                "sequencer glide defaults to its minimum");
    expect (initial.sequenceLength == 4, "sequencer defaults to four active steps");

    processor.activateSequencer();
    initial = processor.getParameterSnapshot();
    expect (initial.mode == morphosis::ProcessingMode::sequencer,
            "sequencer activation selects the sequencer mode");
    expect (processor.getSequencerStep() == 0,
            "fresh sequencer activation starts with step zero");
    for (const auto preset : initial.sequencePresets)
        expect (preset == initial.preset, "first sequencer activation copies the regular preset");

    const std::array<int, morphosis::kMaxBlendSources> cornerPresets { 3, 17, 41, 88 };
    const std::array<bool, morphosis::kMaxBlendSources> cornerNonlinear { false, true, false, true };
    const auto topLeft = morphosis::MorphosisDSP::makeBilinearBlend (
        cornerPresets, cornerNonlinear, 0.0, 1.0);
    expect (topLeft.count == 1 && topLeft.presets[0] == 3
                && topLeft.weights[0] == 1.0,
            "XY top-left is a one-hot A corner");
    const auto bottomRight = morphosis::MorphosisDSP::makeBilinearBlend (
        cornerPresets, cornerNonlinear, 1.0, 0.0);
    expect (bottomRight.count == 1 && bottomRight.presets[0] == 88
                && bottomRight.weights[0] == 1.0,
            "XY bottom-right is a one-hot D corner");
    const auto centre = morphosis::MorphosisDSP::makeBilinearBlend (
        cornerPresets, cornerNonlinear, 0.5, 0.5);
    expect (centre.count == morphosis::kMaxBlendSources,
            "XY centre keeps all four source descriptors");
    double centreWeightSum = 0.0;
    for (int index = 0; index < centre.count; ++index)
        centreWeightSum += centre.weights[static_cast<std::size_t> (index)];
    expectNear (centreWeightSum, 1.0, 1.0e-12, "XY weights remain normalized");

    std::array<int, morphosis::kSequenceSlotCount> sequencePresets {};
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        sequencePresets[static_cast<std::size_t> (index)] = index + 10;
    morphosis::ParameterSnapshot manualSnapshot;
    manualSnapshot.mode = morphosis::ProcessingMode::sequencer;
    manualSnapshot.sequenceManual = true;
    manualSnapshot.sequenceLength = 4;
    manualSnapshot.sequencePosition = 0.5;
    manualSnapshot.sequencePresets = sequencePresets;
    morphosis::MorphosisDSP manualDsp;
    manualDsp.prepare (48000.0, 64);
    manualDsp.beginBlock (manualSnapshot);
    const auto& allStepsMiddle = manualDsp.getCurrentBlend();
    expect (allStepsMiddle.count == 1 && allStepsMiddle.presets[0] == 18,
            "production Manual mode selects the nearest one of all sixteen steps");

    manualSnapshot.sequencePosition = 1.0;
    morphosis::MorphosisDSP manualEndpointDsp;
    manualEndpointDsp.prepare (48000.0, 64);
    manualEndpointDsp.beginBlock (manualSnapshot);
    const auto& allStepsLast = manualEndpointDsp.getCurrentBlend();
    expect (allStepsLast.count == 1 && allStepsLast.presets[0] == 25,
            "production Manual endpoint reaches sequence step sixteen");

    setParameter (processor, morphosis::parameter_ids::sequenceSource, 1.0f);
    setParameter (processor, morphosis::parameter_ids::sequencePosition, 0.5f);
    setParameter (processor, morphosis::parameter_ids::sequenceLength, 4.0f);
    processor.setSequenceSlot (0, 10);
    processor.setSequenceSlot (1, 11);
    processor.setSequenceSlot (2, 12);
    processor.setSequenceSlot (3, 13);
    const auto before = processor.getParameterSnapshot();
    expect (before.sequenceManual, "manual source state is retained");
    expectNear (before.sequencePosition, 0.5, 1.0e-5,
                "manual position is host automatable state");

    juce::MemoryBlock state;
    processor.getStateInformation (state);
    MorphosisAudioProcessor restored;
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    const auto after = restored.getParameterSnapshot();
    expect (after.mode == morphosis::ProcessingMode::sequencer,
            "state recalls sequencer mode");
    expect (after.sequenceManual, "state recalls manual source mode");
    expectNear (after.sequencePosition, before.sequencePosition, 1.0e-5,
                "state recalls manual position");
    for (int index = 0; index < 4; ++index)
        expect (after.sequencePresets[static_cast<std::size_t> (index)] == 10 + index,
                "state recalls sequence slot cube IDs");

    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto blockSize : { 1, 64, 512 })
        {
            MorphosisAudioProcessor modes;
            expect (modes.setBusesLayout (makeLayout (2)),
                    "mode validation accepts stereo layout");
            modes.prepareToPlay (rate, blockSize);
            setParameter (modes, morphosis::parameter_ids::softClip, 0.0f);
            modes.activateXY();
            modes.setXYSlot (0, 3);
            modes.setXYSlot (1, 17);
            modes.setXYSlot (2, 41);
            modes.setXYSlot (3, 88);

            for (int pass = 0; pass < 3; ++pass)
            {
                setParameter (modes, morphosis::parameter_ids::xyX,
                              pass == 1 ? 1.0f : 0.1f);
                setParameter (modes, morphosis::parameter_ids::xyY,
                              pass == 2 ? 0.9f : 0.2f);
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();
                for (int sample = 0; sample < blockSize; ++sample)
                    buffer.setSample (0, sample, static_cast<float> (0.23 * std::sin (
                        0.01 * static_cast<double> (sample + pass * blockSize))));
                juce::MidiBuffer midi;
                modes.processBlock (buffer, midi);
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    expect (std::isfinite (buffer.getSample (0, sample)),
                            "XY mode remains finite during movement");
                    expect (buffer.getSample (1, sample) == 0.0f,
                            "XY stereo state remains isolated");
                }
            }

            modes.activateSequencer();
            setParameter (modes, morphosis::parameter_ids::sequenceSource, 1.0f);
            for (int index = 0; index < 4; ++index)
                modes.setSequenceSlot (index, 10 + index);
            for (const auto position : { 0.0f, 0.33f, 0.66f, 1.0f, 0.12f })
            {
                setParameter (modes, morphosis::parameter_ids::sequencePosition, position);
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();
                buffer.addFrom (0, 0, std::vector<float> (static_cast<std::size_t> (blockSize),
                                                            0.17f).data(),
                                blockSize, 1.0f);
                juce::MidiBuffer midi;
                modes.processBlock (buffer, midi);
                for (int sample = 0; sample < blockSize; ++sample)
                    expect (std::isfinite (buffer.getSample (0, sample)),
                            "manual sequence mode remains finite during scans");
            }

            setParameter (modes, morphosis::parameter_ids::sequenceSource, 0.0f);
            for (int pass = 0; pass < 3; ++pass)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();
                buffer.setSample (0, 0, 0.17f);
                juce::MidiBuffer midi;
                modes.processBlock (buffer, midi);
                for (int sample = 0; sample < blockSize; ++sample)
                    expect (std::isfinite (buffer.getSample (0, sample)),
                            "host sequence mode remains finite without a playhead");
            }
        }
}

void checkExtraBufferChannelAndEditor()
{
    MorphosisAudioProcessor processor;
    expect (processor.setBusesLayout (makeLayout (2)), "stereo layout can be applied");
    processor.prepareToPlay (48000.0, 8);
    juce::AudioBuffer<float> buffer (3, 8);
    buffer.clear();
    for (int sample = 0; sample < 8; ++sample)
        buffer.setSample (2, sample, -0.375f);
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    for (int sample = 0; sample < 8; ++sample)
        expect (buffer.getSample (2, sample) == -0.375f, "surplus process-buffer channel is untouched");

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    expect (editor != nullptr, "fresh editor is constructed");
    expect (editor->getWidth() == 600 && editor->getHeight() == 1113,
            "editor starts at a screen-friendly portrait size");
    expectNear (static_cast<double> (editor->getWidth()) / editor->getHeight(),
                900.0 / 1670.0, 0.001,
                "editor preserves the portrait design aspect ratio");
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc == 3 && juce::String (argv[1]) == "--render-editor")
    {
        auto processor = std::make_unique<MorphosisAudioProcessor>();
        constexpr auto snapshotPreset = 4;
        expect (juce::String (processor->getPresetName (snapshotPreset)) != "Null Cube",
                "snapshot fixture uses a non-null preset");
        processor->prepareToPlay (48000.0, 512);
        processor->setCurrentProgram (snapshotPreset);
        MorphosisAudioProcessor::SequencePresetArray sequencePresets {};
        sequencePresets.fill (snapshotPreset);
        processor->setSequenceSlotsBatch (sequencePresets);
        processor->activateSequencer();
        setParameter (*processor, morphosis::parameter_ids::sequenceSource, 0.0f);
        const auto configuredSlots = processor->getParameterSnapshot().sequencePresets;
        expect (std::all_of (configuredSlots.begin(), configuredSlots.end(),
                             [snapshotPreset] (int preset) { return preset == snapshotPreset; }),
                "all active sequence slots use the non-null snapshot preset");

        processSnapshotAudioBlock (*processor);
        setParameter (*processor, morphosis::parameter_ids::sequenceSource, 1.0f);
        setParameter (*processor, morphosis::parameter_ids::sequencePosition, 0.72f);
        setParameter (*processor, morphosis::parameter_ids::manualTransitionMs, 145.0f);
        processSnapshotAudioBlock (*processor);

        auto editor = std::make_unique<MorphosisAudioProcessorEditor> (*processor);
        const juce::Point<int> sizes[] { { 720, 1336 }, { 900, 1670 }, { 1080, 2004 } };
        const auto outputDirectory = juce::File (argv[2]);
        outputDirectory.createDirectory();
        editor->setVisible (true);
        const auto writeSnapshot = [&] (juce::Point<int> size, const juce::String& mode)
        {
            editor->setSize (size.x, size.y);
            MorphosisEditorTestAccess::refresh (*editor);
            expect (editor->getWidth() == size.x && editor->getHeight() == size.y,
                    "portrait snapshot keeps its requested editor size");
            if (mode == "host" || mode == "manual")
            {
                const auto manual = mode == "manual";
                expect (MorphosisEditorTestAccess::sequencePositionVisible (*editor) == manual
                            && MorphosisEditorTestAccess::sequenceSyncVisible (*editor) != manual
                            && MorphosisEditorTestAccess::sequenceLengthVisible (*editor) != manual,
                        "snapshot captures the requested host or MANUAL sequencer state");
                expect (MorphosisEditorTestAccess::sequenceGridVisible (*editor)
                            && MorphosisEditorTestAccess::sequenceGlideVisible (*editor)
                            && MorphosisEditorTestAccess::sequenceVisibleLength (*editor)
                                   == (manual ? morphosis::kSequenceSlotCount : 4),
                        "snapshot keeps all manual slots and the lower transition/glide control visible");
            }
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
            expectVisibleGraphCurve (image);
            expectJuceSplashHasFaded (image);
            const auto file = outputDirectory.getChildFile (
                "morphosis-portrait-" + mode + "-" + juce::String (size.x)
                + "x" + juce::String (size.y) + ".png");
            juce::FileOutputStream output (file);
            expect (output.openedOk(), "snapshot output opens");
            output.setPosition (0);
            output.truncate();
            expect (juce::PNGImageFormat().writeImageToStream (image, output),
                    "editor snapshot writes");
        };

        const auto splashPrime = editor->createComponentSnapshot (editor->getLocalBounds());
        expect (splashPrime.isValid(), "initial editor render starts the licensed JUCE splash timer");
        pumpEditorMessageLoop (4500);
        setParameter (*processor, morphosis::parameter_ids::sequenceSource, 0.0f);
        MorphosisEditorTestAccess::refresh (*editor);
        for (const auto size : sizes)
            writeSnapshot (size, "host");

        setParameter (*processor, morphosis::parameter_ids::sequenceSource, 1.0f);
        setParameter (*processor, morphosis::parameter_ids::sequencePosition, 0.72f);
        setParameter (*processor, morphosis::parameter_ids::manualTransitionMs, 145.0f);
        MorphosisEditorTestAccess::refresh (*editor);
        for (const auto size : sizes)
            writeSnapshot (size, "manual");

        processor->returnToPresetMode();
        processSnapshotAudioBlock (*processor);
        pumpEditorMessageLoop (100);
        for (const auto size : sizes)
            writeSnapshot (size, "preset");

        MorphosisAudioProcessor::XYPresetArray xyPresets {};
        xyPresets.fill (snapshotPreset);
        processor->setXYSlotsBatch (xyPresets);
        processor->activateXY();
        processSnapshotAudioBlock (*processor);
        pumpEditorMessageLoop (100);
        for (const auto size : sizes)
            writeSnapshot (size, "xy");
        return EXIT_SUCCESS;
    }
    checkLayoutsAndMetadata();
    checkCoefficientsAndReferenceAudio();
    checkResponseAndRateAdaptation();
    checkAudioSafetyAndStereoIsolation();
    checkPresetTransitionsAndState();
    checkPresetTaxonomies();
    checkModeModelAndState();
    checkExtraBufferChannelAndEditor();

    std::cout << "Morphosis smoke test passed: 289 programs, exact coefficient/audio fixtures, "
                 "44.1/48/96 kHz mono/stereo safety, transitions, state recall, and editor construction.\n";
    return EXIT_SUCCESS;
}
