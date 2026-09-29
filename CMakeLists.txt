cmake_minimum_required(VERSION 3.20)
project(mini_sql C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Emit compile_commands.json for clangd / IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

# Every module except main. Listed once because two builds of the engine
# compile it (see the small-fan-out build below).
set(MINI_SQL_SOURCES
    src/input_buffer.c
    src/meta_command.c
    src/statement.c
    src/executor.c
    src/schema.c
    src/record.c
    src/table.c
    src/pager.c
    src/cursor.c
    src/btree.c
)

# Library target — every module except main lives here so tests/ and bench/
# can link against it later without recompiling.
add_library(mini_sql_lib ${MINI_SQL_SOURCES})
target_include_directories(mini_sql_lib PUBLIC include)

# Executable — just the REPL entry point, linked against the library.
add_executable(mini_sql src/main.c)
target_link_libraries(mini_sql PRIVATE mini_sql_lib)

# Test-only build of the same engine with internal nodes capped at 3 keys
# instead of the 510 a page holds. Reaching a full internal node then takes a
# few dozen rows rather than thousands, so the tests can pin what happens
# there. Same sources and file format; only the cap differs.
add_library(mini_sql_small_fanout_lib ${MINI_SQL_SOURCES})
target_include_directories(mini_sql_small_fanout_lib PUBLIC include)
target_compile_definitions(mini_sql_small_fanout_lib PRIVATE MINI_SQL_INTERNAL_NODE_MAX_KEYS=3)

add_executable(mini_sql_small_fanout src/main.c)
target_link_libraries(mini_sql_small_fanout PRIVATE mini_sql_small_fanout_lib)

# Black-box tests: each pipes commands into the built REPL and checks
# its output. One CTest entry per case so failures point at the exact test.
# Every case is handed both binaries and uses the one it needs.
enable_testing()
set(MINI_SQL_TESTS
    inserts_and_retrieves
    max_length_strings
    string_too_long
    negative_id
    persistence
    constants
    btree_one_node
    duplicate_key
    btree_split
    btree_split_persists
    btree_insert_after_split
    select_multilevel
    select_empty
    btree_nonroot_split
    internal_split_unimplemented
)
foreach(t ${MINI_SQL_TESTS})
    add_test(
        NAME ${t}
        COMMAND ${CMAKE_COMMAND} -E env
                MINI_SQL_BIN=$<TARGET_FILE:mini_sql>
                MINI_SQL_SMALL_FANOUT_BIN=$<TARGET_FILE:mini_sql_small_fanout>
                bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh ${t}
    )
endforeach()

# Scans follow on-disk sibling pointers, so a bad pointer can turn one into an
# endless loop. Fail such a test fast instead of hanging for CTest's default.
set_tests_properties(${MINI_SQL_TESTS} PROPERTIES TIMEOUT 10)
