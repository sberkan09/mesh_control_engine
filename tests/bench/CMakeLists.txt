# M-BENCH: the benchmark tool runs end to end at a small size and prints every case with its fields. No timing is
# asserted: a number is the machine's. A bad argument is a usage error (exit 2).
add_test(NAME bench_smoke COMMAND mce_bench --quick --json)
set_tests_properties(bench_smoke PROPERTIES PASS_REGULAR_EXPRESSION
    "\"format\":\"mce-bench\".*\"name\":\"ecs_iterate\".*\"name\":\"ecs_spawn\".*\"name\":\"ecs_destroy\".*\"name\":\"ecs_add_remove\".*\"name\":\"jobs_run_wait\".*\"name\":\"container_write\".*\"mb_per_s\".*\"name\":\"container_open\".*\"mb_per_s\".*\"name\":\"container_find\".*\"ns_per_item\"")
add_test(NAME bench_usage COMMAND mce_bench --bogus)
set_tests_properties(bench_usage PROPERTIES WILL_FAIL TRUE)
