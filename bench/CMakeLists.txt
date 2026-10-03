add_executable(kairos_bench
    bench_engine.cpp
)
target_link_libraries(kairos_bench PRIVATE
    kairos::kairos
    kairos_core
    benchmark::benchmark
)

add_executable(kairos_bench_replay
    bench_replay.cpp
)
target_link_libraries(kairos_bench_replay PRIVATE
    kairos::kairos
    kairos_core
    benchmark::benchmark
)
