#include "PluginProcessor.h"
#include "PresetPickerModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

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
    if (! std::isfinite (actual) || ! std::isfinite (expected)
        || std::abs (actual - expected) > tolerance)
    {
        std::cerr << "FAIL: " << message << " actual=" << actual
                  << " expected=" << expected << '\n';
        std::exit (EXIT_FAILURE);
    }
}

juce::AudioProcessor::BusesLayout stereoLayout()
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    return layout;
}

void setParameter (MorphosisAudioProcessor& processor, const char* id, float value)
{
    auto& state = processor.getParameters();
    auto* parameter = state.getParameter (id);
    expect (parameter != nullptr, "feature-test parameter exists");
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

struct StereoValue
{
    float left = 0.0f;
    float right = 0.0f;
};

StereoValue renderSteady (double dryWet, double preClipGainDb = 0.0)
{
    MorphosisAudioProcessor processor;
    expect (processor.setBusesLayout (stereoLayout()), "dry/wet stereo layout is supported");
    processor.prepareToPlay (48000.0, 256);
    setParameter (processor, morphosis::parameter_ids::preset, 0.0f);
    setParameter (processor, morphosis::parameter_ids::inputGainDb, 12.0f);
    setParameter (processor, morphosis::parameter_ids::preClipGainDb,
                  static_cast<float> (preClipGainDb));
    setParameter (processor, morphosis::parameter_ids::postClipGainDb, 0.0f);
    setParameter (processor, morphosis::parameter_ids::softClip, 0.0f);
    setParameter (processor, morphosis::parameter_ids::dryWet, static_cast<float> (dryWet));

    StereoValue last;
    for (int block = 0; block < 18; ++block)
    {
        juce::AudioBuffer<float> buffer (2, 256);
        buffer.clear();
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            buffer.setSample (0, sample, 0.25f);
            buffer.setSample (1, sample, -0.4f);
        }
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        last.left = buffer.getSample (0, buffer.getNumSamples() - 1);
        last.right = buffer.getSample (1, buffer.getNumSamples() - 1);
    }
    expect (! processor.hasSafetyFault(), "dry/wet endpoint remains safety-fault free");
    return last;
}

void checkDryWetDefaultsAndMigration()
{
    MorphosisAudioProcessor defaults;
    expectNear (defaults.getParameterSnapshot().dryWet, 1.0, 1.0e-12,
                "new instances default to 100% wet");
    expect (defaults.getParameterSnapshot().xyInterpolation
                == morphosis::XYInterpolationMode::response,
            "new instances default to Response XY interpolation");
    expect (! defaults.getParameterSnapshot().xyEncodedDomain,
            "new instances keep the encoded XY override off");

    juce::MemoryBlock state;
    defaults.getStateInformation (state);
    auto xml = std::unique_ptr<juce::XmlElement> (
        juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize())));
    expect (xml != nullptr, "default state has XML envelope");
    auto valueTree = juce::ValueTree::fromXml (*xml);
    for (int index = valueTree.getNumChildren() - 1; index >= 0; --index)
        if (valueTree.getChild (index).getProperty ("id").toString()
            == morphosis::parameter_ids::dryWet)
            valueTree.removeChild (index, nullptr);
    valueTree.setProperty ("stateVersion", 3, nullptr);
    const auto oldXml = valueTree.createXml();
    expect (oldXml != nullptr, "old state fixture is serializable");
    juce::MemoryBlock oldState;
    juce::AudioProcessor::copyXmlToBinary (*oldXml, oldState);

    MorphosisAudioProcessor restored;
    restored.setStateInformation (oldState.getData(), static_cast<int> (oldState.getSize()));
    expectNear (restored.getParameterSnapshot().dryWet, 1.0, 1.0e-12,
                "states without dry/wet migrate to 100% wet");

    const auto wet = renderSteady (1.0);
    const auto dry = renderSteady (0.0);
    const auto dryWithPreClip = renderSteady (0.0, 18.0);
    const auto midpoint = renderSteady (0.5);
    const auto inputGain = std::pow (10.0, 12.0 / 20.0);
    expectNear (dry.left, 0.25 * inputGain, 2.0e-3,
                "0% wet keeps input gain but bypasses filter/preclip on left");
    expectNear (dry.right, -0.4 * inputGain, 2.0e-3,
                "0% wet keeps input gain but bypasses filter/preclip on right");
    expectNear (dryWithPreClip.left, dry.left, 2.0e-3,
                "0% wet bypasses pre-clip gain on left");
    expectNear (dryWithPreClip.right, dry.right, 2.0e-3,
                "0% wet bypasses pre-clip gain on right");
    expectNear (wet.left, 0.25 * inputGain, 2.0e-3,
                "100% wet retains the existing input gain path on left");
    expectNear (wet.right, -0.4 * inputGain, 2.0e-3,
                "100% wet retains the existing input gain path on right");
    expectNear (midpoint.left, (dry.left + wet.left) * 0.5, 3.0e-3,
                "midpoint is a linear dry/wet mix on left");
    expectNear (midpoint.right, (dry.right + wet.right) * 0.5, 3.0e-3,
                "midpoint is a linear dry/wet mix on right");
}

void checkDryWetAutomationAndIsolation()
{
    MorphosisAudioProcessor processor;
    expect (processor.setBusesLayout (stereoLayout()), "automation stereo layout is supported");
    processor.prepareToPlay (44100.0, 64);
    setParameter (processor, morphosis::parameter_ids::inputGainDb, 12.0f);
    setParameter (processor, morphosis::parameter_ids::dryWet, 0.0f);
    setParameter (processor, morphosis::parameter_ids::softClip, 0.0f);

    float previous = 0.0f;
    bool sawChange = false;
    for (int block = 0; block < 80; ++block)
    {
        const auto amount = block % 2 == 0 ? 0.0f : 1.0f;
        setParameter (processor, morphosis::parameter_ids::dryWet, amount);
        juce::AudioBuffer<float> buffer (2, 64);
        for (int sample = 0; sample < 64; ++sample)
        {
            buffer.setSample (0, sample, 0.21f);
            buffer.setSample (1, sample, -0.17f);
        }
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        for (int sample = 0; sample < 64; ++sample)
        {
            const auto value = buffer.getSample (0, sample);
            expect (std::isfinite (value), "dry/wet automation remains finite");
            if (block != 0 || sample != 0)
                expect (std::abs (value - previous) < 100.0f,
                        "dry/wet automation has no discontinuous numeric jump");
            previous = value;
            sawChange = sawChange || std::abs (value - 0.21f) > 1.0e-4f;
        }
    }
    expect (sawChange, "dry/wet automation actually changes the processed path");
    expect (! processor.hasSafetyFault(), "dry/wet automation does not trigger safety recovery");
}

void checkPickerModel()
{
    const auto all = morphosis::preset_picker::search ({});
    expect (all.size() == 289, "empty search restores all canonical presets");
    const auto numeric = morphosis::preset_picker::search ("289");
    expect (std::find (numeric.begin(), numeric.end(), 288) != numeric.end(),
            "numeric search matches displayed preset id");
    const auto caseInsensitive = morphosis::preset_picker::search ("vOwElSpAcE");
    expect (std::find (caseInsensitive.begin(), caseInsensitive.end(), 43) != caseInsensitive.end(),
            "search matches names case-insensitively");
    expect (morphosis::preset_picker::search ("definitely-missing").empty(),
            "missing search returns no results");

    morphosis::preset_picker::FavouriteFlags flags {};
    flags[0] = true;
    flags[184] = true;
    const auto encoded = morphosis::preset_picker::encodeFavourites (flags);
    const auto decoded = morphosis::preset_picker::decodeFavourites (encoded);
    expect (decoded == flags, "favourites persist by canonical id, independent of grouping");
    morphosis::preset_picker::toggleFavourite (flags, 43);
    expect (flags[43] && flags[0] && flags[184],
            "star toggle changes favourite state without changing preset identity");
    morphosis::preset_picker::toggleFavourite (flags, 43);
    expect (! flags[43], "second star toggle restores the prior favourite state");
    const auto listed = morphosis::preset_picker::favourites (decoded);
    expect (listed.size() == 2 && listed[0] == 0 && listed[1] == 184,
            "favourites list is stable in canonical numeric order");
}

void checkCategoryPresetNavigation()
{
    using morphosis::preset_taxonomy::Grouping;

    for (const auto grouping : { Grouping::recommended, Grouping::manual })
    {
        const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
        std::array<int, morphosis::cube_data::kRecordCount> appearances {};
        auto flattenedCategories = 0;
        auto multiSubcategoryCategories = 0;
        auto foundManualComplexFilters = false;

        const auto recordPreset = [&] (int preset)
        {
            expect (preset >= 0 && preset < morphosis::cube_data::kRecordCount,
                    "category navigation keeps preset IDs within canonical bounds");
            if (preset >= 0 && preset < morphosis::cube_data::kRecordCount)
                ++appearances[static_cast<std::size_t> (preset)];
        };

        for (std::size_t categoryIndex = 0;
             categoryIndex < taxonomy.categoryCount; ++categoryIndex)
        {
            const auto& category = taxonomy.categories[categoryIndex];
            const auto* direct = morphosis::preset_picker::singleSubcategoryFor (category);
            if (grouping == Grouping::manual && category.name != nullptr
                && juce::String::fromUTF8 (category.name) == "06 Complex Filters")
            {
                foundManualComplexFilters = true;
                expect (direct == nullptr && category.subcategoryCount == 4,
                        "manual Complex Filters keeps its four-child submenu");
            }
            if (direct != nullptr)
            {
                ++flattenedCategories;
                expect (category.subcategoryCount == 1
                            && direct == &category.subcategories[0],
                        "single-child navigation exposes exactly its existing subcategory");
                expect (direct->name != nullptr && direct->name[0] != '\0',
                        "flattened flyout retains the subcategory label");
                expect (juce::String::fromUTF8 (
                            morphosis::preset_picker::categoryContentsName (category))
                            == juce::String::fromUTF8 (direct->name),
                        "single-child category content retains the subcategory title");
                for (std::size_t row = 0; row < direct->presetCount; ++row)
                    recordPreset (direct->presetIds[row]);
            }
            else
            {
                ++multiSubcategoryCategories;
                expect (category.subcategoryCount > 1,
                        "categories with multiple children retain the existing submenu path");
                expect (juce::String::fromUTF8 (
                            morphosis::preset_picker::categoryContentsName (category))
                            == juce::String::fromUTF8 (category.name),
                        "multi-child category keeps its category title");
                for (std::size_t child = 0; child < category.subcategoryCount; ++child)
                {
                    const auto& subcategory = category.subcategories[child];
                    expect (subcategory.name != nullptr && subcategory.name[0] != '\0',
                            "multi-child navigation retains each subcategory label and order");
                    for (std::size_t row = 0; row < subcategory.presetCount; ++row)
                        recordPreset (subcategory.presetIds[row]);
                }
            }
        }

        expect (flattenedCategories > 0 && multiSubcategoryCategories > 0,
                "each grouping exercises flattened and unchanged category paths");
        if (grouping == Grouping::manual)
            expect (foundManualComplexFilters,
                    "manual grouping retains its Complex Filters category");
        for (std::size_t preset = 0; preset < appearances.size(); ++preset)
            expect (appearances[preset] == 1,
                    "each grouping exposes every canonical preset exactly once in source order");
    }
}

void checkBatchRandomizationAndState()
{
    MorphosisAudioProcessor processor;
    setParameter (processor, morphosis::parameter_ids::frequency, 2.0f);
    setParameter (processor, morphosis::parameter_ids::morph, -1.0f);
    setParameter (processor, morphosis::parameter_ids::transform, 3.0f);
    processor.activateSequencer();
    expect (processor.getProcessingMode() == morphosis::ProcessingMode::sequencer,
            "sequence randomization preserves sequencer mode");
    const auto before = processor.getParameterSnapshot();
    processor.randomizeSequencePresets();
    const auto sequence = processor.getParameterSnapshot();
    expect (sequence.mode == morphosis::ProcessingMode::sequencer,
            "random sequence preserves active mode");
    expectNear (sequence.frequency, before.frequency, 1.0e-5,
                "random sequence preserves frequency control");
    for (const auto preset : sequence.sequencePresets)
        expect (preset >= 0 && preset < 289, "random sequence uses canonical bounds");

    const MorphosisAudioProcessor::SequencePresetArray explicitSequence {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    processor.setSequenceSlotsBatch (explicitSequence);
    expect (processor.getParameterSnapshot().sequencePresets == explicitSequence,
            "sequence batch commit updates all sixteen slots together");

    processor.activateXY();
    const auto xyBefore = processor.getParameterSnapshot();
    processor.randomizeXYPresets();
    const auto xy = processor.getParameterSnapshot();
    expect (xy.mode == morphosis::ProcessingMode::xy, "random XY preserves active mode");
    expectNear (xyBefore.frequency, xy.frequency, 1.0e-5,
                "random XY preserves frequency control");
    for (const auto preset : xy.xyPresets)
        expect (preset >= 0 && preset < 289, "random XY uses canonical bounds");

    const auto stablePreset = processor.getPresetIndex();
    processor.randomizePreset();
    expect (processor.getPresetIndex() == stablePreset,
            "random regular preset does not silently switch out of XY mode");

    juce::MemoryBlock state;
    processor.getStateInformation (state);
    MorphosisAudioProcessor restored;
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    const auto restoredSnapshot = restored.getParameterSnapshot();
    expect (restoredSnapshot.xyPresets == xy.xyPresets,
            "state recall preserves the complete randomized XY assignment");
    expect (restoredSnapshot.sequencePresets == processor.getParameterSnapshot().sequencePresets,
            "state recall preserves the complete randomized sequence assignment");
}

} // namespace

int main()
{
    checkDryWetDefaultsAndMigration();
    checkDryWetAutomationAndIsolation();
    checkPickerModel();
    checkCategoryPresetNavigation();
    checkBatchRandomizationAndState();
    std::cout << "Preset feature regressions passed: dry/wet defaults, migration, linear mix, "
                 "automation, stereo isolation, search, favourites, randomization, and state batches.\n";
    return EXIT_SUCCESS;
}
