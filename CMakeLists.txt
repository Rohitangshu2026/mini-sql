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
add_library(mini_sql_lib STATIC
    src/input_buffer.c
)
target_include_directories(mini_sql_lib PUBLIC include)

# Executable — just the REPL entry point, linked against the library.
add_executable(mini_sql src/main.c)
target_link_libraries(mini_sql PRIVATE mini_sql_lib)
