#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace protocol;

//==============================================================================
AiAccompanimentProcessor::AiAccompanimentProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Bass",   juce::AudioChannelSet::stereo(), true)
          .withOutput ("Drums",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Guitar", juce::AudioChannelSet::stereo(), true)
          .withOutput ("Piano",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Mix",    juce::AudioChannelSet::stereo(), true))
    , juce::Thread ("AiAccompaniment-Net")
    , context_ (std::make_unique<ContextRingBuffer> (kContextSamples))
{
    // ~16 hops of cushion per stem (1.5 s each * 16 ≈ 24 s, plenty for host scheduling jitter)
    constexpr int kStemFifoSize = 1 << 20;  // 1 Mi samples ≈ 23.8 s
    for (int i = 0; i < kNumStems; ++i)
        stemBuffers_[(size_t) i] = std::make_unique<StemOutputBuffer> (kStemFifoSize);

    contextScratch_.resize ((size_t) kContextSamples);
}

AiAccompanimentProcessor::~AiAccompanimentProcessor()
{
    stopThread (2000);
    bridge_.disconnect();
}

//==============================================================================
bool AiAccompanimentProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (int i = 0; i < layouts.outputBuses.size(); ++i)
        if (layouts.getChannelSet (false, i) != juce::AudioChannelSet::stereo())
            return false;

    return true;
}

//==============================================================================
void AiAccompanimentProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRateOk_.store (std::abs (sampleRate - kSampleRate) < 0.5);
    samplesSeen_.store (0);
    samplesSentAt_.store (0);
    currentBatchId_.store (0);
    needsContextSend_.store (false);

    if (! isThreadRunning())
        startThread (juce::Thread::Priority::normal);
}

void AiAccompanimentProcessor::releaseResources()
{
    stopThread (2000);
}

//==============================================================================
void AiAccompanimentProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals _;

    const int numSamples = buffer.getNumSamples();
    const int totalBuses = getBusCount (false);

    // --- SR guard ----------------------------------------------------
    if (! sampleRateOk_.load (std::memory_order_relaxed))
    {
        for (int bus = 0; bus < totalBuses; ++bus)
        {
            auto out = getBusBuffer (buffer, false, bus);
            if (! out.hasBeenCleared())
                out.clear();
        }
        return;
    }

    // --- Ingest: mix input L+R to mono, push into context ring -------
    auto inBus = getBusBuffer (buffer, true, 0);
    const float* inL = inBus.getReadPointer (0);
    const float* inR = inBus.getNumChannels() > 1 ? inBus.getReadPointer (1) : inL;

    // Small scratch on the stack for the mono mix - block sizes are bounded.
    constexpr int kMaxBlock = 8192;
    float monoStack[kMaxBlock];
    const int n = std::min (numSamples, kMaxBlock);
    for (int i = 0; i < n; ++i) monoStack[i] = 0.5f * (inL[i] + inR[i]);
    context_->push (monoStack, n);

    // If block > kMaxBlock (unusual), chunk it
    for (int offset = kMaxBlock; offset < numSamples; offset += kMaxBlock)
    {
        const int m = std::min (kMaxBlock, numSamples - offset);
        for (int i = 0; i < m; ++i)
            monoStack[i] = 0.5f * (inL[offset + i] + inR[offset + i]);
        context_->push (monoStack, m);
    }

    // --- Schedule: every hop samples, mark pending -------------------
    const uint64_t seen = samplesSeen_.fetch_add ((uint64_t) numSamples,
                                                   std::memory_order_relaxed) + (uint64_t) numSamples;
    const uint64_t lastSent = samplesSentAt_.load (std::memory_order_relaxed);
    if (serverReady_.load (std::memory_order_relaxed)
        && seen >= (uint64_t) kContextSamples                       // first window filled
        && seen - lastSent >= (uint64_t) kDefaultHopSamples)
    {
        samplesSentAt_.store (seen, std::memory_order_relaxed);
        needsContextSend_.store (true, std::memory_order_release);
        notify();                                                   // wake net thread
    }

    // --- Egress: pull stems into their buses -------------------------
    // Bus 0..3 = stems, Bus 4 = input passthrough (Mix monitor)
    for (int stem = 0; stem < kNumStems; ++stem)
    {
        auto outBus = getBusBuffer (buffer, false, stem);
        float* outL = outBus.getWritePointer (0);
        float* outR = outBus.getNumChannels() > 1 ? outBus.getWritePointer (1) : outL;

        const int produced = stemBuffers_[(size_t) stem]->read (outL, numSamples);

        if (produced < numSamples)
            juce::FloatVectorOperations::clear (outL + produced, numSamples - produced);

        if (outR != outL)
            juce::FloatVectorOperations::copy (outR, outL, numSamples);

        // Level meter (peak of this block)
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (outL[i]));
        stemLevels_[(size_t) stem].store (peak, std::memory_order_relaxed);
    }

    // Bus 4 — dry input mix for monitoring
    if (totalBuses > kNumStems)
    {
        auto mixBus = getBusBuffer (buffer, false, kNumStems);
        if (mixBus.getNumChannels() >= 1) mixBus.copyFrom (0, 0, inBus, 0, 0, numSamples);
        if (mixBus.getNumChannels() >= 2 && inBus.getNumChannels() >= 2)
            mixBus.copyFrom (1, 0, inBus, 1, 0, numSamples);
    }
}

//==============================================================================
void AiAccompanimentProcessor::run()
{
    // Background sender loop: wait until processBlock flags a pending send
    // (every hop samples), then snapshot the ring and push /context chunks.
    while (! threadShouldExit())
    {
        wait (50);
        if (needsContextSend_.exchange (false, std::memory_order_acquire))
            triggerContextSend();
    }
}

//==============================================================================
bool AiAccompanimentProcessor::connectBridge (const juce::String& host,
                                              int                 serverPort,
                                              int                 listenPort,
                                              juce::String&       errorOut)
{
    OscBridge::Callbacks cbs;
    cbs.onReady          = [this] { serverReady_.store (true); };
    cbs.onBatchDropped   = [this] (int) { batchesDropped_.fetch_add (1); };
    cbs.onStemChunk      = [this] (int stem, int bid, int ci, int tc,
                                    const float* d, int n)
                           { onOscStemChunk (stem, bid, ci, tc, d, n); };
    cbs.onPacketTestResponse = [] (double) {};

    if (! bridge_.connect (host, serverPort, listenPort, cbs, errorOut))
    {
        serverReady_.store (false);
        return false;
    }

    // Push current predict mask so the server knows which stems to generate.
    bridge_.sendPredictInstruments (predictMask_);
    return true;
}

void AiAccompanimentProcessor::disconnectBridge()
{
    bridge_.disconnect();
    serverReady_.store (false);
}

void AiAccompanimentProcessor::triggerContextSend()
{
    if (! bridge_.isConnected()) return;

    const uint64_t total = context_->snapshot (contextScratch_.data());
    if (total < (uint64_t) kContextSamples) return;

    const int batchId = currentBatchId_.fetch_add (1) + 1;

    // Seek each stem's consumer to the newly-reserved region
    for (int i = 0; i < kNumStems; ++i)
    {
        const uint32_t base = (uint32_t) batchId * (uint32_t) kDefaultStemSamples;
        stemConsumerBase_[(size_t) i].store (base, std::memory_order_release);
        stemBuffers_[(size_t) i]->seekTo (base);
        stemBatchId_[(size_t) i].store (batchId, std::memory_order_release);
    }

    bridge_.sendContextWindow (batchId,
                               contextScratch_.data(),
                               kContextSamples,
                               kDefaultPackageSize);
}

void AiAccompanimentProcessor::onOscStemChunk (int stemIndex, int batchId,
                                               int chunkIdx, int /*totalChunks*/,
                                               const float* data, int numFloats)
{
    if ((unsigned) stemIndex >= (unsigned) kNumStems) return;

    // Drop chunks from an older batch (consumer has already moved past them)
    const int myBatch = stemBatchId_[(size_t) stemIndex].load (std::memory_order_acquire);
    if (batchId < myBatch) return;

    const uint32_t base = stemConsumerBase_[(size_t) stemIndex].load (std::memory_order_acquire)
                        + (uint32_t) (batchId - myBatch) * (uint32_t) kDefaultStemSamples;
    const uint32_t absIdx = base + (uint32_t) (chunkIdx * kDefaultPackageSize);

    stemBuffers_[(size_t) stemIndex]->writeAt (absIdx, data, numFloats);
}

void AiAccompanimentProcessor::setPredictMask (const std::array<int, kNumStems>& mask)
{
    predictMask_ = mask;
    if (bridge_.isConnected()) bridge_.sendPredictInstruments (mask);
}

//==============================================================================
juce::AudioProcessorEditor* AiAccompanimentProcessor::createEditor()
{
    return new AiAccompanimentEditor (*this);
}

//==============================================================================
void AiAccompanimentProcessor::getStateInformation (juce::MemoryBlock&) {}
void AiAccompanimentProcessor::setStateInformation (const void*, int) {}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AiAccompanimentProcessor();
}
