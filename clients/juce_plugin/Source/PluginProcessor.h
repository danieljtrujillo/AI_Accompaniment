#pragma once

#include <array>
#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "OscBridge.h"
#include "ContextRingBuffer.h"
#include "StemOutputBuffer.h"
#include "ProtocolConstants.h"

//==============================================================================
class AiAccompanimentProcessor  : public juce::AudioProcessor,
                                  private juce::Thread
{
public:
    AiAccompanimentProcessor();
    ~AiAccompanimentProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    bool isBusesLayoutSupported (const BusesProperties&) const { return true; }
    bool isBusesLayoutSupported (const BusesLayout&)     const override;

    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override              { return "CUE"; }
    bool   acceptsMidi() const override                      { return false; }
    bool   producesMidi() const override                     { return false; }
    bool   isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override             { return 0.0; }

    int  getNumPrograms() override                            { return 1; }
    int  getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                     {}
    const juce::String getProgramName (int) override          { return {}; }
    void changeProgramName (int, const juce::String&) override{}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Public API for the editor
    //==============================================================================
    OscBridge& getBridge() noexcept { return bridge_; }

    // Installs all internal callbacks on the bridge and connects. On success,
    // starts sending context windows automatically once the ring has filled.
    bool connectBridge (const juce::String& host,
                        int                 serverPort,
                        int                 listenPort,
                        juce::String&       errorOut);

    void disconnectBridge();

    bool isServerReady()     const noexcept { return serverReady_.load(); }
    bool isSampleRateOk()    const noexcept { return sampleRateOk_.load(); }
    int  getBatchesDropped() const noexcept { return batchesDropped_.load(); }
    int  getCurrentBatchId() const noexcept { return currentBatchId_.load(); }

    float getStemLevel (int stem) const noexcept
    {
        return stemLevels_[(size_t) stem].load (std::memory_order_relaxed);
    }

    // Mutators called by the editor
    void setPredictMask (const std::array<int, protocol::kNumStems>& mask);
    std::array<int, protocol::kNumStems> getPredictMask() const { return predictMask_; }

private:
    void run() override;                 // background thread: snapshots + send
    void triggerContextSend();
    void onOscStemChunk (int stemIndex, int batchId, int chunkIdx,
                         int totalChunks, const float* data, int numFloats);

    //--------------------------------------------------------------------
    // Audio
    //--------------------------------------------------------------------
    std::unique_ptr<ContextRingBuffer>   context_;      // mono, T samples
    std::array<std::unique_ptr<StemOutputBuffer>, protocol::kNumStems> stemBuffers_;

    // Per-batch absolute sample index for the consumer to seek to on batch change
    std::array<std::atomic<int>, protocol::kNumStems> stemBatchId_ {};
    std::array<std::atomic<uint32_t>, protocol::kNumStems> stemConsumerBase_ {};

    //--------------------------------------------------------------------
    // Networking
    //--------------------------------------------------------------------
    OscBridge bridge_;

    //--------------------------------------------------------------------
    // Transport / scheduling
    //--------------------------------------------------------------------
    std::atomic<uint64_t> samplesSeen_      { 0 };
    std::atomic<uint64_t> samplesSentAt_    { 0 };     // samplesSeen at last send
    std::atomic<int>      currentBatchId_   { 0 };
    std::atomic<bool>     needsContextSend_ { false };

    //--------------------------------------------------------------------
    // UI-visible state
    //--------------------------------------------------------------------
    std::atomic<bool> serverReady_      { false };
    std::atomic<bool> sampleRateOk_     { false };
    std::atomic<int>  batchesDropped_   { 0 };
    std::array<std::atomic<float>, protocol::kNumStems> stemLevels_ {};

    std::array<int, protocol::kNumStems> predictMask_ { 1, 1, 1, 1 };

    //--------------------------------------------------------------------
    // Ingress scratch (background thread only)
    //--------------------------------------------------------------------
    std::vector<float> contextScratch_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AiAccompanimentProcessor)
};
