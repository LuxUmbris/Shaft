if(NOT DEFINED SHAFT_CLANG OR NOT DEFINED RUNTIME_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Runtime cross-compilation test inputs are incomplete")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(targets
    "x86_64-unknown-linux-gnu|linux.c|linux-x86_64.o"
    "aarch64-unknown-linux-gnu|linux.c|linux-arm64.o"
    "x86_64-apple-darwin|darwin.c|darwin-x86_64.o"
    "aarch64-apple-darwin|darwin.c|darwin-arm64.o"
    "x86_64-apple-darwin|macos.c|macos-x86_64.o"
    "aarch64-apple-darwin|macos.c|macos-arm64.o"
    "x86_64-w64-windows-gnu|windows.c|windows-x86_64.o"
    "aarch64-w64-windows-gnu|windows.c|windows-arm64.o")

foreach(entry IN LISTS targets)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 target)
    list(GET fields 1 source)
    list(GET fields 2 output)
    execute_process(
        COMMAND "${SHAFT_CLANG}" "--target=${target}" -ffreestanding -fno-stack-protector -Werror
                -c "${RUNTIME_DIR}/${source}" -o "${WORK_DIR}/${output}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error)
    if(NOT result EQUAL 0 OR NOT EXISTS "${WORK_DIR}/${output}")
        message(FATAL_ERROR "Failed to compile ${source} for ${target} (${result}):\n${command_output}${command_error}")
    endif()
endforeach()
