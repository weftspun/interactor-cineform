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

# x86_64 and arm64. cineform-sdk includes <emmintrin.h>, <xmmintrin.h> and <mmintrin.h>
# and uses both the 128-bit SSE surface and the 64-bit MMX one -- 1687 __m64 declarations
# and roughly 4900 _mm_*_pi* calls -- with no scalar fallback: the one #if XMMOPT site has
# no #else. ARM is served by Codec/simd_compat.h, which is sse2neon for the 128-bit half
# and Codec/mmx2neon.h for the 64-bit half, so the includes resolve and the intrinsics map
# to NEON rather than being emulated.
#
# Other architectures still fail here rather than hundreds of files deep in the codec.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8
   OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|arm64|aarch64|ARM64")
  message(FATAL_ERROR
    "interactor-cineform builds for x86_64 and arm64. cineform-sdk needs SSE2 and MMX; "
    "ARM gets them through Codec/simd_compat.h. A third architecture means adding a "
    "mapping there, not changing this line.")
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
  # Codec/ carries simd_compat.h, which encoder.cpp needs for the SSSE3 intrinsic on
  # ARM. Adding the directory keeps that a plain include rather than a relative path
  # out of the source tree, which would differ between the workspace and deployment
  # layouts.
  "${CINEFORM_DIR}/Codec"
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
  # -mssse3 is an x86 flag and clang rejects it outright on arm64 rather than ignoring
  # it. The SSSE3 path it enables is reached through sse2neon on ARM, which needs no
  # flag: NEON is baseline on aarch64 and always available.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set_source_files_properties(src/encoder.cpp PROPERTIES COMPILE_OPTIONS "-mssse3")
  endif()
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

