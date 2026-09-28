cmake_minimum_required(VERSION 3.22)

project(Morphosis VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE" CACHE PATH
    "Pinned JUCE 7.0.12 source directory")
set(MORPHOSIS_STAGE4_BASELINE_DIR "" CACHE PATH
    "Optional preserved pre-Stage-4 source for the deterministic timing regression")
set(MORPHOSIS_STAGE5_BASELINE_DIR "" CACHE PATH
    "Optional preserved pre-Stage-5 source for the latency contract regression")
set(MORPHOSIS_STAGE6_BASELINE_DIR "" CACHE PATH
    "Optional preserved pre-Stage-6 source for the tail-policy regression")

if (NOT IS_DIRECTORY "${JUCE_SOURCE_DIR}" OR NOT EXISTS "${JUCE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE_SOURCE_DIR must point to a JUCE source checkout. "
        "The vendored copy is expected at third_party/JUCE.")
endif()

# Keep plugin output in this build tree. JUCE's copy step is deliberately disabled.
set(JUCE_COPY_PLUGIN_AFTER_BUILD OFF CACHE BOOL
    "Do not copy plugins to a system folder" FORCE)

add_subdirectory("${JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_juce" EXCLUDE_FROM_ALL)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src")

juce_add_binary_data(MorphosisAssets
    HEADER_NAME MorphosisAssets.h
    NAMESPACE MorphosisAssets
    SOURCES
        assets/fonts/IBMPlexMono-Regular.ttf
        assets/fonts/IBMPlexMono-SemiBold.ttf)

juce_add_plugin(Morphosis
    VERSION ${PROJECT_VERSION}
    PRODUCT_NAME "MORPHOSIS"
    PLUGIN_NAME "MORPHOSIS"
    COMPANY_NAME "Morphosis Inc"
    BUNDLE_ID "com.MorphosisInc.Morphosis"
    DESCRIPTION "MORPHOSIS seven-stage recovered filter"
    PLUGIN_MANUFACTURER_CODE Temu
    PLUGIN_CODE Mphs
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Morphosis
    PRIVATE
        src/PluginEditor.cpp
        src/PluginProcessor.cpp
        src/MorphosisDSP.cpp
        src/PluginEditor.h
        src/PluginProcessor.h
        src/MorphosisSequencer.h
        src/MorphosisUIUtilities.h
        src/MorphosisDSP.h
        src/CubeData.h
        src/PresetRouting.h
        src/PresetTaxonomy.h
        src/PresetPickerModel.h)

target_compile_features(Morphosis PRIVATE cxx_std_17)

target_compile_definitions(Morphosis
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Morphosis
    PRIVATE
        juce::juce_audio_utils
        MorphosisAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

include(CTest)

if (BUILD_TESTING)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/reference.json"
        "${CMAKE_BINARY_DIR}/MorphosisReference.json"
        COPYONLY)

    add_executable(morphosis_ui_behavior_test
        tests/UIBehaviorTest.cpp)
    target_compile_features(morphosis_ui_behavior_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_ui_behavior_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME Morphosis.UIBehavior COMMAND morphosis_ui_behavior_test)

    add_executable(morphosis_smoke_test
        tests/MorphosisSmokeTest.cpp)

    target_compile_features(morphosis_smoke_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_smoke_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(morphosis_smoke_test
        PRIVATE
            Morphosis
            MorphosisAssets
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME Morphosis.Smoke COMMAND morphosis_smoke_test)
    set_tests_properties(Morphosis.Smoke PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_coherent_modulation_test
        tests/MorphosisCoherentModulationTest.cpp
        src/MorphosisDSP.cpp
        src/MorphosisDSP.h
        src/CubeData.h)

    target_compile_features(morphosis_coherent_modulation_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_coherent_modulation_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_coherent_modulation_test
        PRIVATE MORPHOSIS_COHERENT_MODULATION_TEST=1)

    add_test(NAME Morphosis.CoherentModulation
        COMMAND morphosis_coherent_modulation_test)
    set_tests_properties(Morphosis.CoherentModulation PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_seq_xy_technical_fix_test
        tests/SeqXYTechnicalFixTest.cpp
        src/MorphosisDSP.cpp
        src/MorphosisDSP.h
        src/MorphosisSequencer.h
        src/CubeData.h)

    target_compile_features(morphosis_seq_xy_technical_fix_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_seq_xy_technical_fix_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_seq_xy_technical_fix_test
        PRIVATE MORPHOSIS_TESTING=1)

    add_test(NAME Morphosis.SeqXYTechnicalFix
        COMMAND morphosis_seq_xy_technical_fix_test)
    set_tests_properties(Morphosis.SeqXYTechnicalFix PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_graph_snapshot_test
        tests/GraphSnapshotTest.cpp)

    target_compile_features(morphosis_graph_snapshot_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_graph_snapshot_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(morphosis_graph_snapshot_test
        PRIVATE
            Morphosis
            MorphosisAssets
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME Morphosis.GraphSnapshot
        COMMAND morphosis_graph_snapshot_test)
    set_tests_properties(Morphosis.GraphSnapshot PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_sequencer_response_candidate_test
        tests/SequencerResponseCandidateTest.cpp)

    target_compile_features(morphosis_sequencer_response_candidate_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_sequencer_response_candidate_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_sequencer_response_candidate_test
        PRIVATE MORPHOSIS_TESTING=1)
    target_link_libraries(morphosis_sequencer_response_candidate_test
        PRIVATE
            Morphosis
            MorphosisAssets
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME Morphosis.SequencerResponseCandidate
        COMMAND morphosis_sequencer_response_candidate_test)
    set_tests_properties(Morphosis.SequencerResponseCandidate PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_preset_features_test
        tests/PresetFeaturesTest.cpp)

    target_compile_features(morphosis_preset_features_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_preset_features_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(morphosis_preset_features_test
        PRIVATE
            Morphosis
            MorphosisAssets
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME Morphosis.PresetFeatures
        COMMAND morphosis_preset_features_test)
    set_tests_properties(Morphosis.PresetFeatures PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_plugin_lifecycle_test
        tests/PluginLifecycleTest.cpp)

    target_compile_features(morphosis_plugin_lifecycle_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_plugin_lifecycle_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(morphosis_plugin_lifecycle_test
        PRIVATE
            Morphosis
            MorphosisAssets
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME Morphosis.PluginLifecycle
        COMMAND morphosis_plugin_lifecycle_test)
    set_tests_properties(Morphosis.PluginLifecycle PROPERTIES
        TIMEOUT 20
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_test(NAME Morphosis.PresetPickerUI
        COMMAND morphosis_plugin_lifecycle_test --picker-only)
    set_tests_properties(Morphosis.PresetPickerUI PROPERTIES
        TIMEOUT 20
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_test(NAME Morphosis.PortraitEditor
        COMMAND morphosis_plugin_lifecycle_test --portrait-only)
    set_tests_properties(Morphosis.PortraitEditor PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_encoded_domain_xy_test
        tests/EncodedDomainXYTest.cpp
        src/MorphosisDSP.cpp
        src/MorphosisDSP.h
        src/CubeData.h)

    target_compile_features(morphosis_encoded_domain_xy_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_encoded_domain_xy_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_encoded_domain_xy_test
        PRIVATE MORPHOSIS_TESTING=1)

    add_test(NAME Morphosis.EncodedDomainXY
        COMMAND morphosis_encoded_domain_xy_test)
    set_tests_properties(Morphosis.EncodedDomainXY PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_fir_scheduling_performance_test
        tests/FirSchedulingPerformanceTest.cpp)

    target_compile_features(morphosis_fir_scheduling_performance_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_fir_scheduling_performance_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Morphosis.FirSchedulingPerformance
        COMMAND morphosis_fir_scheduling_performance_test)
    set_tests_properties(Morphosis.FirSchedulingPerformance PROPERTIES
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage2_regression_test
        tests/DspStage2RegressionTest.cpp)

    target_compile_features(morphosis_dsp_stage2_regression_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_dsp_stage2_regression_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Morphosis.DspStage2Regression
        COMMAND morphosis_dsp_stage2_regression_test)
    set_tests_properties(Morphosis.DspStage2Regression PROPERTIES
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage3_regression_test
        tests/DspStage3RegressionTest.cpp)

    target_compile_features(morphosis_dsp_stage3_regression_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_dsp_stage3_regression_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME Morphosis.DspStage3Regression
        COMMAND morphosis_dsp_stage3_regression_test)
    set_tests_properties(Morphosis.DspStage3Regression PROPERTIES
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage4_regression_test
        tests/DspStage4RegressionTest.cpp)

    target_compile_features(morphosis_dsp_stage4_regression_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_dsp_stage4_regression_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_dsp_stage4_regression_test
        PRIVATE MORPHOSIS_TESTING=1)

    add_test(NAME Morphosis.DspStage4Regression
        COMMAND morphosis_dsp_stage4_regression_test)
    set_tests_properties(Morphosis.DspStage4Regression PROPERTIES
        TIMEOUT 600
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage5_regression_test
        tests/DspStage5RegressionTest.cpp
        src/MorphosisDSP.cpp
        src/MorphosisDSP.h
        src/CubeData.h)
    target_compile_features(morphosis_dsp_stage5_regression_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_dsp_stage5_regression_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_dsp_stage5_regression_test
        PRIVATE MORPHOSIS_TESTING=1)
    add_test(NAME Morphosis.DspStage5Regression
        COMMAND morphosis_dsp_stage5_regression_test)
    set_tests_properties(Morphosis.DspStage5Regression PROPERTIES
        TIMEOUT 300
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage6_tail_characterization
        tests/DspStage6TailCharacterization.cpp
        src/MorphosisDSP.h
        src/CubeData.h
        src/PresetRouting.h)
    target_compile_features(morphosis_dsp_stage6_tail_characterization PRIVATE cxx_std_17)
    target_include_directories(morphosis_dsp_stage6_tail_characterization
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_dsp_stage6_tail_characterization
        PRIVATE MORPHOSIS_TESTING=1)
    add_test(NAME Morphosis.DspStage6TailCharacterization
        COMMAND morphosis_dsp_stage6_tail_characterization)
    set_tests_properties(Morphosis.DspStage6TailCharacterization PROPERTIES
        TIMEOUT 600
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage6_tail_policy_test
        tests/DspStage6TailPolicyTest.cpp)
    target_compile_features(morphosis_dsp_stage6_tail_policy_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_dsp_stage6_tail_policy_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(morphosis_dsp_stage6_tail_policy_test
        PRIVATE
            Morphosis
            MorphosisAssets
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_test(NAME Morphosis.DspStage6TailPolicy
        COMMAND morphosis_dsp_stage6_tail_policy_test)
    set_tests_properties(Morphosis.DspStage6TailPolicy PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage6_vst3_tail_wrapper_test
        tests/DspStage6Vst3TailWrapperTest.cpp)
    target_compile_features(morphosis_dsp_stage6_vst3_tail_wrapper_test PRIVATE cxx_std_17)
    target_compile_definitions(morphosis_dsp_stage6_vst3_tail_wrapper_test
        PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(morphosis_dsp_stage6_vst3_tail_wrapper_test
        PRIVATE
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_dependencies(morphosis_dsp_stage6_vst3_tail_wrapper_test Morphosis_VST3)
    add_test(NAME Morphosis.DspStage6Vst3TailWrapper
        COMMAND morphosis_dsp_stage6_vst3_tail_wrapper_test "$<TARGET_FILE:Morphosis_VST3>")
    set_tests_properties(Morphosis.DspStage6Vst3TailWrapper PROPERTIES
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_dsp_stage8_regression_test
        tests/DspStage8RegressionTest.cpp
        src/MorphosisDSP.cpp
        src/MorphosisDSP.h
        src/CubeData.h)
    target_compile_features(morphosis_dsp_stage8_regression_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_dsp_stage8_regression_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(morphosis_dsp_stage8_regression_test
        PRIVATE MORPHOSIS_STAGE8_PROFILE=1)
    add_test(NAME Morphosis.DspStage8Regression
        COMMAND morphosis_dsp_stage8_regression_test)
    set_tests_properties(Morphosis.DspStage8Regression PROPERTIES
        TIMEOUT 600
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(morphosis_plugin_latency_bypass_test
        tests/PluginLatencyBypassTest.cpp)
    target_compile_features(morphosis_plugin_latency_bypass_test PRIVATE cxx_std_17)
    target_include_directories(morphosis_plugin_latency_bypass_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(morphosis_plugin_latency_bypass_test
        PRIVATE
            Morphosis
            MorphosisAssets
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_test(NAME Morphosis.PluginLatencyBypass
        COMMAND morphosis_plugin_latency_bypass_test)
    set_tests_properties(Morphosis.PluginLatencyBypass PROPERTIES
        TIMEOUT 300
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    if (MORPHOSIS_STAGE4_BASELINE_DIR)
        foreach (baseline_file MorphosisDSP.cpp MorphosisDSP.h)
            if (NOT EXISTS "${MORPHOSIS_STAGE4_BASELINE_DIR}/${baseline_file}")
                message(FATAL_ERROR
                    "MORPHOSIS_STAGE4_BASELINE_DIR is missing ${baseline_file}")
            endif ()
        endforeach ()

        add_executable(morphosis_dsp_stage4_preedit_reproduction
            tests/DspStage4RegressionTest.cpp)
        target_compile_features(morphosis_dsp_stage4_preedit_reproduction PRIVATE cxx_std_17)
        target_include_directories(morphosis_dsp_stage4_preedit_reproduction
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(morphosis_dsp_stage4_preedit_reproduction
            PRIVATE
                MORPHOSIS_STAGE4_PREEDIT_ONLY=1
                MORPHOSIS_STAGE4_DSP_IMPLEMENTATION="${MORPHOSIS_STAGE4_BASELINE_DIR}/MorphosisDSP.cpp")
        add_test(NAME Morphosis.DspStage4PreEditReproduction
            COMMAND morphosis_dsp_stage4_preedit_reproduction)
        set_tests_properties(Morphosis.DspStage4PreEditReproduction PROPERTIES
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif ()

    if (MORPHOSIS_STAGE5_BASELINE_DIR)
        foreach (baseline_file MorphosisDSP.cpp MorphosisDSP.h
                               PluginProcessor.cpp PluginProcessor.h
                               PluginEditor.cpp PluginEditor.h)
            if (NOT EXISTS "${MORPHOSIS_STAGE5_BASELINE_DIR}/${baseline_file}")
                message(FATAL_ERROR
                    "MORPHOSIS_STAGE5_BASELINE_DIR is missing ${baseline_file}")
            endif ()
        endforeach ()

        add_executable(morphosis_dsp_stage5_preedit_reproduction
            tests/DspStage5PreEditReproduction.cpp
            "${MORPHOSIS_STAGE5_BASELINE_DIR}/MorphosisDSP.cpp"
            "${MORPHOSIS_STAGE5_BASELINE_DIR}/PluginProcessor.cpp"
            "${MORPHOSIS_STAGE5_BASELINE_DIR}/PluginEditor.cpp")
        target_compile_features(morphosis_dsp_stage5_preedit_reproduction PRIVATE cxx_std_17)
        target_include_directories(morphosis_dsp_stage5_preedit_reproduction
            PRIVATE
                "${MORPHOSIS_STAGE5_BASELINE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(morphosis_dsp_stage5_preedit_reproduction
            PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
        target_link_libraries(morphosis_dsp_stage5_preedit_reproduction
            PRIVATE
                MorphosisAssets
                juce::juce_audio_utils
                juce::juce_recommended_config_flags
                juce::juce_recommended_warning_flags)
        add_test(NAME Morphosis.DspStage5PreEditReproduction
            COMMAND morphosis_dsp_stage5_preedit_reproduction)
        set_tests_properties(Morphosis.DspStage5PreEditReproduction PROPERTIES
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif ()

    if (MORPHOSIS_STAGE6_BASELINE_DIR)
        foreach (baseline_file MorphosisDSP.cpp MorphosisDSP.h
                               PluginProcessor.cpp PluginProcessor.h
                               PluginEditor.cpp PluginEditor.h)
            if (NOT EXISTS "${MORPHOSIS_STAGE6_BASELINE_DIR}/${baseline_file}")
                message(FATAL_ERROR
                    "MORPHOSIS_STAGE6_BASELINE_DIR is missing ${baseline_file}")
            endif ()
        endforeach ()

        add_executable(morphosis_dsp_stage6_preedit_tail_policy_reproduction
            tests/DspStage6TailPolicyTest.cpp
            "${MORPHOSIS_STAGE6_BASELINE_DIR}/MorphosisDSP.cpp"
            "${MORPHOSIS_STAGE6_BASELINE_DIR}/PluginProcessor.cpp"
            "${MORPHOSIS_STAGE6_BASELINE_DIR}/PluginEditor.cpp")
        target_compile_features(morphosis_dsp_stage6_preedit_tail_policy_reproduction PRIVATE cxx_std_17)
        target_include_directories(morphosis_dsp_stage6_preedit_tail_policy_reproduction
            PRIVATE
                "${MORPHOSIS_STAGE6_BASELINE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(morphosis_dsp_stage6_preedit_tail_policy_reproduction
            PRIVATE
                MORPHOSIS_STAGE6_EXPECT_ZERO=1
                JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
        target_link_libraries(morphosis_dsp_stage6_preedit_tail_policy_reproduction
            PRIVATE
                MorphosisAssets
                juce::juce_audio_utils
                juce::juce_recommended_config_flags
                juce::juce_recommended_warning_flags)
        add_test(NAME Morphosis.DspStage6PreEditTailPolicy
            COMMAND morphosis_dsp_stage6_preedit_tail_policy_reproduction)
        set_tests_properties(Morphosis.DspStage6PreEditTailPolicy PROPERTIES
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif ()

    # External chord validation is intentionally a manually-run executable:
    # it consumes the user's verified source WAV and is not a portable CTest.
    add_executable(morphosis_production_chord_validation
        tests/ProductionChordValidation.cpp
        src/MorphosisDSP.cpp
        src/MorphosisDSP.h
        src/CubeData.h)

    target_compile_features(morphosis_production_chord_validation PRIVATE cxx_std_17)
    target_include_directories(morphosis_production_chord_validation
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
