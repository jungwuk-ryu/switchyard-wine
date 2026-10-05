# SPDX-License-Identifier: MIT

# Compile the current Wine policy, not a copied model. These anchors deliberately
# fail closed if the production source is reorganized. No source file is edited.
set(SWITCHYARD_FEX_WINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.."
  CACHE PATH "Wine source checkout used by the real-source x18 integration test")
set(wine_signal_source "${SWITCHYARD_FEX_WINE_SOURCE_DIR}/dlls/ntdll/unix/signal_arm64.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${wine_signal_source}")
file(READ "${wine_signal_source}" wine_signal_text)
file(SHA256 "${wine_signal_source}" wine_signal_hash)
string(FIND "${wine_signal_text}" "/*" notice_start)
string(FIND "${wine_signal_text}" "*/" notice_end)
if(NOT notice_start EQUAL 0 OR notice_end LESS 0)
  message(FATAL_ERROR "Wine signal source copyright header changed")
endif()
math(EXPR notice_length "${notice_end} + 2")
string(SUBSTRING "${wine_signal_text}" 0 ${notice_length} wine_signal_notice)

function(extract_wine_x18_region start_marker end_marker result)
  string(FIND "${wine_signal_text}" "${start_marker}" start)
  string(FIND "${wine_signal_text}" "${start_marker}" start_last REVERSE)
  string(FIND "${wine_signal_text}" "${end_marker}" end)
  string(FIND "${wine_signal_text}" "${end_marker}" end_last REVERSE)
  if(start LESS 0 OR end LESS_EQUAL start OR NOT start EQUAL start_last OR NOT end EQUAL end_last)
    message(FATAL_ERROR "Wine x18 test extraction anchors changed: ${start_marker}")
  endif()
  math(EXPR length "${end} - ${start}")
  string(SUBSTRING "${wine_signal_text}" ${start} ${length} region)
  set(${result} "${region}" PARENT_SCOPE)
endfunction()

extract_wine_x18_region("#if defined(HAVE_OS_CUSTOM_X18_ABI) && \\\n"
  "\n#endif\n\n#define NTDLL_DWARF_H_NO_UNWINDER" wine_x18_mode)
extract_wine_x18_region("typedef void (*signal_handler_func)( int signal, siginfo_t *siginfo, void *sigcontext );"
  "\n#define DEFINE_SYSTEM_X18_SIGNAL_WRAPPER" wine_x18_signal)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wine_x18_policy.inc"
  CONTENT "${wine_signal_notice}\n/* Generated from Wine signal_arm64.c; do not edit.\n * Source SHA256: ${wine_signal_hash}\n */\n${wine_x18_mode}\n${wine_x18_signal}\n")
