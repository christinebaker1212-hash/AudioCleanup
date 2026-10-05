# Audio file I/O on top of JUCE's codecs. Compiled into each executable
# (JUCE modules must be linked exactly once per binary).
add_library(af_io INTERFACE)
target_sources(af_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/AudioIO.cpp ${CMAKE_CURRENT_SOURCE_DIR}/AudioIO.h)
target_include_directories(af_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# JUCE provides WAV, FLAC, AIFF and Ogg Vorbis; MP3 and Opus use the libraries below.
target_compile_definitions(af_io INTERFACE JUCE_USE_OGGVORBIS=1)
# Robust decoders and the MP3 encoder (cmake/Dependencies.cmake).
target_link_libraries(af_io INTERFACE af_minimp3 af_opusfile af_lame)
