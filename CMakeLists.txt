cmake_minimum_required(VERSION 3.22)

project(RecLight VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Local dev checkouts symlink JUCE/ to a shared checkout (see the repo's JUCE
# symlink); CI machines won't have that path, so fetch a pinned JUCE release
# from GitHub instead when the symlink/checkout isn't present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.14
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

# AU is Apple-only; Windows/Linux builds are VST3 + Standalone.
if(APPLE)
    set(RECLIGHT_FORMATS VST3 AU Standalone)
else()
    set(RECLIGHT_FORMATS VST3 Standalone)
endif()

juce_add_plugin(RecLight
    COMPANY_NAME                "Steinbach Audio"
    COMPANY_WEBSITE             ""
    COMPANY_EMAIL               ""
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT    "RecLight uses Bluetooth to find and control the ESP over its RecLightBLEHelper process."
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    PLUGIN_MANUFACTURER_CODE    Stbh
    PLUGIN_CODE                 Onar
    FORMATS                     ${RECLIGHT_FORMATS}
    PRODUCT_NAME                "RecLight"
    BUNDLE_ID                   "com.steinbach-audio.reclight"
    VST3_CATEGORIES             "Fx" "Tools"
)

target_sources(RecLight
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

if(APPLE)
    target_sources(RecLight PRIVATE Source/RecLightBluetoothBridge.mm)
    set_source_files_properties(Source/RecLightBluetoothBridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    find_library(FOUNDATION_FRAMEWORK Foundation)
    target_link_libraries(RecLight PRIVATE ${FOUNDATION_FRAMEWORK})

    # RecLightBLEHelper: standalone process that does the actual CoreBluetooth
    # work for "Plugin via Bluetooth" mode. Kept out of the DAW-hosted plugin
    # binary on purpose -- see RecLightBLEProtocol.h for why. The plugin
    # spawns it on demand and talks to it over loopback UDP.
    add_executable(RecLightBLEHelper Source/RecLightBLEHelper.mm)
    set_source_files_properties(Source/RecLightBLEHelper.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(RecLightBLEHelper PRIVATE Source)

    find_library(COREBLUETOOTH_FRAMEWORK CoreBluetooth)
    target_link_libraries(RecLightBLEHelper PRIVATE ${COREBLUETOOTH_FRAMEWORK} ${FOUNDATION_FRAMEWORK})

    # Copy the helper into every plugin bundle format's Contents/Resources, both
    # in the build tree and at the final installed location (COPY_PLUGIN_AFTER_BUILD
    # runs its own copy step before this one is registered, so we copy directly
    # to the installed bundle too rather than relying on that later step).
    set(_reclight_vst3_installed "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/RecLight.vst3")
    set(_reclight_au_installed   "$ENV{HOME}/Library/Audio/Plug-Ins/Components/RecLight.component")

    foreach(_fmt VST3 AU Standalone)
        if(TARGET RecLight_${_fmt})
            add_custom_command(TARGET RecLight_${_fmt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:RecLight_${_fmt}>/Resources"
                COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:RecLightBLEHelper>" "$<TARGET_BUNDLE_CONTENT_DIR:RecLight_${_fmt}>/Resources/RecLightBLEHelper"
                DEPENDS RecLightBLEHelper
                COMMENT "Copying RecLightBLEHelper into RecLight_${_fmt} bundle"
            )
        endif()
    endforeach()

    if(TARGET RecLight_VST3)
        add_custom_command(TARGET RecLight_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_reclight_vst3_installed}/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:RecLightBLEHelper>" "${_reclight_vst3_installed}/Contents/Resources/RecLightBLEHelper"
            DEPENDS RecLightBLEHelper
        )
    endif()
    if(TARGET RecLight_AU)
        add_custom_command(TARGET RecLight_AU POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_reclight_au_installed}/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:RecLightBLEHelper>" "${_reclight_au_installed}/Contents/Resources/RecLightBLEHelper"
            DEPENDS RecLightBLEHelper
        )
    endif()
else()
    target_sources(RecLight PRIVATE Source/RecLightBluetoothBridge.cpp)
endif()

target_include_directories(RecLight
    PRIVATE
        Source
)

target_compile_definitions(RecLight
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(RecLight
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

juce_generate_juce_header(RecLight)
