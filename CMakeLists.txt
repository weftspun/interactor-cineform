# interactor-cineform: a CineForm encoder that answers commands on the weft bus.
#
# Three dependencies, all from this repository's own default.xml, which makes this a `repo`
# manifest ROOT: a clone of this repository plus `repo sync` is enough to build it.
#
#   repo init -u https://github.com/weftspun/interactor-cineform -m default.xml
#   repo sync
#
# None of them is a git submodule. CLAUDE.md blocklists those: a submodule pins a dependency
# in a file only git reads, invisible to `repo status`, and a bump appears in a diff as a bare
# hash with no name attached.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.22)
project(interactor_cineform CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# x86_64 only, and that is the codec's property rather than a choice. cineform-sdk includes
# <emmintrin.h> unconditionally in Codec/codec.c, convert.c and encoder.c, and sixteen files
# use __m128 with no NEON path and no scalar fallback. _XMMOPT gates code paths but not the
# include, so a non-x86 target fails in the preprocessor hundreds of files deep inside a
# vendored dependency. Refusing here means the message names the real reason.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  message(FATAL_ERROR
    "interactor-cineform is x86_64 only. cineform-sdk has no NEON path and no scalar "
    "fallback; sixteen of its files use __m128 and three include <emmintrin.h> "
    "unconditionally. Porting means writing those paths, not changing this line.")
endif()

set(HARNESS_DIR   "${CMAKE_SOURCE_DIR}/thirdparty/harness"      CACHE PATH "contract-bus checkout")
set(CINEFORM_DIR  "${CMAKE_SOURCE_DIR}/thirdparty/cineform-sdk" CACHE PATH "cineform-sdk checkout")
set(LIBWEBM_DIR   "${CMAKE_SOURCE_DIR}/thirdparty/libwebm"      CACHE PATH "libwebm checkout")

foreach(dep "${HARNESS_DIR}" "${CINEFORM_DIR}" "${LIBWEBM_DIR}")
  if(NOT EXISTS "${dep}")
    message(FATAL_ERROR
      "missing dependency at ${dep}.\n"
      "  They all come from this repository's own manifest. From the repository root:\n"
      "    repo init -u https://github.com/weftspun/interactor-cineform -m default.xml\n"
      "    repo sync")
  endif()
endforeach()

# The SDK builds a test tool by default and this does not need it. BUILD_STATIC with
# BUILD_SEPARATED off yields one target, CFHDCodecStatic, holding encoder and decoder.
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_SEPARATED OFF CACHE BOOL "" FORCE)

# The harness's own proof programs are not this build's business.
set(WEFT_HARNESS_PROOF OFF CACHE BOOL "" FORCE)

add_subdirectory("${HARNESS_DIR}" harness EXCLUDE_FROM_ALL)
add_subdirectory("${CINEFORM_DIR}" cineform-sdk EXCLUDE_FROM_ALL)

# Codec/decoder.c passes int32_t* to InterlockedIncrement, whose LPLONG parameter is long*.
# On Windows both are 32-bit signed, so the call is ABI-correct and the types merely differ
# in name. MSVC accepts it, clang has made -Wincompatible-pointer-types an error by default,
# and the build stops on two lines of a 24,000-line file.
#
# The flag goes HERE rather than into a patch of the SDK, because it is this build's warning
# policy and not a defect in the codec's behaviour. CLAUDE.md's rule on other people's
# codebases points the same way: a change matches the density of the code it edits, and the
# smallest change that is honest about what it is doing is not to edit that code at all.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(CFHDCodecStatic PRIVATE -Wno-incompatible-pointer-types)
endif()

# libwebm's own CMake builds a demuxer, a set of sample tools and its tests. Only the muxer is
# wanted, so the six files it needs are compiled directly. mkvparser is in the list because
# mkvmuxer.cc includes it, not because anything here demuxes.
add_library(cineform_mkvmuxer STATIC
  "${LIBWEBM_DIR}/mkvmuxer/mkvmuxer.cc"
  "${LIBWEBM_DIR}/mkvmuxer/mkvmuxerutil.cc"
  "${LIBWEBM_DIR}/mkvmuxer/mkvwriter.cc"
  "${LIBWEBM_DIR}/common/hdr_util.cc"
  "${LIBWEBM_DIR}/common/webm_endian.cc"
  "${LIBWEBM_DIR}/mkvparser/mkvparser.cc"
)
target_include_directories(cineform_mkvmuxer PUBLIC "${LIBWEBM_DIR}")

add_executable(interactor-cineform
  src/main.cpp
  src/job.cpp
  src/encoder.cpp
  src/mkv_writer.cpp
  src/source.cpp
)
target_include_directories(interactor-cineform PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/src"
  "${CINEFORM_DIR}/Common"
  "${CINEFORM_DIR}/EncoderSDK"
)
target_link_libraries(interactor-cineform PRIVATE weft::harness CFHDCodecStatic cineform_mkvmuxer)

# libuuid, on Linux only. SampleEncoder.cpp and MetadataWriter.cpp call uuid_generate, and
# cineform-sdk sets ADDITIONAL_LIBS to "-luuid -lpthread" but applies it ONLY to its own
# TestCFHD target -- CFHDCodecStatic carries no link interface at all, so every consumer
# has to know this. Compiling succeeded and linking did not, with two undefined references
# and no mention of the package that supplies them.
#
# Not on macOS: uuid_generate is in libSystem there, and asking for -luuid fails to find a
# library that does not exist rather than finding one that is not needed.
if(UNIX AND NOT APPLE)
  find_library(UUID_LIBRARY NAMES uuid REQUIRED)
  target_link_libraries(interactor-cineform PRIVATE ${UUID_LIBRARY})
endif()
find_package(Threads REQUIRED)
target_link_libraries(interactor-cineform PRIVATE Threads::Threads)

# encoder.cpp uses _mm_shuffle_epi8, which is SSSE3. clang and gcc require the feature to be
# enabled before the intrinsic may be used and refuse to inline it otherwise; MSVC enables
# every intrinsic it knows and needs no flag. The raise from the x86-64 SSE2 baseline to
# SSSE3 (Core 2, 2006) is argued in encoder.cpp beside the code that takes it.
if(NOT MSVC)
  set_source_files_properties(src/encoder.cpp PROPERTIES COMPILE_OPTIONS "-mssse3")
endif()

# The wire agreement gate. It reads the struct out of the header rather than restating it, so
# a field added without a matching size change fails a command instead of a deployment.
option(CINEFORM_PROOF "Build the wire agreement proof" ON)
if(CINEFORM_PROOF)
  enable_testing()
  add_executable(cineform-proof-wire proof/wire_agreement.cpp src/job.cpp)
  target_include_directories(cineform-proof-wire PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(cineform-proof-wire PRIVATE weft::harness)
  add_test(NAME wire_agreement COMMAND cineform-proof-wire)

endif()

