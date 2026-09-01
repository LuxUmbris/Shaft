cmake_minimum_required(VERSION 3.20)
project(ShaftLLVM VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

find_package(LLVM REQUIRED CONFIG)
find_program(SHAFT_CLANG NAMES clang REQUIRED)
find_program(SHAFT_LLVM_AR NAMES llvm-ar-18 llvm-ar REQUIRED)
find_program(SHAFT_LLD NAMES ld.lld lld ld.lld-${LLVM_VERSION_MAJOR} lld-${LLVM_VERSION_MAJOR})

add_executable(shaftc
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/checker.cpp
    src/codegen.cpp
    src/error.cpp
)

target_include_directories(shaftc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
target_compile_definitions(shaftc PRIVATE ${LLVM_DEFINITIONS}
    SHAFT_SOURCE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SHAFT_CLANG_PATH="${SHAFT_CLANG}"
    SHAFT_LLD_PATH="${SHAFT_LLD}"
    SHAFT_LLVM_AR_PATH="${SHAFT_LLVM_AR}"
    SHAFT_VERSION="alpha-0.1-bootstrap")
target_link_libraries(shaftc PRIVATE LLVM)

install(TARGETS shaftc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES std/std.shaft DESTINATION ${CMAKE_INSTALL_DATADIR}/shaft/std)
install(FILES
    std/runtime/linux.c
    std/runtime/darwin.c
    std/runtime/macos.c
    std/runtime/windows.c
    DESTINATION ${CMAKE_INSTALL_DATADIR}/shaft/std/runtime)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME shaft-installer
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_install.py)
    add_test(
        NAME shaftc-flags
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_compiler_flags.py)
    add_test(
        NAME shaftc-std-file-io
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_std_file_io.py)
    add_test(
        NAME shaftc-std-network
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_std_network.py)
    add_test(
        NAME shaftc-std-string-conversion
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_std_string_conversion.py)
    add_test(
        NAME shaftc-std-network-server
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_std_network_server.py)
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(SHAFT_LLVM_AS NAMES llvm-as-18 llvm-as REQUIRED)

    add_test(
        NAME shaftc-smoke
        COMMAND ${CMAKE_COMMAND}
            -DSHAFTC=$<TARGET_FILE:shaftc>
            -DSMOKE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.shaft
            -DCLASS_INDEX_INIT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/class-index-init.shaft
            -DINVALID_CLASS_INDEX_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid-class-index.shaft
            -DORDERED_USING_MACRO_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/ordered-using-macro.shaft
            -DFORWARD_USING_MACRO_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/forward-using-macro.shaft
            -DDUPLICATE_USING_MACRO_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/duplicate-using-macro.shaft
            -DRECURSIVE_USING_MACRO_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/recursive-using-macro.shaft
            -DINVALID_STRING_USING_MACRO_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid-string-using-macro.shaft
            -DNESTED_TEMPLATE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/nested-template.shaft
            -DMALFORMED_NESTED_TEMPLATE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/malformed-nested-template.shaft
            -DQUALIFIED_TYPES_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/qualified-types.shaft
            -DSTD_HASH_COLLECTIONS_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/std-hash-collections.shaft
            -DSTD_ENTRY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/std-entry.shaft
            -DSTDLIB_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/std/std.shaft
            -DUNKNOWN_CALL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/unknown-call.shaft
            -DDUPLICATE_DEFINITION_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/duplicate-definition.shaft
            -DLLVM_AS=${SHAFT_LLVM_AS}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/shaftc-smoke
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_smoke.cmake
    )

    add_test(
        NAME shaft-runtime-cross-compile
        COMMAND ${CMAKE_COMMAND}
            -DSHAFT_CLANG=${SHAFT_CLANG}
            -DRUNTIME_DIR=${CMAKE_CURRENT_SOURCE_DIR}/std/runtime
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/runtime-cross-compile
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_runtimes.cmake
    )
endif()
