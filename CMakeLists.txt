cmake_minimum_required(VERSION 3.28)
project(livehime-obs VERSION 0.4.3 LANGUAGES C CXX)

include(cmake/ObsSdk.cmake)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
list(PREPEND CMAKE_PREFIX_PATH "${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23")
find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Core Gui Widgets Network)
include(CTest)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static codec dependencies" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "No zlib examples" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "No zlib upstream tests" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "Static decompression only" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "No upstream runtime installation" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "No upstream Brotli tests" FORCE)
set(BROTLI_BUNDLED_MODE ON CACHE BOOL "No upstream installation" FORCE)
set(BROTLI_BUILD_TOOLS OFF CACHE BOOL "Decoder library only" FORCE)
set(GOIM_DEPS "${LIVEHIME_DEPS_ROOT}/goim")
if(NOT EXISTS "${GOIM_DEPS}/zlib-1.4/CMakeLists.txt" OR NOT EXISTS "${GOIM_DEPS}/brotli-1.2.1/CMakeLists.txt")
  message(FATAL_ERROR "Run scripts/bootstrap-goim-codecs.ps1 -DependenciesRoot ${LIVEHIME_DEPS_ROOT}/goim")
endif()
add_subdirectory("${GOIM_DEPS}/zlib-1.4" "${CMAKE_BINARY_DIR}/goim-zlib" EXCLUDE_FROM_ALL)
add_subdirectory("${GOIM_DEPS}/brotli-1.2.1" "${CMAKE_BINARY_DIR}/goim-brotli" EXCLUDE_FROM_ALL)

file(GLOB plugin_sources CONFIGURE_DEPENDS "src/*.cpp" "src/*.hpp")
if(NOT plugin_sources)
  message(FATAL_ERROR "Plugin sources are missing from src/.")
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/session-store-test.cpp")
  add_executable(session-store-test tests/session-store-test.cpp src/session-store.cpp src/session-store.hpp)
  target_include_directories(session-store-test PRIVATE src)
  target_link_libraries(session-store-test PRIVATE Qt6::Core Qt6::Network crypt32)
  target_compile_definitions(session-store-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(session-store-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME session-store-roundtrip COMMAND session-store-test)
  set_tests_properties(session-store-roundtrip PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
endif()
add_library(livehime-obs MODULE ${plugin_sources})
target_include_directories(livehime-obs PRIVATE src "${WEBVIEW2_ROOT}/build/native/include")
target_link_libraries(livehime-obs PRIVATE OBS::libobs OBS::frontend-api
  zlibstatic brotlidec brotlicommon
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network "${WEBVIEW2_LOADER}" crypt32 bcrypt ole32 shell32 user32 shlwapi)
target_compile_definitions(livehime-obs PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  PLUGIN_VERSION="${PROJECT_VERSION}" LIVEHIME_OBS_VERSION="${PROJECT_VERSION}")
target_compile_options(livehime-obs PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_include_directories(livehime-obs PRIVATE "${GOIM_DEPS}/zlib-1.4" "${CMAKE_BINARY_DIR}/goim-zlib" "${GOIM_DEPS}/brotli-1.2.1/c/include")
set_target_properties(livehime-obs PROPERTIES PREFIX "" OUTPUT_NAME "livehime-obs")

install(TARGETS livehime-obs RUNTIME DESTINATION obs-plugins/64bit LIBRARY DESTINATION obs-plugins/64bit)
function(add_update_helper target)
  add_executable(${target} WIN32 updater/update-helper.cpp)
  set_target_properties(${target} PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY MultiThreaded)
  target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_link_libraries(${target} PRIVATE bcrypt shell32 ole32 user32)
endfunction()
add_update_helper(livehime-update-helper)
install(TARGETS livehime-update-helper RUNTIME DESTINATION data/obs-plugins/livehime-obs)
install(DIRECTORY data/ DESTINATION data/obs-plugins/livehime-obs)
set(LIVEHIME_SCHANNEL_BACKEND "${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins/tls/qschannelbackend.dll")
file(SHA256 "${LIVEHIME_SCHANNEL_BACKEND}" schannel_sha256)
if(NOT schannel_sha256 STREQUAL "0d1a73019c732551f3b1fca1391435fc1f7e551e50cd22d2b273d0495e7ff3dd")
  message(FATAL_ERROR "The private Schannel backend does not match the pinned Qt 6.11.1 bundle.")
endif()
install(FILES "${LIVEHIME_SCHANNEL_BACKEND}"
  DESTINATION data/obs-plugins/livehime-obs/qt-plugins/tls)

add_executable(network-probe EXCLUDE_FROM_ALL tests/network-probe.cpp)
target_link_libraries(network-probe PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(network-probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(network-probe PRIVATE /W4 /permissive- /utf-8 /EHsc)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol-test.cpp")
  add_executable(protocol-test tests/protocol-test.cpp src/protocol.cpp src/protocol.hpp
    src/moderation-notice.cpp src/moderation-notice.hpp)
  target_include_directories(protocol-test PRIVATE src)
  target_link_libraries(protocol-test PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
  target_compile_definitions(protocol-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(protocol-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME protocol-vectors COMMAND protocol-test)
  set_tests_properties(protocol-vectors PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/controller-test.cpp")
  add_executable(controller-test tests/controller-test.cpp
    src/live-controller.cpp src/live-controller.hpp src/session-store.cpp src/session-store.hpp
    src/bili-client.hpp src/obs-bridge.hpp)
  target_include_directories(controller-test PRIVATE src)
  target_link_libraries(controller-test PRIVATE Qt6::Core Qt6::Gui Qt6::Network
    OBS::libobs OBS::frontend-api crypt32)
  target_compile_definitions(controller-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(controller-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME controller-state-transitions COMMAND controller-test)
  set_tests_properties(controller-state-transitions PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/obs-bridge-test.cpp")
  add_executable(obs-bridge-test tests/obs-bridge-test.cpp src/obs-bridge.cpp src/obs-bridge.hpp)
  target_include_directories(obs-bridge-test PRIVATE src)
  target_include_directories(obs-bridge-test SYSTEM PRIVATE "${OBS_SOURCE_ROOT}/libobs"
    "${OBS_SOURCE_ROOT}/frontend/api" "${OBS_SDK_ROOT}/include")
  target_link_libraries(obs-bridge-test PRIVATE Qt6::Core Qt6::Network)
  target_compile_definitions(obs-bridge-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(obs-bridge-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME obs-bridge-lifecycle COMMAND obs-bridge-test)
  set_tests_properties(obs-bridge-lifecycle PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui-smoke.cpp")
  add_executable(ui-smoke tests/ui-smoke.cpp src/room-dock.cpp src/room-dock.hpp
    tests/native-controls-fixture.hpp src/native-stream-controls.cpp src/native-stream-controls.hpp
    src/cover-crop-dialog.cpp src/cover-crop-dialog.hpp src/protocol.cpp src/protocol.hpp
    src/live-controller.hpp src/obs-bridge.hpp src/login-dialog.hpp)
  target_include_directories(ui-smoke PRIVATE src)
  target_include_directories(ui-smoke SYSTEM PRIVATE "${OBS_SOURCE_ROOT}/libobs"
    "${OBS_SOURCE_ROOT}/frontend/api" "${OBS_SDK_ROOT}/include")
  target_link_libraries(ui-smoke PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
  target_compile_definitions(ui-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(ui-smoke PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME ui-crop-smoke COMMAND ui-smoke "${CMAKE_BINARY_DIR}/final-validation/crop-fixture.png")
  set_tests_properties(ui-crop-smoke PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit"
    ENVIRONMENT "QT_QPA_PLATFORM=minimal;QT_QPA_PLATFORM_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins/platforms;QT_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins")
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/native-stream-controls-test.cpp")
  add_executable(native-stream-controls-test tests/native-stream-controls-test.cpp tests/native-controls-fixture.hpp
    src/native-stream-controls.cpp src/native-stream-controls.hpp)
  target_include_directories(native-stream-controls-test PRIVATE src)
  target_link_libraries(native-stream-controls-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
  target_compile_definitions(native-stream-controls-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(native-stream-controls-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME native-stream-controls COMMAND native-stream-controls-test)
  set_tests_properties(native-stream-controls PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit"
    ENVIRONMENT "QT_QPA_PLATFORM=minimal;QT_QPA_PLATFORM_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins/platforms;QT_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins")
endif()

if(BUILD_TESTING)
  add_update_helper(update-helper-fixture)
  target_compile_definitions(update-helper-fixture PRIVATE LIVEHIME_UPDATE_TEST_FIXTURE)
  add_test(NAME update-after-obs-exit COMMAND pwsh -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-update-helper.ps1" -HelperPath "$<TARGET_FILE:update-helper-fixture>")
  set_tests_properties(update-after-obs-exit PROPERTIES TIMEOUT 90)
  add_executable(npm-signature-test tests/npm-signature-test.cpp src/npm-signature.cpp)
  target_include_directories(npm-signature-test PRIVATE src)
  target_link_libraries(npm-signature-test PRIVATE Qt6::Core bcrypt crypt32)
  add_test(NAME npm-registry-signature COMMAND npm-signature-test)
  add_executable(plugin-updater-test tests/plugin-updater-test.cpp src/plugin-updater.cpp src/update-package.cpp src/npm-signature.cpp)
  target_include_directories(plugin-updater-test PRIVATE src "${GOIM_DEPS}/zlib-1.4" "${CMAKE_BINARY_DIR}/goim-zlib")
  target_link_libraries(plugin-updater-test PRIVATE Qt6::Core Qt6::Network zlibstatic bcrypt crypt32)
  add_test(NAME npm-plugin-updater COMMAND plugin-updater-test)
  add_executable(update-controller-test tests/update-controller-test.cpp src/update-controller.cpp
    src/update-controller.hpp src/plugin-updater.hpp)
  target_include_directories(update-controller-test PRIVATE src)
  target_link_libraries(update-controller-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
  target_compile_definitions(update-controller-test PRIVATE LIVEHIME_OBS_VERSION="${PROJECT_VERSION}")
  add_test(NAME update-preferences COMMAND update-controller-test)
  set_tests_properties(update-preferences PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit"
    ENVIRONMENT "QT_QPA_PLATFORM=minimal;QT_QPA_PLATFORM_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins/platforms;QT_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins")
  foreach(target IN ITEMS npm-signature-test plugin-updater-test update-controller-test)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endforeach()
  set_tests_properties(npm-registry-signature npm-plugin-updater PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
endif()
get_property(livehime_tests DIRECTORY PROPERTY TESTS)
if(BUILD_TESTING)
  add_executable(goim-test tests/goim-test.cpp src/goim-codec.cpp src/goim-codec.hpp src/goim-client.cpp src/goim-client.hpp src/gift-v2.cpp src/gift-v2.hpp)
  target_include_directories(goim-test PRIVATE src "${GOIM_DEPS}/zlib-1.4" "${CMAKE_BINARY_DIR}/goim-zlib" "${GOIM_DEPS}/brotli-1.2.1/c/include")
  target_link_libraries(goim-test PRIVATE Qt6::Core Qt6::Network zlibstatic brotlidec brotlicommon brotlienc)
  target_compile_definitions(goim-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(goim-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME goim-codec-and-tcp COMMAND goim-test)
  set_tests_properties(goim-codec-and-tcp PROPERTIES TIMEOUT 60 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
  add_executable(realtime-state-test tests/realtime-state-test.cpp src/room-realtime.cpp src/room-realtime.hpp
    src/room-interaction.cpp src/room-interaction.hpp
    src/moderation-notice.cpp src/moderation-notice.hpp
    src/live-controller.cpp src/live-controller.hpp src/session-store.cpp src/session-store.hpp src/bili-client.hpp src/obs-bridge.hpp
    src/protocol.cpp src/protocol.hpp src/goim-client.cpp src/goim-client.hpp src/goim-codec.cpp src/goim-codec.hpp src/gift-v2.cpp src/gift-v2.hpp)
  target_include_directories(realtime-state-test PRIVATE src "${GOIM_DEPS}/zlib-1.4" "${CMAKE_BINARY_DIR}/goim-zlib" "${GOIM_DEPS}/brotli-1.2.1/c/include")
  target_link_libraries(realtime-state-test PRIVATE Qt6::Core Qt6::Gui Qt6::Network OBS::libobs OBS::frontend-api crypt32 zlibstatic brotlidec brotlicommon)
  target_compile_definitions(realtime-state-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(realtime-state-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME realtime-state-isolation COMMAND realtime-state-test)
  set_tests_properties(realtime-state-isolation PROPERTIES TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
  add_executable(realtime-ui-test tests/realtime-ui-test.cpp src/floating-windows.cpp src/floating-windows.hpp
    src/chat-user-menu.cpp src/chat-user-menu.hpp src/danmaku-follow.cpp src/danmaku-follow.hpp
    src/connection-indicator.cpp src/connection-indicator.hpp
    src/danmaku-emotes.cpp src/danmaku-emotes.hpp
    src/danmaku-input-row.cpp src/danmaku-input-row.hpp
    src/dock-focus.cpp src/dock-focus.hpp src/display-asset-cache.cpp src/display-asset-cache.hpp src/room-realtime.hpp src/live-controller.hpp
    src/moderation-alerts.cpp src/moderation-alerts.hpp)
  target_include_directories(realtime-ui-test PRIVATE src)
  target_include_directories(realtime-ui-test SYSTEM PRIVATE "${OBS_SOURCE_ROOT}/libobs" "${OBS_SOURCE_ROOT}/frontend/api" "${OBS_SDK_ROOT}/include")
  target_link_libraries(realtime-ui-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
  target_compile_definitions(realtime-ui-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(realtime-ui-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME realtime-floating-windows COMMAND realtime-ui-test)
  add_executable(danmaku-follow-test tests/danmaku-follow-test.cpp src/danmaku-follow.cpp src/danmaku-follow.hpp)
  target_include_directories(danmaku-follow-test PRIVATE src)
  target_link_libraries(danmaku-follow-test PRIVATE Qt6::Widgets)
  target_compile_options(danmaku-follow-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME danmaku-unread-follow COMMAND danmaku-follow-test)
  set_tests_properties(danmaku-unread-follow PROPERTIES TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit"
    ENVIRONMENT "QT_QPA_PLATFORM=minimal;QT_QPA_PLATFORM_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins/platforms;QT_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins")
  set_tests_properties(realtime-floating-windows PROPERTIES TIMEOUT 45 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit"
    ENVIRONMENT "QT_QPA_PLATFORM=minimal;QT_QPA_PLATFORM_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins/platforms;QT_PLUGIN_PATH=${LIVEHIME_DEPS_ROOT}/qt6-2026-10-23/plugins")
  add_executable(danmaku-emotes-test tests/danmaku-emotes-test.cpp src/danmaku-emotes.cpp src/danmaku-emotes.hpp
    src/display-asset-cache.cpp src/display-asset-cache.hpp)
  target_include_directories(danmaku-emotes-test PRIVATE src)
  target_link_libraries(danmaku-emotes-test PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
  target_compile_definitions(danmaku-emotes-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(danmaku-emotes-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME danmaku-emotes COMMAND danmaku-emotes-test)
  set_tests_properties(danmaku-emotes PROPERTIES TIMEOUT 15 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
  add_executable(gift-v2-test tests/gift-v2-test.cpp src/gift-v2.cpp src/gift-v2.hpp)
  target_include_directories(gift-v2-test PRIVATE src)
  target_link_libraries(gift-v2-test PRIVATE Qt6::Core)
  target_compile_options(gift-v2-test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME gift-v2-wire-parser COMMAND gift-v2-test)
  set_tests_properties(gift-v2-wire-parser PROPERTIES TIMEOUT 15 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_ROOT}/bin/64bit")
endif()
add_executable(realtime-network-probe EXCLUDE_FROM_ALL tests/realtime-network-probe.cpp
  src/bili-client.cpp src/bili-client.hpp src/protocol.cpp src/protocol.hpp src/session-store.cpp src/session-store.hpp
  src/goim-client.cpp src/goim-client.hpp src/goim-codec.cpp src/goim-codec.hpp src/gift-v2.cpp src/gift-v2.hpp)
target_include_directories(realtime-network-probe PRIVATE src "${GOIM_DEPS}/zlib-1.4" "${CMAKE_BINARY_DIR}/goim-zlib" "${GOIM_DEPS}/brotli-1.2.1/c/include")
target_link_libraries(realtime-network-probe PRIVATE Qt6::Core Qt6::Gui Qt6::Network crypt32 zlibstatic brotlidec brotlicommon)
target_compile_definitions(realtime-network-probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(realtime-network-probe PRIVATE /W4 /permissive- /utf-8 /EHsc)
get_property(livehime_tests DIRECTORY PROPERTY TESTS)
foreach(test_name IN LISTS livehime_tests)
  set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endforeach()
