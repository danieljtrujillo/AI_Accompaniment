#include "OscBridge.h"

#include <juce_core/juce_core.h>

//==============================================================================
bool OscBridge::connect (const juce::String& serverHost,
                         int                 serverPort,
                         int                 listenPort,
                         const Callbacks&    callbacks,
                         juce::String&       errorMessage)
{
    disconnect();
    cbs_ = callbacks;

    if (! sender_.connect (serverHost, serverPort))
    {
        errorMessage = "OSCSender could not connect to " + serverHost
                     + ":" + juce::String (serverPort);
        return false;
    }

    if (! receiver_.connect (listenPort))
    {
        sender_.disconnect();
        errorMessage = "OSCReceiver could not bind port " + juce::String (listenPort);
        return false;
    }

    receiver_.addListener (this);
    connected_.store (true);
    return true;
}

void OscBridge::disconnect()
{
    if (! connected_.exchange (false)) return;
    receiver_.removeListener (this);
    receiver_.disconnect();
    sender_.disconnect();
}

//==============================================================================
// Control plane
//==============================================================================
void OscBridge::sendLoadModel()
{
    sender_.send (protocol::addr::kLoadModel);
}

void OscBridge::sendReset()
{
    sender_.send (juce::OSCMessage (juce::OSCAddressPattern (protocol::addr::kReset), (juce::int32) 1));
}

void OscBridge::sendUpdatePackageSize (int size)
{
    sender_.send (juce::OSCMessage (juce::OSCAddressPattern (protocol::addr::kUpdatePackageSize),
                                    (juce::int32) size));
}

void OscBridge::sendUpdateR (float r)
{
    sender_.send (juce::OSCMessage (juce::OSCAddressPattern (protocol::addr::kUpdateR), r));
}

void OscBridge::sendUpdateFade (float fade)
{
    sender_.send (juce::OSCMessage (juce::OSCAddressPattern (protocol::addr::kUpdateFade), fade));
}

void OscBridge::sendW (float w)
{
    sender_.send (juce::OSCMessage (juce::OSCAddressPattern (protocol::addr::kW), w));
}

void OscBridge::sendPredictInstruments (const std::array<int, protocol::kNumStems>& oneHot)
{
    juce::OSCMessage m { juce::OSCAddressPattern (protocol::addr::kPredictInstruments) };
    for (int v : oneHot) m.addInt32 ((juce::int32) v);
    sender_.send (m);
}

void OscBridge::sendVerbose (bool enabled)
{
    sender_.send (juce::OSCMessage (juce::OSCAddressPattern (protocol::addr::kVerbose),
                                    (juce::int32) (enabled ? 1 : 0)));
}

void OscBridge::sendPacketTest (int numFloats)
{
    juce::OSCMessage m { juce::OSCAddressPattern (protocol::addr::kPacketTest) };
    m.addInt32 ((juce::int32) numFloats);
    juce::Random r;
    for (int i = 0; i < numFloats; ++i) m.addFloat32 (r.nextFloat());

    pendingPacketTestTimeMs_.store (juce::Time::getMillisecondCounterHiRes());
    sender_.send (m);
}

//==============================================================================
// Audio ingress  —  /context chunks
//==============================================================================
void OscBridge::sendContextWindow (int          batchId,
                                   const float* mono,
                                   int          numSamples,
                                   int          packageSize)
{
    if (numSamples <= 0 || packageSize <= 0) return;

    const int totalChunks = (numSamples + packageSize - 1) / packageSize;

    for (int chunkIdx = 0; chunkIdx < totalChunks; ++chunkIdx)
    {
        const int startIdx = chunkIdx * packageSize;
        const int len      = std::min (packageSize, numSamples - startIdx);

        juce::OSCMessage m { juce::OSCAddressPattern (protocol::addr::kContext) };
        m.addInt32 ((juce::int32) batchId);
        m.addInt32 ((juce::int32) startIdx);     // SAMPLE offset (not chunk idx)
        m.addInt32 ((juce::int32) totalChunks);

        for (int i = 0; i < len; ++i)
            m.addFloat32 (mono[startIdx + i]);

        sender_.send (m);
        bytesSent_.fetch_add ((uint64_t) (len * 4 + 64), std::memory_order_relaxed);
    }
}

//==============================================================================
// Receive
//==============================================================================
void OscBridge::oscMessageReceived (const juce::OSCMessage& msg)
{
    const auto addr = msg.getAddressPattern().toString();

    if (addr == protocol::addr::kReady)
    {
        if (cbs_.onReady) cbs_.onReady();
        return;
    }

    if (addr == protocol::addr::kBatchDropped)
    {
        if (msg.size() >= 1 && msg[0].isInt32() && cbs_.onBatchDropped)
            cbs_.onBatchDropped (msg[0].getInt32());
        return;
    }

    if (addr == protocol::addr::kPacketTestResp)
    {
        const double then = pendingPacketTestTimeMs_.exchange (0.0);
        if (then > 0.0 && cbs_.onPacketTestResponse)
            cbs_.onPacketTestResponse (juce::Time::getMillisecondCounterHiRes() - then);
        return;
    }

    // Stem chunks  ----------------------------------------------
    for (int i = 0; i < protocol::kNumStems; ++i)
    {
        if (addr == juce::String (protocol::kStemAddresses[(size_t) i].data()))
        {
            dispatchStemChunk (i, msg);
            return;
        }
    }

    // Unknown address - just count the bytes.
    bytesReceived_.fetch_add ((uint64_t) 16, std::memory_order_relaxed);
}

void OscBridge::oscBundleReceived (const juce::OSCBundle& bundle)
{
    for (auto& el : bundle)
        if (el.isMessage()) oscMessageReceived (el.getMessage());
}

void OscBridge::dispatchStemChunk (int stemIndex, const juce::OSCMessage& msg)
{
    if (msg.size() < 3) return;
    if (! (msg[0].isInt32() && msg[1].isInt32() && msg[2].isInt32())) return;

    const int batchId     = msg[0].getInt32();
    const int chunkIdx    = msg[1].getInt32();
    const int totalChunks = msg[2].getInt32();
    const int numFloats   = msg.size() - 3;

    // Pack into a temp stack buffer; kMaxFloats caps at one UDP datagram
    // worth of floats so this stays stack-friendly.
    constexpr int kMaxFloats = 16384;
    if (numFloats <= 0 || numFloats > kMaxFloats) return;

    float buf[kMaxFloats];
    for (int i = 0; i < numFloats; ++i)
    {
        const auto& a = msg[3 + i];
        buf[i] = a.isFloat32() ? a.getFloat32()
               : a.isInt32()   ? (float) a.getInt32()
               : 0.0f;
    }

    bytesReceived_.fetch_add ((uint64_t) (numFloats * 4 + 64), std::memory_order_relaxed);

    if (cbs_.onStemChunk)
        cbs_.onStemChunk (stemIndex, batchId, chunkIdx, totalChunks, buf, numFloats);
}
