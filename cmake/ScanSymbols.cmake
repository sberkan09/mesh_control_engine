# cmake -DNM=<nm> -DFILES="<a>;<b>" -DPATTERN=<regex> -P ScanSymbols.cmake
# Fails when any file defines or imports a symbol matching PATTERN (FR-UI-048: no ImGui symbol in
# the engine library or a shipped executable; the runtime UI is mce_ui).
foreach(file IN LISTS FILES)
    execute_process(COMMAND ${NM} -C ${file} OUTPUT_VARIABLE syms RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "nm failed on ${file}")
    endif()
    string(REGEX MATCHALL "[^\n]*${PATTERN}[^\n]*" hits "${syms}")
    list(LENGTH hits n)
    if(n GREATER 0)
        list(GET hits 0 first)
        message(FATAL_ERROR "${file}: ${n} symbol(s) match '${PATTERN}', e.g. ${first}")
    endif()
    message(STATUS "${file}: no '${PATTERN}' symbols")
endforeach()
