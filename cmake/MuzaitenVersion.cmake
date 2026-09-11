find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/version.py" --json
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_json
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "Could not determine the application version")
endif()
string(JSON MUZAITEN_PROJECT_VERSION GET "${version_json}" project_version)
string(JSON MUZAITEN_VERSION GET "${version_json}" version)
