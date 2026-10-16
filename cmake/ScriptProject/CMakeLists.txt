# One script project (M-SCRIPT step 5): the engine's top-level CMakeLists.txt adds this directory with
# MCE_SCRIPT_PROJECT_DIR set, so its binary directory is projects/<name> like any project's.
mce_add_script_project("${MCE_SCRIPT_PROJECT_DIR}")
