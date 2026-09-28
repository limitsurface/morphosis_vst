#pragma once

#include "PluginProcessor.h"

#include <array>
#include <functional>
#include <memory>
#include <utility>

class MorphosisPresetPicker;

class MorphosisCaptionLabel final : public juce::Label
{
public:
    std::function<void (bool)> onHover;
    std::function<void()> onClick;

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
};

class MorphosisKnob final : public juce::Slider
{
public:
    enum class IndicatorStyle
    {
        line,
        triangle
    };

    explicit MorphosisKnob (IndicatorStyle = IndicatorStyle::triangle);

    void setAccent (juce::Colour colour) noexcept;
    void setIndicatorStyle (IndicatorStyle newStyle) noexcept;
    void setUnipolar (bool shouldBeUnipolar) noexcept;
    std::function<void (bool)> onInteraction;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Colour accent;
    IndicatorStyle indicatorStyle;
    bool unipolar = false;
};

class MorphosisKnobControl final : public juce::Component
{
public:
    enum class CaptionPlacement
    {
        above,
        below,
        left,
        right
    };

    MorphosisKnobControl (juce::String title,
                          juce::String suffix,
                          int decimals,
                          bool labelBelow,
                          MorphosisKnob::IndicatorStyle indicatorStyle);

    MorphosisKnob& getSlider() noexcept { return slider; }
    void attach (juce::AudioProcessorValueTreeState&, const char* parameterId);
    void detach() noexcept;
    void setAccent (juce::Colour colour);
    void setTitle (juce::String newTitle);
    void setSuffix (juce::String newSuffix) noexcept { suffix = std::move (newSuffix); }
    void setLabelBelow (bool shouldBeBelow) noexcept;
    void setCaptionPlacement (CaptionPlacement newPlacement) noexcept;
    void setKnobDesignSize (float newSize) noexcept;
    void setCaptionDesignFontSize (float newSize) noexcept;
    void setScale (float newScale) noexcept;
    void setRange (double minimum, double maximum, double interval);
    void setUnipolar (bool shouldBeUnipolar) noexcept;
    void setDisplayMultiplier (double newMultiplier) noexcept;
    void setHorizontal (bool shouldBeHorizontal) noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::String formatValue() const;
    void setValueVisible (bool visible);

    juce::String title;
    juce::String suffix;
    int decimals = 2;
    bool labelBelow = false;
    CaptionPlacement captionPlacement = CaptionPlacement::above;
    float scale = 1.0f;
    float knobDesignSize = 168.0f;
    float captionDesignFontSize = 24.0f;
    double displayMultiplier = 1.0;
    juce::Colour accent;
    MorphosisKnob slider;
    MorphosisCaptionLabel caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    bool hovering = false;
    bool showingValue = false;
    bool horizontal = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphosisKnobControl)
};

class MorphosisInlineToggle final : public juce::ToggleButton
{
public:
    enum class Style
    {
        softClip
    };

    MorphosisInlineToggle (juce::String text, Style style);

    void paint (juce::Graphics&) override;

private:
    Style style;
};

class MorphosisArrowButton final : public juce::Button
{
public:
    explicit MorphosisArrowButton (bool pointsLeft);

    void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

private:
    bool pointsLeft;
};

class MorphosisModeTabButton final : public juce::Button
{
public:
    explicit MorphosisModeTabButton (juce::String label);

    void setSelected (bool shouldBeSelected) noexcept;
    bool isSelected() const noexcept { return selected; }
    void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

private:
    bool selected = false;
};

class MorphosisModePowerButton final : public juce::Button
{
public:
    explicit MorphosisModePowerButton (juce::String description);

    void setActive (bool shouldBeActive) noexcept;
    bool isActive() const noexcept { return active; }
    void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

private:
    bool active = false;
};

class MorphosisSequenceGrid final : public juce::Component
{
public:
    std::function<void (int)> onStepClicked;

    void setState (const std::array<int, morphosis::kSequenceSlotCount>& newPresets,
                   int newLength, int newCurrentStep) noexcept;
    juce::Rectangle<int> getStepBounds (int step) const noexcept;
    int getCurrentStep() const noexcept { return currentStep; }
    int getVisibleLength() const noexcept { return length; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    int stepAt (juce::Point<float>) const noexcept;

    std::array<int, morphosis::kSequenceSlotCount> presets {};
    int length = 4;
    int currentStep = 0;
};

class MorphosisXYPad final : public juce::Component
{
public:
    ~MorphosisXYPad() override;

    std::function<void (float, float)> onPositionChanged;
    std::function<void (bool)> onDragGestureChanged;
    std::function<void (int)> onSlotClicked;

    void setPosition (float newX, float newY) noexcept;
    void setSlots (const std::array<int, morphosis::kMaxBlendSources>& newPresets) noexcept;
    juce::Rectangle<int> getSlotBounds (int slot) const noexcept;
    bool isDragging() const noexcept { return dragging; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

private:
    juce::Rectangle<float> padBounds() const noexcept;
    int slotAt (juce::Point<float>) const noexcept;
    void updatePosition (juce::Point<float>) noexcept;
    void endDrag() noexcept;

    std::array<int, morphosis::kMaxBlendSources> presets {};
    float x = 0.5f;
    float y = 0.5f;
    bool dragging = false;
};

class MorphosisGraph final : public juce::Component,
                             private juce::Timer
{
public:
    explicit MorphosisGraph (MorphosisAudioProcessor&);
    ~MorphosisGraph() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void updateResponse (const MorphosisAudioProcessor::GraphSnapshot&) noexcept;

    MorphosisAudioProcessor& processor;
    MorphosisAudioProcessor::GraphSnapshot latestSnapshot;
    std::array<float, 1024> responseDb {};
    int responsePoints = 1024;
    bool hasResponse = false;
    bool hasAudioSnapshot = false;
    std::uint64_t snapshotGeneration = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphosisGraph)
};

class MorphosisFmxIndicator final : public juce::Component
{
public:
    void setValues (float frequency, float morph, float transform) noexcept;
    void paint (juce::Graphics&) override;

private:
    std::array<float, 3> values {};
};

class MorphosisMeter final : public juce::Component
{
public:
    void setLevel (float newLevel) noexcept;
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f;
    float peak = 0.0f;
    int holdFrames = 0;
};

class MorphosisAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit MorphosisAudioProcessorEditor (MorphosisAudioProcessor&);
    ~MorphosisAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    friend struct MorphosisEditorTestAccess;
    void timerCallback() override;
    void drawStaticShell (juce::Graphics&);
    void updateLayoutMode();
    void updateModeView();
    void updateSequenceControls();
    void updatePresetLabel();
    void showPresetMenu (juce::Rectangle<int> anchor,
                         std::function<void (int)> onSelected,
                         bool preferUpwards = false);
    juce::Rectangle<int> getPickerPanelBounds() const;
    juce::Rectangle<int> getPickerFlyoutBounds() const;
    juce::Rectangle<int> getPickerLeafBounds() const;
    juce::Component* getPickerComponent() const noexcept;
    void selectModePanel (bool selectXY);
    void toggleSelectedModePower();
    void setIntegerParameter (const char* parameterId, int value);
    void setFloatParameter (const char* parameterId, float value);
    juce::Rectangle<float> toScreen (juce::Rectangle<float> designBounds) const;
    juce::Rectangle<int> toScreenInt (juce::Rectangle<float> designBounds) const;
    juce::Rectangle<float> getDesignArea() const noexcept;

    MorphosisAudioProcessor& processorRef;
    juce::Rectangle<float> designArea;
    float designScale = 1.0f;

    struct ShellVisualState
    {
        bool sequenceActive = false;
        bool xyActive = false;
        bool selectedXY = false;
        bool manualSequence = false;
        int sequenceDivision = -1;
        int sequenceSync = -1;
        int sequenceLength = -1;
        int xyInterpolation = -1;
        bool xyEncodedDomain = false;

        bool operator== (const ShellVisualState& other) const noexcept
        {
            return sequenceActive == other.sequenceActive
                && xyActive == other.xyActive
                && selectedXY == other.selectedXY
                && manualSequence == other.manualSequence
                && sequenceDivision == other.sequenceDivision
                && sequenceSync == other.sequenceSync
                && sequenceLength == other.sequenceLength
                && xyInterpolation == other.xyInterpolation
                && xyEncodedDomain == other.xyEncodedDomain;
        }
    };

    ShellVisualState lastShellVisualState;
    bool hasShellVisualState = false;

    MorphosisFmxIndicator fmxIndicator;
    MorphosisMeter leftMeter;
    MorphosisMeter rightMeter;
    MorphosisGraph graph;
    MorphosisModeTabButton sequenceTab { "SEQUENCER" };
    MorphosisModeTabButton xyTab { "X-Y PAD" };
    MorphosisModePowerButton modePower { "Toggle selected Sequencer or X-Y mode" };
    MorphosisSequenceGrid sequenceGrid;
    MorphosisXYPad xyPad;
    MorphosisKnobControl sequenceGlide { "GLIDE", " %", 0, true,
                                         MorphosisKnob::IndicatorStyle::triangle };
    MorphosisKnobControl sequencePosition { "POSITION", "", 2, true,
                                             MorphosisKnob::IndicatorStyle::triangle };
    juce::ComboBox sequenceDivisionBox;
    juce::ComboBox sequenceSyncBox;
    juce::ComboBox sequenceLengthBox;
    juce::ComboBox xyInterpolationBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sequenceSyncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sequenceLengthAttachment;
    MorphosisInlineToggle softClip { "SOFT-CLIP", MorphosisInlineToggle::Style::softClip };

    MorphosisKnobControl frequency { "FREQ", " V", 2, true,
                                     MorphosisKnob::IndicatorStyle::triangle };
    MorphosisKnobControl morph { "MORPH", " V", 2, true,
                                 MorphosisKnob::IndicatorStyle::triangle };
    MorphosisKnobControl transform { "XFORM", " V", 2, true,
                                     MorphosisKnob::IndicatorStyle::triangle };
    MorphosisKnobControl dryWet { "DRY/WET", " %", 1, false,
                                  MorphosisKnob::IndicatorStyle::line };
    MorphosisKnobControl inputGain { "INPUT", " dB", 1, false,
                                     MorphosisKnob::IndicatorStyle::line };
    MorphosisKnobControl preClipGain { "PRE-CLIP", " dB", 1, false,
                                       MorphosisKnob::IndicatorStyle::line };
    MorphosisKnobControl postClipGain { "POST-CLIP", " dB", 1, false,
                                        MorphosisKnob::IndicatorStyle::line };
    MorphosisKnobControl threshold { "DIST", "", 0, true,
                                     MorphosisKnob::IndicatorStyle::triangle };

    std::unique_ptr<juce::LookAndFeel> presetLookAndFeel;
    std::unique_ptr<MorphosisPresetPicker> presetPicker;
    juce::ComboBox presetBox;
    MorphosisCaptionLabel presetText;
    MorphosisArrowButton previousPreset { true };
    MorphosisArrowButton nextPreset { false };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> presetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> softClipAttachment;

    enum class PresetGrouping
    {
        recommended,
        manual
    };

    enum class ModePanel
    {
        sequencer,
        xy
    };

    bool distortionLayout = false;
    PresetGrouping presetGrouping = PresetGrouping::recommended;
    ModePanel selectedModePanel = ModePanel::sequencer;
    bool updatingSequenceControls = false;
    bool sequenceGlideUsesManualParameter = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphosisAudioProcessorEditor)
};
