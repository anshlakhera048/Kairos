cmake_minimum_required(VERSION 3.24)

pybind11_add_module(kairos_py bindings.cpp)
target_link_libraries(kairos_py PRIVATE kairos::kairos kairos_core)
# Our strict warnings apply to our code; pybind11 headers are system-like.
target_compile_options(kairos_py PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

# Place the built module next to the Python package for easy import.
set_target_properties(kairos_py PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/kairos
)

# Exercise the bindings in CI, not just build them: generate a deterministic
# synthetic capture and run the full binding smoke test (quote parity,
# simulator run, A-S fills) against it. The capture lands in the build
# tree, never the source tree.
#
# Skipped under sanitizers: an ASan/TSan-instrumented .so dlopen'ed by a
# non-instrumented python3 needs LD_PRELOAD juggling. The sanitizer presets
# still compile and link the module, which is the coverage that matters
# there.
if(KAIROS_BUILD_PYTHON AND NOT CMAKE_CXX_FLAGS MATCHES "sanitize")
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME python_bindings
            COMMAND ${CMAKE_COMMAND}
                -D PYTHON=${Python3_EXECUTABLE}
                -D SYNTH_SCRIPT=${CMAKE_SOURCE_DIR}/tools/recorder/synth_market.py
                -D BINDINGS_TEST=${CMAKE_CURRENT_SOURCE_DIR}/test_bindings.py
                -D SRC_DIR=${CMAKE_SOURCE_DIR}
                -D OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_bindings_test.cmake)
    endif()
endif()
