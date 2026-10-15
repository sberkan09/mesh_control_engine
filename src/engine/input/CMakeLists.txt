# mce_input (L2): tick-quantized device sampling + the action mapping layer (audio-input-ui §2.1,
# §3.1–3.2). Gameplay reads mce::input::InputState; it never sees a device or an SDL event.
mce_add_library(mce_input
    SOURCES
        src/input_map.cpp
        src/input_state.cpp
        src/input_system.cpp
        src/replay.cpp
        src/script.cpp
        src/default_map.cpp
        src/ecs_bridge.cpp
        src/bindings.cpp
        src/binding_names.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_platform
        mce::mce_ecs
        mce::mce_refl)
