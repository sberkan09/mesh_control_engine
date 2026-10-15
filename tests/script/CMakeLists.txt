# mce_script spike (ADR-0039): sandbox, budgets, reflected field access and determinism.
mce_add_test(test_script
    SOURCES
        test_script.cpp
    DEPS
        mce::mce_script)
