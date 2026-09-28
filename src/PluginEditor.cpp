#include "PluginEditor.h"
#include "MorphosisUIUtilities.h"

#include "MorphosisAssets.h"
#include "PresetPickerModel.h"
#include "PresetRouting.h"
#include "PresetTaxonomy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{

constexpr float kDesignWidth = 900.0f;
constexpr float kDesignHeight = 1670.0f;
constexpr float kDesignAspect = kDesignWidth / kDesignHeight;

const auto kBackground = juce::Colour::fromRGB (21, 21, 21);
const auto kLeftPanelFill = juce::Colour::fromRGB (34, 34, 34);
const auto kPanelFill = juce::Colour::fromRGB (34, 34, 34);
const auto kPanelBorder = juce::Colour::fromRGB (101, 101, 101);
const auto kGraphFill = juce::Colour::fromRGB (17, 17, 17);
const auto kGraphBorder = juce::Colour::fromRGB (104, 104, 104);
const auto kStaticGrey = juce::Colour::fromRGB (189, 189, 189);
const auto kFooterGrey = juce::Colour::fromRGB (208, 208, 208);
const auto kArrowGrey = juce::Colour::fromRGB (130, 130, 130);
const auto kKnobBody = juce::Colour::fromRGB (58, 58, 58);
const auto kKnobRing = juce::Colour::fromRGB (122, 122, 122);
const auto kFmxFrequency = juce::Colour::fromRGB (208, 208, 208);
const auto kFmxMorph = juce::Colour::fromRGB (77, 221, 144);
const auto kFmxTransform = juce::Colour::fromRGB (255, 112, 123);
const auto kInputAccent = juce::Colour::fromRGB (77, 221, 144);
const auto kPreAccent = juce::Colour::fromRGB (208, 208, 208);
const auto kPostAccent = juce::Colour::fromRGB (255, 175, 99);
const auto kDistAccent = juce::Colour::fromRGB (255, 175, 99);
const auto kMeterGreen = juce::Colour::fromRGB (81, 217, 134);
const auto kMeterYellow = juce::Colour::fromRGB (255, 195, 91);
const auto kMeterRed = juce::Colour::fromRGB (255, 113, 113);
const auto kTrackGrey = juce::Colour::fromRGB (89, 89, 89);
const auto kMeterLeftLabel = juce::Colour::fromRGB (77, 221, 144);
const auto kMeterRightLabel = juce::Colour::fromRGB (255, 175, 99);
constexpr int kPresetMenuFirstId = 2000;
constexpr int kUseRecommendedGroupingId = 1800;
constexpr int kUseManualGroupingId = 1801;

class MorphosisPresetLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    MorphosisPresetLookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, kBackground);
        setColour (juce::PopupMenu::textColourId, juce::Colour::fromRGB (215, 215, 215));
        setColour (juce::PopupMenu::headerTextColourId, kFooterGrey);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, kPanelBorder);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    }

    void drawComboBox (juce::Graphics& graphics, int width, int height,
                       bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox& box) override
    {
        // ComboBox owns a child Label for its closed-state value. Drawing the
        // value here as well produces two copies of every SEQ value.
        juce::ignoreUnused (graphics, width, height, isButtonDown,
                            buttonX, buttonY, buttonW, buttonH, box);
    }
};

const juce::Typeface::Ptr& plexTypeface (bool bold)
{
    static const juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (
        MorphosisAssets::IBMPlexMonoRegular_ttf,
        static_cast<size_t> (MorphosisAssets::IBMPlexMonoRegular_ttfSize));
    static const juce::Typeface::Ptr semiBold = juce::Typeface::createSystemTypefaceFor (
        MorphosisAssets::IBMPlexMonoSemiBold_ttf,
        static_cast<size_t> (MorphosisAssets::IBMPlexMonoSemiBold_ttfSize));
    return bold ? semiBold : regular;
}

juce::Font plexFont (float height, bool bold)
{
    return juce::Font (plexTypeface (bold)).withHeight (height);
}

juce::String presetLabel (int index, const char* name)
{
    // Do not pass the narrow name through String::formatted on Windows: JUCE
    // uses a wide formatter there, where %s expects wchar_t*.
    return juce::String::formatted ("%03d - ", index + 1)
         + juce::String::fromUTF8 (name != nullptr ? name : "");
}

juce::String valueText (double value, int decimals, const juce::String& suffix)
{
    if (std::abs (value) < std::pow (10.0, -decimals) * 0.5)
        value = 0.0;
    return juce::String (value, decimals) + suffix;
}

float bipolarAmount (double value, double minimum, double maximum) noexcept
{
    if (! std::isfinite (value))
        return 0.0f;

    if (value < 0.0 && minimum < 0.0)
        return static_cast<float> (std::clamp (value / std::abs (minimum), -1.0, 0.0));

    if (value > 0.0 && maximum > 0.0)
        return static_cast<float> (std::clamp (value / maximum, 0.0, 1.0));

    return 0.0f;
}

} // namespace

namespace
{

struct MorphosisUserProperties
{
    MorphosisUserProperties()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "Morphosis";
        options.filenameSuffix = "settings";
        options.folderName = "Morphosis";
        options.commonToAllUsers = false;
        properties.setStorageParameters (options);
    }

    juce::ApplicationProperties properties;
};

} // namespace

class MorphosisPresetPicker final : public juce::Component,
                                    private juce::TextEditor::Listener,
                                    private juce::KeyListener
{
public:
    using Grouping = morphosis::preset_taxonomy::Grouping;

    explicit MorphosisPresetPicker (MorphosisAudioProcessor& newProcessor)
        : processor (newProcessor)
    {
        setInterceptsMouseClicks (true, true);
        searchBox.setMultiLine (false);
        searchBox.setReturnKeyStartsNewLine (false);
        searchBox.setTextToShowWhenEmpty ("SEARCH PRESETS", kStaticGrey);
        searchBox.setColour (juce::TextEditor::backgroundColourId, kPanelFill);
        searchBox.setColour (juce::TextEditor::outlineColourId, kTrackGrey);
        searchBox.setColour (juce::TextEditor::textColourId,
                             juce::Colour::fromRGB (215, 215, 215));
        searchBox.setColour (juce::TextEditor::highlightColourId, kPanelBorder);
        searchBox.setColour (juce::TextEditor::highlightedTextColourId,
                             juce::Colours::white);
        searchBox.addListener (this);
        searchBox.addKeyListener (this);
        addChildComponent (searchBox);

        if (auto* settings = userProperties->properties.getUserSettings())
            favourites = morphosis::preset_picker::decodeFavourites (
                settings->getValue ("favourites", {}));
    }

    ~MorphosisPresetPicker() override
    {
        searchBox.removeKeyListener (this);
        searchBox.removeListener (this);
    }

    void setScale (float newScale) noexcept
    {
        scale = std::max (0.1f, newScale);
        searchBox.setFont (plexFont (18.0f * std::max (0.7f, scale), false));
        resized();
    }

    void show (juce::Rectangle<int> screenAnchor,
               std::function<void (int)> newSelection,
               bool shouldPreferUpwards,
               Grouping newGrouping,
               std::function<void (Grouping)> newGroupingChanged)
    {
        onSelected = std::move (newSelection);
        onGroupingChanged = std::move (newGroupingChanged);
        grouping = newGrouping;
        preferUpwards = shouldPreferUpwards;
        if (auto* settings = userProperties->properties.getUserSettings())
            favourites = morphosis::preset_picker::decodeFavourites (
                settings->getValue ("favourites", {}));
        view = View::root;
        categoryIndex = -1;
        subcategoryIndex = -1;
        scrollOffset = 0;
        keyboardRow = -1;
        hoveredPane = -1;
        hoveredRow = -1;
        favouritesFlyout = false;
        flyoutScrollOffset = 0;
        leafScrollOffset = 0;
        flyoutAnchorY = -1;
        leafAnchorY = -1;
        searchBox.setText ({}, juce::dontSendNotification);
        anchorScreenBounds = screenAnchor;

        if (auto* parent = getParentComponent())
            setBounds (parent->getLocalBounds());

        setVisible (true);
        toFront (false);
        layoutPanels (true);
        searchBox.grabKeyboardFocus();
        repaint();
    }

    void dismiss()
    {
        setVisible (false);
        onSelected = nullptr;
        onGroupingChanged = nullptr;
        keyboardRow = -1;
    }

    juce::Rectangle<int> getPanelBoundsForEditor() const noexcept { return panelBounds; }
    juce::Rectangle<int> getFlyoutBoundsForEditor() const noexcept { return flyoutBounds; }
    juce::Rectangle<int> getLeafBoundsForEditor() const noexcept { return leafBounds; }

    void paint (juce::Graphics& graphics) override
    {
        if (! isVisible() || panelBounds.isEmpty())
            return;

        rebuildRows();
        const auto panel = panelBounds.toFloat();
        graphics.setColour (kBackground);
        graphics.fillRect (panel);
        graphics.setColour (kTrackGrey);
        graphics.drawRect (panel, std::max (1.0f, scale));

        const auto title = view == View::root ? "PRESETS"
                         : view == View::categories ? taxonomyCategoryContentsName()
                         : view == View::subcategory ? taxonomySubcategoryName()
                         : view == View::favourites ? "FAVOURITES"
                                                     : "SEARCH RESULTS";
        graphics.setColour (kStaticGrey);
        graphics.setFont (plexFont (18.0f * std::max (0.7f, scale), true));
        graphics.drawText (view == View::root ? title : juce::String ("<  ") + title,
                           panel.reduced (12.0f, 6.0f).withHeight (
                               static_cast<float> (scaledHeaderHeight() - 8)),
                           juce::Justification::centredLeft, false);

        auto listArea = juce::Rectangle<float> (panel.getX() + 8.0f,
                                                panel.getY() + scaledHeaderHeight(),
                                                panel.getWidth() - 16.0f,
                                                std::max (1.0f,
                                                          static_cast<float> (searchBox.getY())
                                                            - panel.getY()
                                                            - scaledHeaderHeight() - 8.0f));
        graphics.saveState();
        graphics.reduceClipRegion (listArea.toNearestInt());
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            const auto& row = rows[rowIndex];
            if (row.kind == RowKind::separator)
            {
                graphics.setColour (kTrackGrey.withAlpha (0.8f));
                graphics.drawHorizontalLine (juce::roundToInt (row.bounds.getCentreY()),
                                             row.bounds.getX(), row.bounds.getRight());
                continue;
            }

            const auto selected = (hoveredPane == 0 && static_cast<int> (rowIndex) == hoveredRow)
                                || static_cast<int> (rowIndex) == keyboardRow;
            if (selected)
            {
                graphics.setColour (kPanelBorder);
                graphics.fillRect (row.bounds);
            }

            graphics.setColour (row.enabled ? juce::Colour::fromRGB (215, 215, 215)
                                            : kStaticGrey.withAlpha (0.55f));
            graphics.setFont (plexFont (17.0f * std::max (0.7f, scale), false));
            graphics.drawText (row.label,
                               row.bounds.withTrimmedLeft (10.0f).withTrimmedRight (
                                   row.kind == RowKind::preset ? 42.0f : 12.0f),
                               juce::Justification::centredLeft, false);

            if (row.kind == RowKind::preset)
            {
                drawStar (graphics, row.bounds.getRight() - 22.0f,
                          row.bounds.getCentreY(),
                          favourites[static_cast<std::size_t> (row.value)], row.enabled);
                if (row.value == processor.getPresetIndex())
                {
                    graphics.setColour (juce::Colour::fromRGB (215, 215, 215));
                    graphics.drawText ("✓", row.bounds.withX (row.bounds.getX() + 2.0f)
                                                   .withWidth (14.0f),
                                       juce::Justification::centred, false);
                }
            }
            else if (row.kind == RowKind::category || row.kind == RowKind::subcategory
                     || row.kind == RowKind::favourites)
            {
                drawChevron (graphics, row.bounds.getRight() - 16.0f,
                             row.bounds.getCentreY(), row.enabled);
            }
        }
        graphics.restoreState();

        if (! flyoutBounds.isEmpty())
            paintFlyoutPane (graphics, flyoutBounds, flyoutRows,
                             categoryIndex >= 0 ? taxonomyCategoryContentsName()
                                               : "FAVOURITES", 1);
        if (! leafBounds.isEmpty())
            paintFlyoutPane (graphics, leafBounds, leafRows, taxonomySubcategoryName(), 2);
    }

    void resized() override
    {
        if (! isVisible())
        {
            searchBox.setVisible (false);
            return;
        }

        layoutPanels (true);
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        if (! isVisible())
            return;

        rebuildRows();
        const auto hit = hitAt (event.position);
        const auto oldPane = hoveredPane;
        const auto oldRow = hoveredRow;
        hoveredPane = hit.pane;
        hoveredRow = hit.row;

        if (hit.pane == 0 && view == View::root && hit.row >= 0
            && getWidth() >= getHeight())
        {
            const auto kind = rows[static_cast<std::size_t> (hit.row)].kind;
            const auto value = rows[static_cast<std::size_t> (hit.row)].value;
            if (kind == RowKind::category && categoryIndex != value)
            {
                flyoutAnchorY = juce::roundToInt (
                    rows[static_cast<std::size_t> (hit.row)].bounds.getCentreY());
                leafAnchorY = -1;
                categoryIndex = value;
                subcategoryIndex = -1;
                favouritesFlyout = false;
                flyoutScrollOffset = 0;
                leafScrollOffset = 0;
                layoutPanels();
            }
            else if (kind == RowKind::favourites && ! favouritesFlyout)
            {
                flyoutAnchorY = juce::roundToInt (
                    rows[static_cast<std::size_t> (hit.row)].bounds.getCentreY());
                leafAnchorY = -1;
                categoryIndex = -1;
                subcategoryIndex = -1;
                favouritesFlyout = true;
                flyoutScrollOffset = 0;
                leafScrollOffset = 0;
                layoutPanels();
            }
            else if (kind != RowKind::category && kind != RowKind::favourites
                     && (categoryIndex >= 0 || favouritesFlyout))
            {
                categoryIndex = -1;
                subcategoryIndex = -1;
                favouritesFlyout = false;
                flyoutAnchorY = -1;
                leafAnchorY = -1;
                layoutPanels();
            }
        }
        else if (hit.pane == 1 && view == View::root && categoryIndex >= 0
                 && hit.row >= 0
                 && flyoutRows[static_cast<std::size_t> (hit.row)].kind == RowKind::subcategory)
        {
            const auto value = flyoutRows[static_cast<std::size_t> (hit.row)].value;
            if (subcategoryIndex != value)
            {
                leafAnchorY = juce::roundToInt (
                    flyoutRows[static_cast<std::size_t> (hit.row)].bounds.getCentreY());
                subcategoryIndex = value;
                leafScrollOffset = 0;
                layoutPanels();
            }
        }

        if (oldPane != hoveredPane || oldRow != hoveredRow)
            repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoveredPane != -1 || hoveredRow != -1)
        {
            hoveredPane = -1;
            hoveredRow = -1;
            repaint();
        }
    }

private:
    enum class View;
    enum class RowKind;
    struct Row;

    int scaledHeaderHeight() const noexcept
    {
        return juce::jmax (32, juce::roundToInt (42.0f * std::max (0.7f, scale)));
    }

    int scaledRowHeight() const noexcept
    {
        return juce::jmax (22, juce::roundToInt (28.0f * std::max (0.7f, scale)));
    }

    int scaledSearchHeight() const noexcept
    {
        return juce::jmax (28, juce::roundToInt (38.0f * std::max (0.7f, scale)));
    }

    int desiredHeight (int itemCount, int separatorCount, bool hasSearch) const noexcept
    {
        const auto parentHeight = juce::jmax (80, getHeight() - 8);
        const auto content = scaledHeaderHeight()
                           + juce::jmax (1, itemCount) * scaledRowHeight()
                           + separatorCount * 10
                           + (hasSearch ? scaledSearchHeight() + 18 : 10);
        const auto minimum = hasSearch ? juce::jmin (190, parentHeight) : juce::jmin (80, parentHeight);
        return juce::jlimit (minimum, parentHeight, content);
    }

    int currentPanelHeight() const noexcept
    {
        if (view == View::root)
        {
            const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
            return desiredHeight (static_cast<int> (taxonomy.categoryCount) + 5, 3, true);
        }

        if (view == View::categories)
        {
            const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
            auto count = 1;
            if (categoryIndex >= 0
                && categoryIndex < static_cast<int> (taxonomy.categoryCount))
            {
                const auto& category = taxonomy.categories[
                    static_cast<std::size_t> (categoryIndex)];
                const auto* direct = morphosis::preset_picker::singleSubcategoryFor (category);
                count = static_cast<int> (direct != nullptr
                    ? direct->presetCount : category.subcategoryCount);
            }
            return desiredHeight (count + 3, 1, true);
        }

        std::size_t count = 0;
        if (view == View::favourites)
            count = morphosis::preset_picker::favourites (favourites).size();
        else if (view == View::search)
            count = morphosis::preset_picker::search (query).size();
        else if (view == View::subcategory)
        {
            const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
            if (categoryIndex >= 0 && categoryIndex < static_cast<int> (taxonomy.categoryCount))
            {
                const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
                if (subcategoryIndex >= 0
                    && subcategoryIndex < static_cast<int> (category.subcategoryCount))
                    count = category.subcategories[static_cast<std::size_t> (subcategoryIndex)].presetCount;
            }
        }
        return desiredHeight (static_cast<int> (count) + 3, 1, true);
    }

    int childPanelHeight (int itemCount) const noexcept
    {
        return desiredHeight (itemCount, 0, false);
    }

    void layoutPanels (bool repositionRoot = false)
    {
        if (! isVisible() || anchorScreenBounds.isEmpty())
            return;

        const auto parentBounds = getLocalBounds();
        if (parentBounds.isEmpty())
            return;

        const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
        const auto* selectedCategory = categoryIndex >= 0
                                    && categoryIndex < static_cast<int> (taxonomy.categoryCount)
                                     ? &taxonomy.categories[static_cast<std::size_t> (categoryIndex)]
                                     : nullptr;
        const auto* directSubcategory = selectedCategory != nullptr
                                      ? morphosis::preset_picker::singleSubcategoryFor (
                                            *selectedCategory)
                                      : nullptr;
        const auto rootWidth = juce::jmin (
            getWidth() < getHeight() ? juce::jmax (1, parentBounds.getWidth() - 8)
                                     : juce::jmax (300, juce::roundToInt (520.0f * scale)),
            juce::jmax (1, parentBounds.getWidth() - 8));
        const auto childWidth = juce::jmax (250, juce::roundToInt (360.0f * std::max (0.7f, scale)));
        const auto leafWidth = juce::jmax (300, juce::roundToInt (500.0f * std::max (0.7f, scale)));
        const auto singlePaneNavigation = getWidth() < getHeight();
        const auto hasChild = ! singlePaneNavigation && view == View::root
                           && (selectedCategory != nullptr || favouritesFlyout);
        const auto hasLeaf = view == View::root && selectedCategory != nullptr
                          && directSubcategory == nullptr && subcategoryIndex >= 0
                          && subcategoryIndex < static_cast<int> (
                                 selectedCategory->subcategoryCount);
        const auto overlap = hasChild ? 1 : 0;
        const auto rootHeight = currentPanelHeight();
        if (repositionRoot || panelBounds.isEmpty())
        {
            const auto localAnchor = getParentComponent() != nullptr
                                       ? getParentComponent()->getLocalPoint (
                                             nullptr, anchorScreenBounds.getPosition())
                                       : anchorScreenBounds.getPosition();
            auto x = juce::jlimit (4,
                                   juce::jmax (4, parentBounds.getWidth() - rootWidth - 4),
                                   localAnchor.x);
            auto y = preferUpwards ? localAnchor.y - rootHeight
                                   : localAnchor.y + anchorScreenBounds.getHeight();
            if (preferUpwards && y < 4)
                y = localAnchor.y + anchorScreenBounds.getHeight();
            if (! preferUpwards && y + rootHeight > parentBounds.getBottom() - 4)
                y = localAnchor.y - rootHeight;
            y = juce::jlimit (4,
                             juce::jmax (4, parentBounds.getHeight() - rootHeight - 4), y);
            panelBounds = { x, y, rootWidth, rootHeight };
        }
        else
        {
            panelBounds.setSize (rootWidth, rootHeight);
            panelBounds.setPosition (
                juce::jlimit (4, juce::jmax (4, parentBounds.getWidth() - rootWidth - 4),
                              panelBounds.getX()),
                juce::jlimit (4, juce::jmax (4, parentBounds.getHeight() - rootHeight - 4),
                              panelBounds.getY()));
        }

        flyoutBounds = {};
        leafBounds = {};
        if (hasChild)
        {
            const auto categoryChildCount = directSubcategory != nullptr
                                          ? directSubcategory->presetCount
                                          : selectedCategory != nullptr
                                              ? selectedCategory->subcategoryCount
                                              : morphosis::preset_picker::favourites (
                                                    favourites).size();
            const auto childHeight = childPanelHeight (
                static_cast<int> (categoryChildCount));
            auto leafHeight = 0;
            if (hasLeaf)
            {
                leafHeight = childPanelHeight (static_cast<int> (
                    selectedCategory->subcategories[
                        static_cast<std::size_t> (subcategoryIndex)].presetCount));
            }

            const auto flyoutSpan = childWidth + (hasLeaf ? leafWidth - overlap : 0);
            const auto rightRoom = parentBounds.getRight() - panelBounds.getRight() - 4;
            const auto leftRoom = panelBounds.getX() - 4;
            const auto openRight = rightRoom >= flyoutSpan || rightRoom >= leftRoom;
            const auto requestedChildY = flyoutAnchorY >= 0
                                       ? flyoutAnchorY - childHeight / 2
                                       : panelBounds.getY();
            const auto childY = juce::jlimit (
                4, juce::jmax (4, parentBounds.getHeight() - childHeight - 4), requestedChildY);

            if (openRight)
            {
                flyoutBounds = { panelBounds.getRight() - overlap, childY,
                                 childWidth, childHeight };
                if (hasLeaf)
                {
                    const auto requestedLeafY = leafAnchorY >= 0
                                              ? leafAnchorY - leafHeight / 2
                                              : panelBounds.getY();
                    const auto leafY = juce::jlimit (
                        4, juce::jmax (4, parentBounds.getHeight() - leafHeight - 4), requestedLeafY);
                    leafBounds = { flyoutBounds.getRight() - overlap, leafY,
                                   leafWidth, leafHeight };
                }
            }
            else
            {
                flyoutBounds = { panelBounds.getX() - childWidth + overlap, childY,
                                 childWidth, childHeight };
                if (hasLeaf)
                {
                    const auto requestedLeafY = leafAnchorY >= 0
                                              ? leafAnchorY - leafHeight / 2
                                              : panelBounds.getY();
                    const auto leafY = juce::jlimit (
                        4, juce::jmax (4, parentBounds.getHeight() - leafHeight - 4), requestedLeafY);
                    leafBounds = { flyoutBounds.getX() - leafWidth + overlap, leafY,
                                   leafWidth, leafHeight };
                }
            }
        }

        const auto searchHeight = scaledSearchHeight();
        searchBox.setBounds (panelBounds.getX() + 10,
                             panelBounds.getBottom() - searchHeight - 10,
                             panelBounds.getWidth() - 20, searchHeight);
        searchBox.setFont (plexFont (18.0f * std::max (0.7f, scale), false));
        searchBox.setVisible (isVisible());
    }

    struct Hit
    {
        int pane = -1;
        int row = -1;
    };

    Hit hitAt (juce::Point<float> position) const noexcept
    {
        const auto point = position.toInt();
        const auto find = [&] (const juce::Rectangle<int>& bounds,
                               const std::vector<Row>& paneRows,
                               int pane) -> Hit
        {
            if (! bounds.contains (point))
                return {};
            for (std::size_t index = 0; index < paneRows.size(); ++index)
                if (paneRows[index].bounds.contains (position))
                    return { pane, static_cast<int> (index) };
            return { pane, -1 };
        };

        if (const auto hit = find (leafBounds, leafRows, 2); hit.pane >= 0)
            return hit;
        if (const auto hit = find (flyoutBounds, flyoutRows, 1); hit.pane >= 0)
            return hit;
        return find (panelBounds, rows, 0);
    }

    bool menuContains (juce::Point<float> position) const noexcept
    {
        const auto point = position.toInt();
        return panelBounds.contains (point) || flyoutBounds.contains (point)
            || leafBounds.contains (point);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! menuContains (event.position))
        {
            dismiss();
            return;
        }

        rebuildRows();
        const auto position = event.position;
        const auto handleFlyout = [&] (const juce::Rectangle<int>& bounds,
                                       const std::vector<Row>& paneRows,
                                       int pane) -> bool
        {
            if (! bounds.contains (position.toInt()))
                return false;

            for (std::size_t rowIndex = 0; rowIndex < paneRows.size(); ++rowIndex)
            {
                const auto& row = paneRows[rowIndex];
                if (! row.bounds.contains (position))
                    continue;
                if (row.kind == RowKind::separator || ! row.enabled)
                    return true;

                if (row.kind == RowKind::preset && position.x >= row.bounds.getRight() - 48.0f)
                {
                    morphosis::preset_picker::toggleFavourite (favourites, row.value);
                    auto& settingsManager = userProperties->properties;
                    if (auto* settings = settingsManager.getUserSettings())
                    {
                        settings->setValue ("favourites",
                                            morphosis::preset_picker::encodeFavourites (favourites));
                        settingsManager.saveIfNeeded();
                    }
                    repaint();
                    return true;
                }

                if (row.kind == RowKind::subcategory && pane == 1)
                {
                    leafAnchorY = juce::roundToInt (row.bounds.getCentreY());
                    subcategoryIndex = row.value;
                    leafScrollOffset = 0;
                    layoutPanels();
                    repaint();
                    return true;
                }

                if (row.kind == RowKind::preset)
                {
                    if (onSelected != nullptr)
                        onSelected (row.value);
                    dismiss();
                    return true;
                }
            }
            return true;
        };

        if (handleFlyout (leafBounds, leafRows, 2)
            || handleFlyout (flyoutBounds, flyoutRows, 1))
            return;

        if (position.y < panelBounds.getY() + scaledHeaderHeight())
        {
            if (view != View::root)
            {
                if (view == View::subcategory)
                    view = View::categories;
                else
                    view = View::root;
                categoryIndex = view == View::categories ? categoryIndex : -1;
                subcategoryIndex = -1;
                scrollOffset = 0;
                keyboardRow = -1;
                repaint();
            }
            return;
        }

        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            auto& row = rows[rowIndex];
            if (! row.bounds.contains (position))
                continue;

            if (row.kind == RowKind::separator || ! row.enabled)
                return;

            if (row.kind == RowKind::preset && position.x >= row.bounds.getRight() - 48.0f)
            {
                morphosis::preset_picker::toggleFavourite (favourites, row.value);
                auto& settingsManager = userProperties->properties;
                if (auto* settings = settingsManager.getUserSettings())
                {
                    settings->setValue ("favourites",
                                        morphosis::preset_picker::encodeFavourites (favourites));
                    settingsManager.saveIfNeeded();
                }
                repaint();
                return;
            }

            switch (row.kind)
            {
                case RowKind::category:
                    if (view == View::root)
                    {
                        categoryIndex = row.value;
                        subcategoryIndex = -1;
                        if (getWidth() < getHeight())
                        {
                            view = View::categories;
                            favouritesFlyout = false;
                        }
                        else
                        {
                            flyoutAnchorY = juce::roundToInt (row.bounds.getCentreY());
                            leafAnchorY = -1;
                            favouritesFlyout = false;
                            flyoutScrollOffset = 0;
                            leafScrollOffset = 0;
                        }
                        layoutPanels();
                    }
                    else
                    {
                        view = View::categories;
                        categoryIndex = row.value;
                    }
                    subcategoryIndex = -1;
                    scrollOffset = 0;
                    keyboardRow = -1;
                    repaint();
                    return;
                case RowKind::subcategory:
                    view = View::subcategory;
                    subcategoryIndex = row.value;
                    scrollOffset = 0;
                    keyboardRow = -1;
                    repaint();
                    return;
                case RowKind::favourites:
                    if (view == View::root)
                    {
                        categoryIndex = -1;
                        subcategoryIndex = -1;
                        if (getWidth() < getHeight())
                        {
                            view = View::favourites;
                            favouritesFlyout = false;
                        }
                        else
                        {
                            flyoutAnchorY = juce::roundToInt (row.bounds.getCentreY());
                            leafAnchorY = -1;
                            favouritesFlyout = true;
                            flyoutScrollOffset = 0;
                            leafScrollOffset = 0;
                        }
                        layoutPanels();
                    }
                    else
                    {
                        view = View::favourites;
                    }
                    scrollOffset = 0;
                    keyboardRow = -1;
                    repaint();
                    return;
                case RowKind::preset:
                    if (onSelected != nullptr)
                        onSelected (row.value);
                    dismiss();
                    return;
                case RowKind::grouping:
                    if (onGroupingChanged != nullptr)
                        onGroupingChanged (grouping == Grouping::recommended
                                               ? Grouping::manual : Grouping::recommended);
                    dismiss();
                    return;
                case RowKind::randomPreset:
                    if (onSelected != nullptr)
                        onSelected (juce::Random::getSystemRandom().nextInt (
                            morphosis::cube_data::kRecordCount));
                    dismiss();
                    return;
                case RowKind::randomSequence:
                    processor.randomizeSequencePresets();
                    dismiss();
                    return;
                case RowKind::randomXY:
                    processor.randomizeXYPresets();
                    dismiss();
                    return;
                case RowKind::empty:
                case RowKind::separator:
                    return;
            }
        }
    }

    void mouseWheelMove (const juce::MouseEvent& event,
                         const juce::MouseWheelDetails& details) override
    {
        const auto position = event.position.toInt();
        int* scroll = nullptr;
        int content = 0;
        juce::Rectangle<int> bounds;
        if (leafBounds.contains (position))
        {
            scroll = &leafScrollOffset;
            content = leafContentHeight;
            bounds = leafBounds;
        }
        else if (flyoutBounds.contains (position))
        {
            scroll = &flyoutScrollOffset;
            content = flyoutContentHeight;
            bounds = flyoutBounds;
        }
        else if (panelBounds.contains (position))
        {
            scroll = &scrollOffset;
            content = contentHeight;
            bounds = panelBounds;
        }
        else
            return;

        const auto listTop = bounds.getY() + scaledHeaderHeight();
        const auto listBottom = scroll == &scrollOffset ? searchBox.getY() - 8
                                                         : bounds.getBottom() - 8;
        const auto listHeight = std::max (1, listBottom - listTop);
        const auto maxScroll = juce::jmax (0, content - listHeight);
        *scroll = juce::jlimit (0, maxScroll,
                                *scroll - juce::roundToInt (details.deltaY * 110.0f));
        rebuildRows();
        repaint();
    }

private:
    enum class View
    {
        root,
        categories,
        subcategory,
        favourites,
        search
    };

    enum class RowKind
    {
        grouping,
        category,
        subcategory,
        favourites,
        preset,
        randomPreset,
        randomSequence,
        randomXY,
        empty,
        separator
    };

    struct Row
    {
        RowKind kind = RowKind::empty;
        int value = -1;
        juce::String label;
        juce::Rectangle<float> bounds;
        bool enabled = true;
    };

    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        if (! isVisible())
            return false;
        rebuildRows();
        const auto navigable = [] (const Row& row)
        {
            return row.enabled && row.kind != RowKind::separator
                && row.kind != RowKind::empty;
        };

        if (key == juce::KeyPress::escapeKey)
        {
            dismiss();
            return true;
        }
        if (key == juce::KeyPress::leftKey)
        {
            if (view == View::root && (categoryIndex >= 0 || favouritesFlyout))
            {
                categoryIndex = -1;
                subcategoryIndex = -1;
                favouritesFlyout = false;
                flyoutAnchorY = -1;
                leafAnchorY = -1;
                layoutPanels();
            }
            else if (view == View::subcategory)
            {
                view = View::categories;
                subcategoryIndex = -1;
            }
            else if (view != View::root)
            {
                view = View::root;
                categoryIndex = -1;
                subcategoryIndex = -1;
            }
            else
                return false;
            scrollOffset = 0;
            keyboardRow = -1;
            layoutPanels();
            repaint();
            return true;
        }
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            int next = -1;
            if (key == juce::KeyPress::downKey)
            {
                for (int index = keyboardRow + 1; index < static_cast<int> (rows.size()); ++index)
                    if (navigable (rows[static_cast<std::size_t> (index)]))
                    {
                        next = index;
                        break;
                    }
                if (next < 0)
                    for (int index = 0; index < static_cast<int> (rows.size()); ++index)
                        if (navigable (rows[static_cast<std::size_t> (index)]))
                        {
                            next = index;
                            break;
                        }
            }
            else
            {
                for (int index = keyboardRow - 1; index >= 0; --index)
                    if (navigable (rows[static_cast<std::size_t> (index)]))
                    {
                        next = index;
                        break;
                    }
                if (next < 0)
                    for (int index = static_cast<int> (rows.size()) - 1; index >= 0; --index)
                        if (navigable (rows[static_cast<std::size_t> (index)]))
                        {
                            next = index;
                            break;
                        }
            }
            if (next >= 0)
            {
                keyboardRow = next;
                const auto listTop = panelBounds.getY() + scaledHeaderHeight();
                const auto listBottom = searchBox.getY() - 8;
                const auto row = rows[static_cast<std::size_t> (keyboardRow)].bounds;
                if (row.getBottom() > listBottom)
                    scrollOffset += juce::roundToInt (row.getBottom() - listBottom);
                else if (row.getY() < listTop)
                    scrollOffset -= juce::roundToInt (listTop - row.getY());
                scrollOffset = juce::jmax (0, scrollOffset);
                rebuildRows();
                repaint();
            }
            return true;
        }
        if (key == juce::KeyPress::returnKey)
        {
            if (keyboardRow >= 0 && keyboardRow < static_cast<int> (rows.size()))
            {
                const auto& row = rows[static_cast<std::size_t> (keyboardRow)];
                if (! navigable (row))
                    return true;

                if (row.kind == RowKind::category && view == View::root)
                {
                    categoryIndex = row.value;
                    subcategoryIndex = -1;
                    if (getWidth() < getHeight())
                    {
                        view = View::categories;
                        favouritesFlyout = false;
                    }
                    else
                    {
                        flyoutAnchorY = juce::roundToInt (row.bounds.getCentreY());
                        leafAnchorY = -1;
                        favouritesFlyout = false;
                    }
                }
                else if (row.kind == RowKind::subcategory)
                {
                    view = View::subcategory;
                    subcategoryIndex = row.value;
                    scrollOffset = 0;
                }
                else if (row.kind == RowKind::favourites)
                {
                    view = View::favourites;
                    categoryIndex = -1;
                    subcategoryIndex = -1;
                    favouritesFlyout = false;
                    scrollOffset = 0;
                }
                else if (row.kind == RowKind::preset && onSelected != nullptr)
                {
                    onSelected (row.value);
                    dismiss();
                }
                else if (row.kind == RowKind::grouping)
                {
                    if (onGroupingChanged != nullptr)
                        onGroupingChanged (grouping == Grouping::recommended
                                               ? Grouping::manual : Grouping::recommended);
                    dismiss();
                }
                else if (row.kind == RowKind::randomPreset)
                {
                    if (onSelected != nullptr)
                        onSelected (juce::Random::getSystemRandom().nextInt (
                            morphosis::cube_data::kRecordCount));
                    dismiss();
                }
                else if (row.kind == RowKind::randomSequence)
                {
                    processor.randomizeSequencePresets();
                    dismiss();
                }
                else if (row.kind == RowKind::randomXY)
                {
                    processor.randomizeXYPresets();
                    dismiss();
                }

                keyboardRow = -1;
                layoutPanels();
                repaint();
            }
            return true;
        }
        return false;
    }

    void textEditorTextChanged (juce::TextEditor& editor) override
    {
        if (&editor != &searchBox)
            return;
        const auto nextQuery = editor.getText().trim();
        query = nextQuery;
        view = nextQuery.isEmpty() ? View::root : View::search;
        scrollOffset = 0;
        keyboardRow = -1;
        categoryIndex = -1;
        subcategoryIndex = -1;
        favouritesFlyout = false;
        flyoutAnchorY = -1;
        leafAnchorY = -1;
        layoutPanels();
        repaint();
    }

    const char* taxonomyCategoryName() const noexcept
    {
        const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
        if (categoryIndex < 0 || categoryIndex >= static_cast<int> (taxonomy.categoryCount))
            return "PRESETS";
        return taxonomy.categories[static_cast<std::size_t> (categoryIndex)].name;
    }

    const char* taxonomyCategoryContentsName() const noexcept
    {
        const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
        if (categoryIndex < 0 || categoryIndex >= static_cast<int> (taxonomy.categoryCount))
            return "PRESETS";
        const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
        return morphosis::preset_picker::categoryContentsName (category);
    }

    const char* taxonomySubcategoryName() const noexcept
    {
        const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
        if (categoryIndex < 0 || categoryIndex >= static_cast<int> (taxonomy.categoryCount))
            return "PRESETS";
        const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
        if (subcategoryIndex < 0
            || subcategoryIndex >= static_cast<int> (category.subcategoryCount))
            return category.name;
        return category.subcategories[static_cast<std::size_t> (subcategoryIndex)].name;
    }

    void rebuildRows()
    {
        rows.clear();
        flyoutRows.clear();
        leafRows.clear();
        const auto panel = panelBounds.toFloat();
        const auto listTop = panel.getY() + scaledHeaderHeight()
                           - static_cast<float> (scrollOffset);
        const auto rowHeight = static_cast<float> (scaledRowHeight());
        const auto separatorHeight = 10.0f;
        float y = listTop;
        const auto add = [&] (RowKind kind, int value, juce::String label, bool enabled = true)
        {
            rows.push_back ({ kind, value, std::move (label),
                              { panel.getX() + 8.0f, y, panel.getWidth() - 16.0f, rowHeight },
                              enabled });
            y += rowHeight;
        };
        const auto separator = [&]
        {
            rows.push_back ({ RowKind::separator, -1, {},
                              { panel.getX() + 8.0f, y, panel.getWidth() - 16.0f,
                                separatorHeight }, true });
            y += separatorHeight;
        };
        const auto addPresetRows = [&] (const std::vector<int>& indices)
        {
            if (indices.empty())
            {
                add (RowKind::empty, -1, "No favourites yet", false);
                return;
            }
            for (const auto index : indices)
                add (RowKind::preset, index,
                     presetLabel (index, processor.getPresetName (index)));
        };

        if (view == View::root)
        {
            add (RowKind::grouping, -1,
                 grouping == Grouping::recommended
                     ? "Use manual grouping on next open"
                     : "Use recommended grouping on next open");
            separator();
            const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
            for (std::size_t index = 0; index < taxonomy.categoryCount; ++index)
                add (RowKind::category, static_cast<int> (index), taxonomy.categories[index].name);
            separator();
            const auto favouriteIndices = morphosis::preset_picker::favourites (favourites);
            add (RowKind::favourites, -1,
                 favouriteIndices.empty() ? "FAVOURITES (EMPTY)" : "FAVOURITES",
                 ! favouriteIndices.empty());
        }
        else if (view == View::categories)
        {
            const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
            if (categoryIndex >= 0 && categoryIndex < static_cast<int> (taxonomy.categoryCount))
            {
                const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
                if (const auto* direct = morphosis::preset_picker::singleSubcategoryFor (category))
                {
                    addPresetRows (std::vector<int> (
                        direct->presetIds, direct->presetIds + direct->presetCount));
                }
                else
                {
                    for (std::size_t index = 0; index < category.subcategoryCount; ++index)
                        add (RowKind::subcategory, static_cast<int> (index),
                             category.subcategories[index].name);
                }
            }
        }
        else if (view == View::subcategory)
        {
            const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
            if (categoryIndex >= 0 && categoryIndex < static_cast<int> (taxonomy.categoryCount))
            {
                const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
                if (subcategoryIndex >= 0
                    && subcategoryIndex < static_cast<int> (category.subcategoryCount))
                {
                    const auto& subcategory = category.subcategories[
                        static_cast<std::size_t> (subcategoryIndex)];
                    std::vector<int> indices (subcategory.presetIds,
                                              subcategory.presetIds + subcategory.presetCount);
                    addPresetRows (indices);
                }
            }
        }
        else if (view == View::favourites)
        {
            addPresetRows (morphosis::preset_picker::favourites (favourites));
        }
        else
        {
            addPresetRows (morphosis::preset_picker::search (query));
        }

        if (view == View::root || view == View::categories || view == View::subcategory
            || view == View::favourites || view == View::search)
        {
            separator();
            add (RowKind::randomPreset, -1, "Random Preset");
            add (RowKind::randomSequence, -1, "Random Sequencer Presets");
            add (RowKind::randomXY, -1, "Random X-Y Presets");
        }

        contentHeight = juce::jmax (0, juce::roundToInt (y + scrollOffset - listTop));
        rebuildFlyoutRows();
    }

    void rebuildFlyoutRows()
    {
        flyoutRows.clear();
        leafRows.clear();
        flyoutContentHeight = 0;
        leafContentHeight = 0;

        if (view != View::root)
            return;

        const auto addRows = [&] (std::vector<Row>& destination,
                                  const juce::Rectangle<int>& bounds,
                                  int scroll,
                                  int& contentHeight,
                                  RowKind kind,
                                  const int* ids,
                                  const std::vector<const char*>* textLabels,
                                  std::size_t count)
        {
            const auto panel = bounds.toFloat();
            const auto listTop = panel.getY() + scaledHeaderHeight()
                               - static_cast<float> (scroll);
            const auto rowHeight = static_cast<float> (scaledRowHeight());
            float y = listTop;
            if (count == 0)
            {
                destination.push_back ({ RowKind::empty, -1, "No favourites yet",
                                         { panel.getX() + 8.0f, y,
                                           panel.getWidth() - 16.0f, rowHeight }, false });
                y += rowHeight;
            }
            else
            {
                for (std::size_t index = 0; index < count; ++index)
                {
                    const auto value = kind == RowKind::subcategory
                                     ? static_cast<int> (index)
                                     : ids[index];
                    const auto label = textLabels != nullptr
                                     ? juce::String ((*textLabels)[index])
                                     : presetLabel (value, processor.getPresetName (value));
                    destination.push_back ({ kind, value, label,
                                             { panel.getX() + 8.0f, y,
                                               panel.getWidth() - 16.0f, rowHeight }, true });
                    y += rowHeight;
                }
            }
            contentHeight = juce::jmax (0, juce::roundToInt (y + scroll - listTop));
        };

        if (categoryIndex >= 0)
        {
            const auto& taxonomy = morphosis::preset_taxonomy::forGrouping (grouping);
            if (categoryIndex >= static_cast<int> (taxonomy.categoryCount))
                return;
            const auto& category = taxonomy.categories[static_cast<std::size_t> (categoryIndex)];
            if (const auto* direct = morphosis::preset_picker::singleSubcategoryFor (category))
            {
                addRows (flyoutRows, flyoutBounds, flyoutScrollOffset, flyoutContentHeight,
                         RowKind::preset, direct->presetIds, nullptr, direct->presetCount);
            }
            else
            {
                std::vector<const char*> names;
                names.reserve (category.subcategoryCount);
                for (std::size_t index = 0; index < category.subcategoryCount; ++index)
                    names.push_back (category.subcategories[index].name);
                addRows (flyoutRows, flyoutBounds, flyoutScrollOffset, flyoutContentHeight,
                         RowKind::subcategory, nullptr, &names, names.size());

                if (subcategoryIndex >= 0
                    && subcategoryIndex < static_cast<int> (category.subcategoryCount))
                {
                    const auto& subcategory = category.subcategories[
                        static_cast<std::size_t> (subcategoryIndex)];
                    addRows (leafRows, leafBounds, leafScrollOffset, leafContentHeight,
                             RowKind::preset, subcategory.presetIds, nullptr,
                             subcategory.presetCount);
                }
            }
        }
        else if (favouritesFlyout)
        {
            const auto favouriteIndices = morphosis::preset_picker::favourites (favourites);
            addRows (flyoutRows, flyoutBounds, flyoutScrollOffset, flyoutContentHeight,
                     RowKind::preset,
                     favouriteIndices.empty() ? nullptr : favouriteIndices.data(),
                     nullptr,
                     favouriteIndices.size());
        }
    }

    void paintFlyoutPane (juce::Graphics& graphics,
                          const juce::Rectangle<int>& bounds,
                          const std::vector<Row>& paneRows,
                          const juce::String& title,
                          int paneIndex)
    {
        const auto panel = bounds.toFloat();
        graphics.setColour (kBackground);
        graphics.fillRect (panel);
        graphics.setColour (kTrackGrey);
        graphics.drawRect (panel, std::max (1.0f, scale));
        graphics.setColour (kStaticGrey);
        graphics.setFont (plexFont (18.0f * std::max (0.7f, scale), true));
        graphics.drawText (title,
                           panel.reduced (12.0f, 6.0f).withHeight (
                               static_cast<float> (scaledHeaderHeight() - 8)),
                           juce::Justification::centredLeft, false);

        const auto listArea = juce::Rectangle<float> (panel.getX() + 8.0f,
                                                       panel.getY() + scaledHeaderHeight(),
                                                       panel.getWidth() - 16.0f,
                                                       panel.getHeight() - scaledHeaderHeight() - 16.0f);
        graphics.saveState();
        graphics.reduceClipRegion (listArea.toNearestInt());
        for (std::size_t rowIndex = 0; rowIndex < paneRows.size(); ++rowIndex)
        {
            const auto& row = paneRows[rowIndex];
            if (row.kind == RowKind::separator)
            {
                graphics.setColour (kTrackGrey.withAlpha (0.8f));
                graphics.drawHorizontalLine (juce::roundToInt (row.bounds.getCentreY()),
                                             row.bounds.getX(), row.bounds.getRight());
                continue;
            }

            if (hoveredPane == paneIndex && hoveredRow == static_cast<int> (rowIndex))
            {
                graphics.setColour (kPanelBorder);
                graphics.fillRect (row.bounds);
            }

            graphics.setColour (row.enabled ? juce::Colour::fromRGB (215, 215, 215)
                                            : kStaticGrey.withAlpha (0.55f));
            graphics.setFont (plexFont (17.0f * std::max (0.7f, scale), false));
            graphics.drawText (row.label,
                               row.bounds.withTrimmedLeft (10.0f).withTrimmedRight (
                                   row.kind == RowKind::preset ? 42.0f : 12.0f),
                               juce::Justification::centredLeft, false);

            if (row.kind == RowKind::preset)
            {
                drawStar (graphics, row.bounds.getRight() - 22.0f,
                          row.bounds.getCentreY(),
                          favourites[static_cast<std::size_t> (row.value)], row.enabled);
                if (row.value == processor.getPresetIndex())
                {
                    graphics.setColour (juce::Colour::fromRGB (215, 215, 215));
                    graphics.drawText ("✓", row.bounds.withX (row.bounds.getX() + 2.0f)
                                                   .withWidth (14.0f),
                                       juce::Justification::centred, false);
                }
            }
            else if (row.kind == RowKind::subcategory)
            {
                drawChevron (graphics, row.bounds.getRight() - 16.0f,
                             row.bounds.getCentreY(), row.enabled);
            }
        }
        graphics.restoreState();
    }

    void drawChevron (juce::Graphics& graphics, float x, float y, bool enabled) const
    {
        graphics.setColour (enabled ? juce::Colour::fromRGB (215, 215, 215)
                                    : kStaticGrey.withAlpha (0.55f));
        graphics.drawLine (x - 4.0f, y - 5.0f, x + 1.0f, y, 1.5f);
        graphics.drawLine (x + 1.0f, y, x - 4.0f, y + 5.0f, 1.5f);
    }

    void drawStar (juce::Graphics& graphics, float x, float y, bool filled, bool enabled) const
    {
        juce::Path star;
        constexpr int points = 5;
        for (int point = 0; point < points * 2; ++point)
        {
            const auto angle = -juce::MathConstants<float>::halfPi
                             + static_cast<float> (point) * juce::MathConstants<float>::pi
                                 / static_cast<float> (points);
            const auto radius = point % 2 == 0 ? 7.0f : 3.0f;
            const juce::Point<float> p (x + std::cos (angle) * radius,
                                        y + std::sin (angle) * radius);
            if (point == 0)
                star.startNewSubPath (p);
            else
                star.lineTo (p);
        }
        star.closeSubPath();
        graphics.setColour (enabled ? juce::Colour::fromRGB (215, 215, 215)
                                    : kStaticGrey.withAlpha (0.45f));
        if (filled)
            graphics.fillPath (star);
        else
            graphics.strokePath (star, juce::PathStrokeType (1.2f));
    }

    MorphosisAudioProcessor& processor;
    juce::TextEditor searchBox;
    // Destroy the timer-owning settings before DLL/process teardown, while
    // sharing favourites across all currently open plugin editors.
    juce::SharedResourcePointer<MorphosisUserProperties> userProperties;
    std::array<bool, morphosis::cube_data::kRecordCount> favourites {};
    std::vector<Row> rows;
    std::vector<Row> flyoutRows;
    std::vector<Row> leafRows;
    std::function<void (int)> onSelected;
    std::function<void (Grouping)> onGroupingChanged;
    Grouping grouping = Grouping::recommended;
    View view = View::root;
    juce::String query;
    juce::Rectangle<int> panelBounds;
    juce::Rectangle<int> flyoutBounds;
    juce::Rectangle<int> leafBounds;
    juce::Rectangle<int> anchorScreenBounds;
    float scale = 1.0f;
    int categoryIndex = -1;
    int subcategoryIndex = -1;
    int scrollOffset = 0;
    int contentHeight = 0;
    int flyoutScrollOffset = 0;
    int leafScrollOffset = 0;
    int flyoutContentHeight = 0;
    int leafContentHeight = 0;
    int keyboardRow = -1;
    int hoveredPane = -1;
    int hoveredRow = -1;
    int flyoutAnchorY = -1;
    int leafAnchorY = -1;
    bool favouritesFlyout = false;
    bool preferUpwards = false;
};

MorphosisAudioProcessorEditor::~MorphosisAudioProcessorEditor()
{
    stopTimer();
}

void MorphosisCaptionLabel::mouseEnter (const juce::MouseEvent& event)
{
    juce::ignoreUnused (event);
    if (onHover != nullptr)
        onHover (true);
}

void MorphosisCaptionLabel::mouseExit (const juce::MouseEvent& event)
{
    juce::ignoreUnused (event);
    if (onHover != nullptr)
        onHover (false);
}

void MorphosisCaptionLabel::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isLeftButtonDown() && onClick != nullptr)
        onClick();
}

MorphosisKnob::MorphosisKnob (IndicatorStyle newStyle)
    : indicatorStyle (newStyle)
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setRange (-5.0, 5.0, 0.001);
    setDoubleClickReturnValue (true, 0.0);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void MorphosisKnob::setAccent (juce::Colour colour) noexcept
{
    accent = colour;
    repaint();
}

void MorphosisKnob::setIndicatorStyle (IndicatorStyle newStyle) noexcept
{
    indicatorStyle = newStyle;
    repaint();
}

void MorphosisKnob::setUnipolar (bool shouldBeUnipolar) noexcept
{
    unipolar = shouldBeUnipolar;
    repaint();
}

void MorphosisKnob::mouseEnter (const juce::MouseEvent& event)
{
    juce::Slider::mouseEnter (event);
    if (onInteraction != nullptr)
        onInteraction (true);
}

void MorphosisKnob::mouseExit (const juce::MouseEvent& event)
{
    juce::Slider::mouseExit (event);
    if (onInteraction != nullptr)
        onInteraction (false);
}

void MorphosisKnob::mouseDown (const juce::MouseEvent& event)
{
    juce::Slider::mouseDown (event);
    if (onInteraction != nullptr)
        onInteraction (true);
}

void MorphosisKnob::mouseUp (const juce::MouseEvent& event)
{
    juce::Slider::mouseUp (event);
    if (onInteraction != nullptr)
        onInteraction (false);
}

void MorphosisKnob::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    if (getSliderStyle() == juce::Slider::LinearHorizontal)
    {
        const auto track = bounds.withHeight (11.0f).withCentre (bounds.getCentre())
                                 .reduced (12.0f, 0.0f);
        const auto proportion = static_cast<float> (
            std::clamp ((getValue() - getMinimum())
                            / std::max (getMaximum() - getMinimum(), 1.0e-12), 0.0, 1.0));
        const auto thumbX = track.getX() + proportion * track.getWidth();
        graphics.setColour (juce::Colour::fromRGB (23, 23, 23));
        graphics.fillRoundedRectangle (track, 5.0f);
        graphics.setColour (kKnobRing);
        graphics.drawRoundedRectangle (track, 5.0f, 1.2f);
        graphics.setColour (accent);
        graphics.fillRoundedRectangle (track.withWidth (thumbX - track.getX()), 5.0f);
        graphics.setColour (juce::Colour::fromRGB (228, 228, 228));
        graphics.fillEllipse (thumbX - 12.0f, track.getCentreY() - 12.0f, 24.0f, 24.0f);
        return;
    }

    const auto side = std::min (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const auto radius = side * 0.42f;

    graphics.setColour (kKnobRing);
    graphics.drawEllipse (centre.x - radius, centre.y - radius,
                          radius * 2.0f, radius * 2.0f, std::max (1.0f, side * 0.025f));
    graphics.setColour (kKnobBody);
    graphics.fillEllipse (centre.x - radius * 0.91f, centre.y - radius * 0.91f,
                          radius * 1.82f, radius * 1.82f);

    const auto minimum = getMinimum();
    const auto maximum = getMaximum();
    const auto amount = unipolar
                            ? static_cast<float> (std::clamp (
                                  2.0 * (getValue() - minimum)
                                      / std::max (maximum - minimum, 1.0e-12) - 1.0,
                                  -1.0, 1.0))
                            : bipolarAmount (getValue(), minimum, maximum);
    const auto zeroAngle = -juce::MathConstants<float>::halfPi;
    const auto angle = zeroAngle + amount * juce::MathConstants<float>::pi * 0.75f;

    if (indicatorStyle == IndicatorStyle::line)
    {
        const auto end = centre + juce::Point<float> (std::cos (angle), std::sin (angle)) * (radius * 0.72f);
        graphics.setColour (accent);
        graphics.drawLine (juce::Line<float> (centre, end), std::max (1.5f, side * 0.045f));
    }
    else
    {
        const auto direction = juce::Point<float> (std::cos (angle), std::sin (angle));
        const auto tangent = juce::Point<float> (-direction.y, direction.x);
        const auto point = centre + direction * (radius * 0.73f);
        const auto tip = point + direction * (radius * 0.13f);
        const auto left = point - direction * (radius * 0.09f) - tangent * (radius * 0.11f);
        const auto right = point - direction * (radius * 0.09f) + tangent * (radius * 0.11f);
        juce::Path triangle;
        triangle.startNewSubPath (tip);
        triangle.lineTo (left);
        triangle.lineTo (right);
        triangle.closeSubPath();
        graphics.setColour (accent);
        graphics.fillPath (triangle);
    }
}

MorphosisKnobControl::MorphosisKnobControl (juce::String newTitle,
                                             juce::String newSuffix,
                                             int newDecimals,
                                             bool shouldBeBelow,
                                             MorphosisKnob::IndicatorStyle indicatorStyle)
    : title (std::move (newTitle)),
      suffix (std::move (newSuffix)),
      decimals (newDecimals),
      labelBelow (shouldBeBelow),
      slider (indicatorStyle)
{
    captionPlacement = labelBelow ? CaptionPlacement::below : CaptionPlacement::above;
    accent = kStaticGrey;

    if (title == "DIST")
    {
        slider.setRange (-24.0, 60.0, 1.0);
        knobDesignSize = 96.0f;
        captionDesignFontSize = 18.7f;
    }
    else if (indicatorStyle == MorphosisKnob::IndicatorStyle::line)
    {
        slider.setRange (-60.0, 24.0, 0.01);
        knobDesignSize = 44.0f;
        captionDesignFontSize = 18.7f;
    }
    else
    {
        slider.setRange (-5.0, 5.0, 0.001);
        knobDesignSize = 168.0f;
        captionDesignFontSize = 24.0f;
    }

    addAndMakeVisible (slider);
    addAndMakeVisible (caption);
    caption.setText (title, juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (plexFont (captionDesignFontSize, true));
    caption.setColour (juce::Label::textColourId,
                       title == "MORPH" || title == "XFORM" || title == "DIST"
                           ? accent : kStaticGrey);
    caption.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    caption.setEditable (true, true, false);
    caption.setMouseCursor (juce::MouseCursor::PointingHandCursor);

    slider.onInteraction = [this] (bool active)
    {
        showingValue = active || hovering;
        setValueVisible (showingValue);
    };
    slider.onValueChange = [this]
    {
        if (showingValue)
            setValueVisible (true);
        repaint();
    };
    caption.onHover = [this] (bool isHovering)
    {
        hovering = isHovering;
        showingValue = hovering || slider.isMouseButtonDown();
        setValueVisible (showingValue);
    };
    caption.onClick = [this]
    {
        showingValue = true;
        setValueVisible (true);
    };
    caption.onTextChange = [this]
    {
        const auto parsed = morphosis::ui::parseScaledCaptionEntry (
            caption.getText().toStdString(), displayMultiplier, slider.getMinimum(),
            slider.getMaximum(), suffix.toStdString());
        if (parsed.has_value())
            slider.setValue (*parsed, juce::sendNotificationAsync);
        setValueVisible (showingValue);
    };
    caption.onEditorHide = [this]
    {
        showingValue = hovering;
        juce::Component::SafePointer<MorphosisKnobControl> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->setValueVisible (safeThis->showingValue);
        });
    };
}

void MorphosisKnobControl::attach (juce::AudioProcessorValueTreeState& state,
                                   const char* parameterId)
{
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        state, parameterId, slider);
}

void MorphosisKnobControl::detach() noexcept
{
    attachment.reset();
}

void MorphosisKnobControl::setAccent (juce::Colour colour)
{
    accent = colour;
    slider.setAccent (colour);
    caption.setColour (juce::Label::textColourId,
                       showingValue || title == "MORPH" || title == "XFORM" || title == "DIST"
                           ? accent : kStaticGrey);
}

void MorphosisKnobControl::setTitle (juce::String newTitle)
{
    title = std::move (newTitle);
    if (! showingValue)
        caption.setText (title, juce::dontSendNotification);
}

void MorphosisKnobControl::setLabelBelow (bool shouldBeBelow) noexcept
{
    labelBelow = shouldBeBelow;
    captionPlacement = shouldBeBelow ? CaptionPlacement::below : CaptionPlacement::above;
    resized();
}

void MorphosisKnobControl::setCaptionPlacement (CaptionPlacement newPlacement) noexcept
{
    captionPlacement = newPlacement;
    labelBelow = newPlacement == CaptionPlacement::below;
    resized();
}

void MorphosisKnobControl::setKnobDesignSize (float newSize) noexcept
{
    knobDesignSize = std::max (8.0f, newSize);
    resized();
}

void MorphosisKnobControl::setCaptionDesignFontSize (float newSize) noexcept
{
    captionDesignFontSize = std::max (9.0f, newSize);
    caption.setFont (plexFont (captionDesignFontSize * scale, true));
}

void MorphosisKnobControl::setScale (float newScale) noexcept
{
    scale = std::max (0.1f, newScale);
    caption.setFont (plexFont (std::max (9.0f, captionDesignFontSize * scale), true));
    resized();
}

void MorphosisKnobControl::setRange (double minimum, double maximum, double interval)
{
    slider.setRange (minimum, maximum, interval);
}

void MorphosisKnobControl::setUnipolar (bool shouldBeUnipolar) noexcept
{
    slider.setUnipolar (shouldBeUnipolar);
}

void MorphosisKnobControl::setDisplayMultiplier (double newMultiplier) noexcept
{
    displayMultiplier = std::isfinite (newMultiplier) && newMultiplier > 0.0
                            ? newMultiplier : 1.0;
}

void MorphosisKnobControl::setHorizontal (bool shouldBeHorizontal) noexcept
{
    horizontal = shouldBeHorizontal;
    slider.setSliderStyle (horizontal ? juce::Slider::LinearHorizontal
                                      : juce::Slider::RotaryHorizontalVerticalDrag);
    resized();
    repaint();
}

juce::String MorphosisKnobControl::formatValue() const
{
    return valueText (slider.getValue() * displayMultiplier, decimals, suffix);
}

void MorphosisKnobControl::setValueVisible (bool visible)
{
    if (caption.isBeingEdited())
        return;
    caption.setText (visible && ! horizontal ? formatValue() : title,
                     juce::dontSendNotification);
    caption.setColour (juce::Label::textColourId,
                       (visible && ! horizontal) || title == "MORPH"
                           || title == "XFORM" || title == "DIST"
                           ? accent : kStaticGrey);
}

void MorphosisKnobControl::paint (juce::Graphics& graphics)
{
    if (! horizontal)
        return;
    graphics.setColour (kFooterGrey);
    graphics.setFont (plexFont (20.0f * scale, false));
    const auto valueBounds = juce::Rectangle<int> (
        juce::roundToInt (330.0f * scale), 0,
        juce::roundToInt (90.0f * scale), getHeight());
    graphics.drawText (formatValue(), valueBounds, juce::Justification::centredRight, false);
}

void MorphosisKnobControl::resized()
{
    const auto area = getLocalBounds();
    if (horizontal)
    {
        caption.setJustificationType (juce::Justification::centredLeft);
        caption.setBounds (0, 0, juce::roundToInt (80.0f * scale), area.getHeight());
        slider.setBounds (juce::roundToInt (85.0f * scale),
                          (area.getHeight() - juce::roundToInt (28.0f * scale)) / 2,
                          juce::roundToInt (250.0f * scale),
                          juce::roundToInt (28.0f * scale));
        return;
    }
    const auto captionHeight = juce::jmax (
        16, juce::roundToInt (std::max (24.0f, captionDesignFontSize + 4.0f) * scale));
    auto captionBounds = area.withHeight (captionHeight);
    auto sliderArea = area;
    switch (captionPlacement)
    {
        case CaptionPlacement::above:
            captionBounds = area.withHeight (captionHeight);
            sliderArea = area.withTrimmedTop (captionHeight);
            break;
        case CaptionPlacement::below:
            captionBounds = area.withY (area.getBottom() - captionHeight
                                        - juce::roundToInt (16.0f * scale))
                                 .withHeight (captionHeight);
            sliderArea = area.withTrimmedBottom (captionHeight);
            break;
        case CaptionPlacement::left:
        {
            const auto labelWidth = juce::roundToInt (area.getWidth() * 0.38f);
            captionBounds = { area.getX(), area.getCentreY() - captionHeight / 2,
                              labelWidth, captionHeight };
            sliderArea = area.withTrimmedLeft (labelWidth);
            caption.setJustificationType (juce::Justification::centredRight);
            break;
        }
        case CaptionPlacement::right:
        {
            const auto sliderWidth = juce::roundToInt (area.getWidth() * 0.64f);
            captionBounds = { area.getX() + sliderWidth,
                              area.getCentreY() - captionHeight / 2,
                              area.getWidth() - sliderWidth, captionHeight };
            sliderArea = area.withWidth (sliderWidth);
            caption.setJustificationType (juce::Justification::centredLeft);
            break;
        }
    }

    if (captionPlacement == CaptionPlacement::above || captionPlacement == CaptionPlacement::below)
        caption.setJustificationType (juce::Justification::centred);

    const auto side = juce::jlimit (8, std::min (sliderArea.getWidth(), sliderArea.getHeight()),
                                   juce::roundToInt (knobDesignSize * scale));
    slider.setBounds (sliderArea.getCentreX() - side / 2,
                      sliderArea.getCentreY() - side / 2,
                      side, side);

    if (captionPlacement == CaptionPlacement::above)
        captionBounds = { area.getX(), area.getCentreY() - side / 2 - captionHeight,
                          area.getWidth(), captionHeight };

    caption.setBounds (captionBounds);
}

MorphosisInlineToggle::MorphosisInlineToggle (juce::String text, Style newStyle)
    : juce::ToggleButton (std::move (text)), style (newStyle)
{
    setClickingTogglesState (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void MorphosisInlineToggle::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto componentScale = bounds.getHeight() / 31.0f;
    graphics.setColour (juce::Colour::fromRGB (48, 48, 48));
    graphics.fillRoundedRectangle (bounds.reduced (1.0f), 7.0f);
    graphics.setColour (juce::Colour::fromRGB (124, 124, 124));
    graphics.drawRoundedRectangle (bounds.reduced (1.0f), 7.0f, 1.0f);
    graphics.setFont (plexFont (21.0f * std::max (0.1f, componentScale), true));
    graphics.setColour (juce::Colour::fromRGB (240, 240, 240));

    if (style == Style::softClip)
    {
        const auto check = juce::Rectangle<float> (10.0f, bounds.getCentreY() - 10.0f,
                                                    20.0f, 20.0f);
        graphics.setColour (getToggleState() ? kFmxMorph : kStaticGrey);
        graphics.fillEllipse (check);
        if (getToggleState())
        {
            graphics.setColour (juce::Colour::fromRGB (15, 44, 34));
            graphics.drawLine (check.getX() + 5.0f, check.getCentreY(),
                               check.getCentreX() - 1.0f, check.getBottom() - 5.0f, 1.8f);
            graphics.drawLine (check.getCentreX() - 1.0f, check.getBottom() - 5.0f,
                               check.getRight() - 4.0f, check.getY() + 5.0f, 1.8f);
        }
        graphics.setColour (juce::Colour::fromRGB (240, 240, 240));
        graphics.drawText ("SOFT CLIP", 42, 0, juce::roundToInt (bounds.getWidth() * 0.45f),
                           getHeight(), juce::Justification::centredLeft, false);
        graphics.setColour (getToggleState() ? kFmxMorph : kStaticGrey);
        graphics.drawText (getToggleState() ? "ON" : "OFF",
                           juce::roundToInt (bounds.getWidth() * 0.78f), 0,
                           juce::roundToInt (bounds.getWidth() * 0.17f), getHeight(),
                           juce::Justification::centredRight, false);
    }
}

MorphosisArrowButton::MorphosisArrowButton (bool shouldPointLeft)
    : juce::Button (shouldPointLeft ? "Previous preset" : "Next preset"),
      pointsLeft (shouldPointLeft)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void MorphosisArrowButton::paintButton (juce::Graphics& graphics,
                                        bool shouldDrawButtonAsHighlighted,
                                        bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (shouldDrawButtonAsDown);
    const auto colour = shouldDrawButtonAsHighlighted ? juce::Colours::white : kFooterGrey;
    const auto bounds = getLocalBounds().toFloat().reduced (4.0f);
    const auto centre = bounds.getCentre();
    const auto halfHeight = bounds.getHeight() * 0.23f;
    juce::Path arrow;
    if (pointsLeft)
    {
        arrow.startNewSubPath (centre.x + bounds.getWidth() * 0.15f, centre.y - halfHeight);
        arrow.lineTo (centre.x - bounds.getWidth() * 0.15f, centre.y);
        arrow.lineTo (centre.x + bounds.getWidth() * 0.15f, centre.y + halfHeight);
    }
    else
    {
        arrow.startNewSubPath (centre.x - bounds.getWidth() * 0.15f, centre.y - halfHeight);
        arrow.lineTo (centre.x + bounds.getWidth() * 0.15f, centre.y);
        arrow.lineTo (centre.x - bounds.getWidth() * 0.15f, centre.y + halfHeight);
    }
    graphics.setColour (colour);
    graphics.strokePath (arrow, juce::PathStrokeType (2.5f,
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

MorphosisModeTabButton::MorphosisModeTabButton (juce::String label)
    : juce::Button (std::move (label))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip (getName());
}

void MorphosisModeTabButton::setSelected (bool shouldBeSelected) noexcept
{
    if (selected == shouldBeSelected)
        return;
    selected = shouldBeSelected;
    repaint();
}

void MorphosisModeTabButton::paintButton (juce::Graphics& graphics,
                                          bool shouldDrawButtonAsHighlighted,
                                          bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (shouldDrawButtonAsDown);
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    const auto fill = selected ? juce::Colour::fromRGB (59, 59, 59)
                               : juce::Colour::fromRGB (30, 30, 30);
    const auto stroke = selected ? juce::Colour::fromRGB (179, 179, 179) : kPanelBorder;
    graphics.setColour (shouldDrawButtonAsHighlighted ? fill.brighter (0.10f) : fill);
    graphics.fillRoundedRectangle (bounds, 8.0f);
    graphics.setColour (stroke);
    graphics.drawRoundedRectangle (bounds, 8.0f, selected ? 1.5f : 1.0f);
    graphics.setColour (selected ? juce::Colour::fromRGB (240, 240, 240) : kStaticGrey);
    graphics.setFont (plexFont (std::max (14.0f, bounds.getHeight() * 0.48f), true));
    graphics.drawText (getName(), bounds, juce::Justification::centred, false);
}

MorphosisModePowerButton::MorphosisModePowerButton (juce::String description)
    : juce::Button (std::move (description))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip (getName());
}

void MorphosisModePowerButton::setActive (bool shouldBeActive) noexcept
{
    if (active == shouldBeActive)
        return;
    active = shouldBeActive;
    repaint();
}

void MorphosisModePowerButton::paintButton (juce::Graphics& graphics,
                                            bool shouldDrawButtonAsHighlighted,
                                            bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (shouldDrawButtonAsDown);
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    const auto centre = bounds.getCentre();
    const auto radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.24f;
    const auto activeColour = juce::Colour::fromRGB (128, 212, 155);
    const auto inactiveColour = juce::Colour::fromRGB (189, 189, 189);
    auto colour = active ? activeColour : inactiveColour.withAlpha (0.60f);
    if (shouldDrawButtonAsHighlighted)
        colour = colour.withAlpha (std::min (1.0f, colour.getFloatAlpha() + 0.15f));

    graphics.setColour (active ? juce::Colour::fromRGB (44, 81, 56)
                               : juce::Colour::fromRGB (30, 30, 30));
    graphics.fillRoundedRectangle (bounds, 8.0f);
    graphics.setColour (active ? activeColour : kPanelBorder);
    graphics.drawRoundedRectangle (bounds, 8.0f, active ? 1.5f : 1.0f);

    graphics.setColour (colour);
    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                       -juce::MathConstants<float>::pi * 0.25f
                           + juce::MathConstants<float>::halfPi,
                       juce::MathConstants<float>::pi * 1.25f
                           + juce::MathConstants<float>::halfPi, true);
    graphics.strokePath (arc, juce::PathStrokeType (
        std::max (1.0f, bounds.getWidth() * 0.050f),
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::Path stem;
    stem.startNewSubPath (centre.x, centre.y - radius * 1.18f);
    stem.lineTo (centre.x, centre.y - radius * 0.12f);
    graphics.strokePath (stem, juce::PathStrokeType (
        std::max (1.0f, bounds.getWidth() * 0.050f),
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void MorphosisSequenceGrid::setState (
    const std::array<int, morphosis::kSequenceSlotCount>& newPresets,
    int newLength, int newCurrentStep) noexcept
{
    presets = newPresets;
    length = std::clamp (newLength, 1, morphosis::kSequenceSlotCount);
    currentStep = std::clamp (newCurrentStep, 0, length - 1);
    repaint();
}

juce::Rectangle<int> MorphosisSequenceGrid::getStepBounds (int step) const noexcept
{
    const auto safeStep = juce::jlimit (0, morphosis::kSequenceSlotCount - 1, step);
    const auto bounds = getLocalBounds().toFloat();
    const auto gap = bounds.getWidth() * 0.08f;
    const auto groupWidth = (bounds.getWidth() - gap) * 0.5f;
    const auto rowHeight = bounds.getHeight() / 8.0f;
    const auto column = safeStep / 8;
    const auto row = safeStep % 8;
    const auto groupX = column == 0 ? bounds.getX() : bounds.getCentreX() + gap * 0.5f;
    const auto top = juce::roundToInt (bounds.getY() + row * rowHeight);
    const auto bottom = juce::roundToInt (bounds.getY() + (row + 1) * rowHeight);
    return juce::Rectangle<int> (juce::roundToInt (groupX), top,
                                juce::roundToInt (groupWidth), bottom - top);
}

int MorphosisSequenceGrid::stepAt (juce::Point<float> point) const noexcept
{
    const auto bounds = getLocalBounds().toFloat();
    if (! bounds.contains (point) || bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return -1;

    const auto gap = bounds.getWidth() * 0.08f;
    const auto groupWidth = (bounds.getWidth() - gap) * 0.5f;
    const auto rowHeight = bounds.getHeight() / 8.0f;
    const auto column = point.x < bounds.getCentreX() ? 0 : 1;
    const auto groupX = column == 0 ? bounds.getX() : bounds.getCentreX() + gap * 0.5f;
    if (point.x < groupX || point.x >= groupX + groupWidth)
        return -1;

    const auto row = juce::jlimit (0, 7,
        static_cast<int> ((point.y - bounds.getY()) / rowHeight));
    return column == 0 ? row : row + 8;
}

void MorphosisSequenceGrid::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto numberWidth = bounds.getWidth() * 0.063f;
    const auto gap = bounds.getWidth() * 0.08f;
    const auto groupWidth = (bounds.getWidth() - gap) * 0.5f;
    const auto columnWidth = groupWidth - numberWidth - 6.0f;
    const auto rowHeight = bounds.getHeight() / 8.0f;
    const auto textHeight = std::max (14.0f, rowHeight * 0.45f);
    graphics.setFont (plexFont (textHeight, false));

    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
    {
        const auto column = index / 8;
        const auto row = index % 8;
        const auto groupX = column == 0 ? bounds.getX() : bounds.getCentreX() + gap * 0.5f;
        const auto cell = juce::Rectangle<float> (groupX + numberWidth,
                                                  bounds.getY() + row * rowHeight + 3.0f,
                                                  columnWidth, rowHeight - 10.0f);
        const auto activeStep = index < length;
        const auto current = activeStep && index == currentStep;
        graphics.setColour (current ? juce::Colour::fromRGB (102, 102, 102)
                                    : activeStep ? juce::Colour::fromRGB (57, 57, 57)
                                                 : juce::Colours::transparentBlack);
        graphics.fillRoundedRectangle (cell, 3.0f);
        graphics.setColour (juce::Colour::fromRGB (115, 115, 115)
                                .withAlpha (activeStep ? 1.0f : 0.75f));
        graphics.drawRoundedRectangle (cell, 3.0f, 1.0f);

        const auto stepText = juce::String::formatted ("%02d", index + 1);
        const auto stepBounds = juce::Rectangle<float> (
            groupX, cell.getY(), numberWidth - 8.0f, cell.getHeight());
        graphics.setColour (activeStep ? juce::Colour::fromRGB (220, 220, 220)
                                      : juce::Colour::fromRGB (180, 180, 180));
        graphics.drawText (stepText, stepBounds, juce::Justification::centredRight, false);

        graphics.setColour (current ? juce::Colour::fromRGB (250, 250, 250)
                                    : activeStep ? juce::Colour::fromRGB (232, 232, 232)
                                                 : juce::Colour::fromRGB (195, 195, 195));
        const auto presetIndex = juce::jlimit (0, morphosis::cube_data::kRecordCount - 1,
                                               presets[static_cast<std::size_t> (index)]);
        const auto presetText = presetLabel (
            presetIndex, morphosis::cube_data::kCubes[static_cast<std::size_t> (presetIndex)].name);
        graphics.setFont (plexFont (std::max (14.0f, rowHeight * 0.45f), current));
        graphics.drawFittedText (presetText, cell.reduced (10.0f, 1.0f).toNearestInt(),
                                 juce::Justification::centredLeft, 1, 0.82f);
        graphics.setFont (plexFont (textHeight, false));
    }
}

void MorphosisSequenceGrid::mouseDown (const juce::MouseEvent& event)
{
    if (! event.mods.isLeftButtonDown())
        return;
    const auto step = stepAt (event.position);
    if (step >= 0 && onStepClicked != nullptr)
        onStepClicked (step);
}

void MorphosisXYPad::setPosition (float newX, float newY) noexcept
{
    x = std::clamp (std::isfinite (newX) ? newX : 0.5f, 0.0f, 1.0f);
    y = std::clamp (std::isfinite (newY) ? newY : 0.5f, 0.0f, 1.0f);
    repaint();
}

MorphosisXYPad::~MorphosisXYPad()
{
    endDrag();
}

void MorphosisXYPad::setSlots (
    const std::array<int, morphosis::kMaxBlendSources>& newPresets) noexcept
{
    presets = newPresets;
    repaint();
}

juce::Rectangle<int> MorphosisXYPad::getSlotBounds (int slot) const noexcept
{
    const auto safeSlot = juce::jlimit (0, morphosis::kMaxBlendSources - 1, slot);
    const auto bounds = getLocalBounds().toFloat();
    const auto header = bounds.withHeight (bounds.getHeight() * 0.28f).reduced (18.0f, 2.0f);
    const auto row = safeSlot / 2;
    const auto column = safeSlot % 2;
    return juce::Rectangle<int> (
        juce::roundToInt (header.getX() + column * header.getWidth() * 0.5f),
        juce::roundToInt (header.getY() + row * header.getHeight() * 0.5f),
        juce::roundToInt (header.getWidth() * 0.5f),
        juce::roundToInt (header.getHeight() * 0.5f));
}

juce::Rectangle<float> MorphosisXYPad::padBounds() const noexcept
{
    const auto bounds = getLocalBounds().toFloat();
    return bounds.withTrimmedTop (bounds.getHeight() * 0.32f)
                 .reduced (18.0f, 12.0f);
}

int MorphosisXYPad::slotAt (juce::Point<float> point) const noexcept
{
    const auto bounds = getLocalBounds().toFloat();
    const auto header = bounds.withHeight (bounds.getHeight() * 0.28f).reduced (18.0f, 2.0f);
    if (! header.contains (point))
        return -1;
    return point.x < header.getCentreX() ? (point.y < header.getCentreY() ? 0 : 2)
                                         : (point.y < header.getCentreY() ? 1 : 3);
}

void MorphosisXYPad::updatePosition (juce::Point<float> point) noexcept
{
    const auto pad = padBounds();
    const auto nextX = std::clamp ((point.x - pad.getX()) / std::max (pad.getWidth(), 1.0f), 0.0f, 1.0f);
    const auto nextY = std::clamp (1.0f - (point.y - pad.getY()) / std::max (pad.getHeight(), 1.0f),
                                   0.0f, 1.0f);
    setPosition (nextX, nextY);
    if (onPositionChanged != nullptr)
        onPositionChanged (x, y);
}

void MorphosisXYPad::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto pad = padBounds();
    graphics.setColour (juce::Colour::fromRGB (19, 19, 19));
    graphics.fillRoundedRectangle (pad, 14.0f);
    graphics.setColour (juce::Colour::fromRGB (104, 104, 104));
    graphics.drawRoundedRectangle (pad, 14.0f, 1.5f);

    const auto header = bounds.withHeight (bounds.getHeight() * 0.28f).reduced (18.0f, 2.0f);
    graphics.setColour (juce::Colour::fromRGB (69, 69, 69));
    const auto dividerHeight = header.getHeight() * 0.60f;
    graphics.drawVerticalLine (juce::roundToInt (header.getCentreX()),
                               static_cast<float> (juce::roundToInt (
                                   header.getCentreY() - dividerHeight * 0.5f)),
                               static_cast<float> (juce::roundToInt (
                                   header.getCentreY() + dividerHeight * 0.5f)));
    graphics.drawHorizontalLine (juce::roundToInt (header.getCentreY()), header.getX(),
                                 header.getRight());
    const float slotFontSize = std::max (16.0f, bounds.getWidth() * 0.028f);
    graphics.setFont (plexFont (slotFontSize, true));
    for (int index = 0; index < 4; ++index)
    {
        const auto row = index / 2;
        const auto column = index % 2;
        const auto labelHeight = std::max (16.0f, slotFontSize * 1.20f);
        const auto labelY = row == 0
                                ? header.getCentreY() - labelHeight - 5.0f
                                : header.getCentreY() + 5.0f;
        const auto cell = juce::Rectangle<float> (
            header.getX() + column * header.getWidth() * 0.5f + 10.0f,
            labelY,
            header.getWidth() * 0.5f - 20.0f, labelHeight);
        const auto presetIndex = juce::jlimit (0, morphosis::cube_data::kRecordCount - 1,
                                               presets[static_cast<std::size_t> (index)]);
        const auto label = presetLabel (presetIndex,
                                        morphosis::cube_data::kCubes[static_cast<std::size_t> (presetIndex)].name);
        graphics.setColour (juce::Colour::fromRGB (215, 215, 215));
        const auto justification = column == 0 ? juce::Justification::centredLeft
                                               : juce::Justification::centredRight;
        graphics.drawFittedText (label, cell.toNearestInt(), justification, 1, 0.80f);
    }

    // The exported dotted divider sits in the gap between the four slot
    // labels and the pad, rather than on the pad itself.
    graphics.setColour (juce::Colour::fromRGB (89, 89, 89));
    const float dividerDashes[] { 2.0f, 4.0f };
    const auto dividerY = header.getBottom() + bounds.getHeight() * 0.010f;
    graphics.drawDashedLine ({ header.getX(), dividerY, header.getRight(), dividerY },
                             dividerDashes, 2, 1.0f);

    const auto handle = juce::Point<float> (pad.getX() + x * pad.getWidth(),
                                            pad.getBottom() - y * pad.getHeight());
    graphics.setColour (juce::Colour::fromRGB (61, 61, 61));
    const float dashes[] { 2.0f, 4.0f };
    graphics.drawDashedLine ({ pad.getX() + 12.0f, handle.y, pad.getRight() - 12.0f, handle.y },
                             dashes, 2, 1.0f);
    graphics.drawDashedLine ({ handle.x, pad.getY() + 12.0f, handle.x, pad.getBottom() - 12.0f },
                             dashes, 2, 1.0f);
    graphics.setColour (juce::Colour::fromRGB (19, 19, 19));
    graphics.fillEllipse (handle.x - 8.0f, handle.y - 8.0f, 16.0f, 16.0f);
    graphics.setColour (juce::Colour::fromRGB (180, 180, 180));
    graphics.drawEllipse (handle.x - 8.0f, handle.y - 8.0f, 16.0f, 16.0f, 1.5f);
}

void MorphosisXYPad::mouseDown (const juce::MouseEvent& event)
{
    if (! event.mods.isLeftButtonDown())
        return;

    endDrag();
    if (const auto slot = slotAt (event.position); slot >= 0)
    {
        if (onSlotClicked != nullptr)
            onSlotClicked (slot);
        return;
    }
    if (padBounds().contains (event.position))
    {
        dragging = true;
        if (onDragGestureChanged != nullptr)
            onDragGestureChanged (true);
        updatePosition (event.position);
    }
}

void MorphosisXYPad::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging)
        return;
    if (! event.mods.isLeftButtonDown())
    {
        endDrag();
        return;
    }
    updatePosition (event.position);
}

void MorphosisXYPad::mouseUp (const juce::MouseEvent& event)
{
    juce::ignoreUnused (event);
    endDrag();
}

void MorphosisXYPad::mouseDoubleClick (const juce::MouseEvent& event)
{
    if (padBounds().contains (event.position))
    {
        setPosition (0.5f, 0.5f);
        if (onPositionChanged != nullptr)
            onPositionChanged (x, y);
    }
}

void MorphosisXYPad::visibilityChanged()
{
    juce::Component::visibilityChanged();
    if (! isVisible())
        endDrag();
}

void MorphosisXYPad::parentHierarchyChanged()
{
    juce::Component::parentHierarchyChanged();
    if (! isShowing())
        endDrag();
}

void MorphosisXYPad::endDrag() noexcept
{
    if (! dragging)
        return;

    dragging = false;
    if (onDragGestureChanged != nullptr)
        onDragGestureChanged (false);
}

MorphosisGraph::MorphosisGraph (MorphosisAudioProcessor& newProcessor)
    : processor (newProcessor)
{
    startTimerHz (20);
}

MorphosisGraph::~MorphosisGraph()
{
    stopTimer();
}

void MorphosisGraph::updateResponse (const MorphosisAudioProcessor::GraphSnapshot& snapshot) noexcept
{
    const auto rate = std::max (1000.0, snapshot.sampleRate);

    for (int i = 0; i < responsePoints; ++i)
    {
        const auto proportion = responsePoints > 1
                                  ? static_cast<double> (i) / static_cast<double> (responsePoints - 1)
                                  : 0.0;
        const auto frequency = 20.0 * std::pow (rate * 0.5 / 20.0, proportion);
        if (snapshot.responseFIR)
        {
            responseDb[static_cast<std::size_t> (i)] = snapshot.responseDb[
                static_cast<std::size_t> (i)];
        }
        else
        {
            const auto magnitude = std::max (morphosis::MorphosisDSP::responseMagnitude (
                                                 snapshot.coefficients, frequency, rate), 1.0e-12);
            responseDb[static_cast<std::size_t> (i)] = static_cast<float> (
                std::clamp (20.0 * std::log10 (magnitude), -72.0, 24.0));
        }
    }
    hasResponse = snapshot.coefficients.valid;
}

void MorphosisGraph::timerCallback()
{
    const auto generation = processor.getGraphSnapshotGeneration();
    if (snapshotGeneration != generation)
    {
        snapshotGeneration = generation;
        hasAudioSnapshot = false;
        hasResponse = false;
    }

    MorphosisAudioProcessor::GraphSnapshot snapshot;
    if (processor.popLatestGraphSnapshot (snapshot))
    {
        latestSnapshot = snapshot;
        hasAudioSnapshot = true;
        updateResponse (latestSnapshot);
    }

    repaint();
}

void MorphosisGraph::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour (kGraphFill);
    graphics.fillRoundedRectangle (bounds, 10.0f);
    graphics.setColour (kGraphBorder);
    graphics.drawRoundedRectangle (bounds.reduced (0.75f), 10.0f, 1.5f);

    const auto plot = bounds.withTrimmedLeft (28.0f).withTrimmedRight (24.0f)
                          .withTrimmedTop (16.0f).withTrimmedBottom (8.0f);
    constexpr auto minimumDb = -72.0f;
    const auto dbRange = 24.0f - minimumDb;
    graphics.setColour (juce::Colour::fromRGB (37, 37, 37));
    for (int i = 0; i <= 6; ++i)
    {
        const auto x = plot.getX() + plot.getWidth() * static_cast<float> (i) / 6.0f;
        graphics.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
    }
    for (int index = 0; index <= 4; ++index)
    {
        const auto y = plot.getY() + plot.getHeight() * index / 4.0f;
        graphics.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
    }
    const auto zeroY = plot.getY() + plot.getHeight() * 24.0f / dbRange;
    const float zeroDashes[] { 4.0f, 6.0f };
    graphics.setColour (juce::Colour::fromRGB (199, 199, 199).withAlpha (0.65f));
    graphics.drawDashedLine ({ plot.getX(), zeroY, plot.getRight(), zeroY },
                             zeroDashes, 2, 1.0f);

    graphics.setFont (plexFont (19.0f * std::max (0.1f, bounds.getWidth() / 800.0f), false));
    graphics.setColour (kStaticGrey);
    for (const auto& guide : { std::pair<const char*, float> { "+24 dB", 24.0f },
                               { "0 dB", 0.0f }, { "-72 dB", -72.0f } })
    {
        const auto y = juce::roundToInt (juce::jlimit (
            plot.getY() + 2.0f, plot.getBottom() - 26.0f,
            plot.getY() + plot.getHeight() * (24.0f - guide.second) / dbRange - 12.0f));
        graphics.drawText (guide.first, juce::roundToInt (plot.getX() + 6.0f), y,
                           90, 24, juce::Justification::centredLeft, false);
    }

    if (! hasResponse)
        return;

    juce::Path curve;
    for (int i = 0; i < responsePoints; ++i)
    {
        const auto x = plot.getX() + plot.getWidth() * static_cast<float> (i)
                     / static_cast<float> (std::max (1, responsePoints - 1));
        const auto db = responseDb[static_cast<std::size_t> (i)];
        const auto clippedDb = juce::jlimit (minimumDb, 24.0f, db);
        const auto y = plot.getY() + plot.getHeight() * (24.0f - clippedDb) / dbRange;
        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }
    auto fill = curve;
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.lineTo (plot.getX(), plot.getBottom());
    fill.closeSubPath();
    graphics.setGradientFill (juce::ColourGradient (
        juce::Colour::fromRGB (222, 222, 222).withAlpha (0.20f),
        plot.getCentreX(), plot.getY(),
        juce::Colour::fromRGB (222, 222, 222).withAlpha (0.0f),
        plot.getCentreX(), plot.getBottom(), false));
    graphics.fillPath (fill);
    graphics.setColour (juce::Colour::fromRGB (224, 224, 224));
    graphics.strokePath (curve, juce::PathStrokeType (3.0f));
}

void MorphosisFmxIndicator::setValues (float frequency, float morph, float transform) noexcept
{
    values = { std::clamp (frequency, -5.0f, 5.0f),
               std::clamp (morph, -5.0f, 5.0f),
               std::clamp (transform, -5.0f, 5.0f) };
    repaint();
}

void MorphosisFmxIndicator::paint (juce::Graphics& graphics)
{
    const std::array<juce::Colour, 3> colours { kFmxFrequency, kFmxMorph, kFmxTransform };
    const auto cellWidth = static_cast<float> (getWidth()) / 3.0f;
    const auto rowHeight = static_cast<float> (getHeight());
    const auto labelWidth = juce::jmin (22.0f, cellWidth * 0.12f);
    const auto trackHeight = juce::jmax (4.0f, rowHeight * 0.30f);
    graphics.setFont (plexFont (std::max (10.0f, rowHeight * 0.62f), true));
    for (int column = 0; column < 3; ++column)
    {
        const auto x = cellWidth * static_cast<float> (column);
        graphics.setColour (colours[static_cast<std::size_t> (column)]);
        graphics.drawText (column == 0 ? "F" : column == 1 ? "M" : "X",
                           juce::roundToInt (x), 0, juce::roundToInt (labelWidth),
                           getHeight(), juce::Justification::centredLeft, false);

        const auto rowTrack = juce::Rectangle<float> (
            x + labelWidth + 5.0f, (rowHeight - trackHeight) * 0.5f,
            cellWidth - labelWidth - 14.0f, trackHeight);
        graphics.setColour (juce::Colour::fromRGB (10, 10, 10));
        graphics.fillRoundedRectangle (rowTrack, 2.0f);
        graphics.setColour (kTrackGrey);
        graphics.drawRoundedRectangle (rowTrack, 2.0f, 1.0f);
        graphics.drawVerticalLine (juce::roundToInt (rowTrack.getCentreX()),
                                    rowTrack.getY(), rowTrack.getBottom());

        const auto amount = values[static_cast<std::size_t> (column)] / 5.0f;
        const auto halfWidth = rowTrack.getWidth() * 0.5f;
        const auto fillWidth = std::abs (amount) * halfWidth;
        const auto fillX = amount < 0.0f ? rowTrack.getCentreX() - fillWidth : rowTrack.getCentreX();
        graphics.setColour (colours[static_cast<std::size_t> (column)]);
        graphics.fillRoundedRectangle (fillX, rowTrack.getY() + 1.0f,
                                       fillWidth, std::max (1.0f, rowTrack.getHeight() - 2.0f), 1.5f);
    }
}

void MorphosisMeter::setLevel (float newLevel) noexcept
{
    level = std::clamp (std::isfinite (newLevel) ? newLevel : 0.0f, 0.0f, 1.0f);
    if (level >= peak)
    {
        peak = level;
        holdFrames = 12;
    }
    else if (holdFrames > 0)
    {
        --holdFrames;
    }
    else
    {
        peak *= 0.94f;
    }
    repaint();
}

void MorphosisMeter::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
    graphics.setColour (juce::Colour::fromRGB (10, 10, 10));
    graphics.fillRoundedRectangle (bounds, 2.0f);
    graphics.setColour (kTrackGrey);
    graphics.drawRoundedRectangle (bounds, 2.0f, 1.0f);

    juce::ColourGradient gradient (kMeterGreen, bounds.getX(), 0.0f,
                                   kMeterRed, bounds.getRight(), 0.0f, false);
    gradient.addColour (0.81, kMeterYellow);
    graphics.setGradientFill (gradient);
    graphics.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * level), 2.0f);

    graphics.setColour (juce::Colour::fromRGB (215, 215, 215));
    const auto peakX = bounds.getX() + bounds.getWidth() * peak;
    graphics.fillRect (juce::Rectangle<float> (peakX - 1.0f, bounds.getY(), 2.0f, bounds.getHeight()));
}

MorphosisAudioProcessorEditor::MorphosisAudioProcessorEditor (MorphosisAudioProcessor& processor)
    : AudioProcessorEditor (&processor),
      processorRef (processor),
      graph (processor)
{
    setOpaque (true);
    setResizable (true, true);
    setResizeLimits (540, 1002, 1800, 3340);
    getConstrainer()->setFixedAspectRatio (kDesignAspect);

    auto& state = processorRef.getParameters();
    frequency.attach (state, morphosis::parameter_ids::frequency);
    morph.attach (state, morphosis::parameter_ids::morph);
    transform.attach (state, morphosis::parameter_ids::transform);
    dryWet.setRange (0.0, 1.0, 0.001);
    dryWet.setUnipolar (true);
    dryWet.setDisplayMultiplier (100.0);
    dryWet.getSlider().setRotaryParameters (
        juce::MathConstants<float>::pi * 1.25f,
        juce::MathConstants<float>::pi * 2.75f,
        true);
    dryWet.attach (state, morphosis::parameter_ids::dryWet);
    inputGain.attach (state, morphosis::parameter_ids::inputGainDb);
    preClipGain.attach (state, morphosis::parameter_ids::preClipGainDb);
    postClipGain.attach (state, morphosis::parameter_ids::postClipGainDb);
    threshold.attach (state, morphosis::parameter_ids::internalThresholdSetting);
    sequenceGlide.setRange (0.0, 1.0, 0.001);
    sequenceGlide.setUnipolar (true);
    sequenceGlide.setDisplayMultiplier (100.0);
    sequenceGlide.setSuffix (" %");
    sequenceGlide.getSlider().setRotaryParameters (
        juce::MathConstants<float>::pi * 1.25f,
        juce::MathConstants<float>::pi * 2.75f,
        true);
    sequenceGlide.attach (state, morphosis::parameter_ids::sequenceGlide);
    sequencePosition.setRange (0.0, 1.0, 0.0001);
    sequencePosition.setUnipolar (true);
    sequencePosition.getSlider().setRotaryParameters (
        juce::MathConstants<float>::pi * 1.25f,
        juce::MathConstants<float>::pi * 2.75f,
        true);
    sequencePosition.attach (state, morphosis::parameter_ids::sequencePosition);

    frequency.setAccent (kFmxFrequency);
    morph.setAccent (kFmxMorph);
    transform.setAccent (kFmxTransform);
    dryWet.setAccent (kStaticGrey);
    inputGain.setAccent (kInputAccent);
    preClipGain.setAccent (kPreAccent);
    postClipGain.setAccent (kPostAccent);
    threshold.setAccent (kDistAccent);
    sequenceGlide.setAccent (kStaticGrey);
    sequencePosition.setAccent (kStaticGrey);
    sequenceGlide.setKnobDesignSize (48.0f);
    sequenceGlide.setCaptionDesignFontSize (18.7f);
    sequenceGlide.setHorizontal (true);
    sequencePosition.setKnobDesignSize (96.0f);
    sequencePosition.setCaptionDesignFontSize (18.7f);

    sequenceDivisionBox.addItem ("4 BARS", 1);
    sequenceDivisionBox.addItem ("2 BARS", 2);
    sequenceDivisionBox.addItem ("1 BAR", 3);
    sequenceDivisionBox.addItem ("1/2", 4);
    sequenceDivisionBox.addItem ("1/4", 5);
    sequenceDivisionBox.addItem ("1/8", 6);
    sequenceDivisionBox.addItem ("1/16", 7);
    sequenceDivisionBox.addItem ("1/32", 8);
    sequenceDivisionBox.addItem ("MANUAL", 9);
    sequenceSyncBox.addItem ("STRAIGHT", 1);
    sequenceSyncBox.addItem ("DOT", 2);
    sequenceSyncBox.addItem ("TRIPLET", 3);
    for (int index = 0; index < morphosis::kSequenceSlotCount; ++index)
        sequenceLengthBox.addItem (juce::String (index + 1), index + 1);
    xyInterpolationBox.addItem ("DESCRIPTOR", 1);
    xyInterpolationBox.addItem ("RESPONSE (EXPERIMENTAL)", 2);
    xyInterpolationBox.addItem ("ENCODED (EXPERIMENTAL)", 3);

    addChildComponent (fmxIndicator);
    addAndMakeVisible (leftMeter);
    addAndMakeVisible (rightMeter);
    addAndMakeVisible (graph);
    addAndMakeVisible (sequenceTab);
    addAndMakeVisible (xyTab);
    addAndMakeVisible (modePower);
    addAndMakeVisible (sequenceGrid);
    addAndMakeVisible (xyPad);
    addAndMakeVisible (sequenceGlide);
    addAndMakeVisible (sequencePosition);
    addAndMakeVisible (sequenceDivisionBox);
    addAndMakeVisible (sequenceSyncBox);
    addAndMakeVisible (sequenceLengthBox);
    addAndMakeVisible (xyInterpolationBox);
    addAndMakeVisible (softClip);
    addAndMakeVisible (frequency);
    addAndMakeVisible (morph);
    addAndMakeVisible (transform);
    addAndMakeVisible (dryWet);
    addAndMakeVisible (inputGain);
    addAndMakeVisible (preClipGain);
    addAndMakeVisible (postClipGain);
    addAndMakeVisible (threshold);

    softClipAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        state, morphosis::parameter_ids::softClip, softClip);
    sequenceSyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        state, morphosis::parameter_ids::sequenceSync, sequenceSyncBox);
    sequenceLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        state, morphosis::parameter_ids::sequenceLength, sequenceLengthBox);

    presetLookAndFeel = std::make_unique<MorphosisPresetLookAndFeel>();
    const auto configureCombo = [this] (juce::ComboBox& box)
    {
        box.setLookAndFeel (presetLookAndFeel.get());
        box.setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
        box.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        box.setColour (juce::ComboBox::arrowColourId, juce::Colours::transparentBlack);
        // Closed-state values are painted once in the design shell so the
        // label/value pair can be centered as one unit.
        box.setColour (juce::ComboBox::textColourId, juce::Colours::transparentBlack);
        box.setJustificationType (juce::Justification::centredRight);
        box.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    };
    configureCombo (sequenceDivisionBox);
    configureCombo (sequenceSyncBox);
    configureCombo (sequenceLengthBox);
    configureCombo (xyInterpolationBox);

    xyInterpolationBox.onChange = [this]
    {
        const auto selected = xyInterpolationBox.getSelectedId();
        if (selected == 3)
        {
            setFloatParameter (morphosis::parameter_ids::xyEncodedDomain, 1.0f);
            return;
        }

        if (selected == 1 || selected == 2)
        {
            setFloatParameter (morphosis::parameter_ids::xyEncodedDomain, 0.0f);
            setIntegerParameter (morphosis::parameter_ids::xyInterpolation, selected - 1);
        }
    };

    sequenceDivisionBox.onChange = [this]
    {
        if (updatingSequenceControls)
            return;
        const auto selected = sequenceDivisionBox.getSelectedId();
        if (selected == 9)
        {
            setIntegerParameter (morphosis::parameter_ids::sequenceSource, 1);
            return;
        }
        if (selected >= 1 && selected <= 8)
        {
            setIntegerParameter (morphosis::parameter_ids::sequenceSource, 0);
            setIntegerParameter (morphosis::parameter_ids::sequenceDivision, selected - 1);
        }
    };
    sequenceTab.onClick = [this]
    {
        selectModePanel (false);
    };
    xyTab.onClick = [this]
    {
        selectModePanel (true);
    };
    modePower.onClick = [this] { toggleSelectedModePower(); };
    sequenceGrid.onStepClicked = [this] (int step)
    {
        const auto local = sequenceGrid.getStepBounds (step);
        const auto origin = sequenceGrid.getScreenBounds().getPosition();
        const auto anchor = juce::Rectangle<int> (origin.x + local.getX(),
                                                  origin.y + local.getBottom(),
                                                  local.getWidth(), 2);
        showPresetMenu (anchor, [this, step] (int preset)
        {
            processorRef.setSequenceSlot (step, preset);
        });
    };
    xyPad.onPositionChanged = [this] (float x, float y)
    {
        setFloatParameter (morphosis::parameter_ids::xyX, x);
        setFloatParameter (morphosis::parameter_ids::xyY, y);
    };
    xyPad.onDragGestureChanged = [this] (bool active)
    {
        auto& parameters = processorRef.getParameters();
        auto* xParameter = parameters.getParameter (morphosis::parameter_ids::xyX);
        auto* yParameter = parameters.getParameter (morphosis::parameter_ids::xyY);
        if (active)
        {
            if (xParameter != nullptr)
                xParameter->beginChangeGesture();
            if (yParameter != nullptr)
                yParameter->beginChangeGesture();
        }
        else
        {
            if (xParameter != nullptr)
                xParameter->endChangeGesture();
            if (yParameter != nullptr)
                yParameter->endChangeGesture();
        }
    };
    xyPad.onSlotClicked = [this] (int slot)
    {
        const auto local = xyPad.getSlotBounds (slot);
        const auto origin = xyPad.getScreenBounds().getPosition();
        const auto anchor = juce::Rectangle<int> (origin.x + local.getX(),
                                                  origin.y + local.getBottom(),
                                                  local.getWidth(), 2);
        showPresetMenu (anchor, [this, slot] (int preset)
        {
            processorRef.setXYSlot (slot, preset);
        });
    };
    for (int index = 0; index < processorRef.getNumPrograms(); ++index)
        presetBox.addItem (presetLabel (index, processorRef.getPresetName (index)), index + 1);
    presetBox.setLookAndFeel (presetLookAndFeel.get());
    presetBox.setInterceptsMouseClicks (false, false);
    presetBox.setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
    presetBox.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    presetBox.setColour (juce::ComboBox::arrowColourId, juce::Colours::transparentBlack);
    presetBox.setColour (juce::ComboBox::textColourId, juce::Colours::transparentBlack);
    presetBox.setJustificationType (juce::Justification::centredLeft);
    presetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        state, morphosis::parameter_ids::preset, presetBox);

    presetText.setFont (plexFont (32.0f, false));
    presetText.setLookAndFeel (presetLookAndFeel.get());
    presetText.setJustificationType (juce::Justification::centredLeft);
    presetText.setColour (juce::Label::textColourId, kFooterGrey);
    presetText.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    presetText.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    presetText.onClick = [this]
    {
        showPresetMenu (presetText.getScreenBounds(),
                        [this] (int index) { processorRef.setCurrentProgram (index); }, false);
    };

    addAndMakeVisible (presetBox);
    addAndMakeVisible (presetText);
    addAndMakeVisible (previousPreset);
    addAndMakeVisible (nextPreset);
    presetPicker = std::make_unique<MorphosisPresetPicker> (processorRef);
    addAndMakeVisible (*presetPicker);
    presetPicker->setVisible (false);
    previousPreset.onClick = [this] { processorRef.selectPreviousPreset(); };
    nextPreset.onClick = [this] { processorRef.selectNextPreset(); };

    setSize (600, 1113);
    updatePresetLabel();
    updateLayoutMode();
    startTimerHz (30);
}

void MorphosisAudioProcessorEditor::setIntegerParameter (const char* parameterId, int value)
{
    if (auto* parameter = processorRef.getParameters().getParameter (parameterId))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (static_cast<float> (value)));
}

void MorphosisAudioProcessorEditor::setFloatParameter (const char* parameterId, float value)
{
    if (auto* parameter = processorRef.getParameters().getParameter (parameterId))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void MorphosisAudioProcessorEditor::showPresetMenu (juce::Rectangle<int> anchor,
                                                    std::function<void (int)> onSelected,
                                                    bool preferUpwards)
{
    const auto grouping = presetGrouping == PresetGrouping::recommended
                              ? morphosis::preset_taxonomy::Grouping::recommended
                              : morphosis::preset_taxonomy::Grouping::manual;
    if (presetPicker == nullptr)
        return;

    presetPicker->show (anchor, std::move (onSelected), preferUpwards, grouping,
                        [this] (morphosis::preset_taxonomy::Grouping nextGrouping)
                        {
                            presetGrouping = nextGrouping == morphosis::preset_taxonomy::Grouping::recommended
                                                ? PresetGrouping::recommended
                                                : PresetGrouping::manual;
                        });
}

juce::Rectangle<int> MorphosisAudioProcessorEditor::getPickerPanelBounds() const
{
    return presetPicker != nullptr ? presetPicker->getPanelBoundsForEditor()
                                   : juce::Rectangle<int>();
}

juce::Rectangle<int> MorphosisAudioProcessorEditor::getPickerFlyoutBounds() const
{
    return presetPicker != nullptr ? presetPicker->getFlyoutBoundsForEditor()
                                   : juce::Rectangle<int>();
}

juce::Rectangle<int> MorphosisAudioProcessorEditor::getPickerLeafBounds() const
{
    return presetPicker != nullptr ? presetPicker->getLeafBoundsForEditor()
                                   : juce::Rectangle<int>();
}

juce::Component* MorphosisAudioProcessorEditor::getPickerComponent() const noexcept
{
    return presetPicker.get();
}

void MorphosisAudioProcessorEditor::selectModePanel (bool selectXY)
{
    selectedModePanel = selectXY ? ModePanel::xy : ModePanel::sequencer;
    const auto activeMode = processorRef.getProcessingMode();
    if (activeMode != morphosis::ProcessingMode::preset)
    {
        if (selectXY && activeMode != morphosis::ProcessingMode::xy)
            processorRef.activateXY();
        else if (! selectXY && activeMode != morphosis::ProcessingMode::sequencer)
            processorRef.activateSequencer();
    }
    updateLayoutMode();
}

void MorphosisAudioProcessorEditor::toggleSelectedModePower()
{
    const auto selectedMode = selectedModePanel == ModePanel::xy
                                ? morphosis::ProcessingMode::xy
                                : morphosis::ProcessingMode::sequencer;
    if (processorRef.getProcessingMode() == selectedMode)
        processorRef.returnToPresetMode();
    else if (selectedMode == morphosis::ProcessingMode::xy)
        processorRef.activateXY();
    else
        processorRef.activateSequencer();
    updateLayoutMode();
}

void MorphosisAudioProcessorEditor::paint (juce::Graphics& graphics)
{
    graphics.fillAll (kBackground);
    if (designArea.isEmpty())
        return;

    drawStaticShell (graphics);
}

void MorphosisAudioProcessorEditor::drawStaticShell (juce::Graphics& graphics)
{
    const auto drawPanel = [this, &graphics] (juce::Rectangle<float> bounds,
                                              float radius,
                                              juce::Colour fill)
    {
        const auto screenBounds = toScreen (bounds);
        graphics.setColour (fill);
        graphics.fillRoundedRectangle (screenBounds, radius * designScale);
        graphics.setColour (kPanelBorder);
        graphics.drawRoundedRectangle (screenBounds, radius * designScale,
                                       std::max (1.0f, 2.0f * designScale));
    };

    const auto drawText = [this, &graphics] (const juce::String& text,
                                             juce::Rectangle<float> bounds,
                                             float fontHeight,
                                             juce::Colour colour,
                                             bool bold,
                                             juce::Justification justification)
    {
        graphics.setColour (colour);
        graphics.setFont (plexFont (std::max (9.0f, fontHeight * designScale * 1.28f), bold));
        graphics.drawText (text, toScreenInt (bounds), justification, false);
    };

    const auto drawDottedLine = [this, &graphics] (float x1, float y1, float x2, float y2,
                                                   float alpha = 1.0f)
    {
        const auto p1 = juce::Point<float> (designArea.getX() + x1 * designScale,
                                            designArea.getY() + y1 * designScale);
        const auto p2 = juce::Point<float> (designArea.getX() + x2 * designScale,
                                            designArea.getY() + y2 * designScale);
        graphics.setColour (kStaticGrey.withAlpha (0.62f * alpha));
        const float dashes[] { 2.0f * designScale, 4.0f * designScale };
        graphics.drawDashedLine (juce::Line<float> (p1, p2), dashes, 2,
                                 std::max (1.0f, designScale));
    };

    graphics.setColour (kPanelBorder);
    graphics.drawRoundedRectangle (toScreen ({ 14.0f, 14.0f, 872.0f, 1642.0f }),
                                   20.0f * designScale,
                                   std::max (1.0f, 1.5f * designScale));

    drawText ("MORPHOSIS", { 32.0f, 25.0f, 370.0f, 42.0f }, 32.0f,
              juce::Colour::fromRGB (240, 240, 240), true, juce::Justification::left);
    drawText ("STEREO MORPHING FILTER", { 580.0f, 32.0f, 289.0f, 25.0f }, 14.0f,
              kStaticGrey, false, juce::Justification::right);

    drawPanel ({ 28.0f, 83.0f, 844.0f, 106.0f }, 14.0f, kPanelFill);
    drawText ("PRESET", { 49.0f, 92.0f, 180.0f, 23.0f }, 14.0f,
              kStaticGrey, true, juce::Justification::left);
    drawPanel ({ 45.0f, 120.0f, 744.0f, 52.0f }, 8.0f,
                juce::Colour::fromRGB (43, 43, 43));
    drawPanel ({ 804.0f, 120.0f, 51.0f, 52.0f }, 8.0f,
                juce::Colour::fromRGB (43, 43, 43));
    juce::Path selectorChevron;
    selectorChevron.startNewSubPath (751.0f, 143.0f);
    selectorChevron.lineTo (758.0f, 151.0f);
    selectorChevron.lineTo (765.0f, 143.0f);
    selectorChevron.applyTransform (juce::AffineTransform::scale (designScale)
        .translated (designArea.getX(), designArea.getY()));
    graphics.setColour (kFooterGrey.withAlpha (
        processorRef.getProcessingMode() == morphosis::ProcessingMode::preset ? 1.0f : 0.42f));
    graphics.strokePath (selectorChevron,
                         juce::PathStrokeType (2.0f * designScale));

    drawPanel ({ 28.0f, 207.0f, 844.0f, 414.0f }, 16.0f, kPanelFill);
    drawText ("L", { 51.0f, 234.0f, 18.0f, 27.0f }, 16.0f,
              kMeterLeftLabel, true, juce::Justification::left);
    drawText ("R", { 51.0f, 260.0f, 18.0f, 27.0f }, 16.0f,
              kMeterRightLabel, true, juce::Justification::left);
    drawPanel ({ 75.0f, 239.0f, 415.0f, 15.0f }, 5.0f,
                juce::Colour::fromRGB (23, 23, 23));
    drawPanel ({ 75.0f, 265.0f, 415.0f, 15.0f }, 5.0f,
                juce::Colour::fromRGB (23, 23, 23));
    drawPanel ({ 28.0f, 639.0f, 844.0f, 210.0f }, 16.0f, kPanelFill);
    drawDottedLine (312.0f, 664.0f, 312.0f, 827.0f);
    drawDottedLine (588.0f, 664.0f, 588.0f, 827.0f);

    drawPanel ({ 28.0f, 869.0f, 844.0f, 590.0f }, 16.0f, kPanelFill);
    const auto snapshot = processorRef.getParameterSnapshot();
    const auto sequenceActive = snapshot.mode == morphosis::ProcessingMode::sequencer;
    const auto xyActive = snapshot.mode == morphosis::ProcessingMode::xy;
    const auto activePanelAlpha = sequenceActive || xyActive ? 1.0f : 0.56f;
    if (selectedModePanel == ModePanel::sequencer)
    {
        drawDottedLine (48.0f, 991.0f, 852.0f, 991.0f);
        drawDottedLine (48.0f, 1383.0f, 852.0f, 1383.0f);
        static constexpr const char* divisionLabels[] {
            "4 BARS", "2 BARS", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/32" };
        static constexpr const char* syncLabels[] { "STRAIGHT", "DOT", "TRIPLET" };
        const auto drawPair = [&] (juce::Rectangle<float> bounds,
                                   const juce::String& label,
                                   const juce::String& value)
        {
            const auto rowFont = plexFont (21.8f, true);
            const auto valueFont = plexFont (21.8f, false);
            constexpr float gap = 10.0f;
            const auto labelWidth = rowFont.getStringWidthFloat (label);
            const auto valueWidth = valueFont.getStringWidthFloat (value);
            const auto pairWidth = labelWidth + gap + valueWidth;
            const auto left = bounds.getCentreX() - pairWidth * 0.5f;
            drawText (label, { left, bounds.getY(), labelWidth + 2.0f, bounds.getHeight() },
                      17.0f, kStaticGrey.withAlpha (activePanelAlpha), true,
                      juce::Justification::left);
            drawText (value, { left + labelWidth + gap, bounds.getY(), valueWidth + 2.0f,
                               bounds.getHeight() }, 17.0f,
                      juce::Colour::fromRGB (240, 240, 240).withAlpha (activePanelAlpha),
                      false, juce::Justification::left);
        };

        const auto sourceText = snapshot.sequenceManual
                                  ? juce::String ("MANUAL")
                                  : juce::String (divisionLabels[
                                        juce::jlimit (0, 7, snapshot.sequenceDivision)]);
        drawPair ({ 48.0f, 944.0f, 246.0f, 36.0f }, "CLOCK", sourceText);
        if (! snapshot.sequenceManual)
        {
            drawPair ({ 326.0f, 944.0f, 238.0f, 36.0f }, "SYNC",
                      syncLabels[juce::jlimit (0, 2, snapshot.sequenceSync)]);
            drawPair ({ 590.0f, 944.0f, 238.0f, 36.0f }, "STEPS",
                      juce::String (snapshot.sequenceLength).paddedLeft ('0', 2));
        }
    }
    else
    {
        drawText ("BLEND FOUR CUBES", { 52.0f, 944.0f, 300.0f, 34.0f }, 16.0f,
                  kStaticGrey.withAlpha (xyActive ? 1.0f : 0.56f), true,
                  juce::Justification::left);
        const auto interpolation = snapshot.xyEncodedDomain
                                     ? juce::String ("ENCODED (EXPERIMENTAL)")
                                     : snapshot.xyInterpolation
                                           == morphosis::XYInterpolationMode::response
                                         ? juce::String ("RESPONSE (EXPERIMENTAL)")
                                         : juce::String ("DESCRIPTOR");
        const auto label = juce::String ("INTERPOLATION");
        const auto valueFont = plexFont (19.2f, false);
        const auto labelFont = plexFont (19.2f, true);
        const auto pairWidth = labelFont.getStringWidthFloat (label) + 12.0f
                             + valueFont.getStringWidthFloat (interpolation);
        const auto left = 842.0f - pairWidth;
        drawText (label, { left, 944.0f, labelFont.getStringWidthFloat (label) + 2.0f, 36.0f },
                  15.0f, kStaticGrey.withAlpha (xyActive ? 1.0f : 0.56f), true,
                  juce::Justification::left);
        drawText (interpolation,
                  { left + labelFont.getStringWidthFloat (label) + 12.0f,
                    944.0f, valueFont.getStringWidthFloat (interpolation) + 3.0f, 36.0f },
                  15.0f, juce::Colour::fromRGB (240, 240, 240)
                      .withAlpha (xyActive ? 1.0f : 0.56f), false,
                  juce::Justification::left);
    }

    drawPanel ({ 28.0f, 1477.0f, 844.0f, 154.0f }, 16.0f, kPanelFill);
}

void MorphosisAudioProcessorEditor::resized()
{
    const auto width = static_cast<float> (getWidth());
    const auto height = static_cast<float> (getHeight());
    designScale = std::min (width / kDesignWidth, height / kDesignHeight);
    designArea = { (width - kDesignWidth * designScale) * 0.5f,
                   (height - kDesignHeight * designScale) * 0.5f,
                   kDesignWidth * designScale,
                   kDesignHeight * designScale };

    const auto setDesignBounds = [this] (juce::Component& component, juce::Rectangle<float> bounds)
    {
        component.setBounds (toScreenInt (bounds));
    };

    setDesignBounds (leftMeter, { 75.0f, 239.0f, 415.0f, 15.0f });
    setDesignBounds (rightMeter, { 75.0f, 265.0f, 415.0f, 15.0f });
    setDesignBounds (softClip, { 526.0f, 239.0f, 326.0f, 41.0f });
    setDesignBounds (graph, { 50.0f, 294.0f, 800.0f, 309.0f });

    setDesignBounds (sequenceTab, { 45.0f, 886.0f, 370.0f, 54.0f });
    setDesignBounds (xyTab, { 423.0f, 886.0f, 370.0f, 54.0f });
    setDesignBounds (modePower, { 801.0f, 886.0f, 54.0f, 54.0f });
    setDesignBounds (sequenceGrid, { 48.0f, 1009.0f, 804.0f, 364.0f });
    setDesignBounds (sequenceDivisionBox, { 48.0f, 944.0f, 246.0f, 36.0f });
    setDesignBounds (sequenceSyncBox, { 326.0f, 944.0f, 238.0f, 36.0f });
    setDesignBounds (sequenceLengthBox, { 590.0f, 944.0f, 238.0f, 36.0f });
    setDesignBounds (sequencePosition, { 455.0f, 942.0f, 270.0f, 46.0f });
    setDesignBounds (sequenceGlide, { 58.0f, 1388.0f, 420.0f, 56.0f });
    setDesignBounds (xyInterpolationBox, { 500.0f, 944.0f, 342.0f, 36.0f });
    setDesignBounds (xyPad, { 58.0f, 986.0f, 784.0f, 438.0f });

    setDesignBounds (frequency, { 36.0f, 650.0f, 276.0f, 199.0f });
    setDesignBounds (morph, { 312.0f, 650.0f, 276.0f, 199.0f });
    setDesignBounds (transform, distortionLayout
                                  ? juce::Rectangle<float> { 594.0f, 650.0f, 136.0f, 199.0f }
                                  : juce::Rectangle<float> { 588.0f, 650.0f, 284.0f, 199.0f });
    setDesignBounds (threshold, { 730.0f, 650.0f, 136.0f, 199.0f });

    setDesignBounds (dryWet, { 38.0f, 1490.0f, 164.0f, 128.0f });
    setDesignBounds (inputGain, { 258.0f, 1490.0f, 164.0f, 128.0f });
    setDesignBounds (preClipGain, { 478.0f, 1490.0f, 164.0f, 128.0f });
    setDesignBounds (postClipGain, { 698.0f, 1490.0f, 164.0f, 128.0f });

    dryWet.setScale (designScale);
    inputGain.setScale (designScale);
    preClipGain.setScale (designScale);
    postClipGain.setScale (designScale);
    frequency.setScale (designScale);
    morph.setScale (designScale);
    transform.setScale (designScale);
    threshold.setScale (designScale);
    sequenceGlide.setScale (designScale);
    sequencePosition.setScale (designScale);
    transform.setCaptionDesignFontSize (distortionLayout ? 25.0f : 30.0f);
    threshold.setCaptionDesignFontSize (25.0f);

    sequencePosition.setCaptionPlacement (MorphosisKnobControl::CaptionPlacement::left);
    sequencePosition.setKnobDesignSize (46.0f);
    sequencePosition.setCaptionDesignFontSize (20.0f);
    sequenceGlide.setCaptionPlacement (MorphosisKnobControl::CaptionPlacement::left);
    sequenceGlide.setKnobDesignSize (52.0f);
    sequenceGlide.setCaptionDesignFontSize (20.0f);
    for (auto* gain : { &dryWet, &inputGain, &preClipGain, &postClipGain })
    {
        gain->setKnobDesignSize (64.0f);
        gain->setCaptionDesignFontSize (22.0f);
    }
    frequency.setKnobDesignSize (144.0f);
    morph.setKnobDesignSize (144.0f);
    frequency.setCaptionDesignFontSize (30.0f);
    morph.setCaptionDesignFontSize (30.0f);

    setDesignBounds (presetBox, { 45.0f, 120.0f, 744.0f, 52.0f });
    setDesignBounds (presetText, { 62.0f, 120.0f, 700.0f, 52.0f });
    presetText.setFont (plexFont (32.0f * designScale, false));
    setDesignBounds (previousPreset, { 804.0f, 120.0f, 25.0f, 52.0f });
    setDesignBounds (nextPreset, { 830.0f, 120.0f, 25.0f, 52.0f });

    transform.setKnobDesignSize (distortionLayout ? 84.0f : 144.0f);
    transform.setCaptionPlacement (MorphosisKnobControl::CaptionPlacement::below);
    threshold.setKnobDesignSize (84.0f);
    threshold.setCaptionPlacement (MorphosisKnobControl::CaptionPlacement::below);
    transform.setVisible (true);
    threshold.setVisible (distortionLayout);
    if (presetPicker != nullptr)
    {
        presetPicker->setBounds (getLocalBounds());
        presetPicker->setScale (designScale);
    }
    repaint();
}

void MorphosisAudioProcessorEditor::timerCallback()
{
    updateLayoutMode();
    updatePresetLabel();
    updateSequenceControls();
    const auto snapshot = processorRef.getParameterSnapshot();
    fmxIndicator.setValues (static_cast<float> (snapshot.frequency),
                            static_cast<float> (snapshot.morph),
                            static_cast<float> (snapshot.transform));
    leftMeter.setLevel (processorRef.consumePeak (0));
    rightMeter.setLevel (processorRef.consumePeak (1));
}

void MorphosisAudioProcessorEditor::updateLayoutMode()
{
    const auto mode = processorRef.getProcessingMode();
    const auto nextMode = mode != morphosis::ProcessingMode::preset
                       || morphosis::usesDotFourDistortionLayout (processorRef.getPresetIndex());
    if (distortionLayout != nextMode)
    {
        distortionLayout = nextMode;
        transform.setTitle ("XFORM");
        threshold.setTitle ("DIST");
        resized();
    }

    updateModeView();
}

void MorphosisAudioProcessorEditor::updateModeView()
{
    const auto mode = processorRef.getProcessingMode();
    const auto sequencerActive = mode == morphosis::ProcessingMode::sequencer;
    const auto xyActive = mode == morphosis::ProcessingMode::xy;
    if (sequencerActive)
        selectedModePanel = ModePanel::sequencer;
    else if (xyActive)
        selectedModePanel = ModePanel::xy;

    const auto sequenceSelected = selectedModePanel == ModePanel::sequencer;
    const auto xySelected = selectedModePanel == ModePanel::xy;
    const auto parameterSnapshot = processorRef.getParameterSnapshot();
    const auto manual = parameterSnapshot.sequenceManual;

    sequenceTab.setSelected (sequenceSelected);
    xyTab.setSelected (xySelected);
    modePower.setActive (sequencerActive || xyActive);

    const auto sequenceAlpha = sequencerActive ? 1.0f : 0.54f;
    const auto xyAlpha = xyActive ? 1.0f : 0.54f;
    sequenceGrid.setVisible (sequenceSelected);
    sequenceGrid.setAlpha (sequenceAlpha);
    sequenceDivisionBox.setVisible (sequenceSelected);
    sequenceDivisionBox.setAlpha (sequenceAlpha);
    sequenceSyncBox.setVisible (sequenceSelected && ! manual);
    sequenceSyncBox.setAlpha (sequenceAlpha);
    sequenceLengthBox.setVisible (sequenceSelected && ! manual);
    sequenceLengthBox.setAlpha (sequenceAlpha);
    sequenceGlide.setVisible (sequenceSelected);
    if (manual != sequenceGlideUsesManualParameter)
    {
        sequenceGlideUsesManualParameter = manual;
        sequenceGlide.detach();
        sequenceGlide.setRange (manual ? 5.0 : 0.0,
                                manual ? 250.0 : 1.0,
                                manual ? 0.01 : 0.001);
        sequenceGlide.attach (processorRef.getParameters(),
                              manual ? morphosis::parameter_ids::manualTransitionMs
                                     : morphosis::parameter_ids::sequenceGlide);
        sequenceGlide.setTitle (manual ? "TRANSITION" : "GLIDE");
        sequenceGlide.setSuffix (manual ? " ms" : " %");
        sequenceGlide.setDisplayMultiplier (manual ? 1.0 : 100.0);
    }
    sequenceGlide.setEnabled (true);
    sequenceGlide.setAlpha (sequenceAlpha);
    sequencePosition.setVisible (sequenceSelected && manual);
    sequencePosition.setAlpha (sequenceAlpha);
    xyPad.setVisible (xySelected);
    xyPad.setAlpha (xyAlpha);
    const auto encodedDomain = parameterSnapshot.xyEncodedDomain;
    // An inactive XY configuration must not disable preset/sequencer controls.
    const auto responseMode = xyActive && ! encodedDomain
                            && parameterSnapshot.xyInterpolation
                                   == morphosis::XYInterpolationMode::response;
    xyInterpolationBox.setVisible (xySelected);
    xyInterpolationBox.setAlpha (xyAlpha);
    xyInterpolationBox.setSelectedId (encodedDomain
                                          ? 3
                                          : parameterSnapshot.xyInterpolation
                                                == morphosis::XYInterpolationMode::response
                                              ? 2 : 1,
                                      juce::dontSendNotification);
    xyInterpolationBox.setTooltip (encodedDomain
        ? "Encoded (Experimental): firmware numeric domain applied between cubes"
        : "Select the XY interpolation domain");
    threshold.setEnabled (! responseMode);
    threshold.setAlpha (responseMode ? 0.35f : 1.0f);
    threshold.getSlider().setTooltip (responseMode
        ? "Disabled in Response (Experimental); value is remembered"
        : juce::String());

    const auto presetMode = mode == morphosis::ProcessingMode::preset;
    presetText.setInterceptsMouseClicks (presetMode, false);
    presetText.setAlpha (presetMode ? 1.0f : 0.42f);
    presetBox.setEnabled (presetMode);
    previousPreset.setEnabled (presetMode);
    nextPreset.setEnabled (presetMode);
    presetBox.setAlpha (presetMode ? 1.0f : 0.25f);
    previousPreset.setAlpha (presetMode ? 1.0f : 0.25f);
    nextPreset.setAlpha (presetMode ? 1.0f : 0.25f);

    const ShellVisualState nextShellVisualState {
        sequencerActive,
        xyActive,
        xySelected,
        manual,
        manual ? -1 : juce::jlimit (0, 7, parameterSnapshot.sequenceDivision),
        manual ? -1 : juce::jlimit (0, 2, parameterSnapshot.sequenceSync),
        manual ? -1 : parameterSnapshot.sequenceLength,
        static_cast<int> (parameterSnapshot.xyInterpolation),
        encodedDomain
    };
    if (! hasShellVisualState || !(lastShellVisualState == nextShellVisualState))
    {
        lastShellVisualState = nextShellVisualState;
        hasShellVisualState = true;
        repaint();
    }
}

void MorphosisAudioProcessorEditor::updateSequenceControls()
{
    const auto snapshot = processorRef.getParameterSnapshot();
    updatingSequenceControls = true;
    sequenceDivisionBox.setSelectedId (snapshot.sequenceManual
                                           ? 9
                                           : snapshot.sequenceDivision + 1,
                                       juce::dontSendNotification);
    sequenceSyncBox.setSelectedId (snapshot.sequenceSync + 1, juce::dontSendNotification);
    sequenceLengthBox.setSelectedId (snapshot.sequenceLength, juce::dontSendNotification);
    updatingSequenceControls = false;

    auto currentStep = processorRef.getSequencerStep();
    if (snapshot.sequenceManual)
        currentStep = morphosis::sequenceStepFromPosition (snapshot.sequencePosition);
    // Manual Position scans all sixteen destinations. The stored host length
    // remains available when returning to HOST mode.
    const auto visibleLength = snapshot.sequenceManual
                                  ? morphosis::kSequenceSlotCount
                                  : snapshot.sequenceLength;
    sequenceGrid.setState (snapshot.sequencePresets, visibleLength, currentStep);
    xyPad.setPosition (static_cast<float> (snapshot.xyX), static_cast<float> (snapshot.xyY));
    xyPad.setSlots (snapshot.xyPresets);
}

void MorphosisAudioProcessorEditor::updatePresetLabel()
{
    const auto index = processorRef.getPresetIndex();
    presetText.setText (presetLabel (index, processorRef.getPresetName (index)),
                        juce::dontSendNotification);
    presetBox.setSelectedItemIndex (index, juce::dontSendNotification);
}

/*
    The remainder of the layout is deliberately kept in design coordinates. This
    makes the mode controls follow the same fixed-aspect contract as the supplied
    artwork while leaving all text and interaction native.
*/

juce::Rectangle<float> MorphosisAudioProcessorEditor::toScreen (
    juce::Rectangle<float> designBounds) const
{
    return { designArea.getX() + designBounds.getX() * designScale,
             designArea.getY() + designBounds.getY() * designScale,
             designBounds.getWidth() * designScale,
             designBounds.getHeight() * designScale };
}

juce::Rectangle<int> MorphosisAudioProcessorEditor::toScreenInt (
    juce::Rectangle<float> designBounds) const
{
    return toScreen (designBounds).toNearestInt();
}

juce::Rectangle<float> MorphosisAudioProcessorEditor::getDesignArea() const noexcept
{
    return designArea;
}
