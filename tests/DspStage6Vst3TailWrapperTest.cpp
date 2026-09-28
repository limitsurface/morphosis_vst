#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdlib>
#include <iostream>

int main (int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: vst3-tail-wrapper-test <plugin-binary>\n";
        return EXIT_FAILURE;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile (descriptions, argv[1]);
    if (descriptions.size() != 1)
    {
        std::cerr << "FAIL: expected one VST3 component in candidate, found "
                  << descriptions.size() << ".\n";
        return EXIT_FAILURE;
    }

    juce::String error;
    auto instance = format.createInstanceFromDescription (
        *descriptions.getFirst(), 48000.0, 512, error);
    if (instance == nullptr)
    {
        std::cerr << "FAIL: could not instantiate the built VST3: "
                  << error << '\n';
        return EXIT_FAILURE;
    }

    instance->prepareToPlay (48000.0, 512);
    const auto tailSeconds = instance->getTailLengthSeconds();
    instance->releaseResources();
    if (! std::isinf (tailSeconds) || tailSeconds <= 0.0)
    {
        std::cerr << "FAIL: VST3 wrapper did not round-trip kInfiniteTail; host-side seconds="
                  << tailSeconds << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Built VST3 wrapper round-tripped kInfiniteTail as positive infinity.\n";
    return EXIT_SUCCESS;
}
