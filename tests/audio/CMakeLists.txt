mce_add_test(test_audio
    SOURCES
        test_mixer.cpp
        test_stream.cpp
        test_device.cpp
    DEPS
        mce::mce_audio)
