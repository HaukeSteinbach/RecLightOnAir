#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// =============================================================================
//  Colour palette (matches SteinbachEQ style)
// =============================================================================
namespace OALook
{
    static const juce::Colour bg       { 0xFF0D0F11u };
    static const juce::Colour header   { 0xFF16181Au };
    static const juce::Colour surface  { 0xFF1A1D22u };
    static const juce::Colour border   { 0xFF2C3342u };
    static const juce::Colour accent   { 0xFF40A8FFu };
    static const juce::Colour recRed   { 0xFFE03030u };
    static const juce::Colour textDim  { 0xFF777777u };
    static const juce::Colour textBrt  { 0xFFDDDDDDu };
}

// =============================================================================
//  OnAirAudioProcessorEditor
// =============================================================================
class OnAirAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit OnAirAudioProcessorEditor (OnAirAudioProcessor&);
    ~OnAirAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    OnAirAudioProcessor& audioProcessor;

    // LookAndFeel (declared before child components)
    juce::LookAndFeel_V4 laf;

    // Controls (manual ON toggle, status)
    juce::TextButton testOn  { "ON" };
    juce::Label      setupStatusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnAirAudioProcessorEditor)
};
