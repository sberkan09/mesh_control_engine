# M-IMPORT: the 2D authoring-tool converters (tools/mce_cli/mce_import.py).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME import_converters
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_import.py" "${CMAKE_CURRENT_BINARY_DIR}/import_test")
