include_guard(GLOBAL)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This plugin currently targets Windows x64 with MSVC.")
endif()

set(LIVEHIME_DEPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps" CACHE PATH "Pinned build dependencies")
set(OBS_ROOT "C:/Program Files/obs-studio" CACHE PATH "OBS 33.0.1 installation")
set(OBS_SOURCE_ROOT "${LIVEHIME_DEPS_ROOT}/obs-studio-33.0.1" CACHE PATH "Official OBS source headers")
set(OBS_SDK_ROOT "${LIVEHIME_DEPS_ROOT}/obs-sdk-33.0.1-x64" CACHE PATH "Generated OBS import libraries")
set(WEBVIEW2_ROOT "${LIVEHIME_DEPS_ROOT}/webview2" CACHE PATH "Microsoft.Web.WebView2 SDK")

foreach(required_path
    "${OBS_SOURCE_ROOT}/libobs/obs.h"
    "${OBS_SOURCE_ROOT}/frontend/api/obs-frontend-api.h"
    "${OBS_SDK_ROOT}/include/obsconfig.h"
    "${OBS_SDK_ROOT}/lib/obs.lib"
    "${OBS_SDK_ROOT}/lib/obs-frontend-api.lib"
    "${WEBVIEW2_ROOT}/build/native/include/WebView2.h")
  if(NOT EXISTS "${required_path}")
    message(FATAL_ERROR "Missing dependency: ${required_path}. Run scripts/bootstrap-deps.ps1.")
  endif()
endforeach()

add_library(OBS::libobs SHARED IMPORTED GLOBAL)
set_target_properties(OBS::libobs PROPERTIES
  IMPORTED_IMPLIB "${OBS_SDK_ROOT}/lib/obs.lib"
  IMPORTED_LOCATION "${OBS_ROOT}/bin/64bit/obs.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_ROOT}/libobs;${OBS_SDK_ROOT}/include")
add_library(OBS::frontend-api SHARED IMPORTED GLOBAL)
set_target_properties(OBS::frontend-api PROPERTIES
  IMPORTED_IMPLIB "${OBS_SDK_ROOT}/lib/obs-frontend-api.lib"
  IMPORTED_LOCATION "${OBS_ROOT}/bin/64bit/obs-frontend-api.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_ROOT}/frontend/api"
  INTERFACE_LINK_LIBRARIES OBS::libobs)

set(WEBVIEW2_LOADER "${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib")
if(NOT EXISTS "${WEBVIEW2_LOADER}")
  message(FATAL_ERROR "Missing WebView2 x64 loader: ${WEBVIEW2_LOADER}")
endif()
