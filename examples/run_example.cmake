# runs one example script and compares its stdout with the golden file. stderr (the
# "[runtime] loaded plugin" banners, diagnostics) is deliberately not compared.
#   cmake -DEMBR=... -DBACKEND=vm -DSCRIPT=... -DEXPECTED=... -P run_example.cmake
execute_process(
    COMMAND ${EMBR} --backend=${BACKEND} ${SCRIPT}
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE  err
    RESULT_VARIABLE rc
    TIMEOUT 50)
file(READ "${EXPECTED}" expected)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "embr exited with ${rc}\n--- stderr ---\n${err}")
endif()
if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "output differs from ${EXPECTED}\n--- expected ---\n${expected}--- actual ---\n${actual}--- stderr ---\n${err}")
endif()
