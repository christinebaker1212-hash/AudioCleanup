# Third-party dependencies, pinned to exact releases.
#
#  JUCE 8.0.15            GUI, audio devices, WAV/FLAC codecs (AGPLv3 / commercial)
#  r8brain-free-src 6.5   linear-phase sample-rate conversion + PFFFT (MIT / BSD)
#  libebur128 1.2.6       ITU-R BS.1770-4 / EBU R128 loudness and true peak (MIT)
#  RNNoise 0.2            bundled recurrent-network speech denoiser with model (BSD)
#  minimp3 (ea99364)      MP3 decoding: MPEG-1/2/2.5 layer III, VBR, gapless (CC0)
#  libogg 1.3.6, Opus 1.5.2, opusfile 0.12   Ogg Opus decoding (BSD)
#  LAME 3.100             MP3 encoding (LGPL 2; static, source available)
#
# Set FETCHCONTENT_SOURCE_DIR_<NAME> to use a local checkout (offline builds).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.15
  GIT_SHALLOW    TRUE)

FetchContent_Declare(r8brain
  GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git
  GIT_TAG        version-6.5
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _no_cmake_)

FetchContent_Declare(ebur128
  GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
  GIT_TAG        v1.2.6
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _no_cmake_)  # we compile ebur128.c ourselves

# The release tarball (unlike the git tag) ships the trained model weights.
FetchContent_Declare(rnnoise
  URL      https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
  URL_HASH SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR  _no_cmake_)

FetchContent_Declare(minimp3
  GIT_REPOSITORY https://github.com/lieff/minimp3.git
  GIT_TAG        ea99364f61c14656440e8d77e9c233ccf3124633
  SOURCE_SUBDIR  _no_cmake_)

FetchContent_Declare(ogg
  URL      https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.gz
  URL_HASH SHA256=83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR  _no_cmake_)  # we compile bitwise.c / framing.c ourselves

FetchContent_Declare(opus
  URL      https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
  URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(opusfile
  URL      https://github.com/xiph/opusfile/releases/download/v0.12/opusfile-0.12.tar.gz
  URL_HASH SHA256=118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR  _no_cmake_)

FetchContent_Declare(lame
  URL      https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
  URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR  _no_cmake_)

# Opus: static, decoder needs only the core library.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
# Match the static CRT used by the app (otherwise Opus forces /MD and MSVC fails to link).
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(r8brain ebur128 rnnoise minimp3 ogg opus opusfile lame)

# --- r8brain (+ the PFFFT copy it ships, which we also use for STFT work) ---
add_library(af_r8brain STATIC
  ${r8brain_SOURCE_DIR}/r8bbase.cpp
  ${r8brain_SOURCE_DIR}/pffft.cpp)
target_include_directories(af_r8brain SYSTEM PUBLIC ${r8brain_SOURCE_DIR})
if(MSVC)
  target_compile_options(af_r8brain PRIVATE /W0)
else()
  target_compile_options(af_r8brain PRIVATE -w)
endif()

# --- libebur128 (compiled directly; avoids its install/shared-lib logic) ---
add_library(af_ebur128 STATIC ${ebur128_SOURCE_DIR}/ebur128/ebur128.c)
target_include_directories(af_ebur128 SYSTEM PUBLIC
  ${ebur128_SOURCE_DIR}/ebur128
  ${ebur128_SOURCE_DIR}/ebur128/queue)
if(MSVC)
  target_compile_options(af_ebur128 PRIVATE /W0)
  target_compile_definitions(af_ebur128 PRIVATE _USE_MATH_DEFINES)
else()
  target_compile_options(af_ebur128 PRIVATE -w)
  target_link_libraries(af_ebur128 PRIVATE m)
endif()

# --- RNNoise 0.2 (generic C path; no runtime CPU dispatch needed) ---
set(RNN_SRC ${rnnoise_SOURCE_DIR}/src)
add_library(af_rnnoise STATIC
  ${RNN_SRC}/denoise.c
  ${RNN_SRC}/rnn.c
  ${RNN_SRC}/pitch.c
  ${RNN_SRC}/kiss_fft.c
  ${RNN_SRC}/celt_lpc.c
  ${RNN_SRC}/nnet.c
  ${RNN_SRC}/nnet_default.c
  ${RNN_SRC}/parse_lpcnet_weights.c
  ${RNN_SRC}/rnnoise_data.c
  ${RNN_SRC}/rnnoise_tables.c)
target_include_directories(af_rnnoise SYSTEM PUBLIC ${rnnoise_SOURCE_DIR}/include)
target_include_directories(af_rnnoise PRIVATE ${RNN_SRC})
target_compile_definitions(af_rnnoise PRIVATE RNNOISE_BUILD)
set_target_properties(af_rnnoise PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(af_rnnoise PRIVATE /W0)
  # MSVC never defines __SSE2__, but SSE2 is baseline on x64. Without it
  # RNNoise selects its generic path, whose os_support.h is missing from the
  # 0.2 release tarball. This is the same SSE path GCC/Clang use on x64.
  target_compile_definitions(af_rnnoise PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS __SSE2__=1)
else()
  target_compile_options(af_rnnoise PRIVATE -w)
  target_link_libraries(af_rnnoise PRIVATE m)
endif()

# --- minimp3 (header-only; the implementation is compiled in io/AudioIO.cpp) ---
add_library(af_minimp3 INTERFACE)
target_include_directories(af_minimp3 SYSTEM INTERFACE ${minimp3_SOURCE_DIR})

# --- libogg (compiled directly; config_types.h generated here) ---
set(AF_OGG_GEN ${CMAKE_BINARY_DIR}/af_ogg_include)
file(WRITE ${AF_OGG_GEN}/ogg/config_types.h
"#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\n"
"typedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\ntypedef int32_t ogg_int32_t;\n"
"typedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
add_library(af_ogg STATIC ${ogg_SOURCE_DIR}/src/bitwise.c ${ogg_SOURCE_DIR}/src/framing.c)
target_include_directories(af_ogg SYSTEM PUBLIC ${ogg_SOURCE_DIR}/include ${AF_OGG_GEN})

# --- opusfile (decoder only: no HTTP, no OpenSSL) ---
add_library(af_opusfile STATIC
  ${opusfile_SOURCE_DIR}/src/info.c
  ${opusfile_SOURCE_DIR}/src/internal.c
  ${opusfile_SOURCE_DIR}/src/opusfile.c
  ${opusfile_SOURCE_DIR}/src/stream.c)
target_include_directories(af_opusfile SYSTEM PUBLIC ${opusfile_SOURCE_DIR}/include)
target_compile_definitions(af_opusfile PRIVATE OP_DISABLE_HTTP OP_DISABLE_DOCS)
target_link_libraries(af_opusfile PUBLIC opus af_ogg)

# --- LAME 3.100 libmp3lame (encoder only; config.h in cmake/lame) ---
set(LAME_SRC ${lame_SOURCE_DIR}/libmp3lame)
add_library(af_lame STATIC
  ${LAME_SRC}/bitstream.c ${LAME_SRC}/encoder.c ${LAME_SRC}/fft.c ${LAME_SRC}/gain_analysis.c
  ${LAME_SRC}/id3tag.c ${LAME_SRC}/lame.c ${LAME_SRC}/newmdct.c ${LAME_SRC}/presets.c
  ${LAME_SRC}/psymodel.c ${LAME_SRC}/quantize.c ${LAME_SRC}/quantize_pvt.c ${LAME_SRC}/reservoir.c
  ${LAME_SRC}/set_get.c ${LAME_SRC}/tables.c ${LAME_SRC}/takehiro.c ${LAME_SRC}/util.c
  ${LAME_SRC}/vbrquantize.c ${LAME_SRC}/VbrTag.c ${LAME_SRC}/version.c)
target_include_directories(af_lame SYSTEM PUBLIC ${lame_SOURCE_DIR}/include)
target_include_directories(af_lame PRIVATE ${CMAKE_CURRENT_LIST_DIR}/lame ${LAME_SRC})
target_compile_definitions(af_lame PRIVATE HAVE_CONFIG_H)
if(MSVC)
  target_compile_options(af_lame PRIVATE /W0)
  target_compile_definitions(af_lame PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
  target_compile_options(af_lame PRIVATE -w)
  target_link_libraries(af_lame PRIVATE m)
endif()

foreach(t af_ogg af_opusfile)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W0)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${t} PRIVATE -w)
  endif()
endforeach()

# --- JUCE (only needed for I/O, CLI, tests and GUI) ---
FetchContent_MakeAvailable(juce)
