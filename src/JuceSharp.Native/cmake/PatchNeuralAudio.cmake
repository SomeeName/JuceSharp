# FetchContent PATCH_COMMAND step for NeuralAudio (run with `cmake -P`, working
# directory = the NeuralAudio checkout root). Idempotent: re-running it on an
# already patched tree, or on an upstream that no longer has the call, changes
# nothing.
#
# NeuralAudio/CMakeLists.txt (pin b1137e1, lines 190-192) calls
#   source_group(<name> ${CMAKE_CURRENT_SOURCE_DIR} FILES ...)
# CMake <= 4.4 silently ignored the stray directory argument; CMake 4.5 (which
# reworked source_group for FILE_SETS) rejects it with "source_group given
# unknown argument", and no policy restores the old parsing. Dropping the
# argument gives the exact grouping the older CMake produced.
set(_file "NeuralAudio/CMakeLists.txt")
if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "PatchNeuralAudio: ${_file} not found under ${CMAKE_CURRENT_SOURCE_DIR}")
endif()
file(READ "${_file}" _old)
string(REGEX REPLACE
    "source_group\\(([^ \t\r\n)]+)[ \t]+\\\${CMAKE_CURRENT_SOURCE_DIR}[ \t]+FILES"
    "source_group(\\1 FILES"
    _new "${_old}")
if(NOT _new STREQUAL _old)
    file(WRITE "${_file}" "${_new}")
    message(STATUS "PatchNeuralAudio: dropped the stray source_group directory argument")
endif()
