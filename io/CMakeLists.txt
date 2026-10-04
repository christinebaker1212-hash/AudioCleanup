# Audio file I/O on top of JUCE's codecs. Compiled into each executable
# (JUCE modules must be linked exactly once per binary).
add_library(af_io INTERFACE)
target_sources(af_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/AudioIO.cpp ${CMAKE_CURRENT_SOURCE_DIR}/AudioIO.h)
target_include_directories(af_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# Enable JUCE's bundled MP3 decoder (decode only; MP3 patents have expired).
target_compile_definitions(af_io INTERFACE JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_OGGVORBIS=1)
