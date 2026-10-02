add_executable(kairos_bench
    bench_smoke.cpp
)
target_link_libraries(kairos_bench PRIVATE
    kairos::kairos
    kairos_core
    benchmark::benchmark
)
