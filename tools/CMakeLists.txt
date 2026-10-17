add_executable(kairos_realism_gap
    realism_gap/experiment.cpp
)
target_link_libraries(kairos_realism_gap PRIVATE
    kairos::kairos
    kairos_core
)
target_include_directories(kairos_realism_gap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_executable(kairos_tournament
    arena/run_tournament.cpp
)
target_link_libraries(kairos_tournament PRIVATE
    kairos::kairos
    kairos_core
)
target_include_directories(kairos_tournament PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# The pip wheel ships this binary inside the kairos package so the `kairos`
# CLI can drive the arena without a source checkout. Developer builds are
# unaffected: install rules only run under `cmake --install` (wheel builds).
install(TARGETS kairos_tournament RUNTIME DESTINATION kairos/bin)
