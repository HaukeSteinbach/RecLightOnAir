#include "PluginEditor.h"

static constexpr int W        = 340;
static constexpr int H        = 160;
static constexpr int HEADER_H = 36;

// =============================================================================
OnAirAudioProcessorEditor::OnAirAudioProcessorEditor (OnAirAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    setSize (W, H);
    setResizable (false, false);
    setLookAndFeel (&laf);
    setWantsKeyboardFocus (true);

    laf.setColour (juce::TextButton::buttonColourId,  OALook::surface);
    laf.setColour (juce::TextButton::textColourOffId, OALook::textDim);
    laf.setColour (juce::Label::textColourId,         OALook::textDim);

    // ON als manueller Toggle (Licht dauerhaft an)
    testOn.setClickingTogglesState (true);
    testOn.onClick = [this]
    {
        audioProcessor.manualOn.store (testOn.getToggleState());
    };
    addAndMakeVisible (testOn);

    setupStatusLabel.setJustificationType (juce::Justification::topLeft);
    setupStatusLabel.setFont (juce::FontOptions (11.f));
    addAndMakeVisible (setupStatusLabel);

    startTimerHz (10);
}

// =============================================================================
void OnAirAudioProcessorEditor::timerCallback()
{
    // Status-Label: Text + Farbe nach Erreichbarkeit
    setupStatusLabel.setText (audioProcessor.getSetupStatus(), juce::dontSendNotification);
    setupStatusLabel.setColour (juce::Label::textColourId,
        audioProcessor.isEspUnreachableWarning()
            ? juce::Colour (0xFFFF8C00u)   // Orange: Warnung
            : OALook::textDim);            // Normal: gedimmt
    repaint (0, 0, W, HEADER_H);

    // Toggle-Knopf mit processor-State synchron halten
    if (testOn.getToggleState() != audioProcessor.manualOn.load())
        testOn.setToggleState (audioProcessor.manualOn.load(), juce::dontSendNotification);
}

// =============================================================================
void OnAirAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (OALook::bg);

    // Header
    g.setColour (OALook::header);
    g.fillRect (0, 0, W, HEADER_H);
    g.setColour (OALook::border);
    g.fillRect (0, HEADER_H - 1, W, 1);

    // Title
    g.setFont (juce::FontOptions (14.f).withStyle ("Bold"));
    g.setColour (OALook::textBrt);
    g.drawText ("Steinbach Rec Light", 12, 0, W - 120, HEADER_H,
                juce::Justification::centredLeft, false);

    // Status pill: REC (rot) / PLAY (blau) / IDLE
    // Uses the same debounced lampIsRecording/lampIsPlaying flags that drive
    // the ESP's physical LED, so the pill never gets stuck on "REC" once
    // recording/audio activity has actually stopped, and never flickers
    // independently of the real lamp state.
    const bool rec  = audioProcessor.lampIsRecording.load() || audioProcessor.manualOn.load();
    const bool play = !rec && audioProcessor.lampIsPlaying.load();
    juce::Rectangle<float> pill (float(W) - 96.f, 10.f, 84.f, 22.f);
    const juce::Colour pillCol = rec  ? OALook::recRed
                                : play ? OALook::accent
                                       : OALook::border;
    g.setColour (pillCol);
    g.fillRoundedRectangle (pill, 5.f);
    g.setFont (juce::FontOptions (11.f).withStyle ("Bold"));
    g.setColour ((rec || play) ? juce::Colours::white : OALook::textDim);
    g.drawText (rec ? "REC" : (play ? "PLAY" : "IDLE"), pill.toNearestInt(),
                juce::Justification::centred, false);
}

// =============================================================================
void OnAirAudioProcessorEditor::resized()
{
    const int pad  = 10;
    const int y0   = HEADER_H + 14;   // = 50
    const int rowH = 22;

    testOn     .setBounds (pad,            y0, 70,          rowH);

    setupStatusLabel.setBounds (pad, y0 + 34, W - pad * 2, 62);
}


