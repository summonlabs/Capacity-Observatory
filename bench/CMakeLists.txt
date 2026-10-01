# Capacity Observatory benchmarks.
#
# One executable, registered with CTest under the "bench" label. The benchmark
# measures completed work with std::chrono::steady_clock and labels every row
# REAL, SYNTHETIC, or UNSUPPORTED.

add_executable(capacity-observatory-bench bench_main.cpp)
target_link_libraries(capacity-observatory-bench PRIVATE CapacityObservatory::capacity_observatory)

target_compile_definitions(capacity-observatory-bench PRIVATE
  CO_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  CO_BENCH_GENERATOR="${CMAKE_GENERATOR}"
  CO_BENCH_WARNINGS_AS_ERRORS=$<BOOL:${CO_WARNINGS_AS_ERRORS}>
  CO_BENCH_LOCK_AUDIT=$<BOOL:${CO_ENABLE_LOCK_AUDIT}>
)

if(MSVC)
  target_compile_options(capacity-observatory-bench PRIVATE /Zc:__cplusplus /permissive- /utf-8 /EHsc /W4)
  if(CO_WARNINGS_AS_ERRORS)
    target_compile_options(capacity-observatory-bench PRIVATE /WX)
  endif()
  target_compile_definitions(capacity-observatory-bench PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if(CO_ENABLE_ASAN)
    target_compile_options(capacity-observatory-bench PRIVATE /fsanitize=address /Zi)
    target_link_options(capacity-observatory-bench PRIVATE /fsanitize=address)
  endif()
else()
  target_compile_options(capacity-observatory-bench PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
  if(CO_WARNINGS_AS_ERRORS)
    target_compile_options(capacity-observatory-bench PRIVATE -Werror)
  endif()
endif()

add_test(NAME capacity-observatory-bench COMMAND capacity-observatory-bench --quick)
set_tests_properties(capacity-observatory-bench PROPERTIES LABELS "bench")
