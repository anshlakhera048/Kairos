# Driver for the python_bindings ctest (see python/CMakeLists.txt).
#
# Generates a deterministic synthetic capture, then runs the full binding
# smoke test against it: quote parity (Python vs C++ A-S), a simulator run,
# and an A-S strategy run that must produce fills.
#
# The capture goes to the build tree, never the source tree.

execute_process(
    COMMAND ${PYTHON} ${SYNTH_SCRIPT}
        --out ${OUT_DIR}/synth_bindings.kai
        --seconds 600
        --seed 42
    RESULT_VARIABLE gen_rc
    OUTPUT_VARIABLE gen_out
    ERROR_VARIABLE gen_err)
if(NOT gen_rc EQUAL 0)
    message(FATAL_ERROR "synth_market.py failed (${gen_rc}):\n${gen_err}")
endif()

# test_bindings.py resolves the package via a relative "python" dir, so it
# must run with the repo root as the working directory.
execute_process(
    COMMAND ${PYTHON} ${BINDINGS_TEST} ${OUT_DIR}/synth_bindings.kai
    WORKING_DIRECTORY ${SRC_DIR}
    RESULT_VARIABLE test_rc
    OUTPUT_VARIABLE test_out
    ERROR_VARIABLE test_err)
if(NOT test_rc EQUAL 0)
    message(FATAL_ERROR "test_bindings.py failed (${test_rc}):\n${test_out}\n${test_err}")
endif()
message(STATUS "python bindings: OK\n${test_out}")
