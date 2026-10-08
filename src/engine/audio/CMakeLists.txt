# mce_audio (L2): 48 kHz float32 mixer on the device callback, SPSC command FIFO, voice pool with
# priority stealing + virtual voices, streamed music ring, AudioSource ECS bridge
# (audio-input-ui §2.3–2.4, §3.3–3.6). The device itself comes from mce_platform's AudioOutput:
# no SDL symbol in this module.
mce_add_library(mce_audio
    SOURCES
        src/audio_device.cpp
        src/mixer.cpp
        src/stream.cpp
        src/bridge.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_platform
        mce::mce_ecs
        mce::mce_refl)
