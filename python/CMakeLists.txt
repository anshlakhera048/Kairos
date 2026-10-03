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
