# mce_refl (L1): generic field/trait metadata registry, MCE_COMPONENT, property paths.
# The sole source of component field metadata (ARCHITECTURE F4 / ADR-0009). Depends on core only.

mce_add_library(mce_refl
    SOURCES
        src/property_path.cpp
        src/registry.cpp
    PUBLIC_DEPS
        mce::mce_core)
