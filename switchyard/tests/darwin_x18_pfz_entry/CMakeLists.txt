# Compile the actual policy, not an assembly spelling model.
cmake_minimum_required(VERSION 3.20)
project(SwitchyardDarwinX18PFZEntry C)
if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  message(FATAL_ERROR "This contract requires native macOS ARM64")
endif()
include(CheckCSourceCompiles)
check_c_source_compiles("#include <os/arch/arm64.h>
  int main(void) { if (__builtin_available(macOS 26.4, *)) {
    os_set_custom_x18_abi_enabled(false); return os_custom_x18_abi_enabled(); }
    return 0; }" SWITCHYARD_PFZ_ENTRY_HAS_CUSTOM_X18_API)
if(NOT SWITCHYARD_PFZ_ENTRY_HAS_CUSTOM_X18_API)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pfz-entry-capability.skip" "unsupported SDK\n")
  return()
endif()
get_filename_component(wine_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(SWITCHYARD_FEX_WINE_SOURCE_DIR "${wine_root}" CACHE PATH "Wine source under test")
include("${wine_root}/switchyard/fex/tests/extract_wine_x18.cmake")
add_executable(darwin-x18-pfz-entry entry_contract.c
  "${wine_root}/switchyard/fex/tests/wine_signal_wrapper.c")
target_compile_features(darwin-x18-pfz-entry PRIVATE c_std_17)
target_compile_options(darwin-x18-pfz-entry PRIVATE -O2 -g -Wall -Wextra -Werror)
target_include_directories(darwin-x18-pfz-entry PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
  "${wine_root}/include" "${wine_root}/switchyard/fex/tests")
