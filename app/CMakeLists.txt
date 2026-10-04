juce_add_gui_app(AudioFinisher
  PRODUCT_NAME "AudioFinisher"
  COMPANY_NAME "AudioFinisher"
  VERSION ${PROJECT_VERSION}
  NEEDS_CURL FALSE
  NEEDS_WEB_BROWSER FALSE)

target_sources(AudioFinisher PRIVATE
  Source/Main.cpp
  Source/MainComponent.cpp
  Source/Session.cpp
  Source/Playback.cpp
  Source/Views.cpp
  Source/StageTable.cpp
  Source/ControlsPanel.cpp
  Source/StemMixerPanel.cpp
  Source/UserPresets.cpp)

target_compile_definitions(AudioFinisher PRIVATE
  JUCE_USE_CURL=0
  JUCE_WEB_BROWSER=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AudioFinisher,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AudioFinisher,JUCE_VERSION>")

target_link_libraries(AudioFinisher PRIVATE
  af_engine af_io
  juce::juce_gui_basics juce::juce_audio_utils juce::juce_audio_devices juce::juce_audio_formats
  juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
