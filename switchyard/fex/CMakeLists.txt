# SPDX-License-Identifier: MIT

if(NOT APPLE OR NOT ARCHITECTURE_arm64)
  message(FATAL_ERROR "The Switchyard FEX adapter requires Apple Silicon macOS")
endif()

if(NOT TARGET FEXCore OR NOT TARGET FEXCore_Base OR NOT TARGET JemallocDummy)
  message(FATAL_ERROR "Add the Switchyard FEX adapter from a configured FEXCore tree")
endif()

string(LENGTH "${GIT_HASH}" SWITCHYARD_FEX_UPSTREAM_REVISION_LENGTH)
if(NOT SWITCHYARD_FEX_UPSTREAM_REVISION_LENGTH EQUAL 40 OR
   NOT GIT_HASH MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "The Switchyard FEX adapter requires an exact lowercase 40-hex FEX revision")
endif()

option(SWITCHYARD_FEX_BUILD_TESTS "Build the Switchyard FEX C ABI tests" ON)
option(SWITCHYARD_FEX_ARM64EC_REGISTER_ABI
  "Experiment with ARM64EC guest register placement while retaining the Darwin host ABI" OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <os/arch/arm64.h>
  int main() { if (__builtin_available(macOS 26.4, *)) {
    os_set_custom_x18_abi_enabled(false); return os_custom_x18_abi_enabled(); }
    return 0; }" SWITCHYARD_FEX_HAS_CUSTOM_X18_API)

if(SWITCHYARD_FEX_ARM64EC_REGISTER_ABI)
  if(NOT TARGET FEXCore_object)
    message(FATAL_ERROR "The register-ABI experiment requires the pinned FEXCore object target")
  endif()
  # FEXCore archives prebuilt OBJECT files. Definitions on the archive alone
  # would affect consumers but silently leave the generated-code ABI unchanged.
  target_compile_definitions(FEXCore_object PRIVATE FEXCORE_ARM64EC_REGISTER_ABI=1)
  target_compile_definitions(FEXCore PUBLIC FEXCORE_ARM64EC_REGISTER_ABI=1)
endif()

add_library(switchyard-fex SHARED
  src/switchyard_fex.cpp)
target_compile_features(switchyard-fex PRIVATE cxx_std_20)
target_compile_options(switchyard-fex PRIVATE
  -Wall
  -Wextra
  -Werror
  -Wconversion
  -Wshadow)
target_include_directories(switchyard-fex
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
target_include_directories(switchyard-fex SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/FEXCore/include"
  "${PROJECT_SOURCE_DIR}/FEXHeaderUtils"
  "${CMAKE_BINARY_DIR}/include")
target_link_libraries(switchyard-fex PRIVATE
  FEXCore
  FEXCore_Base
  JemallocDummy)
target_compile_definitions(switchyard-fex PRIVATE
  SWITCHYARD_FEX_UPSTREAM_REVISION="${GIT_HASH}")
if(SWITCHYARD_FEX_HAS_CUSTOM_X18_API)
  target_compile_definitions(switchyard-fex PRIVATE SWITCHYARD_FEX_HAS_CUSTOM_X18_API=1)
endif()
target_link_options(switchyard-fex PRIVATE
  "-Wl,-dead_strip"
  "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
set_target_properties(switchyard-fex PROPERTIES
  OUTPUT_NAME switchyard-fex
  VERSION 6.0.0
  SOVERSION 6
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.txt"
  MACOSX_RPATH YES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(SWITCHYARD_FEX_ENTITLEMENTS
  "${CMAKE_CURRENT_SOURCE_DIR}/../wine-runtime-native-arm64.entitlements"
  CACHE FILEPATH "Native ARM64 JIT entitlements used by the FEX ABI test")
if(NOT EXISTS "${SWITCHYARD_FEX_ENTITLEMENTS}")
  message(FATAL_ERROR "Missing native ARM64 JIT entitlements: ${SWITCHYARD_FEX_ENTITLEMENTS}")
endif()

add_custom_command(TARGET switchyard-fex POST_BUILD
  COMMAND /usr/bin/codesign --force --timestamp=none --sign -
          "$<TARGET_FILE:switchyard-fex>"
  VERBATIM)

install(TARGETS switchyard-fex
  LIBRARY DESTINATION lib
  COMPONENT SwitchyardFEXRuntime)
install(FILES include/switchyard_fex.h include/switchyard_fex_admission.h
  DESTINATION include
  COMPONENT SwitchyardFEXRuntime)

if(SWITCHYARD_FEX_BUILD_TESTS)
  enable_testing()
  # Keep the ordinary adapter tests buildable with older SDKs/targets. This
  # test alone exercises the public mode-toggle APIs introduced in 26.4.
  if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_GREATER_EQUAL "26.4" AND
     SWITCHYARD_FEX_HAS_CUSTOM_X18_API)
    include(tests/extract_wine_x18.cmake)
    add_executable(switchyard-fex-darwin-abi-test
      tests/darwin_abi_test.cpp tests/wine_signal_wrapper.c)
    target_compile_features(switchyard-fex-darwin-abi-test PRIVATE cxx_std_20 c_std_17)
    target_compile_options(switchyard-fex-darwin-abi-test PRIVATE
      -Wall -Wextra -Werror -Wconversion -Wshadow)
    target_compile_definitions(switchyard-fex-darwin-abi-test PRIVATE SWITCHYARD_FEX_HAS_CUSTOM_X18_API=1)
    target_include_directories(switchyard-fex-darwin-abi-test PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}")
    target_include_directories(switchyard-fex-darwin-abi-test SYSTEM PRIVATE
      "${SWITCHYARD_FEX_WINE_SOURCE_DIR}/include"
      "${PROJECT_SOURCE_DIR}/FEXCore/Source"
      "${PROJECT_SOURCE_DIR}/FEXCore/include"
      "${PROJECT_SOURCE_DIR}/FEXHeaderUtils"
      "${CMAKE_BINARY_DIR}/include")
    target_link_libraries(switchyard-fex-darwin-abi-test PRIVATE FEXCore FEXCore_Base JemallocDummy)
    add_custom_command(TARGET switchyard-fex-darwin-abi-test POST_BUILD
      COMMAND /usr/bin/codesign --force --timestamp=none --sign -
              --entitlements "${SWITCHYARD_FEX_ENTITLEMENTS}"
              "$<TARGET_FILE:switchyard-fex-darwin-abi-test>"
      VERBATIM)
    add_test(NAME switchyard-fex-darwin-abi COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>")
    add_test(NAME switchyard-fex-wine-signal COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --wine-signal)
    add_test(NAME switchyard-fex-wine-signal-public COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --wine-signal-public)
    add_test(NAME switchyard-fex-darwin-mode-recovery COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --omit-final-system-transition)
    add_test(NAME switchyard-fex-warm-entry COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --check-warm-entry)
    add_test(NAME switchyard-fex-detached-dispatch COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --detached-dispatch)
    add_test(NAME switchyard-fex-public-detached-dispatch COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --public-detached-dispatch)
    add_test(NAME switchyard-fex-vector-state COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --vector-state)
    add_test(NAME switchyard-fex-admission COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --admission)
    add_test(NAME switchyard-fex-shared-admission COMMAND "$<TARGET_FILE:switchyard-fex-darwin-abi-test>" --shared-admission)
    set_tests_properties(switchyard-fex-darwin-abi switchyard-fex-wine-signal
      switchyard-fex-wine-signal-public switchyard-fex-darwin-mode-recovery switchyard-fex-warm-entry
      switchyard-fex-detached-dispatch switchyard-fex-public-detached-dispatch switchyard-fex-vector-state switchyard-fex-admission switchyard-fex-shared-admission
      PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
  else()
    message(STATUS "Skipping custom-x18 ABI test: macOS 26.4+ SDK and deployment target required")
  endif()
  add_executable(switchyard-fex-allocator-test tests/allocator_test.cpp)
  target_compile_features(switchyard-fex-allocator-test PRIVATE cxx_std_20)
  target_compile_options(switchyard-fex-allocator-test PRIVATE
    -Wall -Wextra -Werror -Wconversion -Wshadow)
  target_include_directories(switchyard-fex-allocator-test SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/FEXCore/include")
  target_link_libraries(switchyard-fex-allocator-test PRIVATE FEXCore_Base JemallocDummy)
  add_test(NAME switchyard-fex-allocator COMMAND "$<TARGET_FILE:switchyard-fex-allocator-test>")
  add_executable(switchyard-fex-allocator-fallback-test tests/allocator_test.cpp)
  target_compile_features(switchyard-fex-allocator-fallback-test PRIVATE cxx_std_20)
  target_compile_options(switchyard-fex-allocator-fallback-test PRIVATE
    -Wall -Wextra -Werror -Wconversion -Wshadow)
  target_compile_definitions(switchyard-fex-allocator-fallback-test PRIVATE
    SWITCHYARD_TEST_ZERO_UNSUPPORTED=1)
  target_include_directories(switchyard-fex-allocator-fallback-test SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/FEXCore/include")
  target_link_libraries(switchyard-fex-allocator-fallback-test PRIVATE FEXCore_Base JemallocDummy)
  add_test(NAME switchyard-fex-allocator-fallback COMMAND "$<TARGET_FILE:switchyard-fex-allocator-fallback-test>")
  add_executable(switchyard-fex-c-api-test
    tests/c_api_test.c)
  target_compile_features(switchyard-fex-c-api-test PRIVATE c_std_17)
  target_compile_options(switchyard-fex-c-api-test PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wconversion
    -Wshadow)
  target_link_libraries(switchyard-fex-c-api-test PRIVATE
    switchyard-fex)
  set_target_properties(switchyard-fex-c-api-test PROPERTIES
    BUILD_RPATH "$<TARGET_FILE_DIR:switchyard-fex>")
  add_custom_command(TARGET switchyard-fex-c-api-test POST_BUILD
    COMMAND /usr/bin/codesign --force --timestamp=none --sign -
            --entitlements "${SWITCHYARD_FEX_ENTITLEMENTS}"
            "$<TARGET_FILE:switchyard-fex-c-api-test>"
    VERBATIM)
  add_test(NAME switchyard-fex-c-api
    COMMAND "$<TARGET_FILE:switchyard-fex-c-api-test>")
  add_executable(switchyard-fex-register-window-test tests/register_window_test.c)
  target_compile_features(switchyard-fex-register-window-test PRIVATE c_std_17)
  target_compile_options(switchyard-fex-register-window-test PRIVATE -Wall -Wextra -Werror -Wconversion -Wshadow)
  target_link_libraries(switchyard-fex-register-window-test PRIVATE switchyard-fex)
  add_executable(switchyard-fex-register-window-state-test tests/register_window_state_test.cpp)
  target_compile_features(switchyard-fex-register-window-state-test PRIVATE cxx_std_20)
  target_compile_options(switchyard-fex-register-window-state-test PRIVATE -Wall -Wextra -Werror -Wconversion -Wshadow)
  target_include_directories(switchyard-fex-register-window-state-test PRIVATE include tests)
  target_include_directories(switchyard-fex-register-window-state-test SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/FEXCore/include" "${PROJECT_SOURCE_DIR}/FEXHeaderUtils" "${CMAKE_BINARY_DIR}/include")
  target_link_libraries(switchyard-fex-register-window-state-test PRIVATE FEXCore FEXCore_Base JemallocDummy)
  target_compile_definitions(switchyard-fex-register-window-state-test PRIVATE
    SWITCHYARD_FEX_HAS_CUSTOM_X18_API=1 SWITCHYARD_FEX_UPSTREAM_REVISION="${GIT_HASH}")
  foreach(window_test switchyard-fex-register-window-test switchyard-fex-register-window-state-test)
    add_custom_command(TARGET ${window_test} POST_BUILD
      COMMAND /usr/bin/codesign --force --timestamp=none --sign -
              --entitlements "${SWITCHYARD_FEX_ENTITLEMENTS}" "$<TARGET_FILE:${window_test}>" VERBATIM)
  endforeach()
  add_test(NAME switchyard-fex-register-window COMMAND "$<TARGET_FILE:switchyard-fex-register-window-test>")
  add_test(NAME switchyard-fex-register-window-state COMMAND "$<TARGET_FILE:switchyard-fex-register-window-state-test>")
  set_tests_properties(switchyard-fex-register-window switchyard-fex-register-window-state PROPERTIES TIMEOUT 30)
endif()
