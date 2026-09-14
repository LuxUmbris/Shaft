if(NOT DEFINED SHAFTC OR NOT DEFINED RUNTIME_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Runtime cross-compilation test inputs are incomplete")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(targets
    "x86_64-unknown-linux-gnu|linux.shaft|linux-x86_64.o"
    "aarch64-unknown-linux-gnu|linux.shaft|linux-arm64.o"
    "x86_64-apple-darwin|darwin.shaft|darwin-x86_64.o"
    "aarch64-apple-darwin|darwin.shaft|darwin-arm64.o"
    "x86_64-apple-macos|macos.shaft|macos-x86_64.o"
    "aarch64-apple-macos|macos.shaft|macos-arm64.o"
    "x86_64-w64-windows-gnu|windows.shaft|windows-x86_64.o"
    "aarch64-w64-windows-gnu|windows.shaft|windows-arm64.o")

foreach(entry IN LISTS targets)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 target)
    list(GET fields 1 source)
    list(GET fields 2 output)
    execute_process(
        COMMAND "${SHAFTC}" --no-std --target "${target}" --emit object
                -o "${WORK_DIR}/${output}" "${RUNTIME_DIR}/${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error)
    if(NOT result EQUAL 0 OR NOT EXISTS "${WORK_DIR}/${output}")
        message(FATAL_ERROR "Failed to compile ${source} for ${target} (${result}):\n${command_output}${command_error}")
    endif()
endforeach()
