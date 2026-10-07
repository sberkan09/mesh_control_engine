mce_add_executable(mce_devhost SOURCES main.cpp DEPS mce::mce_core)
add_test(NAME devhost_headless_10k COMMAND mce_devhost -ticks 10000)
