#include "MorphosisSequencer.h"
#include "MorphosisUIUtilities.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <optional>

namespace
{

int failures = 0;

void require (bool condition, const char* message)
{
    if (! condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void expectNear (std::optional<double> actual, double expected, const char* message)
{
    require (actual.has_value() && std::abs (*actual - expected) < 1.0e-12, message);
}

void checkScaledCaptionEntries()
{
    using morphosis::ui::parseScaledCaptionEntry;

    expectNear (parseScaledCaptionEntry ("50", 100.0, 0.0, 1.0, "%"),
                0.5, "plain displayed percent converts to parameter units");
    expectNear (parseScaledCaptionEntry ("50%", 100.0, 0.0, 1.0, "%"),
                0.5, "percent suffix converts to parameter units");
    expectNear (parseScaledCaptionEntry ("5e1%", 100.0, 0.0, 1.0, "%"),
                0.5, "valid scientific notation converts to parameter units");
    expectNear (parseScaledCaptionEntry (" 50.25 % ", 100.0, 0.0, 1.0, "%"),
                0.5025, "whitespace around value and suffix is accepted");
    expectNear (parseScaledCaptionEntry ("150%", 100.0, 0.0, 1.0, "%"),
                1.0, "values above the control range clamp to its maximum");
    expectNear (parseScaledCaptionEntry ("-5", 100.0, 0.0, 1.0, "%"),
                0.0, "values below the control range clamp to its minimum");

    for (const auto* invalid : { "", "noise", "50percent", "50%%", "%50",
                                 "1e", "1e+", "1e999", "nan", "inf" })
        require (! parseScaledCaptionEntry (invalid, 100.0, 0.0, 1.0, "%").has_value(),
                 "malformed or non-finite scaled caption entry is rejected");

    require (! parseScaledCaptionEntry ("50", 0.0, 0.0, 1.0, "%").has_value(),
             "invalid display multiplier is rejected");
    require (! parseScaledCaptionEntry ("50", 100.0,
                                        std::numeric_limits<double>::quiet_NaN(), 1.0, "%")
                  .has_value(),
             "invalid slider bounds are rejected");
}

void checkManualSequenceStepBoundaries()
{
    constexpr auto lastStep = morphosis::kSequenceSlotCount - 1;

    for (int step = 0; step <= lastStep; ++step)
        require (morphosis::sequenceStepFromPosition (
                     static_cast<double> (step) / static_cast<double> (lastStep)) == step,
                 "each normalized step coordinate maps back to its exact step");

    for (int lowerStep = 0; lowerStep < lastStep; ++lowerStep)
    {
        const auto boundary = (static_cast<double> (lowerStep) + 0.5)
                            / static_cast<double> (lastStep);
        require (morphosis::sequenceStepFromPosition (boundary - 1.0e-8) == lowerStep,
                 "position just below a step boundary selects the lower step");
        require (morphosis::sequenceStepFromPosition (boundary + 1.0e-8) == lowerStep + 1,
                 "position just above a step boundary selects the upper step");
    }

    require (morphosis::sequenceStepFromPosition (0.5) == 8,
             "the midpoint tie follows DSP round-to-nearest behavior");
    require (morphosis::sequenceStepFromPosition (-1.0) == 0
                 && morphosis::sequenceStepFromPosition (2.0) == lastStep,
             "positions outside the normalized range clamp to the endpoints");
    require (morphosis::sequenceStepFromPosition (
                 std::numeric_limits<double>::quiet_NaN()) == 0,
             "non-finite manual positions map to the safe first step");
}

} // namespace

int main()
{
    checkScaledCaptionEntries();
    checkManualSequenceStepBoundaries();

    if (failures != 0)
    {
        std::cerr << failures << " UI behavior regression(s) failed\n";
        return 1;
    }

    std::cout << "Scaled caption parsing and manual step boundary tests passed\n";
    return 0;
}
