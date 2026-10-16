mce_add_test(test_audio
    SOURCES
        test_mixer.cpp
        test_stream.cpp
        test_device.cpp
        test_clip_bank.cpp
        test_mixer_v05.cpp
        test_pitch_sets.cpp
        test_bus_effects.cpp
    DEPS
        mce::mce_audio)
target_compile_definitions(test_audio PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
