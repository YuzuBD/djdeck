// djdeck - a minimal two-deck DJ player for Windows.
// Audio engine: miniaudio playback device + lock-free UI/audio handoff.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

struct ma_device;  // opaque; defined in miniaudio.h (dj.cpp only)

namespace dj {

constexpr uint32_t kMaxBlock      = 2048;   // largest block renderBlock() accepts per call
constexpr uint32_t kDeclickFrames = 128;    // ~2.7 ms @48k: seek / end-of-track ramp
constexpr int      kWaveImageH    = 128;    // waveform texture height

// ---------------------------------------------------------------- track
// Fully decoded, resampled to the engine rate, interleaved stereo int16.
// Immutable after load; handed to the audio thread through Deck::pending.
struct Track {
    std::string          path;
    std::string          title;
    uint32_t             sampleRate = 0;
    uint64_t             frames     = 0;   // stereo frames
    std::vector<int16_t> pcm;              // L,R interleaved

    int                  peakW = 0;        // columns in `peaks`
    std::vector<uint8_t> peaks;            // 2 bytes per column: [min,max] scaled to 0..255

    double seconds() const { return sampleRate ? double(frames) / double(sampleRate) : 0.0; }
};

// Blocking decode+analyze. Call from a worker thread, never from the audio thread.
Track* LoadTrack(const std::string& path, uint32_t outRate, std::string* err);
// How many decodes have run since startup: lets a test prove that a track handover
// happened without decoding anything.
extern std::atomic<uint64_t> tracksLoaded;
Track* MakeSineTrack(double seconds, double hz, uint32_t rate, float amp);
void   FreeTrack(Track* t);
// Renders the peak columns into an RGBA image (baked deck colour) for the GPU texture.
void   BuildWaveformRGBA(const Track& t, std::vector<uint8_t>& rgba,
                         int height, uint8_t r, uint8_t g, uint8_t b);
// Writes a 16-bit stereo WAV demo loop. style 0/1 => different key + pattern.
bool   GenerateDemoWav(const std::string& path, double seconds, double bpm, int style,
                       uint32_t rate, std::string* err);

// ---------------------------------------------------------------- dsp
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};
    void  reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    inline float process(float x, int ch) {
        float y = b0 * x + z1[ch];
        z1[ch]  = b1 * x - a1 * y + z2[ch];
        z2[ch]  = b2 * x - a2 * y;
        return y;
    }
};
void MakeLowShelf(Biquad& q, float fs, float f0, float db);
void MakePeak(Biquad& q, float fs, float f0, float q_, float db);
void MakeHighShelf(Biquad& q, float fs, float f0, float db);

// ---------------------------------------------------------------- deck
// One deck. Everything the UI touches from another thread is atomic;
// members below the "audio-thread private" line are touched only by renderBlock().
struct Deck {
    // published by loader threads / retired by the audio thread (see RenderBlock)
    std::atomic<Track*> pending{nullptr};   // loader -> audio
    std::atomic<Track*> current{nullptr};   // audio -> everyone else
    std::atomic<bool>   ejectReq{false};    // UI -> audio: unload current track

    // SPSC retire ring: audio thread hands old tracks back, UI thread frees them.
    static constexpr int  kRetireSlots = 4;
    std::atomic<Track*>   retireQ[kRetireSlots]{};
    std::atomic<uint32_t> retireHead{0}, retireTail{0};
    void   Retire(Track* t);      // audio thread only
    Track* TakeRetired();         // UI thread only: nullptr when empty
    bool   RetireEmpty() const;
    void   ResetTransport();      // audio thread: clear transport state after a swap

    // UI -> audio
    std::atomic<uint64_t> seekReq{UINT64_MAX};   // absolute frame; consumed by audio
    std::atomic<int64_t>  scrubFrames{0};        // relative frames, consumed each block
    std::atomic<float>    rate{1.0f};            // playback speed (pitch fader)
    std::atomic<float>    faderDb{0.0f};         // channel fader, dB
    std::atomic<float>    eqDb[3]{};             // low / mid / high, dB (<= -24 = kill)
    std::atomic<uint32_t> fadeSeq{0};            // bump to arm a fade
    std::atomic<uint32_t> fadeFrames{0};
    std::atomic<float>    fadeTarget{1.0f};
    std::atomic<bool>     stopWhenFaded{false};
    std::atomic<bool>     playing{false};
    std::atomic<uint64_t> loopIn{UINT64_MAX}, loopOut{UINT64_MAX};
    std::atomic<bool>     loopOn{false};

    // audio -> UI
    std::atomic<uint64_t> pos{0};                // frames
    std::atomic<float>    meter[2]{};            // per channel peak
    std::atomic<float>    envNow{1.0f};          // current fade envelope (UI indicator)
    std::atomic<bool>     atEnd{false};

    // audio-thread private
    double   frac     = 0.0;
    float    chan     = 0.0f;                    // smoothed fader gain
    float    env      = 1.0f, envStep = 0.0f;
    uint32_t envLeft  = 0;
    float    dk       = 1.0f, dkStep = 0.0f;     // declick ramp around seeks
    uint32_t dkLeft   = 0;
    bool     jumpPending = false;
    uint64_t jumpTarget  = 0;
    uint32_t lastFadeSeq = 0;
    float    eqCache[3]  = {12345.f, 12345.f, 12345.f};
    Biquad   bq[3];
    bool     eqBypass    = true;

    // Arm a fade: target envelope (0..1) over `frames` frames.
    void ArmFade(float target, uint32_t frames, bool stopAfter) {
        fadeTarget.store(target, std::memory_order_relaxed);
        fadeFrames.store(frames, std::memory_order_relaxed);
        stopWhenFaded.store(stopAfter, std::memory_order_relaxed);
        fadeSeq.fetch_add(1, std::memory_order_release);
    }
    void RequestSeek(double seconds) {
        Track* t = current.load(std::memory_order_acquire);
        if (!t) return;
        uint64_t f = uint64_t(seconds * t->sampleRate);
        seekReq.store(f >= t->frames ? t->frames - 1 : f, std::memory_order_release);
    }
};

// ---------------------------------------------------------------- engine
struct Engine {
    Deck     deck[2];
    std::atomic<float>    xfader{0.5f};          // 0 = deck A, 1 = deck B (constant power)
    std::atomic<float>    masterGain{0.85f};
    std::atomic<float>    masterPeak{0.0f};
    std::atomic<float>    cpuLoad{0.0f};         // callback time / block duration
    std::atomic<uint64_t> callbacks{0}, xruns{0};

    uint32_t    sampleRate   = 48000;            // set by Start() to the device rate
    std::atomic<uint32_t> periodFrames{0};       // frames per data callback (set on the audio thread)
    std::string deviceName;
    std::string deviceNote;                      // which device configuration actually opened

    void InitOffline(uint32_t rate);   // size the scratch buffers without a device
    bool Start(std::string* err);
    void Stop();
    // Realtime-safe. Never allocates, locks, or touches the filesystem.
    void RenderBlock(float* outInterleaved, uint32_t frames);

  private:
    ma_device*         dev   = nullptr;
    std::vector<float> mix[2][2];                // [deck][channel] scratch
    float              limGain = 1.0f;
    uint32_t           renderChunk(float* out, uint32_t frames);
};

// Constant-power crossfader law, shared by the engine and the UI meters.
inline void CrossfaderGains(float x, float* gA, float* gB) {
    constexpr float kHalfPi = 1.5707963267948966f;
    *gA = cosf(x * kHalfPi);
    *gB = sinf(x * kHalfPi);
}

double NowSeconds();   // monotonic clock, for UI/status timing

}  // namespace dj
