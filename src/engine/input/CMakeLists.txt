# mce_input (L2): tick-quantized input sampling (audio-input-ui.md §3.2, FR-INP-030).
# M1 skeleton: fixed keyboard/mouse bindings into the InputState fast-path fields. Cooked action
# maps, name lookup, rebinding and record/replay arrive at M3.
mce_add_library(mce_input
    SOURCES
        src/input.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_platform)
