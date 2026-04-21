#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

//==============================================================================
class AiAccompanimentEditor : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit AiAccompanimentEditor (AiAccompanimentProcessor&);
    ~AiAccompanimentEditor() override;

    void paint  (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void handleConnectClicked();
    void handleLoadModelClicked();

    AiAccompanimentProcessor& proc_;

    juce::TextEditor   hostField_         { "host" };
    juce::TextEditor   serverPortField_   { "serverPort" };
    juce::TextEditor   listenPortField_   { "listenPort" };
    juce::TextButton   connectBtn_        { "Connect" };
    juce::TextButton   loadModelBtn_      { "Load Model" };

    juce::Label        statusLabel_;
    juce::Label        srLabel_;
    juce::Label        batchLabel_;

    std::array<juce::ToggleButton, protocol::kNumStems> stemToggles_;
    std::array<juce::Label,        protocol::kNumStems> stemLevelLabels_;

    bool connected_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AiAccompanimentEditor)
};
