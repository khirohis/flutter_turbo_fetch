cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "flutter_turbo_fetch")
project(${PROJECT_NAME} LANGUAGES CXX)

set(PLUGIN_NAME "flutter_turbo_fetch_plugin")
set(COMMON_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp")

list(APPEND PLUGIN_SOURCES
  "flutter_turbo_fetch_plugin.cpp"
  "flutter_turbo_fetch_plugin.h"
  "turbo_http_executor.cpp"
  "turbo_http_executor.h"
  "turbo_session_manager.cpp"
  "turbo_session_manager.h"
  "${COMMON_CPP_DIR}/messages.g.cpp"
  "${COMMON_CPP_DIR}/messages.g.h"
)

if(TARGET flutter_wrapper_plugin)
  add_library(${PLUGIN_NAME} SHARED
    "include/flutter_turbo_fetch/flutter_turbo_fetch_plugin_c_api.h"
    "flutter_turbo_fetch_plugin_c_api.cpp"
    ${PLUGIN_SOURCES}
  )

  if(COMMAND apply_standard_settings)
    apply_standard_settings(${PLUGIN_NAME})
  else()
    target_compile_features(${PLUGIN_NAME} PUBLIC cxx_std_17)
  endif()

  set_target_properties(${PLUGIN_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
  )
  target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
  target_compile_definitions(${PLUGIN_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

  target_include_directories(${PLUGIN_NAME} INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
  target_include_directories(${PLUGIN_NAME} PRIVATE
    "${COMMON_CPP_DIR}"
  )
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    flutter
    flutter_wrapper_plugin
    winhttp
  )

  set(flutter_turbo_fetch_bundled_libraries
    ""
    PARENT_SCOPE
  )
endif()

# === Standalone / Unit Test Target (enabled via -DBUILD_TESTING=ON) ===
option(BUILD_TESTING "Build Windows C++ unit tests" OFF)
if(BUILD_TESTING)
  enable_testing()

  set(TEST_BINARY_NAME "${PROJECT_NAME}_windows_test")

  if(NOT DEFINED FLUTTER_ENGINE_DIR)
    set(FLUTTER_ENGINE_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/../.fvm/flutter_sdk/bin/cache/artifacts/engine/windows-x64"
    )
  endif()

  if(EXISTS "${FLUTTER_ENGINE_DIR}/flutter_windows.dll.lib")
    set(FLUTTER_LIB_DIR "${FLUTTER_ENGINE_DIR}")
  elseif(EXISTS "${FLUTTER_ENGINE_DIR}-debug/flutter_windows.dll.lib")
    set(FLUTTER_LIB_DIR "${FLUTTER_ENGINE_DIR}-debug")
  else()
    set(FLUTTER_LIB_DIR "${FLUTTER_ENGINE_DIR}-release")
  endif()

  set(WRAPPER_ROOT "${FLUTTER_ENGINE_DIR}/cpp_client_wrapper")

  add_executable(${TEST_BINARY_NAME}
    "test/turbo_session_manager_test.cpp"
    "test/turbo_http_executor_test.cpp"
    "test/flutter_turbo_fetch_plugin_test.cpp"
    ${PLUGIN_SOURCES}
    "${WRAPPER_ROOT}/core_implementations.cc"
    "${WRAPPER_ROOT}/plugin_registrar.cc"
    "${WRAPPER_ROOT}/standard_codec.cc"
  )

  target_compile_features(${TEST_BINARY_NAME} PRIVATE cxx_std_17)
  target_compile_definitions(${TEST_BINARY_NAME} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _HAS_EXCEPTIONS=1
  )
  if(MSVC)
    target_compile_options(${TEST_BINARY_NAME} PRIVATE /W4 /EHsc /utf-8)
  endif()

  target_include_directories(${TEST_BINARY_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${COMMON_CPP_DIR}"
    "${FLUTTER_ENGINE_DIR}"
    "${FLUTTER_LIB_DIR}"
    "${WRAPPER_ROOT}/include"
  )

  target_link_libraries(${TEST_BINARY_NAME} PRIVATE
    "${FLUTTER_LIB_DIR}/flutter_windows.dll.lib"
    winhttp
  )

  add_custom_command(TARGET ${TEST_BINARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FLUTTER_LIB_DIR}/flutter_windows.dll"
      "$<TARGET_FILE_DIR:${TEST_BINARY_NAME}>"
  )

  add_test(NAME ${TEST_BINARY_NAME} COMMAND ${TEST_BINARY_NAME})
endif()

