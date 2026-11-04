- Windows: a Luau script with a syntax error no longer crashes the cook, the editor or `mce_cook --pack`
  (M-PLATFORM-CI step 4, script group). `mce_script_compiler` now builds with exceptions and the STL's normal
  `std::exception` on MSVC, like Luau itself; the engine policy's `_HAS_EXCEPTIONS=0` gave Luau's
  `ParseError` two layouts in one binary.
