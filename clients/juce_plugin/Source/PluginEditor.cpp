#include "PluginEditor.h"

using namespace protocol;

//==============================================================================
AiAccompanimentEditor::AiAccompanimentEditor (AiAccompanimentProcessor& p)
    : AudioProcessorEditor (&p), proc_ (p)
{
    setSize (540, 360);

    hostField_.setText ("127.0.0.1", juce::dontSendNotification);
    serverPortField_.setText (juce::String (kDefaultServerPort), juce::dontSendNotification);
    listenPortField_.setText (juce::String (kDefaultClientPort), juce::dontSendNotification);
    serverPortField_.setInputRestrictions (5, "0123456789");
    listenPortField_.setInputRestrictions (5, "0123456789");

    addAndMakeVisible (hostField_);
    addAndMakeVisible (serverPortField_);
    addAndMakeVisible (listenPortField_);
    addAndMakeVisible (connectBtn_);
    addAndMakeVisible (loadModelBtn_);
    addAndMakeVisible (statusLabel_);
    addAndMakeVisible (srLabel_);
    addAndMakeVisible (batchLabel_);

    connectBtn_.onClick   = [this] { handleConnectClicked(); };
    loadModelBtn_.onClick = [this] { handleLoadModelClicked(); };
    loadModelBtn_.setEnabled (false);

    statusLabel_.setColour (juce::Label::textColourId, juce::Colours::orange);
    statusLabel_.setText ("Disconnected", juce::dontSendNotification);

    const auto mask = proc_.getPredictMask();
    for (int i = 0; i < kNumStems; ++i)
    {
        auto& tog = stemToggles_[(size_t) i];
        tog.setButtonText (juce::String (kStemNames[(size_t) i].data(),
                                         kStemNames[(size_t) i].size()));
        tog.setToggleState (mask[(size_t) i] != 0, juce::dontSendNotification);
        tog.onClick = [this]
        {
            std::array<int, kNumStems> m {};
            for (int j = 0; j < kNumStems; ++j)
                m[(size_t) j] = stemToggles_[(size_t) j].getToggleState() ? 1 : 0;
            proc_.setPredictMask (m);
        };
        addAndMakeVisible (tog);

        auto& lab = stemLevelLabels_[(size_t) i];
        lab.setText ("-inf dB", juce::dontSendNotification);
        lab.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (lab);
    }

    startTimerHz (15);
}

AiAccompanimentEditor::~AiAccompanimentEditor()
{
    stopTimer();
}

//==============================================================================
void AiAccompanimentEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    g.setColour (juce::Colours::white);
    g.setFont (18.0f);
    g.drawText ("AI Accompaniment", 12, 6, 300, 22, juce::Justification::left);
}

void AiAccompanimentEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (32);                                // title

    // Row: host / ports / buttons
    auto row = area.removeFromTop (28);
    hostField_.setBounds       (row.removeFromLeft (160));
    row.removeFromLeft (6);
    serverPortField_.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (6);
    listenPortField_.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (12);
    connectBtn_.setBounds      (row.removeFromLeft (90));
    row.removeFromLeft (6);
    loadModelBtn_.setBounds    (row.removeFromLeft (100));

    area.removeFromTop (8);
    statusLabel_.setBounds (area.removeFromTop (22));
    srLabel_.setBounds     (area.removeFromTop (22));
    batchLabel_.setBounds  (area.removeFromTop (22));

    area.removeFromTop (14);
    for (int i = 0; i < kNumStems; ++i)
    {
        auto r = area.removeFromTop (26);
        stemToggles_[(size_t) i].setBounds     (r.removeFromLeft (180));
        stemLevelLabels_[(size_t) i].setBounds (r.removeFromLeft (140));
    }
}

//==============================================================================
void AiAccompanimentEditor::timerCallback()
{
    srLabel_.setText (proc_.isSampleRateOk()
        ? "Sample rate: 44100 Hz OK"
        : "SR MISMATCH - set host to 44100 Hz",
        juce::dontSendNotification);
    srLabel_.setColour (juce::Label::textColourId,
                        proc_.isSampleRateOk() ? juce::Colours::lightgreen : juce::Colours::red);

    if (connected_)
    {
        statusLabel_.setText (proc_.isServerReady() ? "Server ready" : "Connected, waiting for /ready",
                              juce::dontSendNotification);
        statusLabel_.setColour (juce::Label::textColourId,
                                proc_.isServerReady() ? juce::Colours::lightgreen : juce::Colours::orange);
        loadModelBtn_.setEnabled (proc_.isServerReady());
    }

    batchLabel_.setText ("Batch: " + juce::String (proc_.getCurrentBatchId())
                       + "   dropped: " + juce::String (proc_.getBatchesDropped()),
                         juce::dontSendNotification);

    for (int i = 0; i < kNumStems; ++i)
    {
        const float lin = proc_.getStemLevel (i);
        const float db  = lin > 1.0e-6f ? 20.0f * std::log10 (lin) : -120.0f;
        stemLevelLabels_[(size_t) i].setText (juce::String (db, 1) + " dB",
                                              juce::dontSendNotification);
    }
}

void AiAccompanimentEditor::handleConnectClicked()
{
    if (connected_)
    {
        proc_.disconnectBridge();
        connected_ = false;
        connectBtn_.setButtonText ("Connect");
        statusLabel_.setText ("Disconnected", juce::dontSendNotification);
        statusLabel_.setColour (juce::Label::textColourId, juce::Colours::orange);
        loadModelBtn_.setEnabled (false);
        return;
    }

    const auto host    = hostField_.getText().trim();
    const int  srvPort = serverPortField_.getText().getIntValue();
    const int  lsnPort = listenPortField_.getText().getIntValue();

    juce::String err;
    if (! proc_.connectBridge (host, srvPort, lsnPort, err))
    {
        statusLabel_.setText (err, juce::dontSendNotification);
        statusLabel_.setColour (juce::Label::textColourId, juce::Colours::red);
        return;
    }

    connected_ = true;
    connectBtn_.setButtonText ("Disconnect");
    statusLabel_.setText ("Connected, waiting for /ready", juce::dontSendNotification);
    statusLabel_.setColour (juce::Label::textColourId, juce::Colours::orange);
}

void AiAccompanimentEditor::handleLoadModelClicked()
{
    proc_.getBridge().sendLoadModel();
    statusLabel_.setText ("Loading model…", juce::dontSendNotification);
    statusLabel_.setColour (juce::Label::textColourId, juce::Colours::orange);
}
