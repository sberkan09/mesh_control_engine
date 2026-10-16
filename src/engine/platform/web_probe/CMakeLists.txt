# The browser's input and audio paths (M-WEB step 4); scripts/web/input_smoke.cjs drives it.
mce_add_executable(mce_input_web_probe UMBRELLA SOURCES main.cpp DEPS mce::mce_platform)
