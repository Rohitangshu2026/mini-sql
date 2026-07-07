cmake_minimum_required(VERSION 3.20)
project(mini_sql C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Emit compile_commands.json for clangd / IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

# Library target — every module except main lives here so tests/ and bench/
# can link against it later without recompiling.
add_library(mini_sql_lib
    src/input_buffer.c
    src/meta_command.c
    src/statement.c
    src/executor.c
    src/schema.c
    src/record.c
    src/table.c
    src/pager.c
    src/cursor.c
)
target_include_directories(mini_sql_lib PUBLIC include)

# Executable — just the REPL entry point, linked against the library.
add_executable(mini_sql src/main.c)
target_link_libraries(mini_sql PRIVATE mini_sql_lib)

# Black-box tests: each pipes commands into the built REPL and checks
# its output. One CTest entry per case so failures point at the exact test.
enable_testing()
set(MINI_SQL_TESTS
    inserts_and_retrieves
    table_full
    max_length_strings
    string_too_long
    negative_id
    persistence
)
foreach(t ${MINI_SQL_TESTS})
    add_test(
        NAME ${t}
        COMMAND ${CMAKE_COMMAND} -E env MINI_SQL_BIN=$<TARGET_FILE:mini_sql>
                bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh ${t}
    )
endforeach()
