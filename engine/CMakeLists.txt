# Pure C++ DSP engine (no JUCE dependency): analysis, processors, decision
# engine and renderer. Linked by the CLI, the tests and the GUI.
file(GLOB AF_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB AF_ENGINE_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/ac/*.h)

add_library(af_engine STATIC ${AF_ENGINE_SOURCES} ${AF_ENGINE_HEADERS})
target_include_directories(af_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(af_engine PRIVATE af_r8brain af_ebur128 af_rnnoise)
target_compile_features(af_engine PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(af_engine PRIVATE /W4 /permissive- /fp:precise /wd4244 /wd4267 /wd4100)
  target_compile_definitions(af_engine PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
  target_compile_options(af_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
