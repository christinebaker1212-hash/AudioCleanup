juce_add_console_app(af_cli PRODUCT_NAME "AudioFinisherCLI")
target_sources(af_cli PRIVATE main.cpp)
target_compile_definitions(af_cli PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(af_cli PRIVATE af_engine af_io juce::juce_audio_formats juce::juce_core
  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
