include(GoogleTest)

# Unit tests. Differential (vs. naive reference) and randomized/property tests
# live in tests/differential and tests/randomized from Phase 1 on.
add_executable(kairos_tests
    unit/test_types.cpp
)
target_link_libraries(kairos_tests PRIVATE
    kairos::kairos
    kairos_core
    GTest::gtest_main
)

gtest_discover_tests(kairos_tests)
