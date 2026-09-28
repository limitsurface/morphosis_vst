#include "PluginProcessor.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

int main()
{
    MorphosisAudioProcessor processor;
    const auto tailSeconds = processor.getTailLengthSeconds();
    if (processor.getLatencySamples() != morphosis::kMorphosisLatencySamples)
    {
        std::cerr << "FAIL: Stage 6 does not change the Stage 5 latency contract.\n";
        return EXIT_FAILURE;
    }

#if defined(MORPHOSIS_STAGE6_EXPECT_ZERO)
    if (tailSeconds != 0.0)
    {
        std::cerr << "FAIL: pre-edit source no longer reproduces the zero-tail claim.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Pre-edit tail claim reproduced: 0 seconds.\n";
#else
    if (! std::isinf (tailSeconds) || tailSeconds <= 0.0)
    {
        std::cerr << "FAIL: current processor must report positive infinity for its recursive IIR tail.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Processor reports positive infinite tail; latency remains "
              << processor.getLatencySamples() << " samples.\n";
#endif

    return EXIT_SUCCESS;
}
