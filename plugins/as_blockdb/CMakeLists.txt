# as_blockdb — Plugify port of the Pisex admin_system module "[AS] BlockDB" (reports bans to
# api.blockdb.ru and announces returning offenders on a Discord webhook).
#
#   plugify-as_blockdb  the plugin shared library (entry "bin/as_blockdb")
#
# Pure s2sdk + the admin_system consumer bindings: no pisexcompat, no hudmenu, no s2guard
# (docs/PLUGIN_STANDARD.md §7). The plugin has no SQL database of its own (the original BlockDB
# module had none either - its state is the remote API plus a process-local dedupe map), so plgdb
# is not linked.

set(AS_BLOCKDB_VERSION "1.1.0")

# The __s2sdk_* / __admin_system_* binding slots are defined by the object libraries below and
# filled by the plugify module from the plugins declared as dependencies.
#
# In this monorepo plugins/admin_system provides admin_system_bindings_obj. A standalone export of
# this plugin ships only the admin_system headers (tools/export/export_plugin.py copies include/
# and no CMakeLists.txt for a consumed plugin), so the target is absent there; build a private,
# uniquely named object library from the checked-in generated bindings instead of failing. The
# name must stay unique: another plugin may need the same fallback in the standalone tree, and two
# object targets with one name cannot coexist.
if(TARGET admin_system_bindings_obj)
    set(AS_BLOCKDB_ADMIN_BINDINGS_OBJECTS "$<TARGET_OBJECTS:admin_system_bindings_obj>")
else()
    set(AS_BLOCKDB_ADMIN_BINDINGS_CPP
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    if(NOT EXISTS "${AS_BLOCKDB_ADMIN_BINDINGS_CPP}")
        message(FATAL_ERROR "as_blockdb needs the generated admin_system consumer bindings (${AS_BLOCKDB_ADMIN_BINDINGS_CPP})")
    endif()
    add_library(as_blockdb_admin_system_bindings_obj OBJECT "${AS_BLOCKDB_ADMIN_BINDINGS_CPP}")
    target_include_directories(as_blockdb_admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_blockdb_admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_BLOCKDB_ADMIN_BINDINGS_OBJECTS "$<TARGET_OBJECTS:as_blockdb_admin_system_bindings_obj>")
endif()

add_library(plugify-as_blockdb SHARED
        src/plugin.cpp
        src/steam_http.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${AS_BLOCKDB_ADMIN_BINDINGS_OBJECTS}
)
target_include_directories(plugify-as_blockdb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated plg/s2sdk/admin_system/nlohmann headers must not fail -Werror
# on our sources.
target_include_directories(plugify-as_blockdb SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
)
# ${CMAKE_DL_LIBS}: steam_http.cpp dlopens the server's libsteam_api.so (the library itself is
# never linked, so the ABI report keeps showing no foreign dependencies).
target_link_libraries(plugify-as_blockdb PRIVATE
        ${CMAKE_DL_LIBS}
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_blockdb PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_blockdb PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_blockdb"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_blockdb PROPERTIES OUTPUT_NAME as_blockdb)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_blockdb PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_blockdb.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_blockdb.pplugin"
)

# --- tests --------------------------------------------------------------------
# as_blockdb_tests: the plugin TU is compiled into the test (its handlers live in an anonymous
# namespace), every __s2sdk_* / __admin_system_* binding slot is a recording fake, and the Steam
# HTTP transport is a recording fake linked in place of src/steam_http.cpp. The config files the
# module ships (and the live production ones) are parsed as data, so a broken file fails CI.
# Nothing here links pisexcompat, plgdb or a database.
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(as_blockdb_tests
            tests/as_blockdb_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            ${AS_BLOCKDB_ADMIN_BINDINGS_OBJECTS}
    )
    target_include_directories(as_blockdb_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_blockdb_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/sdk/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
    )
    target_link_libraries(as_blockdb_tests PRIVATE -static-libstdc++ -static-libgcc)
    target_compile_options(as_blockdb_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_blockdb_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="as_blockdb"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            AS_BLOCKDB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    )
    set_target_properties(as_blockdb_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # as_blockdb_steam_http_tests: the real src/steam_http.cpp is compiled with
    # AS_BLOCKDB_STEAM_HTTP_TESTING
    # and fed a fake SteamApi, so the CreateHTTPRequest/SendHTTPRequest/Shutdown error branches and
    # the URL redaction (the Discord webhook URL carries its token in the path) are exercised
    # without network or libsteam_api.so.
    add_executable(as_blockdb_steam_http_tests
            tests/steam_http_tests.cpp
            src/steam_http.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
    )
    target_include_directories(as_blockdb_steam_http_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_blockdb_steam_http_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(as_blockdb_steam_http_tests PRIVATE ${CMAKE_DL_LIBS} -static-libstdc++ -static-libgcc)
    target_compile_options(as_blockdb_steam_http_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_blockdb_steam_http_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            AS_BLOCKDB_STEAM_HTTP_TESTING=1
            PLUGIFY_MODULE="as_blockdb"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
    )
    set_target_properties(as_blockdb_steam_http_tests PROPERTIES
            OUTPUT_NAME as_blockdb_steam_http_tests
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # as_blockdb_package_tests: runs the real package.sh from an isolated copy of the plugin
    # directory (no ref/), with temp BUILD_DIR/OUT_DIR and fake artifacts. It pins the blank
    # ApiKey/webhook in the checked-in sample, the no-clobber deploy rule and the OUT_DIR guard
    # before `rm -rf`. The target name is plugin-prefixed: the tree is a monorepo and every port
    # may add its own package test.
    add_executable(as_blockdb_package_tests
            tests/package_tests.cpp
    )
    target_include_directories(as_blockdb_package_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(as_blockdb_package_tests PRIVATE -static-libstdc++ -static-libgcc)
    target_compile_options(as_blockdb_package_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_blockdb_package_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            AS_BLOCKDB_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set_target_properties(as_blockdb_package_tests PROPERTIES
            OUTPUT_NAME as_blockdb_package_tests
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
