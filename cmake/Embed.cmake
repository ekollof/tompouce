get_filename_component(directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${directory}")
execute_process(COMMAND "${XXD}" -i -n "${SYMBOL}" "${INPUT}"
    OUTPUT_FILE "${OUTPUT}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "Embedding ${INPUT} failed: ${status}")
endif()
