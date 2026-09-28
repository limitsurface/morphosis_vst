# Provenance and Scope

This repository contains the buildable MORPHOSIS VST3, not the research
workspace. The embedded 289-record cube table in `src/CubeData.h` was
derived from the Morpheus cube update `cubes_v1.01vc_170120.wav`; DSP
behavior was studied against `vulcan_v1.00v_170111.wav` and audio/video
references. Original update WAVs and third-party demo media are not
distributed here.

The original research and historical stage reports remain in the separate
decomp workspace. Paths in those reports are not build dependencies. The
current DSP preserves the recovered filter structure and adds host-rate
adaptation, modulation smoothing, sequencing, X-Y interpolation choices, and
output soft clipping. Hardware-exact distortion, gain calibration, and
unavailable user-program records are not claimed.

The JUCE copy is release 7.0.12, upstream commit
`4f43011b96eb0636104cb3e433894cda98243626`. The portrait concept in
`docs/design/` is a design reference; the functional editor is native JUCE
code in `src/PluginEditor.cpp`.
