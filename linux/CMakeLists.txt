cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "flutter_turbo_fetch")
project(${PROJECT_NAME} LANGUAGES CXX)

set(PLUGIN_NAME "flutter_turbo_fetch_plugin")

find_package(CURL REQUIRED)

list(APPEND PLUGIN_SOURCES
  "flutter_turbo_fetch_plugin.cc"
  "flutter_turbo_fetch_plugin.h"
  "turbo_http_executor.cc"
  "turbo_http_executor.h"
  "turbo_session_manager.cc"
  "turbo_session_manager.h"
  "messages.g.cc"
  "messages.g.h"
)

# Standard Flutter Linux Plugin Target
if(TARGET flutter)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK REQUIRED gtk+-3.0)

  add_library(${PLUGIN_NAME} SHARED
    "include/flutter_turbo_fetch/flutter_turbo_fetch_plugin.h"
    ${PLUGIN_SOURCES}
  )

  apply_standard_settings(${PLUGIN_NAME})
  set_target_properties(${PLUGIN_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
  )
  target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

  target_include_directories(${PLUGIN_NAME} INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
  target_include_directories(${PLUGIN_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${GTK_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
  )
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    flutter
    ${GTK_LIBRARIES}
    ${CURL_LIBRARIES}
  )

  set(flutter_turbo_fetch_bundled_libraries
    ""
    PARENT_SCOPE
  )
endif()

# === Standalone / Unit Test Target (enabled via -DBUILD_TESTING=ON) ===
option(BUILD_TESTING "Build Linux C++ unit tests" OFF)
if(BUILD_TESTING)
  enable_testing()

  set(TEST_BINARY_NAME "${PROJECT_NAME}_linux_test")

  if(NOT DEFINED FLUTTER_ENGINE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../.fvm/flutter_sdk/bin/cache/artifacts/engine/linux-x64")
      set(FLUTTER_ENGINE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../.fvm/flutter_sdk/bin/cache/artifacts/engine/linux-x64"
      )
    else()
      set(FLUTTER_ENGINE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../.fvm/flutter_sdk/bin/cache/artifacts/engine/windows-x64"
      )
    endif()
  endif()

  set(WRAPPER_ROOT "${FLUTTER_ENGINE_DIR}/cpp_client_wrapper")

  add_executable(${TEST_BINARY_NAME}
    "test/turbo_session_manager_test.cpp"
    "test/turbo_http_executor_test.cpp"
    "test/flutter_turbo_fetch_plugin_test.cpp"
    "flutter_turbo_fetch_plugin.cc"
    "turbo_http_executor.cc"
    "turbo_session_manager.cc"
  )

  target_compile_features(${TEST_BINARY_NAME} PRIVATE cxx_std_17)
  target_compile_options(${TEST_BINARY_NAME} PRIVATE -Wall -Wextra -pthread)
  target_compile_definitions(${TEST_BINARY_NAME} PRIVATE TURBO_STANDALONE_TEST)

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK REQUIRED gtk+-3.0)

  target_include_directories(${TEST_BINARY_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${GTK_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
  )

  target_link_libraries(${TEST_BINARY_NAME} PRIVATE
    pthread
    ${GTK_LIBRARIES}
    ${CURL_LIBRARIES}
  )

  add_test(NAME ${TEST_BINARY_NAME} COMMAND ${TEST_BINARY_NAME})
endif()

