# Third-party dependencies, pinned to exact releases.
#
#  JUCE 8.0.15            GUI, audio devices, WAV/FLAC codecs (AGPLv3 / commercial)
#  r8brain-free-src 6.5   linear-phase sample-rate conversion + PFFFT (MIT / BSD)
#  libebur128 1.2.6       ITU-R BS.1770-4 / EBU R128 loudness and true peak (MIT)
#  RNNoise 0.2            bundled recurrent-network speech denoiser with model (BSD)
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

FetchContent_MakeAvailable(r8brain ebur128 rnnoise)

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

# --- JUCE (only needed for I/O, CLI, tests and GUI) ---
FetchContent_MakeAvailable(juce)
