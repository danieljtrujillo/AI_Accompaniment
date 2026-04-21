#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <juce_osc/juce_osc.h>

#include "ProtocolConstants.h"

//==============================================================================
// OscBridge
// ---------
// Owns a juce::OSCSender + juce::OSCReceiver and speaks the PROTOCOL.md v1.0
// wire format. All threading lives here - the AudioProcessor calls these
// methods from either the audio thread (non-blocking sends of pre-queued
// chunks) or the message thread (UI actions).
//==============================================================================
class OscBridge : private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
{
public:
    struct Callbacks
    {
        std::function<void()>                  onReady;
        std::function<void (int batchId)>      onBatchDropped;
        std::function<void (int stemIndex,
                            int batchId,
                            int chunkIdx,
                            int totalChunks,
                            const float* data,
                            int numFloats)>    onStemChunk;
        std::function<void (double roundTripMs)> onPacketTestResponse;
    };

    OscBridge() = default;
    ~OscBridge() override { disconnect(); }

    //--------------------------------------------------------------------
    // Connection
    //--------------------------------------------------------------------
    bool connect (const juce::String& serverHost,
                  int                 serverPort,
                  int                 listenPort,
                  const Callbacks&    callbacks,
                  juce::String&       errorMessage);

    void disconnect();

    bool isConnected() const noexcept { return connected_.load(); }

    //--------------------------------------------------------------------
    // Control plane (message thread)
    //--------------------------------------------------------------------
    void sendLoadModel();
    void sendReset();
    void sendUpdatePackageSize (int size);
    void sendUpdateR (float r);
    void sendUpdateFade (float fade);
    void sendW (float w);
    void sendPredictInstruments (const std::array<int, protocol::kNumStems>& oneHot);
    void sendVerbose (bool enabled);
    void sendPacketTest (int numFloats);

    //--------------------------------------------------------------------
    // Audio ingress
    //--------------------------------------------------------------------
    // Fires total = ceil(numSamples / packageSize) messages on /context.
    // Call from a background thread (not audio) - this blocks on UDP send.
    void sendContextWindow (int          batchId,
                            const float* mono,
                            int          numSamples,
                            int          packageSize);

    //--------------------------------------------------------------------
    // Diagnostics
    //--------------------------------------------------------------------
    uint64_t getBytesSent()     const noexcept { return bytesSent_.load(); }
    uint64_t getBytesReceived() const noexcept { return bytesReceived_.load(); }

private:
    void oscMessageReceived (const juce::OSCMessage& msg) override;
    void oscBundleReceived  (const juce::OSCBundle&  bundle) override;

    void dispatchStemChunk (int stemIndex, const juce::OSCMessage& msg);

    juce::OSCSender   sender_;
    juce::OSCReceiver receiver_;
    Callbacks         cbs_;
    std::atomic<bool> connected_     { false };
    std::atomic<uint64_t> bytesSent_     { 0 };
    std::atomic<uint64_t> bytesReceived_ { 0 };

    // For RTT measurement on /packet_test
    std::atomic<double> pendingPacketTestTimeMs_ { 0.0 };
};
