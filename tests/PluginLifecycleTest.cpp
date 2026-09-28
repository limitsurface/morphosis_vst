#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PresetPickerModel.h"
#include "PresetRouting.h"
#include "PresetTaxonomy.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#if JUCE_WINDOWS
#include <windows.h>
#endif

struct MorphosisEditorTestAccess
{
    static void refresh (MorphosisAudioProcessorEditor& editor)
    {
        editor.timerCallback();
    }

    static void forgetShellVisualState (MorphosisAudioProcessorEditor& editor)
    {
        editor.hasShellVisualState = false;
    }

    static juce::Rectangle<int> transformBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.transform.getBounds();
    }

    static juce::Rectangle<float> designArea (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.getDesignArea();
    }

    static float designScale (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.designScale;
    }

    static void selectSequenceTab (MorphosisAudioProcessorEditor& editor)
    {
        if (editor.sequenceTab.onClick != nullptr)
            editor.sequenceTab.onClick();
    }

    static void selectXYTab (MorphosisAudioProcessorEditor& editor)
    {
        if (editor.xyTab.onClick != nullptr)
            editor.xyTab.onClick();
    }

    static void toggleSelectedPower (MorphosisAudioProcessorEditor& editor)
    {
        if (editor.modePower.onClick != nullptr)
            editor.modePower.onClick();
    }

    static bool sequenceTabSelected (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceTab.isSelected();
    }

    static bool xyTabSelected (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.xyTab.isSelected();
    }

    static bool modePowerActive (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.modePower.isActive();
    }

    static bool sequenceGridVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGrid.isVisible();
    }

    static bool sequenceDivisionVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceDivisionBox.isVisible();
    }

    static bool sequenceSyncVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceSyncBox.isVisible();
    }

    static bool sequenceLengthVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceLengthBox.isVisible();
    }

    static bool sequenceGlideVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGlide.isVisible();
    }

    static bool sequencePositionVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequencePosition.isVisible();
    }

    static juce::Rectangle<int> sequenceDivisionBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceDivisionBox.getBounds();
    }

    static juce::Rectangle<int> sequenceSyncBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceSyncBox.getBounds();
    }

    static juce::Rectangle<int> sequenceLengthBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceLengthBox.getBounds();
    }

    static juce::Rectangle<int> sequenceGlideBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGlide.getBounds();
    }

    static juce::Slider& sequenceGlideSlider (MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGlide.getSlider();
    }

    static juce::String sequenceGlideCaption (
        const MorphosisAudioProcessorEditor& editor)
    {
        auto* label = dynamic_cast<MorphosisCaptionLabel*> (
            editor.sequenceGlide.getChildComponent (1));
        return label != nullptr ? label->getText() : juce::String();
    }

    static void selectSequenceDivision (MorphosisAudioProcessorEditor& editor, int itemId)
    {
        editor.sequenceDivisionBox.setSelectedId (itemId, juce::sendNotificationSync);
    }

    static juce::Rectangle<int> xyPadBounds (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.xyPad.getBounds();
    }

    static juce::Rectangle<int> xySlotBounds (
        const MorphosisAudioProcessorEditor& editor, int slot)
    {
        const auto origin = editor.xyPad.getBounds().getPosition();
        return editor.xyPad.getSlotBounds (slot).translated (origin.x, origin.y);
    }

    static MorphosisSequenceGrid& sequenceGrid (MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGrid;
    }

    static MorphosisXYPad& xyPad (MorphosisAudioProcessorEditor& editor)
    {
        return editor.xyPad;
    }

    static bool xyPadVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.xyPad.isVisible();
    }

    static bool xyInterpolationVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.xyInterpolationBox.isVisible();
    }

    static juce::Rectangle<int> sequencePositionBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequencePosition.getBounds();
    }

    static juce::Rectangle<int> sequenceGridBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGrid.getBounds();
    }

    static juce::Rectangle<int> gridStepBounds (
        const MorphosisAudioProcessorEditor& editor, int step)
    {
        const auto origin = editor.sequenceGrid.getBounds().getPosition();
        return editor.sequenceGrid.getStepBounds (step).translated (origin.x, origin.y);
    }

    static int sequenceCurrentStep (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGrid.getCurrentStep();
    }

    static int sequenceVisibleLength (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequenceGrid.getVisibleLength();
    }

    static juce::Slider& sequencePositionSlider (MorphosisAudioProcessorEditor& editor)
    {
        return editor.sequencePosition.getSlider();
    }

    static juce::Component* picker (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.getPickerComponent();
    }

    static juce::Rectangle<int> pickerPanelBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.getPickerPanelBounds();
    }

    static juce::Rectangle<int> pickerFlyoutBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.getPickerFlyoutBounds();
    }

    static juce::Rectangle<int> pickerLeafBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.getPickerLeafBounds();
    }

    static void showPicker (MorphosisAudioProcessorEditor& editor,
                            juce::Rectangle<int> anchor,
                            std::function<void (int)> callback)
    {
        editor.showPresetMenu (anchor, std::move (callback), false);
    }

    static void useManualPickerGrouping (MorphosisAudioProcessorEditor& editor)
    {
        editor.presetGrouping = MorphosisAudioProcessorEditor::PresetGrouping::manual;
    }

    static void useRecommendedPickerGrouping (MorphosisAudioProcessorEditor& editor)
    {
        editor.presetGrouping = MorphosisAudioProcessorEditor::PresetGrouping::recommended;
    }

    static bool thresholdVisible (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.threshold.isVisible();
    }

    static bool thresholdEnabled (const MorphosisAudioProcessorEditor& editor)
    {
        return editor.threshold.isEnabled();
    }

    static juce::Rectangle<int> thresholdBounds (
        const MorphosisAudioProcessorEditor& editor)
    {
        return editor.threshold.getBounds();
    }

    static juce::Rectangle<int> presetSelectorBoundsInEditor (
        const MorphosisAudioProcessorEditor& editor)
    {
        const auto screen = editor.presetText.getScreenBounds();
        return screen.withPosition (editor.getLocalPoint (nullptr, screen.getPosition()));
    }

    static void clickMainPresetSelector (MorphosisAudioProcessorEditor& editor)
    {
        if (editor.presetText.onClick != nullptr)
            editor.presetText.onClick();
    }

    static void check (MorphosisAudioProcessorEditor& editor, bool visible, bool enabled)
    {
        editor.timerCallback();
        if (editor.threshold.isVisible() != visible
            || editor.threshold.isEnabled() != enabled
            || std::abs (editor.threshold.getAlpha() - (enabled ? 1.0f : 0.35f)) > 0.001f)
            throw std::runtime_error ("DIST visibility/enabling does not match active mode");
        if (visible && editor.threshold.getBounds().isEmpty())
            throw std::runtime_error ("visible DIST has empty bounds");
        if (visible && editor.threshold.getBounds().intersects (editor.transform.getBounds()))
            throw std::runtime_error ("DIST overlaps XFORM in its compact layout");
    }
};

namespace
{

class RepaintTrackingImage final : public juce::CachedComponentImage
{
public:
    void paint (juce::Graphics&) override {}

    bool invalidateAll() override
    {
        ++wholeInvalidations;
        return true;
    }

    bool invalidate (const juce::Rectangle<int>&) override
    {
        ++regionInvalidations;
        return true;
    }

    void releaseResources() override {}

    void reset() noexcept
    {
        wholeInvalidations = 0;
        regionInvalidations = 0;
    }

    int wholeInvalidations = 0;
    int regionInvalidations = 0;
};

void setParameter (MorphosisAudioProcessor& processor,
                   const char* parameterId,
                   float value)
{
    auto* parameter = processor.getParameters().getParameter (parameterId);
    if (parameter == nullptr)
        throw std::runtime_error ("missing lifecycle-test parameter");
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

juce::MouseEvent mouseEventAt (juce::Component& component,
                               juce::Point<float> position,
                               juce::ModifierKeys modifiers)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                             position, modifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &component, &component, now, position, now, 1, false);
}

class ParameterGestureRecorder final : public juce::AudioProcessorParameter::Listener
{
public:
    void parameterValueChanged (int, float) override { ++valueChanges; }

    void parameterGestureChanged (int, bool starting) override
    {
        if (starting)
            ++gestureStarts;
        else
            ++gestureEnds;
    }

    int valueChanges = 0;
    int gestureStarts = 0;
    int gestureEnds = 0;
};

void processBlocks (MorphosisAudioProcessor& processor, int blockCount)
{
    juce::AudioBuffer<float> buffer (2, 64);
    juce::MidiBuffer midi;

    for (int block = 0; block < blockCount; ++block)
    {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto value = 0.2f * std::sin (0.01f * static_cast<float> (
                block * buffer.getNumSamples() + sample));
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, -value);
        }
        processor.processBlock (buffer, midi);
    }
}

void checkCaptionKeyboardEntry()
{
    MorphosisAudioProcessor processor;
    juce::Component host;
    host.setBounds (-10000, -10000, 200, 200);
    MorphosisKnobControl control ("DRY/WET", " %", 1, false,
                                  MorphosisKnob::IndicatorStyle::line);
    control.setRange (0.0, 1.0, 0.001);
    control.setDisplayMultiplier (100.0);
    control.attach (processor.getParameters(), morphosis::parameter_ids::dryWet);
    host.addAndMakeVisible (control);
    control.setBounds (0, 0, 120, 138);
    host.addToDesktop (juce::ComponentPeer::windowIsTemporary);
    host.setVisible (true);

    auto* caption = dynamic_cast<MorphosisCaptionLabel*> (control.getChildComponent (1));
    if (caption == nullptr)
        throw std::runtime_error ("knob caption is unavailable for keyboard test");

    const auto click = [&] (juce::ModifierKeys modifiers)
    {
        const auto point = caption->getLocalBounds().getCentre().toFloat();
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                                 point, modifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 caption, caption, now, point, now, 1, false);
    };

    caption->mouseDown (click (juce::ModifierKeys::leftButtonModifier));
    if (caption->isBeingEdited())
        throw std::runtime_error ("caption editor opened before mouse release");
    // JUCE's Label::mouseUp opens the editor after a real component hit test.
    // In this offscreen test, open it explicitly to check the edit lifecycle.
    caption->showEditor();
    if (! caption->isBeingEdited())
        throw std::runtime_error ("caption editor did not open for keyboard input");

    caption->mouseExit (click (juce::ModifierKeys()));
    if (! caption->isBeingEdited())
        throw std::runtime_error ("caption hover exit discarded the editor");

    auto* textEditor = caption->getCurrentTextEditor();
    if (textEditor == nullptr)
        throw std::runtime_error ("caption text editor is missing");
    textEditor->setText ("50%", false);
    caption->hideEditor (false);
    if (caption->isBeingEdited() || std::abs (control.getSlider().getValue() - 0.5) > 1.0e-6)
        throw std::runtime_error ("caption entry did not commit 50% after mouse release");
    if (caption->getText() != "DRY/WET")
        throw std::runtime_error ("caption did not restore its title after editing");

    caption->showEditor();
    caption->getCurrentTextEditor()->setText ("75%", false);
    caption->getCurrentTextEditor()->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
#if JUCE_WINDOWS
    MSG message;
    while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
#endif
    if (caption->isBeingEdited()
        || std::abs (control.getSlider().getValue() - 0.75) > 1.0e-6
        || std::abs (processor.getParameters().getRawParameterValue (
            morphosis::parameter_ids::dryWet)->load() - 0.75f) > 1.0e-6f)
        throw std::runtime_error ("Enter did not commit caption entry to attached parameter");

    caption->showEditor();
    caption->getCurrentTextEditor()->setText ("not a number", false);
    caption->hideEditor (false);
    if (std::abs (control.getSlider().getValue() - 0.75) > 1.0e-6
        || caption->getText() != "DRY/WET")
        throw std::runtime_error ("invalid caption entry changed the knob or remained visible");

    MorphosisKnobControl frequency ("FREQUENCY", " V", 2, true,
                                    MorphosisKnob::IndicatorStyle::triangle);
    frequency.attach (processor.getParameters(), morphosis::parameter_ids::frequency);
    host.addAndMakeVisible (frequency);
    frequency.setBounds (0, 0, 120, 138);
    auto* cvCaption = dynamic_cast<MorphosisCaptionLabel*> (frequency.getChildComponent (1));
    if (cvCaption == nullptr)
        throw std::runtime_error ("FMX caption is unavailable for keyboard test");
    cvCaption->showEditor();
    cvCaption->getCurrentTextEditor()->setText ("-1.87 V", false);
    cvCaption->getCurrentTextEditor()->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
#if JUCE_WINDOWS
    while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
#endif
    if (cvCaption->isBeingEdited()
        || std::abs (frequency.getSlider().getValue() + 1.87) > 1.0e-6
        || std::abs (processor.getParameters().getRawParameterValue (
            morphosis::parameter_ids::frequency)->load() + 1.87f) > 1.0e-6f)
        throw std::runtime_error ("FMX Enter entry did not commit to its parameter");
}

juce::Image captureShell (MorphosisAudioProcessorEditor& editor)
{
    juce::Image image (juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true);
    juce::Graphics graphics (image);
    editor.paint (graphics);
    return image;
}

int countDifferentPixels (const juce::Image& left, const juce::Image& right)
{
    if (left.getWidth() != right.getWidth() || left.getHeight() != right.getHeight())
        return 1;

    juce::Image::BitmapData leftData (left, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData rightData (right, juce::Image::BitmapData::readOnly);
    auto differentPixels = 0;
    for (int y = 0; y < left.getHeight(); ++y)
        for (int x = 0; x < left.getWidth(); ++x)
            if (leftData.getPixelColour (x, y) != rightData.getPixelColour (x, y))
                ++differentPixels;
    return differentPixels;
}

void checkShellRepaintGating()
{
    auto processor = std::make_unique<MorphosisAudioProcessor>();
    MorphosisAudioProcessorEditor editor (*processor);
    editor.setVisible (true);
    editor.setCachedComponentImage (new RepaintTrackingImage());
    auto& repaintTracker = *static_cast<RepaintTrackingImage*> (
        editor.getCachedComponentImage());

    MorphosisEditorTestAccess::forgetShellVisualState (editor);
    MorphosisEditorTestAccess::refresh (editor);
    if (repaintTracker.wholeInvalidations == 0)
        throw std::runtime_error ("the first shell visual state did not invalidate the editor");

    repaintTracker.reset();
    auto before = captureShell (editor);
    MorphosisEditorTestAccess::refresh (editor);
    auto after = captureShell (editor);
    if (repaintTracker.wholeInvalidations != 0
        || countDifferentPixels (before, after) != 0)
        throw std::runtime_error ("a stable editor refresh repainted or changed the shell");

    const auto checkVisibleShellChange = [&] (const std::function<void()>& change,
                                               const char* message)
    {
        before = captureShell (editor);
        repaintTracker.reset();
        change();
        MorphosisEditorTestAccess::refresh (editor);
        after = captureShell (editor);
        if (repaintTracker.wholeInvalidations == 0
            || countDifferentPixels (before, after) == 0)
            throw std::runtime_error (message);

        repaintTracker.reset();
        MorphosisEditorTestAccess::refresh (editor);
        if (repaintTracker.wholeInvalidations != 0)
            throw std::runtime_error ("a shell state change kept repainting on stable ticks");
    };

    const auto checkHiddenSequenceChange = [&] (const std::function<void()>& change)
    {
        before = captureShell (editor);
        repaintTracker.reset();
        change();
        MorphosisEditorTestAccess::refresh (editor);
        after = captureShell (editor);
        if (repaintTracker.wholeInvalidations != 0
            || countDifferentPixels (before, after) != 0)
            throw std::runtime_error (
                "hidden host-sequencer values invalidated or changed the manual shell");
    };

    checkVisibleShellChange ([&] { processor->activateSequencer(); },
                             "sequencer mode did not repaint its shell state");
    checkVisibleShellChange ([&]
    {
        setParameter (*processor, morphosis::parameter_ids::sequenceSource, 1.0f);
    }, "manual mode did not repaint its shell state");

    checkHiddenSequenceChange ([&]
    {
        setParameter (*processor, morphosis::parameter_ids::sequenceDivision, 2.0f);
        setParameter (*processor, morphosis::parameter_ids::sequenceSync, 2.0f);
        setParameter (*processor, morphosis::parameter_ids::sequenceLength, 12.0f);
    });

    checkVisibleShellChange ([&]
    {
        setParameter (*processor, morphosis::parameter_ids::sequenceSource, 0.0f);
    }, "returning to host sequence values did not repaint the shell");
    checkVisibleShellChange ([&]
    {
        setParameter (*processor, morphosis::parameter_ids::sequenceDivision, 5.0f);
    }, "sequence division automation did not repaint the shell");
    checkVisibleShellChange ([&]
    {
        setParameter (*processor, morphosis::parameter_ids::sequenceSync, 1.0f);
    }, "sequence sync automation did not repaint the shell");
    checkVisibleShellChange ([&]
    {
        setParameter (*processor, morphosis::parameter_ids::sequenceLength, 8.0f);
    }, "sequence length automation did not repaint the shell");
    checkVisibleShellChange ([&] { processor->activateXY(); },
                             "XY mode did not repaint its shell state");
    checkVisibleShellChange ([&] { processor->returnToPresetMode(); },
                             "preset mode did not repaint its shell state");
    MorphosisEditorTestAccess::selectSequenceTab (editor);
    MorphosisEditorTestAccess::refresh (editor);

    juce::MemoryBlock savedState;
    processor->getStateInformation (savedState);
    const auto savedShell = captureShell (editor);
    repaintTracker.reset();
    setParameter (*processor, morphosis::parameter_ids::sequenceDivision, 0.0f);
    setParameter (*processor, morphosis::parameter_ids::sequenceSync, 2.0f);
    setParameter (*processor, morphosis::parameter_ids::sequenceLength, 3.0f);
    MorphosisEditorTestAccess::refresh (editor);
    const auto changedShell = captureShell (editor);
    if (countDifferentPixels (savedShell, changedShell) == 0)
        throw std::runtime_error ("host parameter updates did not change the shell pixels");

    repaintTracker.reset();
    processor->setStateInformation (savedState.getData(),
                                    static_cast<int> (savedState.getSize()));
    MorphosisEditorTestAccess::refresh (editor);
    const auto restoredShell = captureShell (editor);
    if (repaintTracker.wholeInvalidations == 0
        || countDifferentPixels (savedShell, restoredShell) != 0)
        throw std::runtime_error ("state restore did not repaint the saved shell values");

    auto dotFourPreset = -1;
    auto otherPreset = -1;
    for (int preset = 0; preset < processor->getNumPrograms(); ++preset)
    {
        if (morphosis::usesDotFourDistortionLayout (preset))
            dotFourPreset = preset;
        else
            otherPreset = preset;
        if (dotFourPreset >= 0 && otherPreset >= 0)
            break;
    }
    if (dotFourPreset < 0 || otherPreset < 0)
        throw std::runtime_error ("the preset catalog lacks both .4 and standard layouts");

    processor->setCurrentProgram (otherPreset);
    MorphosisEditorTestAccess::refresh (editor);
    const auto standardTransformBounds = MorphosisEditorTestAccess::transformBounds (editor);
    repaintTracker.reset();
    processor->setCurrentProgram (dotFourPreset);
    MorphosisEditorTestAccess::refresh (editor);
    if (repaintTracker.wholeInvalidations == 0
        || ! MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) == standardTransformBounds)
        throw std::runtime_error (".4 layout change did not preserve the resized repaint");

    repaintTracker.reset();
    const auto largeTransformBounds = MorphosisEditorTestAccess::transformBounds (editor);
    editor.setSize (editor.getWidth() + 20, editor.getHeight() + 10);
    if (repaintTracker.wholeInvalidations == 0
        || MorphosisEditorTestAccess::transformBounds (editor) == largeTransformBounds)
        throw std::runtime_error ("editor resize did not repaint and relayout controls");
}

void checkDistortionControls()
{
    auto processor = std::make_unique<MorphosisAudioProcessor>();
    auto editor = std::make_unique<MorphosisAudioProcessorEditor> (*processor);
    editor->setSize (900, 1670);
    for (int preset = 0; preset < processor->getNumPrograms(); ++preset)
    {
        processor->setCurrentProgram (preset);
        const auto visible = morphosis::usesDotFourDistortionLayout (preset);
        for (int interpolation = 0; interpolation < 3; ++interpolation)
        {
            setParameter (*processor, morphosis::parameter_ids::xyInterpolation,
                          interpolation == 0 ? 0.0f : 1.0f);
            setParameter (*processor, morphosis::parameter_ids::xyEncodedDomain,
                          interpolation == 2 ? 1.0f : 0.0f);
            MorphosisEditorTestAccess::check (*editor, visible, true);
            processor->activateSequencer();
            MorphosisEditorTestAccess::check (*editor, true, true);
            processor->returnToPresetMode();
            MorphosisEditorTestAccess::check (*editor, visible, true);
            processor->activateXY();
            MorphosisEditorTestAccess::check (*editor, true, interpolation != 1);
            processor->activateSequencer();
            MorphosisEditorTestAccess::check (*editor, true, true);
            processor->activateXY();
            processor->returnToPresetMode();
            MorphosisEditorTestAccess::check (*editor, visible, true);
        }
        editor.reset();
        editor = std::make_unique<MorphosisAudioProcessorEditor> (*processor);
        MorphosisEditorTestAccess::check (*editor, visible, true);
    }
}

void checkPortraitManualPositionLayout()
{
    auto processor = std::make_unique<MorphosisAudioProcessor>();
    MorphosisAudioProcessorEditor editor (*processor);
    editor.setVisible (true);
    setParameter (*processor, morphosis::parameter_ids::sequenceSource, 1.0f);
    setParameter (*processor, morphosis::parameter_ids::manualTransitionMs, 173.5f);

    const juce::Point<int> sizes[] { { 900, 1670 }, { 720, 1336 }, { 1080, 2004 } };
    for (const auto size : sizes)
    {
        editor.setSize (size.x, size.y);
        MorphosisEditorTestAccess::refresh (editor);
        const auto designArea = MorphosisEditorTestAccess::designArea (editor);
        const auto scale = MorphosisEditorTestAccess::designScale (editor);
        if (editor.getWidth() != size.x || editor.getHeight() != size.y
            || std::abs (designArea.getWidth() - static_cast<float> (size.x)) > 1.0f
            || std::abs (designArea.getHeight() - static_cast<float> (size.y)) > 1.0f
            || std::abs (designArea.getWidth() / designArea.getHeight()
                         - 900.0f / 1670.0f) > 0.0001f)
            throw std::runtime_error ("portrait editor did not preserve its 900x1670 aspect at each size");

        if (! MorphosisEditorTestAccess::sequenceGridVisible (editor)
            || ! MorphosisEditorTestAccess::sequencePositionVisible (editor)
            || ! MorphosisEditorTestAccess::sequenceGlideVisible (editor)
            || MorphosisEditorTestAccess::sequenceVisibleLength (editor) != 16)
            throw std::runtime_error ("manual portrait mode hid Position, Transition, or a sequence slot");

        const auto positionBounds = MorphosisEditorTestAccess::sequencePositionBounds (editor);
        const auto gridBounds = MorphosisEditorTestAccess::sequenceGridBounds (editor);
        const auto manualBand = juce::Rectangle<int> (
            juce::roundToInt (designArea.getX() + 326.0f * scale),
            juce::roundToInt (designArea.getY() + 930.0f * scale),
            juce::roundToInt (502.0f * scale),
            juce::roundToInt (70.0f * scale));
        if (positionBounds.isEmpty() || ! manualBand.contains (positionBounds))
            throw std::runtime_error ("MANUAL POSITION left the upper SYNC/STEPS control band");

        for (int step = 0; step < morphosis::kSequenceSlotCount; ++step)
        {
            const auto bounds = MorphosisEditorTestAccess::gridStepBounds (editor, step);
            if (bounds.isEmpty() || ! editor.getLocalBounds().contains (bounds)
                || ! gridBounds.contains (bounds) || positionBounds.intersects (bounds))
                throw std::runtime_error ("MANUAL POSITION overlaps or clips a sequence-slot hit area");
            for (int previous = 0; previous < step; ++previous)
                if (bounds.intersects (MorphosisEditorTestAccess::gridStepBounds (
                                           editor, previous)))
                    throw std::runtime_error ("sequence slot hit areas overlap in portrait layout");
        }

        const auto transitionBounds = MorphosisEditorTestAccess::sequenceGlideBounds (editor);
        const auto lowerPanelTop = juce::roundToInt (designArea.getY() + 1477.0f * scale);
        if (transitionBounds.isEmpty() || transitionBounds.getY() < gridBounds.getBottom()
            || transitionBounds.getBottom() > lowerPanelTop
            || ! editor.getLocalBounds().contains (transitionBounds)
            || MorphosisEditorTestAccess::sequenceGlideCaption (editor) != "TRANSITION")
            throw std::runtime_error ("manual 5-250 ms TRANSITION is missing from the lower band");

        auto& transition = MorphosisEditorTestAccess::sequenceGlideSlider (editor);
        if (std::abs (transition.getMinimum() - 5.0) > 1.0e-8
            || std::abs (transition.getMaximum() - 250.0) > 1.0e-8
            || std::abs (transition.getValue() - 173.5) > 0.02)
            throw std::runtime_error ("TRANSITION is not attached to the existing 5-250 ms parameter");

        auto& position = MorphosisEditorTestAccess::sequencePositionSlider (editor);
        for (int step = 0; step < morphosis::kSequenceSlotCount; ++step)
        {
            const auto value = static_cast<float> (step)
                             / static_cast<float> (morphosis::kSequenceSlotCount - 1);
            setParameter (*processor, morphosis::parameter_ids::sequencePosition, value);
            MorphosisEditorTestAccess::refresh (editor);
            const auto snapshot = processor->getParameterSnapshot();
            const auto expected = morphosis::sequenceStepFromPosition (snapshot.sequencePosition);
            if (MorphosisEditorTestAccess::sequenceCurrentStep (editor) != expected
                || expected != step
                || std::abs (position.getValue() - snapshot.sequencePosition) > 0.001)
                throw std::runtime_error ("MANUAL POSITION did not follow the host position/current step");
        }

        position.setValue (0.375, juce::sendNotificationSync);
        if (std::abs (processor->getParameterSnapshot().sequencePosition - 0.375) > 0.001)
            throw std::runtime_error ("POSITION slider is not attached to sequencePosition");
        MorphosisEditorTestAccess::refresh (editor);
    }

    setParameter (*processor, morphosis::parameter_ids::sequenceSource, 0.0f);
    MorphosisEditorTestAccess::refresh (editor);
    if (MorphosisEditorTestAccess::sequencePositionVisible (editor)
        || ! MorphosisEditorTestAccess::sequenceDivisionVisible (editor)
        || ! MorphosisEditorTestAccess::sequenceSyncVisible (editor)
        || ! MorphosisEditorTestAccess::sequenceLengthVisible (editor))
        throw std::runtime_error ("host division mode did not restore SYNC/STEPS and hide POSITION");

    MorphosisEditorTestAccess::selectSequenceDivision (editor, 6);
    MorphosisEditorTestAccess::refresh (editor);
    const auto hostSnapshot = processor->getParameterSnapshot();
    if (hostSnapshot.sequenceManual
        || hostSnapshot.sequenceDivision != 5
        || MorphosisEditorTestAccess::sequencePositionVisible (editor)
        || ! MorphosisEditorTestAccess::sequenceSyncVisible (editor)
        || ! MorphosisEditorTestAccess::sequenceLengthVisible (editor))
        throw std::runtime_error ("division selector could not leave MANUAL for host clock mode");
}

void checkModeTabPowerContract()
{
    auto processor = std::make_unique<MorphosisAudioProcessor>();
    MorphosisAudioProcessorEditor editor (*processor);
    editor.setSize (900, 1670);
    MorphosisEditorTestAccess::refresh (editor);

    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset)
        throw std::runtime_error ("mode-panel fixture did not begin powered off");
    MorphosisEditorTestAccess::selectXYTab (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset
        || ! MorphosisEditorTestAccess::xyTabSelected (editor)
        || MorphosisEditorTestAccess::sequenceTabSelected (editor))
        throw std::runtime_error ("selecting X-Y while power is off changed processing mode");
    MorphosisEditorTestAccess::toggleSelectedPower (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::xy
        || ! MorphosisEditorTestAccess::modePowerActive (editor)
        || ! MorphosisEditorTestAccess::xyPadVisible (editor))
        throw std::runtime_error ("power did not activate the selected X-Y panel");
    MorphosisEditorTestAccess::toggleSelectedPower (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset
        || MorphosisEditorTestAccess::modePowerActive (editor)
        || ! MorphosisEditorTestAccess::xyTabSelected (editor))
        throw std::runtime_error ("power-off did not preserve the selected X-Y panel");

    MorphosisEditorTestAccess::selectSequenceTab (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset
        || ! MorphosisEditorTestAccess::sequenceTabSelected (editor)
        || ! MorphosisEditorTestAccess::sequenceGridVisible (editor))
        throw std::runtime_error ("selecting Sequencer while power is off changed processing mode");
    MorphosisEditorTestAccess::toggleSelectedPower (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::sequencer
        || ! MorphosisEditorTestAccess::modePowerActive (editor))
        throw std::runtime_error ("power did not activate the selected sequencer panel");
    MorphosisEditorTestAccess::selectXYTab (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::xy
        || ! MorphosisEditorTestAccess::xyTabSelected (editor))
        throw std::runtime_error ("switching tabs while active did not switch processor mode");

    processor->activateSequencer();
    MorphosisEditorTestAccess::refresh (editor);
    if (! MorphosisEditorTestAccess::sequenceTabSelected (editor)
        || MorphosisEditorTestAccess::xyTabSelected (editor))
        throw std::runtime_error ("external sequencer activation did not synchronize the selected tab");
    processor->activateXY();
    MorphosisEditorTestAccess::refresh (editor);
    if (! MorphosisEditorTestAccess::xyTabSelected (editor)
        || MorphosisEditorTestAccess::sequenceTabSelected (editor))
        throw std::runtime_error ("external X-Y activation did not synchronize the selected tab");
    processor->returnToPresetMode();
    MorphosisEditorTestAccess::refresh (editor);
    MorphosisEditorTestAccess::selectSequenceTab (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset
        || ! MorphosisEditorTestAccess::sequenceTabSelected (editor)
        || MorphosisEditorTestAccess::modePowerActive (editor))
        throw std::runtime_error ("off-tab selection changed power or processing mode");
}

void checkDistortionLayoutAcrossModeChanges()
{
    auto processor = std::make_unique<MorphosisAudioProcessor>();
    MorphosisAudioProcessorEditor editor (*processor);
    editor.setSize (900, 1670);

    auto standardPreset = -1;
    auto dotFourPreset = -1;
    for (int preset = 0; preset < processor->getNumPrograms(); ++preset)
    {
        if (morphosis::usesDotFourDistortionLayout (preset))
            dotFourPreset = preset;
        else
            standardPreset = preset;
        if (standardPreset >= 0 && dotFourPreset >= 0)
            break;
    }
    if (standardPreset < 0 || dotFourPreset < 0)
        throw std::runtime_error ("distortion layout fixtures are missing");

    processor->setCurrentProgram (standardPreset);
    MorphosisEditorTestAccess::refresh (editor);
    const auto standardBounds = MorphosisEditorTestAccess::transformBounds (editor);
    if (MorphosisEditorTestAccess::thresholdVisible (editor) || standardBounds.isEmpty())
        throw std::runtime_error ("standard preset does not use the no-DIST XFORM layout");

    MorphosisEditorTestAccess::selectXYTab (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset
        || MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) != standardBounds)
        throw std::runtime_error ("off-tab selection changed standard preset distortion layout");
    MorphosisEditorTestAccess::toggleSelectedPower (editor);
    const auto activeTransformBounds = MorphosisEditorTestAccess::transformBounds (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::xy
        || ! MorphosisEditorTestAccess::thresholdVisible (editor)
        || activeTransformBounds == standardBounds)
        throw std::runtime_error ("X-Y power-on did not select the compact DIST/XFORM layout");

    setParameter (*processor, morphosis::parameter_ids::xyInterpolation, 1.0f);
    MorphosisEditorTestAccess::refresh (editor);
    if (! MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::thresholdEnabled (editor))
        throw std::runtime_error ("response interpolation did not disable DIST while X-Y is active");
    setParameter (*processor, morphosis::parameter_ids::xyEncodedDomain, 1.0f);
    MorphosisEditorTestAccess::refresh (editor);
    if (! MorphosisEditorTestAccess::thresholdEnabled (editor))
        throw std::runtime_error ("encoded override did not restore DIST interactivity");

    MorphosisEditorTestAccess::selectSequenceTab (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::sequencer
        || ! MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) != activeTransformBounds)
        throw std::runtime_error ("active sequencer tab lost the compact DIST/XFORM layout");
    MorphosisEditorTestAccess::toggleSelectedPower (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset
        || MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) != standardBounds)
        throw std::runtime_error ("power-off did not restore standard preset layout");

    MorphosisEditorTestAccess::selectXYTab (editor);
    processor->setCurrentProgram (dotFourPreset);
    MorphosisEditorTestAccess::refresh (editor);
    const auto dotFourBounds = MorphosisEditorTestAccess::transformBounds (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::preset
        || ! MorphosisEditorTestAccess::thresholdVisible (editor)
        || dotFourBounds == standardBounds)
        throw std::runtime_error (".4 preset change did not restore its compact DIST/XFORM layout");
    MorphosisEditorTestAccess::toggleSelectedPower (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::xy
        || ! MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) != activeTransformBounds)
        throw std::runtime_error (".4 X-Y activation did not keep the active compact layout");

    processor->setCurrentProgram (standardPreset);
    MorphosisEditorTestAccess::refresh (editor);
    if (processor->getProcessingMode() != morphosis::ProcessingMode::xy
        || processor->getPresetIndex() != dotFourPreset
        || ! MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) != activeTransformBounds)
        throw std::runtime_error ("active X-Y mode did not retain its protected .4 preset layout");
    processor->returnToPresetMode();
    MorphosisEditorTestAccess::refresh (editor);
    if (processor->getPresetIndex() != dotFourPreset
        || ! MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) != dotFourBounds)
    {
        const auto current = MorphosisEditorTestAccess::transformBounds (editor);
        throw std::runtime_error ("power-off did not preserve .4 preset distortion layout: mode="
            + std::to_string (static_cast<int> (processor->getProcessingMode()))
            + " preset=" + std::to_string (processor->getPresetIndex())
            + " dot4=" + std::to_string (dotFourPreset)
            + " dist=" + std::to_string (MorphosisEditorTestAccess::thresholdVisible (editor))
            + " xform=" + std::to_string (current.getX()) + ","
            + std::to_string (current.getY()) + "," + std::to_string (current.getWidth())
            + "," + std::to_string (current.getHeight())
            + " expected=" + std::to_string (dotFourBounds.getX()) + ","
            + std::to_string (dotFourBounds.getY()) + ","
            + std::to_string (dotFourBounds.getWidth()) + ","
            + std::to_string (dotFourBounds.getHeight()));
    }
    processor->setCurrentProgram (standardPreset);
    MorphosisEditorTestAccess::refresh (editor);
    if (MorphosisEditorTestAccess::thresholdVisible (editor)
        || MorphosisEditorTestAccess::transformBounds (editor) != standardBounds)
        throw std::runtime_error ("standard preset change did not restore XFORM layout while off");
}

void checkMainPresetAnchorAndSlotCallbacks();

void checkPortraitEditorContracts()
{
    checkPortraitManualPositionLayout();
    checkModeTabPowerContract();
    checkDistortionLayoutAcrossModeChanges();
    checkMainPresetAnchorAndSlotCallbacks();
}

int pickerHeaderHeight (const MorphosisAudioProcessorEditor& editor)
{
    return juce::jmax (32, juce::roundToInt (
        42.0f * juce::jmax (0.7f, MorphosisEditorTestAccess::designScale (editor))));
}

int pickerRowHeight (const MorphosisAudioProcessorEditor& editor)
{
    return juce::jmax (22, juce::roundToInt (
        28.0f * juce::jmax (0.7f, MorphosisEditorTestAccess::designScale (editor))));
}

juce::Point<float> pickerRootCategoryPoint (
    const MorphosisAudioProcessorEditor& editor, int categoryIndex)
{
    const auto panel = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    const auto rowHeight = pickerRowHeight (editor);
    const auto y = panel.getY() + pickerHeaderHeight (editor) + rowHeight + 10
                 + categoryIndex * rowHeight + rowHeight / 2;
    return { static_cast<float> (panel.getCentreX()), static_cast<float> (y) };
}

juce::Point<float> pickerPaneRowPoint (
    const MorphosisAudioProcessorEditor& editor,
    juce::Rectangle<int> pane,
    int rowIndex)
{
    const auto rowHeight = pickerRowHeight (editor);
    const auto y = pane.getY() + pickerHeaderHeight (editor)
                 + rowIndex * rowHeight + rowHeight / 2;
    return { static_cast<float> (pane.getCentreX()), static_cast<float> (y) };
}

void clickPicker (MorphosisAudioProcessorEditor& editor, juce::Point<float> point)
{
    auto* picker = MorphosisEditorTestAccess::picker (editor);
    if (picker == nullptr)
        throw std::runtime_error ("preset picker component is missing");
    picker->mouseDown (mouseEventAt (*picker, point,
                                     juce::ModifierKeys::leftButtonModifier));
}

void choosePresetZeroFromOpenPicker (MorphosisAudioProcessorEditor& editor)
{
    auto* picker = MorphosisEditorTestAccess::picker (editor);
    if (picker == nullptr || ! picker->isVisible())
        throw std::runtime_error ("slot click did not open the preset picker");
    clickPicker (editor, pickerRootCategoryPoint (editor, 0));
    const auto directContents = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    clickPicker (editor, pickerPaneRowPoint (editor, directContents, 0));
    if (picker->isVisible())
        throw std::runtime_error ("slot picker selection did not dismiss the menu");
}

int findTaxonomyCategory (morphosis::preset_taxonomy::Grouping grouping,
                          const juce::String& label)
{
    const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
    for (std::size_t index = 0; index < taxonomy.categoryCount; ++index)
        if (juce::String::fromUTF8 (taxonomy.categories[index].name) == label)
            return static_cast<int> (index);
    return -1;
}

void checkNarrowPickerSingleChildSelection (
    MorphosisAudioProcessorEditor& editor,
    morphosis::preset_taxonomy::Grouping grouping,
    int categoryIndex,
    int presetRow)
{
    const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
    const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
    const auto* direct = morphosis::preset_picker::singleSubcategoryFor (category);
    if (direct == nullptr || presetRow < 0
        || presetRow >= static_cast<int> (direct->presetCount))
        throw std::runtime_error ("single-child picker test selected invalid taxonomy data");

    auto selectedPreset = -1;
    auto selectionCount = 0;
    if (grouping == morphosis::preset_taxonomy::Grouping::manual)
        MorphosisEditorTestAccess::useManualPickerGrouping (editor);
    else
        MorphosisEditorTestAccess::useRecommendedPickerGrouping (editor);
    MorphosisEditorTestAccess::showPicker (editor, { 80, 80, 300, 36 },
        [&] (int preset) { selectedPreset = preset; ++selectionCount; });

    auto* picker = MorphosisEditorTestAccess::picker (editor);
    const auto root = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    if (picker == nullptr || root.isEmpty()
        || ! picker->getLocalBounds().contains (root))
        throw std::runtime_error ("picker root pane escaped its editor bounds");

    clickPicker (editor, pickerRootCategoryPoint (editor, categoryIndex));
    if (! picker->isVisible()
        || ! MorphosisEditorTestAccess::pickerFlyoutBounds (editor).isEmpty()
        || ! MorphosisEditorTestAccess::pickerLeafBounds (editor).isEmpty())
        throw std::runtime_error ("narrow singleton category did not open its direct contents view");

    const auto contents = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    clickPicker (editor, pickerPaneRowPoint (editor, contents, presetRow));
    if (picker->isVisible() || selectionCount != 1
        || selectedPreset != direct->presetIds[presetRow])
        throw std::runtime_error ("narrow singleton preset row did not select its canonical ID");
}

void checkNarrowPickerMultiChildSelection (
    MorphosisAudioProcessorEditor& editor,
    morphosis::preset_taxonomy::Grouping grouping,
    int categoryIndex)
{
    const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
    const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
    if (category.subcategoryCount < 2)
        throw std::runtime_error ("multi-child picker test selected invalid taxonomy data");

    auto selectedPreset = -1;
    auto selectionCount = 0;
    if (grouping == morphosis::preset_taxonomy::Grouping::manual)
        MorphosisEditorTestAccess::useManualPickerGrouping (editor);
    else
        MorphosisEditorTestAccess::useRecommendedPickerGrouping (editor);
    MorphosisEditorTestAccess::showPicker (editor, { 80, 80, 300, 36 },
        [&] (int preset) { selectedPreset = preset; ++selectionCount; });

    auto* picker = MorphosisEditorTestAccess::picker (editor);
    clickPicker (editor, pickerRootCategoryPoint (editor, categoryIndex));
    if (picker == nullptr || ! picker->isVisible()
        || ! MorphosisEditorTestAccess::pickerFlyoutBounds (editor).isEmpty())
        throw std::runtime_error ("narrow multi-child category unexpectedly opened a flyout");

    auto panel = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    clickPicker (editor, pickerPaneRowPoint (editor, panel, 0));
    if (! picker->isVisible() || selectionCount != 0)
        throw std::runtime_error ("multi-child navigation skipped its subcategory level");

    panel = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    clickPicker (editor, pickerPaneRowPoint (editor, panel, 0));
    const auto& firstSubcategory = category.subcategories[0];
    if (picker->isVisible() || selectionCount != 1
        || selectedPreset != firstSubcategory.presetIds[0])
        throw std::runtime_error ("multi-child subcategory did not select its first preset");
}

void checkMainPresetAnchorAndSlotCallbacks()
{
    auto processor = std::make_unique<MorphosisAudioProcessor>();
    MorphosisAudioProcessorEditor editor (*processor);
    editor.setSize (900, 1670);
    editor.setVisible (true);
    MorphosisEditorTestAccess::useRecommendedPickerGrouping (editor);

    MorphosisEditorTestAccess::clickMainPresetSelector (editor);
    auto* picker = MorphosisEditorTestAccess::picker (editor);
    const auto selector = MorphosisEditorTestAccess::presetSelectorBoundsInEditor (editor);
    const auto panel = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    if (picker == nullptr || ! picker->isVisible() || selector.isEmpty()
        || panel.getY() < selector.getBottom()
        || ! picker->getLocalBounds().contains (panel))
        throw std::runtime_error ("top preset selector did not open a bounded menu below its anchor");
    clickPicker (editor, { 1.0f, 1.0f });
    if (picker->isVisible())
        throw std::runtime_error ("main-selector outside click did not close its picker");

    MorphosisEditorTestAccess::useManualPickerGrouping (editor);
    setParameter (*processor, morphosis::parameter_ids::transform, 2.0f);
    MorphosisEditorTestAccess::clickMainPresetSelector (editor);
    clickPicker (editor, pickerRootCategoryPoint (editor, 1));
    clickPicker (editor, pickerPaneRowPoint (
        editor, MorphosisEditorTestAccess::pickerPanelBounds (editor), 0));
    if (processor->getPresetIndex() != 1
        || std::abs (processor->getParameterSnapshot().transform + 5.0f) > 1.0e-5f)
        throw std::runtime_error ("main-selector .4 preset did not default XFORM to -5");

    setParameter (*processor, morphosis::parameter_ids::transform, 2.0f);
    MorphosisEditorTestAccess::clickMainPresetSelector (editor);
    choosePresetZeroFromOpenPicker (editor);
    if (processor->getPresetIndex() != 0
        || std::abs (processor->getParameterSnapshot().transform - 2.0f) > 1.0e-5f)
        throw std::runtime_error ("main-selector non-.4 preset changed XFORM");

    constexpr int sequenceSlot = 11;
    processor->activateSequencer();
    processor->setSequenceSlot (sequenceSlot, 123);
    MorphosisEditorTestAccess::refresh (editor);
    auto& grid = MorphosisEditorTestAccess::sequenceGrid (editor);
    const auto stepBounds = grid.getStepBounds (sequenceSlot);
    grid.mouseDown (mouseEventAt (grid, stepBounds.getCentre().toFloat(),
                                 juce::ModifierKeys::leftButtonModifier));
    picker = MorphosisEditorTestAccess::picker (editor);
    if (picker == nullptr || ! picker->isVisible()
        || ! picker->getLocalBounds().contains (
               MorphosisEditorTestAccess::pickerPanelBounds (editor)))
        throw std::runtime_error ("sequence-slot picker escaped the editor bounds");
    choosePresetZeroFromOpenPicker (editor);
    if (processor->getParameterSnapshot().sequencePresets[
            static_cast<std::size_t> (sequenceSlot)] != 0)
        throw std::runtime_error ("sequence-slot preset callback did not update the selected slot");
    if (std::abs (processor->getParameterSnapshot().transform - 2.0f) > 1.0e-5f)
        throw std::runtime_error ("sequence-slot selection changed XFORM");

    constexpr int xySlot = 2;
    processor->activateXY();
    MorphosisEditorTestAccess::refresh (editor);
    auto& pad = MorphosisEditorTestAccess::xyPad (editor);
    const auto slotBounds = pad.getSlotBounds (xySlot);
    pad.mouseDown (mouseEventAt (pad, slotBounds.getCentre().toFloat(),
                                 juce::ModifierKeys::leftButtonModifier));
    picker = MorphosisEditorTestAccess::picker (editor);
    if (picker == nullptr || ! picker->isVisible()
        || ! picker->getLocalBounds().contains (
               MorphosisEditorTestAccess::pickerPanelBounds (editor)))
        throw std::runtime_error ("X-Y slot picker escaped the editor bounds");
    choosePresetZeroFromOpenPicker (editor);
    if (processor->getParameterSnapshot().xyPresets[static_cast<std::size_t> (xySlot)] != 0)
        throw std::runtime_error ("X-Y slot preset callback did not update the selected slot");
}

void checkPresetPickerNavigation()
{
    auto processor = std::make_unique<MorphosisAudioProcessor>();
    MorphosisAudioProcessorEditor editor (*processor);
    editor.setSize (900, 1670);
    editor.setVisible (true);

    MorphosisEditorTestAccess::clickMainPresetSelector (editor);
    auto* picker = MorphosisEditorTestAccess::picker (editor);
    const auto selector = MorphosisEditorTestAccess::presetSelectorBoundsInEditor (editor);
    const auto anchoredPanel = MorphosisEditorTestAccess::pickerPanelBounds (editor);
    if (picker == nullptr || ! picker->isVisible()
        || anchoredPanel.getY() < selector.getBottom()
        || ! picker->getLocalBounds().contains (anchoredPanel))
        throw std::runtime_error ("main preset selector did not open down inside the portrait editor");
    clickPicker (editor, { 1.0f, 1.0f });

    const auto recommended = morphosis::preset_taxonomy::Grouping::recommended;
    const auto manual = morphosis::preset_taxonomy::Grouping::manual;
    checkNarrowPickerSingleChildSelection (editor, recommended, 0, 0);
    checkNarrowPickerMultiChildSelection (editor, recommended, 1);

    const auto manualSingleton = findTaxonomyCategory (manual, "02 Flangers");
    if (manualSingleton < 0)
        throw std::runtime_error ("manual singleton category fixture is missing");
    const auto& manualTaxonomy = morphosis::preset_taxonomy::forGrouping (manual);
    const auto* manualDirect = morphosis::preset_picker::singleSubcategoryFor (
        manualTaxonomy.categories[static_cast<std::size_t> (manualSingleton)]);
    checkNarrowPickerSingleChildSelection (editor, manual, manualSingleton, 0);
    checkNarrowPickerSingleChildSelection (editor, manual, manualSingleton,
                                           static_cast<int> (manualDirect->presetCount) - 1);

    const auto manualMulti = findTaxonomyCategory (manual, "06 Complex Filters");
    if (manualMulti < 0)
        throw std::runtime_error ("manual multi-child category fixture is missing");
    checkNarrowPickerMultiChildSelection (editor, manual, manualMulti);
    auto selectedPreset = -1;
    auto selectionCount = 0;
    picker = MorphosisEditorTestAccess::picker (editor);
    const auto& firstSubcategory = manualTaxonomy.categories[
        static_cast<std::size_t> (manualMulti)].subcategories[0];

    editor.setSize (1670, 1002);
    if (editor.getWidth() < editor.getHeight())
        throw std::runtime_error ("landscape picker fixture did not reach flyout navigation");

    selectedPreset = -1;
    selectionCount = 0;
    MorphosisEditorTestAccess::showPicker (editor, { 80, 80, 300, 36 },
        [&] (int preset) { selectedPreset = preset; ++selectionCount; });
    clickPicker (editor, pickerRootCategoryPoint (editor, manualSingleton));
    auto flyout = MorphosisEditorTestAccess::pickerFlyoutBounds (editor);
    if (flyout.isEmpty() || ! MorphosisEditorTestAccess::pickerLeafBounds (editor).isEmpty()
        || ! picker->getLocalBounds().contains (flyout))
        throw std::runtime_error ("landscape singleton did not use one bounded preset flyout");
    clickPicker (editor, pickerPaneRowPoint (editor, flyout, 0));
    if (picker->isVisible() || selectionCount != 1
        || selectedPreset != manualDirect->presetIds[0])
        throw std::runtime_error ("landscape singleton flyout selected the wrong preset ID");

    selectedPreset = -1;
    selectionCount = 0;
    MorphosisEditorTestAccess::showPicker (editor, { 80, 80, 300, 36 },
        [&] (int preset) { selectedPreset = preset; ++selectionCount; });
    clickPicker (editor, pickerRootCategoryPoint (editor, manualMulti));
    flyout = MorphosisEditorTestAccess::pickerFlyoutBounds (editor);
    if (flyout.isEmpty() || ! MorphosisEditorTestAccess::pickerLeafBounds (editor).isEmpty())
        throw std::runtime_error ("landscape multi-child category lost its subcategory flyout");
    clickPicker (editor, pickerPaneRowPoint (editor, flyout, 0));
    auto leaf = MorphosisEditorTestAccess::pickerLeafBounds (editor);
    if (leaf.isEmpty() || ! picker->getLocalBounds().contains (leaf))
        throw std::runtime_error ("landscape multi-child category lost its preset leaf pane");
    clickPicker (editor, pickerPaneRowPoint (editor, leaf, 0));
    if (picker->isVisible() || selectionCount != 1
        || selectedPreset != firstSubcategory.presetIds[0])
        throw std::runtime_error ("landscape leaf pane selected the wrong preset ID");

    MorphosisEditorTestAccess::showPicker (editor, { 80, 80, 300, 36 }, {});
    picker->mouseDown (mouseEventAt (*picker, { 1.0f, 1.0f },
                                     juce::ModifierKeys::leftButtonModifier));
    if (picker->isVisible())
        throw std::runtime_error ("outside click did not dismiss the preset picker");
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    try
    {
        if (argc == 2 && juce::String (argv[1]) == "--picker-only")
        {
            checkPresetPickerNavigation();
            checkMainPresetAnchorAndSlotCallbacks();
            std::cout << "Preset picker interaction regressions passed\n";
            return 0;
        }
        if (argc == 2 && juce::String (argv[1]) == "--portrait-only")
        {
            checkPortraitEditorContracts();
            std::cout << "Portrait editor contract regressions passed\n";
            return 0;
        }

        checkCaptionKeyboardEntry();
        checkShellRepaintGating();
        checkDistortionControls();
        checkPortraitEditorContracts();
        checkPresetPickerNavigation();
        const auto started = std::chrono::steady_clock::now();

        for (int cycle = 0; cycle < 4; ++cycle)
        {
            MorphosisAudioProcessor processor;
            // Opening an editor constructs the favourites PropertiesFile (a
            // Timer owner). Exercise shared ownership, last-close and reopen.
            std::unique_ptr<juce::AudioProcessorEditor> first (processor.createEditor());
            std::unique_ptr<juce::AudioProcessorEditor> second (processor.createEditor());
            first.reset();
            second.reset();
            first.reset (processor.createEditor());
            first.reset();
            processor.prepareToPlay (48000.0, 64);
            processor.activateXY();
            setParameter (processor, morphosis::parameter_ids::xyInterpolation, 1.0f);
            processBlocks (processor, 12);

            // This is the host's non-audio teardown boundary. It must stop the
            // response worker before DSP state is reset and be safely repeatable.
            processor.releaseResources();
            processor.prepareToPlay (44100.0, 64);
            setParameter (processor, morphosis::parameter_ids::xyInterpolation, 0.0f);
            processBlocks (processor, 2);
            processor.releaseResources();
        }

        const auto elapsed = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - started).count();
        if (! std::isfinite (elapsed) || elapsed > 2500.0)
            throw std::runtime_error ("plugin lifecycle teardown exceeded bound");

        std::cout << "Plugin lifecycle prepare/release/reprepare/destruction passed in "
                  << elapsed << " ms\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Plugin lifecycle test failed: " << error.what() << '\n';
        return 1;
    }
}
