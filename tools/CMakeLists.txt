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
