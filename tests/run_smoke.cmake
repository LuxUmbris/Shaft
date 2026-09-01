if(NOT DEFINED SHAFTC OR NOT DEFINED SMOKE_SOURCE OR NOT DEFINED CLASS_INDEX_INIT_SOURCE OR NOT DEFINED INVALID_CLASS_INDEX_SOURCE OR
   NOT DEFINED ORDERED_USING_MACRO_SOURCE OR NOT DEFINED FORWARD_USING_MACRO_SOURCE OR NOT DEFINED DUPLICATE_USING_MACRO_SOURCE OR
   NOT DEFINED RECURSIVE_USING_MACRO_SOURCE OR NOT DEFINED INVALID_STRING_USING_MACRO_SOURCE OR NOT DEFINED NESTED_TEMPLATE_SOURCE OR
   NOT DEFINED MALFORMED_NESTED_TEMPLATE_SOURCE OR NOT DEFINED QUALIFIED_TYPES_SOURCE OR
   NOT DEFINED STD_HASH_COLLECTIONS_SOURCE OR NOT DEFINED STD_ENTRY_SOURCE OR NOT DEFINED STDLIB_SOURCE OR NOT DEFINED UNKNOWN_CALL_SOURCE OR
   NOT DEFINED DUPLICATE_DEFINITION_SOURCE OR NOT DEFINED LLVM_AS OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "The shaftc smoke-test inputs are incomplete")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
file(READ "${STDLIB_SOURCE}" stdlib_source)
string(FIND "${stdlib_source}" "cdef __shaft_entry(i32 argc, **i8 argv) -> i32" std_entry_offset)
if(std_entry_offset EQUAL -1)
    message(FATAL_ERROR "std/std.shaft does not define the __shaft_entry ABI wrapper")
endif()
set(IR_FILE "${WORK_DIR}/smoke.ll")
set(BITCODE_FILE "${WORK_DIR}/smoke.bc")
set(EXECUTABLE_FILE "${WORK_DIR}/smoke")
set(OBJECT_FILE "${WORK_DIR}/smoke.o")
set(ASSEMBLY_FILE "${WORK_DIR}/smoke.s")
set(STATIC_LIBRARY_FILE "${WORK_DIR}/smoke.a")
set(DYNAMIC_LIBRARY_FILE "${WORK_DIR}/smoke.so")
set(CLASS_INDEX_INIT_FILE "${WORK_DIR}/class-index-init")
set(ORDERED_USING_MACRO_FILE "${WORK_DIR}/ordered-using-macro")
set(NESTED_TEMPLATE_FILE "${WORK_DIR}/nested-template")
set(QUALIFIED_TYPES_FILE "${WORK_DIR}/qualified-types")
set(STD_HASH_COLLECTIONS_FILE "${WORK_DIR}/std-hash-collections")
set(STD_ENTRY_FILE "${WORK_DIR}/std-entry")

execute_process(
    COMMAND "${SHAFTC}" --emit llvm -o "${IR_FILE}" "${SMOKE_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed (${result}):\n${output}${error}")
endif()

execute_process(
    COMMAND "${LLVM_AS}" "${IR_FILE}" -o "${BITCODE_FILE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "llvm-as rejected generated IR (${result}):\n${output}${error}")
endif()

execute_process(
    COMMAND "${SHAFTC}" -o "${EXECUTABLE_FILE}" "${SMOKE_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed to emit the freestanding binary (${result}):\n${output}${error}")
endif()

execute_process(COMMAND "${EXECUTABLE_FILE}" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "Smoke executable returned ${result}; expected 42")
endif()

execute_process(
    COMMAND "${SHAFTC}" -o "${CLASS_INDEX_INIT_FILE}" "${CLASS_INDEX_INIT_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed to compile class index/init source (${result}):\n${output}${error}")
endif()
execute_process(COMMAND "${CLASS_INDEX_INIT_FILE}" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "Class index/init executable returned ${result}; expected 42")
endif()

execute_process(
    COMMAND "${SHAFTC}" -o "${ORDERED_USING_MACRO_FILE}" "${ORDERED_USING_MACRO_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed to compile ordered using macro source (${result}):\n${output}${error}")
endif()
execute_process(COMMAND "${ORDERED_USING_MACRO_FILE}" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "Ordered using macro executable returned ${result}; expected 42")
endif()

execute_process(
    COMMAND "${SHAFTC}" --emit llvm -o "${WORK_DIR}/forward-using-macro.ll" "${FORWARD_USING_MACRO_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "A forward using macro compiled successfully")
endif()
string(FIND "${error}" "unknown function 'later_answer'" error_offset)
if(error_offset EQUAL -1)
    message(FATAL_ERROR "Unexpected forward using macro diagnostic (${result}):\n${output}${error}")
endif()

foreach(case duplicate-using-macro recursive-using-macro invalid-string-using-macro)
    if(case STREQUAL "duplicate-using-macro")
        set(source "${DUPLICATE_USING_MACRO_SOURCE}")
        set(expected "duplicate using macro alias")
    elseif(case STREQUAL "recursive-using-macro")
        set(source "${RECURSIVE_USING_MACRO_SOURCE}")
        set(expected "recursive using macro alias")
    else()
        set(source "${INVALID_STRING_USING_MACRO_SOURCE}")
        set(expected "string using macro aliases must end with '!'")
    endif()
    execute_process(
        COMMAND "${SHAFTC}" --emit llvm -o "${WORK_DIR}/${case}.ll" "${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR "Unsafe using macro case '${case}' compiled successfully")
    endif()
    string(FIND "${error}" "${expected}" error_offset)
    if(error_offset EQUAL -1)
        message(FATAL_ERROR "Unexpected ${case} diagnostic (${result}):\n${output}${error}")
    endif()
endforeach()

execute_process(
    COMMAND "${SHAFTC}" -o "${NESTED_TEMPLATE_FILE}" "${NESTED_TEMPLATE_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed to compile nested template source (${result}):\n${output}${error}")
endif()
execute_process(COMMAND "${NESTED_TEMPLATE_FILE}" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "Nested template executable returned ${result}; expected 42")
endif()

execute_process(
    COMMAND "${SHAFTC}" --emit llvm -o "${WORK_DIR}/malformed-nested-template.ll" "${MALFORMED_NESTED_TEMPLATE_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "A malformed nested template compiled successfully")
endif()
string(FIND "${error}" "Error at line" error_offset)
if(error_offset EQUAL -1)
    message(FATAL_ERROR "Unexpected malformed nested template diagnostic (${result}):\n${output}${error}")
endif()

execute_process(
    COMMAND "${SHAFTC}" -o "${QUALIFIED_TYPES_FILE}" "${QUALIFIED_TYPES_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed to compile qualified types source (${result}):\n${output}${error}")
endif()
execute_process(COMMAND "${QUALIFIED_TYPES_FILE}" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "Qualified types executable returned ${result}; expected 42")
endif()

execute_process(
    COMMAND "${SHAFTC}" -o "${STD_HASH_COLLECTIONS_FILE}" "${STD_HASH_COLLECTIONS_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed to compile standard hash collections source (${result}):\n${output}${error}")
endif()
execute_process(COMMAND "${STD_HASH_COLLECTIONS_FILE}" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "Standard hash collections executable returned ${result}; expected 42")
endif()

execute_process(
    COMMAND "${SHAFTC}" -o "${STD_ENTRY_FILE}" "${STD_ENTRY_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shaftc failed to compile the std entry wrapper regression (${result}):\n${output}${error}")
endif()
execute_process(COMMAND "${STD_ENTRY_FILE}" 12345678901 RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The std entry wrapper executable returned ${result}; expected 0")
endif()

execute_process(
    COMMAND "${SHAFTC}" --emit llvm -o "${WORK_DIR}/invalid-class-index.ll" "${INVALID_CLASS_INDEX_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "A scalar class index backing field compiled successfully")
endif()
string(FIND "${error}" "must be a pointer or runtime-sized array" error_offset)
if(error_offset EQUAL -1)
    message(FATAL_ERROR "Unexpected invalid class index diagnostic (${result}):\n${output}${error}")
endif()

foreach(kind object asm staticlib dynamiclib)
    if(kind STREQUAL "object")
        set(output_file "${OBJECT_FILE}")
    elseif(kind STREQUAL "asm")
        set(output_file "${ASSEMBLY_FILE}")
    elseif(kind STREQUAL "staticlib")
        set(output_file "${STATIC_LIBRARY_FILE}")
    else()
        set(output_file "${DYNAMIC_LIBRARY_FILE}")
    endif()
    execute_process(
        COMMAND "${SHAFTC}" --emit "${kind}" -o "${output_file}" "${SMOKE_SOURCE}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0 OR NOT EXISTS "${output_file}")
        message(FATAL_ERROR "shaftc failed to emit ${kind} (${result}):\n${output}${error}")
    endif()
endforeach()

execute_process(
    COMMAND "${SHAFTC}" --emit llvm -o "${WORK_DIR}/unknown-call.ll" "${UNKNOWN_CALL_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "An unresolved function call compiled successfully")
endif()
string(FIND "${error}" "Unknown function 'missing'" error_offset)
if(error_offset EQUAL -1)
    message(FATAL_ERROR "Unexpected unresolved-call diagnostic (${result}):\n${output}${error}")
endif()

execute_process(
    COMMAND "${SHAFTC}" --emit llvm -o "${WORK_DIR}/duplicate-definition.ll" "${DUPLICATE_DEFINITION_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "A duplicate C function definition compiled successfully")
endif()
string(FIND "${error}" "duplicate C function definition" error_offset)
if(error_offset EQUAL -1)
    message(FATAL_ERROR "Unexpected duplicate-definition diagnostic (${result}):\n${output}${error}")
endif()
