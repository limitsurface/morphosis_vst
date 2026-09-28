#include "MorphosisDSP.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

int failures = 0;

void expect (bool condition, const std::string& message)
{
    if (! condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

double directResponse (const std::array<float, morphosis::kExperimentalFirTaps>& impulse,
                       double frequencyHz,
                       double hostRate)
{
    constexpr double pi = 3.1415926535897932384626433832795;
    const auto omega = 2.0 * pi
                     * std::clamp (frequencyHz, 0.0, hostRate * 0.5) / hostRate;
    double real = 0.0;
    double imaginary = 0.0;
    for (int tap = 0; tap < morphosis::kExperimentalFirTaps; ++tap)
    {
        const auto phase = omega * static_cast<double> (tap);
        const auto coefficient = static_cast<double> (impulse[static_cast<std::size_t> (tap)]);
        real += coefficient * std::cos (phase);
        imaginary -= coefficient * std::sin (phase);
    }
    return std::hypot (real, imaginary);
}

double graphDb (double magnitude)
{
    constexpr double targetFloor = 1.0e-7;
    return std::clamp (20.0 * std::log10 (std::max (magnitude, targetFloor)), -72.0, 24.0);
}

struct Fixture
{
    const char* name;
    std::array<int, morphosis::kMaxBlendSources> presets;
    double x;
    double y;
    double frequency;
    double morph;
    double transform;
};

void checkImpulsePrimitives()
{
    std::array<float, morphosis::kExperimentalFirTaps> impulse {};
    impulse[0] = 1.0f;
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto frequency : { 0.0, 20.0, 1000.0, rate * 0.25, rate * 0.5 })
        {
            const auto actual = morphosis::MorphosisDSP::responseMagnitudeFromFir (
                impulse, frequency, rate);
            const auto expected = directResponse (impulse, frequency, rate);
            expect (std::abs (actual - expected) <= 1.0e-14,
                    "unit impulse remains unity at DC, interior frequencies, and Nyquist");
        }
    }

    impulse.fill (0.0f);
    const auto zeroResponse = morphosis::MorphosisDSP::responseMagnitudeFromFir (
        impulse, 12000.0, 48000.0);
    expect (zeroResponse == 0.0,
            "zero impulse remains exactly zero for graph floor handling");
    expect (graphDb (zeroResponse) == -72.0 && graphDb (1.0e-7) == -72.0,
            "target-floor values remain inside the displayed -72 dB clamp");

    std::uint32_t random = 0x82c3a5d1u;
    for (auto& value : impulse)
    {
        random = random * 1664525u + 1013904223u;
        const auto signedValue = static_cast<std::int32_t> (random >> 8) / 8388608.0f;
        value = signedValue * 0.001f;
    }
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto frequency : { 0.0, 20.0, 200.0, 3000.0, 12000.0, rate * 0.5 })
        {
            const auto actual = morphosis::MorphosisDSP::responseMagnitudeFromFir (
                impulse, frequency, rate);
            const auto expected = directResponse (impulse, frequency, rate);
            const auto tolerance = 2.0e-11 * std::max (1.0, expected);
            expect (std::abs (actual - expected) <= tolerance,
                    "general FIR response agrees with independent direct DFT");
        }
    }
}

void checkDesignGraphs()
{
    constexpr std::array<bool, morphosis::kMaxBlendSources> noDistortion {{
        false, false, false, false
    }};
    const std::array<Fixture, 6> fixtures {{
        { "Null", { 0, 0, 0, 0 }, 0.5, 0.5, 0.0, 0.0, 0.0 },
        { "high-Q Dave's Rave", { 197, 197, 197, 197 }, 0.5, 0.5, 0.35, 4.5, 0.1 },
        { "notch peaks", { 121, 121, 121, 121 }, 0.5, 0.5, -0.7, 1.5, 0.1 },
        { "LPFlange.4", { 1, 1, 1, 1 }, 0.5, 0.5, -0.2, -4.0, 3.0 },
        { "four-cube mixed", { 0, 43, 17, 184 }, 0.37, 0.63, 0.35, -1.87, 2.37 },
        { "four-cube boundary", { 0, 43, 17, 184 }, 0.001, 0.999, -4.0, 3.0, -2.5 }
    }};

    double maximumMagnitudeError = 0.0;
    double maximumGraphErrorDb = 0.0;
    int floorBins = 0;
    int graphFloorBins = 0;
    int floorClassificationMismatches = 0;
    int nyquistChecks = 0;
    constexpr double targetFloor = 1.0e-7;

    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto& fixture : fixtures)
        {
            const auto profile = morphosis::MorphosisDSP::buildFirForStage8Profiling (
                fixture.presets, noDistortion, fixture.x, fixture.y, fixture.frequency,
                fixture.morph, fixture.transform, rate);
            expect (profile.valid && ! profile.usedSafeFallback,
                    std::string (fixture.name) + " makes a valid FIR design");
            if (! profile.valid)
                continue;

            for (int index = 0; index < morphosis::kExperimentalFirGraphPoints; ++index)
            {
                const auto proportion = index > 0
                    ? static_cast<double> (index)
                        / static_cast<double> (morphosis::kExperimentalFirGraphPoints - 1)
                    : 0.0;
                const auto frequency = 20.0 * std::pow (rate * 0.5 / 20.0, proportion);
                const auto expectedMagnitude = directResponse (profile.impulse, frequency, rate);
                const auto actualMagnitude = morphosis::MorphosisDSP::responseMagnitudeFromFir (
                    profile.impulse, frequency, rate);
                const auto magnitudeError = std::abs (actualMagnitude - expectedMagnitude);
                maximumMagnitudeError = std::max (maximumMagnitudeError, magnitudeError);
                if (magnitudeError > 2.0e-11 * std::max (1.0, expectedMagnitude))
                    expect (false, std::string (fixture.name) + " direct graph DFT agrees");

                const auto expectedDb = graphDb (expectedMagnitude);
                const auto actualDb = static_cast<double> (
                    profile.graphResponseDb[static_cast<std::size_t> (index)]);
                const auto graphError = std::abs (actualDb - expectedDb);
                maximumGraphErrorDb = std::max (maximumGraphErrorDb, graphError);
                if (graphError > 1.0e-4)
                    expect (false, std::string (fixture.name) + " graph stays within 1e-4 dB");

                floorBins += expectedMagnitude <= targetFloor ? 1 : 0;
                graphFloorBins += expectedDb <= -72.0 ? 1 : 0;
                floorClassificationMismatches +=
                    (actualMagnitude <= targetFloor) != (expectedMagnitude <= targetFloor) ? 1 : 0;
                if (index == morphosis::kExperimentalFirGraphPoints - 1)
                {
                    ++nyquistChecks;
                    expect (std::abs (frequency - rate * 0.5) <= rate * 1.0e-12,
                            "last logarithmic graph point is Nyquist");
                    expect (graphError <= 1.0e-4,
                            std::string (fixture.name) + " Nyquist graph agrees with direct DFT");
                }
            }
        }
    }

    std::cout << "stage8_graph_max_magnitude_error=" << maximumMagnitudeError
              << " stage8_graph_max_db_error=" << maximumGraphErrorDb
              << " target_floor_bins=" << floorBins
              << " displayed_floor_bins=" << graphFloorBins
              << " floor_classification_mismatches=" << floorClassificationMismatches
              << " nyquist_checks=" << nyquistChecks << '\n';
    expect (graphFloorBins > 0, "fixtures exercise the displayed -72 dB clamp");
    expect (floorClassificationMismatches == 0,
            "recurrence does not move any graph bin across the internal target floor");
    expect (nyquistChecks == 18, "every fixture and sample rate checks Nyquist");
}

} // namespace

int main()
{
    checkImpulsePrimitives();
    checkDesignGraphs();
    if (failures != 0)
    {
        std::cerr << failures << " Stage 8 FIR graph regression(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Stage 8 FIR graph regression passed\n";
    return EXIT_SUCCESS;
}
