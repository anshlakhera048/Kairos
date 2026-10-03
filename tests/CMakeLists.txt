include(GoogleTest)

# Unit tests. Differential (vs. naive reference) and randomized/property tests
# live in tests/differential and tests/randomized from Phase 1 on.
add_executable(kairos_tests
    unit/test_types.cpp
    unit/test_orderbook.cpp
    unit/test_histogram.cpp
    unit/test_replay.cpp
    unit/test_latency.cpp
    unit/test_fill_model.cpp
    unit/test_sim.cpp
    unit/test_simulator.cpp
    unit/test_as_model.cpp
    unit/test_as_strategy.cpp
)
target_include_directories(kairos_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bench
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_compile_definitions(kairos_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Differential test: engine vs naive reference over random op streams.
add_executable(kairos_diff_test
    differential/diff_test.cpp
    differential/reference.cpp
)
target_link_libraries(kairos_diff_test PRIVATE kairos_core)
target_compile_options(kairos_diff_test PRIVATE ${KAIROS_WARNINGS})

# Randomized/property tests.
add_executable(kairos_prop_test
    randomized/test_properties.cpp
)
target_link_libraries(kairos_prop_test PRIVATE kairos_core GTest::gtest_main)
target_compile_options(kairos_prop_test PRIVATE ${KAIROS_WARNINGS})
add_test(NAME Properties COMMAND kairos_prop_test)
target_link_libraries(kairos_tests PRIVATE
    kairos::kairos
    kairos_core
    GTest::gtest_main
)

gtest_discover_tests(kairos_tests)
