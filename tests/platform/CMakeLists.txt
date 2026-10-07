# Runs on SDL's "dummy" video driver: no display needed, so this is a plain unit test everywhere.
mce_add_test(test_platform SOURCES test_platform.cpp DEPS mce::mce_platform)
