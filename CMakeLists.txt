cmake_minimum_required(VERSION 3.20)
project(ShaftLLVM VERSION 0.1.0 LANGUAGES C CXX)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.llvm-cache" CACHE PATH "Directory for FetachContent dependencies")
endif()

set(LLVM_BUILD_LLVM_DYLIB OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLVM_ENABLE_PROJECTS "lld" CACHE STRING "" FORCE)
set(LLVM_TARGETS_TO_BUILD "X86;AArch64;RISCV" CACHE STRING "" FORCE)
set(LLVM_ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(LLVM_ENABLE_EH ON CACHE BOOL "" FORCE)
set(LLVM_OPTIMIZED_TABLEGEN ON CACHE BOOL "" FORCE)

set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLVM_ENABLE_BINDINGS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    llvm-project
    URL https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/llvm-project-21.1.8.src.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR llvm
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(llvm-project)

find_program(SHAFT_CLANG NAMES clang REQUIRED)
find_program(SHAFT_LLVM_AR NAMES llvm-ar-18 llvm-ar REQUIRED)
find_package(Threads REQUIRED)

add_executable(shaftc
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/checker.cpp
    src/codegen.cpp
    src/error.cpp
    src/toolchain.cpp
    src/thirdparty/llvm.cpp
)

target_include_directories(shaftc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${llvm-project_SOURCE_DIR}/llvm/include
    ${llvm-project_BINARY_DIR}/include
    ${llvm-project_SOURCE_DIR}/lld/include
    ${llvm-project_BINARY_DIR}/tools/lld/include
)

target_compile_definitions(shaftc PRIVATE 
    SHAFT_SOURCE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SHAFT_CLANG_PATH="${SHAFT_CLANG}"
    SHAFT_LLVM_AR_PATH="${SHAFT_LLVM_AR}"
    SHAFT_VERSION="alpha-0.2-bootstrap"
)

target_link_libraries(shaftc PRIVATE
    lldELF
    lldCOFF
    lldMachO
    lldCommon
    lldMinGW
    lldWasm
    LLVMCore
    LLVMAnalysis
    LLVMBitReader
    LLVMBitWriter
    LLVMExecutionEngine
    LLVMPasses
    LLVMTarget
    LLVMMC
    LLVMSupport
    LLVMX86CodeGen
    LLVMX86AsmParser
    LLVMAArch64CodeGen
    LLVMAArch64AsmParser
    LLVMRISCVCodeGen
    LLVMRISCVAsmParser
    Threads::Threads
)

add_custom_target(shaftls ALL
    COMMAND $<TARGET_FILE:shaftc> --build Shaft.build
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/shaftls/build/shaftls
        ${CMAKE_CURRENT_BINARY_DIR}/shaftls
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaftls
    DEPENDS shaftc
        ${CMAKE_CURRENT_SOURCE_DIR}/shaftls/Shaft.build
        ${CMAKE_CURRENT_SOURCE_DIR}/shaftls/src/main.shaft
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/shaftls)

install(TARGETS shaftc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/shaftls DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES std/std.shaft DESTINATION ${CMAKE_INSTALL_DATADIR}/shaft/std)
install(FILES
    std/runtime/linux.shaft
    std/runtime/darwin.shaft
    std/runtime/windows.shaft
    DESTINATION ${CMAKE_INSTALL_DATADIR}/shaft/std/runtime)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SHAFT_TEST_ARGS
        --shaftc $<TARGET_FILE:shaftc>
        --build-dir ${CMAKE_CURRENT_BINARY_DIR})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(SHAFT_LLVM_AS NAMES llvm-as-18 llvm-as REQUIRED)
        list(APPEND SHAFT_TEST_ARGS --llvm-as ${SHAFT_LLVM_AS})
    endif()
    add_test(
        NAME shaft-all
        COMMAND ${CMAKE_COMMAND} -E env SHAFTC=$<TARGET_FILE:shaftc>
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test.py ${SHAFT_TEST_ARGS})
    set_tests_properties(shaft-all PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()