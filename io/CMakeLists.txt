# Audio file I/O on top of JUCE's codecs. Compiled into each executable
# (JUCE modules must be linked exactly once per binary).
add_library(af_io INTERFACE)
target_sources(af_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/AudioIO.cpp ${CMAKE_CURRENT_SOURCE_DIR}/AudioIO.h)
target_include_directories(af_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
