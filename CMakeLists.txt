cmake_minimum_required(VERSION 3.28)
project(livehime-obs VERSION 0.1.0 LANGUAGES CXX)

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
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network "${WEBVIEW2_LOADER}" crypt32 ole32 shell32 user32 shlwapi)
target_compile_definitions(livehime-obs PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  PLUGIN_VERSION="${PROJECT_VERSION}" LIVEHIME_OBS_VERSION="${PROJECT_VERSION}")
target_compile_options(livehime-obs PRIVATE /W4 /permissive- /utf-8 /EHsc)
set_target_properties(livehime-obs PROPERTIES PREFIX "" OUTPUT_NAME "livehime-obs")

install(TARGETS livehime-obs RUNTIME DESTINATION obs-plugins/64bit LIBRARY DESTINATION obs-plugins/64bit)
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
  add_executable(protocol-test tests/protocol-test.cpp src/protocol.cpp src/protocol.hpp)
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

get_property(livehime_tests DIRECTORY PROPERTY TESTS)
foreach(test_name IN LISTS livehime_tests)
  set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endforeach()
