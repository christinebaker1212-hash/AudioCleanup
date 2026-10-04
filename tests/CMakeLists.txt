juce_add_console_app(af_tests PRODUCT_NAME "AudioFinisherTests")
target_sources(af_tests PRIVATE TestMain.cpp TestSignals.cpp TestDsp.cpp TestRestoration.cpp TestPipeline.cpp)
target_compile_definitions(af_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(af_tests PRIVATE af_engine af_io juce::juce_audio_formats juce::juce_core
  juce::juce_recommended_config_flags)
add_test(NAME dsp_validation COMMAND af_tests)
