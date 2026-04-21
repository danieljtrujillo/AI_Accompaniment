#pragma once

// =====================================================================
// Single source of truth mirroring /PROTOCOL.md.
// Change PROTOCOL.md first, then update these. Any divergence is a bug.
// =====================================================================

#include <array>
#include <cstdint>
#include <string_view>

namespace protocol
{
    // --- Transport ------------------------------------------------------
    constexpr int  kDefaultServerPort  = 7000;          // client -> server
    constexpr int  kDefaultClientPort  = 8000;          // server -> client

    // --- Audio conventions ---------------------------------------------
    constexpr double kSampleRate        = 44100.0;
    constexpr int    kContextSamples    = 264600;       // 6.0 s
    constexpr int    kNumStems          = 4;

    // Stem order is load-bearing: the index is the class label the server
    // uses in /predict_instruments one-hot vectors.
    constexpr std::array<std::string_view, kNumStems> kStemNames = {
        "bass", "drums", "guitar", "piano"
    };

    constexpr std::array<std::string_view, kNumStems> kStemAddresses = {
        "/bass", "/drums", "/guitar", "/piano"
    };

    // --- Defaults (match server CLI defaults) --------------------------
    constexpr int    kDefaultPackageSize = 5120;        // floats per chunk
    constexpr float  kDefaultR           = 0.25f;       // hop = r * T
    constexpr float  kDefaultFade        = 0.02f;       // fade seconds
    constexpr float  kDefaultW           = 0.0f;        // regime: -1 / 0 / +1

    // --- Derived -------------------------------------------------------
    // Samples produced per prediction cycle by the server, per stem:
    //   r * T + fade * kSampleRate
    // (baked-in fade-in on the first `fade * SR` samples).
    constexpr int kDefaultHopSamples    = 66150;        // 0.25 * 264600
    constexpr int kDefaultFadeSamples   = 882;          // 0.02 * 44100
    constexpr int kDefaultStemSamples   = kDefaultHopSamples + kDefaultFadeSamples;   // 67032

    // --- OSC addresses -------------------------------------------------
    namespace addr
    {
        // control
        constexpr const char* kReady              = "/ready";
        constexpr const char* kLoadModel          = "/load_model";
        constexpr const char* kPredict            = "/predict";
        constexpr const char* kReset              = "/reset";
        constexpr const char* kPrint              = "/print";
        constexpr const char* kVerbose            = "/verbose";
        constexpr const char* kUpdatePackageSize  = "/update_package_size";
        constexpr const char* kUpdateR            = "/update_r";
        constexpr const char* kW                  = "/w";
        constexpr const char* kUpdateFade         = "/update_fade";
        constexpr const char* kPredictInstruments = "/predict_instruments";
        constexpr const char* kPacketTest         = "/packet_test";
        constexpr const char* kPacketTestResp     = "/packet_test_response";
        constexpr const char* kBatchDropped       = "/batch_dropped";

        // audio ingress
        constexpr const char* kContext            = "/context";
    }
}
