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
add_executable(envhosts main.cpp envhosts.rc)
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

# Opt-in Qt Quick front-end (migration prototype). Off by default so the shipped
# GDI window and its GUI regression suite keep working while slices land on the
# qml branch. Qt DLLs are built with the dynamic CRT, so this build must use it
# too; the static-CRT rule above does not apply here.
file(READ "${CMAKE_SOURCE_DIR}/plugin.json" ENVHOSTS_MANIFEST)
string(JSON ENVHOSTS_VERSION GET "${ENVHOSTS_MANIFEST}" version)
string(REPLACE "." "," ENVHOSTS_FILE_VERSION "${ENVHOSTS_VERSION}.0")
file(READ "${CMAKE_SOURCE_DIR}/edition.json" ENVHOSTS_EDITION)
string(JSON ENVHOSTS_BACKEND GET "${ENVHOSTS_EDITION}" backend)
set(ENVHOSTS_DEFAULT_QML OFF)
if(ENVHOSTS_BACKEND STREQUAL "qml")
    set(ENVHOSTS_DEFAULT_QML ON)
endif()
option(ENVHOSTS_QML_EDITOR "Build the Qt Quick editor" ${ENVHOSTS_DEFAULT_QML})
if((ENVHOSTS_QML_EDITOR AND NOT ENVHOSTS_BACKEND STREQUAL "qml") OR
   (NOT ENVHOSTS_QML_EDITOR AND NOT ENVHOSTS_BACKEND STREQUAL "gdi"))
    message(FATAL_ERROR "Build the backend selected by edition.json; use the other branch for the other edition")
endif()
configure_file(build_info.h.in build_info.h @ONLY)
target_include_directories(envhosts PRIVATE "${CMAKE_BINARY_DIR}")
if(ENVHOSTS_QML_EDITOR)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Concurrent Network Test)
    qt_standard_project_setup()
    # New resource prefix: QML files land under /qt/qml/<URI>/, matching the URL
    # the C++ side loads (qrc:/qt/qml/EnvHosts/Editor/Main.qml).
    qt_policy(SET QTP0001 NEW)
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
        qt_policy(SET QTP0004 NEW)
    endif()
    # envhosts already exists, so the global AUTOMOC that qt_standard_project_setup
    # enables does not reach it; QML type registration needs it on the target.
    set_target_properties(envhosts PROPERTIES AUTOMOC ON)
    if(MSVC)
        set_target_properties(envhosts PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:MultiThreadedDebugDLL>")
    endif()
    target_compile_definitions(envhosts PRIVATE ENVHOSTS_QML_EDITOR)
    add_library(envhosts_qml STATIC qt/editor_types.h qt/editor_session.h qt/editor_session.cpp
        qt/editor_models.h qt/editor_models.cpp qt/write_service.h qt/write_service.cpp qt/qt_editor.h qt/qt_editor.cpp)
    target_include_directories(envhosts_qml PUBLIC "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/qt")
    target_compile_definitions(envhosts_qml PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(envhosts_qml PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Concurrent Qt6::Network advapi32 user32)
    if(MSVC)
        target_compile_options(envhosts_qml PRIVATE /utf-8 /W4 /permissive-)
    endif()
    target_link_libraries(envhosts PRIVATE envhosts_qml)
    # Share the executable icon artwork with Qt windows at every supported size.
    qt_add_resources(envhosts "plugin_window_icon"
        PREFIX /plugin-icons BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/app-16.png
            icons/app-20.png
            icons/app-24.png
            icons/app-32.png
            icons/app-40.png
            icons/app-48.png
            icons/app-64.png
            icons/app-256.png
    )
    qt_add_qml_module(envhosts
        URI EnvHosts.Editor
        VERSION 1.0
        QML_FILES
            qt/qml/Main.qml
            qt/qml/RowDelegate.qml
            qt/qml/DesignTokens.qml
            qt/qml/controls/CommandButton.qml
            qt/qml/controls/CompactCheckBox.qml
            qt/qml/controls/CompactTextField.qml
            qt/qml/controls/CompactComboBox.qml
            qt/qml/controls/ListSurface.qml
            qt/qml/controls/RowSurface.qml
            qt/qml/controls/ColumnHeader.qml
            qt/qml/controls/EditField.qml
            qt/qml/controls/EntryList.qml
            qt/qml/controls/StatusBanner.qml
            qt/qml/pages/EnvironmentPage.qml
            qt/qml/pages/PartsEditor.qml
            qt/qml/pages/HostsEntriesPage.qml
            qt/qml/pages/HostsRawPage.qml
            qt/qml/pages/BackupsPage.qml
            qt/qml/dialogs/SaveReview.qml
            qt/qml/dialogs/ConflictReview.qml
            qt/qml/dialogs/DraftRecovery.qml
    )
endif()

# The shipped GDI build writes envhosts.exe next to plugin.json because the
# manifest names it as runtime.program. The Qt prototype keeps its own output
# directory so an opt-in QML build never replaces the installed executable.
if(ENVHOSTS_QML_EDITOR)
    set(ENVHOSTS_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin")
else()
    set(ENVHOSTS_RUNTIME_DIR "${CMAKE_SOURCE_DIR}")
endif()

set_target_properties(envhosts PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ENVHOSTS_RUNTIME_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(envhosts PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${ENVHOSTS_RUNTIME_DIR}")
endforeach()

# Pure-logic tests: logic.h has no OS dependency, so they build and run on any
# C++17 compiler (Linux, macOS, Windows) without starting a plugin process.
add_executable(envhosts_tests tests/test_envhosts_logic.cpp)
target_include_directories(envhosts_tests PRIVATE "${CMAKE_SOURCE_DIR}")
# logic.h/native.h pull in windows.h on Windows; without NOMINMAX its min/max
# macros break std::min/std::max on MSVC.
target_compile_definitions(envhosts_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
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
target_compile_definitions(editor_model_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
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
if(ENVHOSTS_QML_EDITOR)
    add_executable(qml_editor_tests tests/test_qml_editor.cpp)
    target_link_libraries(qml_editor_tests PRIVATE envhosts_qml Qt6::Test)
    target_compile_definitions(qml_editor_tests PRIVATE ENVHOSTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(qml_editor_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME qml_editor COMMAND qml_editor_tests -o "${CMAKE_BINARY_DIR}/qml-editor-results.txt,txt")
    set_tests_properties(qml_editor PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")
endif()

add_executable(edition_storage_tests tests/test_edition_storage.cpp)
target_include_directories(edition_storage_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(edition_storage_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(edition_storage_tests PRIVATE /utf-8 /W4 /permissive-)
endif()
add_test(NAME edition_storage COMMAND edition_storage_tests)
