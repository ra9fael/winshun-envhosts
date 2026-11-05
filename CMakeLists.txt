cmake_minimum_required(VERSION 3.21)
project(envhosts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A 32-bit build is redirected by the Windows file-system redirector to
# SysWOW64 and would silently edit the wrong hosts file. Refuse to build it.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ra9fael.envhosts must be built as x64 (pointer size 8); a 32-bit build edits the wrong System32\\drivers\\etc\\hosts")
endif()

# The generated C++ template builds the exe straight into the plugin folder,
# because plugin.json names it as runtime.program. Keep that convention.
add_executable(envhosts main.cpp)
target_compile_definitions(envhosts PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# Registry (advapi32) for environment writes, user32 for the launcher window glue,
# gdi32 for the interactive editor window's GDI double-buffered painting.
target_link_libraries(envhosts PRIVATE advapi32 user32 gdi32)

if(MSVC)
    target_compile_options(envhosts PRIVATE /utf-8 /W4 /permissive-)
    # Static CRT: the exe needs no runtime DLL, which the normal-mode host cannot load.
    set_target_properties(envhosts PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(envhosts PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
    target_link_options(envhosts PRIVATE -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(envhosts PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(envhosts PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}")
endforeach()

# Pure-logic tests: logic.h has no OS dependency, so they build and run on any
# C++17 compiler (Linux, macOS, Windows) without starting a plugin process.
add_executable(envhosts_tests tests/test_envhosts_logic.cpp)
target_include_directories(envhosts_tests PRIVATE "${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_compile_options(envhosts_tests PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(envhosts_tests PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
endif()
set_target_properties(envhosts_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(envhosts_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/tests")
endforeach()

# Editor model tests: pure data model behind editor_win.h (dirty tracking, undo/redo,
# validation, PATH split/join, hosts multi-hostname rows, byte-exact save, drafts).
add_executable(editor_model_tests tests/test_editor_model.cpp)
target_include_directories(editor_model_tests PRIVATE "${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_compile_options(editor_model_tests PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(editor_model_tests PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
endif()
set_target_properties(editor_model_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(editor_model_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/tests")
endforeach()

enable_testing()
add_test(NAME envhosts_logic COMMAND envhosts_tests)
add_test(NAME editor_model COMMAND editor_model_tests)
