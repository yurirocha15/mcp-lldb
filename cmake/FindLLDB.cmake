# Finds LLDB's C++ API (the SB API): the lldb/API headers and liblldb.
#
# Looks in LLDB_ROOT when it is set, then in the prefix of each llvm-config it
# finds (newest version first), then in the usual LLVM locations of Debian and
# Ubuntu (/usr/lib/llvm-N), Homebrew (its lldb and llvm formulas) and the LLVM
# Windows installer.
#
# Defines the imported target LLDB::LLDB and sets LLDB_FOUND, LLDB_VERSION,
# LLDB_INCLUDE_DIR, LLDB_LIBRARY and LLDB_LIBRARY_DIR.

set(_lldb_versions 23 22 21 20 19 18)

set(_lldb_hints)
if(LLDB_ROOT)
  list(APPEND _lldb_hints "${LLDB_ROOT}")
endif()
if(DEFINED ENV{LLDB_ROOT})
  list(APPEND _lldb_hints "$ENV{LLDB_ROOT}")
endif()

set(_llvm_config_names)
foreach(_v IN LISTS _lldb_versions)
  list(APPEND _llvm_config_names "llvm-config-${_v}" "llvm-config${_v}")
endforeach()
list(APPEND _llvm_config_names llvm-config)
foreach(_name IN LISTS _llvm_config_names)
  find_program(
    _llvm_config_${_name} ${_name} PATHS /opt/homebrew/opt/llvm/bin
                                         /usr/local/opt/llvm/bin NO_CACHE)
  if(_llvm_config_${_name})
    execute_process(
      COMMAND "${_llvm_config_${_name}}" --prefix
      OUTPUT_VARIABLE _llvm_prefix
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _llvm_result)
    if(_llvm_result EQUAL 0 AND _llvm_prefix)
      list(APPEND _lldb_hints "${_llvm_prefix}")
    endif()
  endif()
endforeach()

foreach(_v IN LISTS _lldb_versions)
  list(APPEND _lldb_hints "/usr/lib/llvm-${_v}" "/usr/local/opt/llvm@${_v}"
       "/opt/homebrew/opt/llvm@${_v}")
endforeach()
# Homebrew ships LLDB as its own formula, apart from llvm.
list(
  APPEND
  _lldb_hints
  /opt/homebrew/opt/lldb
  /usr/local/opt/lldb
  /opt/homebrew/opt/llvm
  /usr/local/opt/llvm
  "C:/Program Files/LLVM")
list(REMOVE_DUPLICATES _lldb_hints)

find_path(
  LLDB_INCLUDE_DIR lldb/API/LLDB.h
  HINTS ${_lldb_hints}
  PATH_SUFFIXES include
  NO_DEFAULT_PATH)
find_path(LLDB_INCLUDE_DIR lldb/API/LLDB.h)

# Search next to the headers first, so that the library matches them.
set(_lldb_library_hints)
if(LLDB_INCLUDE_DIR)
  get_filename_component(_lldb_prefix "${LLDB_INCLUDE_DIR}" DIRECTORY)
  list(APPEND _lldb_library_hints "${_lldb_prefix}")
endif()
find_library(
  LLDB_LIBRARY
  NAMES lldb liblldb
  HINTS ${_lldb_library_hints} ${_lldb_hints}
  PATH_SUFFIXES lib lib64
  NO_DEFAULT_PATH)
find_library(LLDB_LIBRARY NAMES lldb liblldb)

# The version: from the directory name of a versioned install (/usr/lib/llvm-18,
# Homebrew's Cellar/lldb/23.1.6 behind its opt/ link), else from
# lldb/Version/Version.inc when it is installed. SBWatchpointOptions.h appeared
# in LLDB 18, the oldest version this project builds against.
set(LLDB_VERSION "")
if(LLDB_INCLUDE_DIR)
  get_filename_component(_lldb_real_include "${LLDB_INCLUDE_DIR}" REALPATH)
  if(_lldb_real_include MATCHES "(llvm|lldb)[-@/]([0-9]+)")
    set(LLDB_VERSION "${CMAKE_MATCH_2}")
  elseif(EXISTS "${LLDB_INCLUDE_DIR}/lldb/Version/Version.inc")
    file(STRINGS "${LLDB_INCLUDE_DIR}/lldb/Version/Version.inc" _lldb_major
         REGEX "#define LLDB_VERSION_MAJOR [0-9]+")
    if(_lldb_major MATCHES "([0-9]+)")
      set(LLDB_VERSION "${CMAKE_MATCH_1}")
    endif()
  endif()
  if(NOT LLDB_VERSION AND EXISTS
                          "${LLDB_INCLUDE_DIR}/lldb/API/SBWatchpointOptions.h")
    set(LLDB_VERSION "18")
  endif()
endif()

if(LLDB_LIBRARY)
  get_filename_component(LLDB_LIBRARY_DIR "${LLDB_LIBRARY}" DIRECTORY)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  LLDB
  REQUIRED_VARS LLDB_LIBRARY LLDB_INCLUDE_DIR
  VERSION_VAR LLDB_VERSION)

if(LLDB_FOUND AND NOT TARGET LLDB::LLDB)
  add_library(LLDB::LLDB UNKNOWN IMPORTED)
  set_target_properties(
    LLDB::LLDB PROPERTIES IMPORTED_LOCATION "${LLDB_LIBRARY}"
                          INTERFACE_INCLUDE_DIRECTORIES "${LLDB_INCLUDE_DIR}")
endif()

mark_as_advanced(LLDB_INCLUDE_DIR LLDB_LIBRARY)
