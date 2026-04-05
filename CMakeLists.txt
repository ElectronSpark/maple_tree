cmake_minimum_required(VERSION 3.10)
project(maple_tree C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Feature options ──────────────────────────────────────────────────────
option(MT_ENABLE_LOCK  "Enable internal locking (MT_CONFIG_LOCK)"  ON)
option(MT_ENABLE_RCU   "Enable RCU support (MT_CONFIG_RCU)"        OFF)
option(MT_BUILD_TESTS  "Build the cmocka test suite"               ON)
option(ENABLE_ASAN     "Enable AddressSanitizer for tests"         ON)

# ── Library ──────────────────────────────────────────────────────────────
add_library(maple_tree src/maple_tree.c)
target_include_directories(maple_tree PUBLIC include)
target_compile_options(maple_tree PRIVATE -Wall -Wextra -Werror)

if(MT_ENABLE_LOCK)
    target_compile_definitions(maple_tree PUBLIC MT_CONFIG_LOCK)
    target_link_libraries(maple_tree PUBLIC pthread)
endif()

if(MT_ENABLE_RCU)
    target_compile_definitions(maple_tree PUBLIC MT_CONFIG_RCU)
endif()

# ── Tests ────────────────────────────────────────────────────────────────
if(MT_BUILD_TESTS)
    find_library(CMOCKA cmocka REQUIRED)

    add_executable(test_maple_tree test/test_maple_tree.c)
    target_link_libraries(test_maple_tree maple_tree ${CMOCKA} pthread)

    if(ENABLE_ASAN)
        target_compile_options(test_maple_tree PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(test_maple_tree PRIVATE -fsanitize=address)
        target_compile_options(maple_tree PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(maple_tree PRIVATE -fsanitize=address)
    endif()

    enable_testing()
    add_test(NAME maple_tree_tests COMMAND test_maple_tree)
endif()
