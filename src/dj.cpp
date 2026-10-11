// djdeck - audio engine implementation (miniaudio + lock-free mixing).
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "dj.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dj {

static constexpr float  kPi     = 3.14159265358979323846f;
static constexpr float  kInvS16 = 1.0f / 32768.0f;

static double g_startTime = 0.0;

double NowSeconds() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    (void)g_startTime;
    return duration<double>(steady_clock::now() - t0).count();
}

// ------------------------------------------------------------------ helpers
static inline float dBToLin(float db) {
    if (db <= -60.0f) return 0.0f;
    return powf(10.0f, db * 0.05f);
}

static inline uint8_t PackPeak(float v) {
    int x = (int)lrintf((v * 0.5f + 0.5f) * 255.0f);
    return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x));
}

static std::string BaseName(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static void BuildPeaks(Track& t) {
    uint64_t w = t.frames / 128;
    w = std::min<uint64_t>(std::max<uint64_t>(w, 1024), 16384);
    t.peakW = (int)w;
    t.peaks.assign((size_t)w * 2, 128);
    const double span = double(t.frames) / double(w);
    for (uint64_t c = 0; c < w; ++c) {
        uint64_t a = (uint64_t)(c * span);
        uint64_t b = (uint64_t)((c + 1) * span);
        if (b > t.frames) b = t.frames;
        if (a >= b) { t.peaks[c * 2] = 128; t.peaks[c * 2 + 1] = 128; continue; }
        const uint64_t step = std::max<uint64_t>(1, (b - a) / 512);
        float mn = 1.0f, mx = -1.0f;
        for (uint64_t i = a; i < b; i += step) {
            float v = (t.pcm[i * 2] + t.pcm[i * 2 + 1]) * (0.5f * kInvS16);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        t.peaks[c * 2]     = PackPeak(mn);
        t.peaks[c * 2 + 1] = PackPeak(mx);
    }
}

void BuildWaveformRGBA(const Track& t, std::vector<uint8_t>& rgba, int h,
                       uint8_t cr, uint8_t cg, uint8_t cb) {
    if (t.peakW <= 0 || h <= 0) { rgba.clear(); return; }
    rgba.assign((size_t)t.peakW * h * 4, 0);
    const float mid = h * 0.5f;
    for (int c = 0; c < t.peakW; ++c) {
        const float lo = (t.peaks[c * 2]     / 255.0f) * 2.0f - 1.0f;  // <= 0
        const float hi = (t.peaks[c * 2 + 1] / 255.0f) * 2.0f - 1.0f;  // >= 0
        int y0 = (int)floorf(mid + lo * mid);
        int y1 = (int)ceilf (mid + hi * mid);
        y0 = std::max(0, std::min(h - 1, y0));
        y1 = std::max(0, std::min(h - 1, y1));
        if (y1 < y0) y1 = y0;
        for (int y = y0; y <= y1; ++y) {
            const float d = fabsf((float)y + 0.5f - mid) / std::max(1.0f, mid);
            const float k = 0.55f + 0.45f * d;
            uint8_t* px = &rgba[((size_t)c + (size_t)y * t.peakW) * 4];
            px[0] = (uint8_t)(cr * k);
            px[1] = (uint8_t)(cg * k);
            px[2] = (uint8_t)(cb * k);
            px[3] = 255;
        }
    }
}

// ------------------------------------------------------------------ filters
void MakeLowShelf(Biquad& q, float fs, float f0, float db) {
    const float A     = powf(10.0f, db * 0.025f);
    const float w     = 2.0f * kPi * f0 / fs;
    const float cs    = cosf(w), sn = sinf(w);
    const float alpha = sn * 0.5f * sqrtf(2.0f);
    const float sq    = 2.0f * sqrtf(A) * alpha;
    const float a0    = (A + 1.0f) + (A - 1.0f) * cs + sq;
    q.b0 = (A * ((A + 1.0f) - (A - 1.0f) * cs + sq)) / a0;
    q.b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs)) / a0;
    q.b2 = (A * ((A + 1.0f) - (A - 1.0f) * cs - sq)) / a0;
    q.a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cs)) / a0;
    q.a2 = ((A + 1.0f) + (A - 1.0f) * cs - sq) / a0;
}

void MakePeak(Biquad& q, float fs, float f0, float qq, float db) {
    const float A     = powf(10.0f, db * 0.025f);
    const float w     = 2.0f * kPi * f0 / fs;
    const float cs    = cosf(w), sn = sinf(w);
    const float alpha = sn / (2.0f * qq);
    const float a0    = 1.0f + alpha / A;
    q.b0 = (1.0f + alpha * A) / a0;
    q.b1 = (-2.0f * cs) / a0;
    q.b2 = (1.0f - alpha * A) / a0;
    q.a1 = (-2.0f * cs) / a0;
    q.a2 = (1.0f - alpha / A) / a0;
}

void MakeHighShelf(Biquad& q, float fs, float f0, float db) {
    const float A     = powf(10.0f, db * 0.025f);
    const float w     = 2.0f * kPi * f0 / fs;
    const float cs    = cosf(w), sn = sinf(w);
    const float alpha = sn * 0.5f * sqrtf(2.0f);
    const float sq    = 2.0f * sqrtf(A) * alpha;
    const float a0    = (A + 1.0f) - (A - 1.0f) * cs + sq;
    q.b0 = (A * ((A + 1.0f) + (A - 1.0f) * cs + sq)) / a0;
    q.b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs)) / a0;
    q.b2 = (A * ((A + 1.0f) + (A - 1.0f) * cs - sq)) / a0;
    q.a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cs)) / a0;
    q.a2 = ((A + 1.0f) - (A - 1.0f) * cs - sq) / a0;
}

// ------------------------------------------------------------------ tracks
Track* MakeSineTrack(double seconds, double hz, uint32_t rate, float amp) {
    auto* t = new Track();
    t->path     = "<sine>";
    t->title    = "<sine>";
    t->sampleRate = rate;
    t->frames   = (uint64_t)(seconds * rate);
    t->pcm.resize((size_t)t->frames * 2);
    for (uint64_t i = 0; i < t->frames; ++i) {
        float v = amp * sinf(2.0f * kPi * (float)hz * (float)i / (float)rate);
        int16_t s = (int16_t)lrintf(v * 32767.0f);
        t->pcm[i * 2] = t->pcm[i * 2 + 1] = s;
    }
    BuildPeaks(*t);
    return t;
}

void FreeTrack(Track* t) { delete t; }

std::atomic<uint64_t> tracksLoaded{0};

Track* LoadTrack(const std::string& path, uint32_t outRate, std::string* err) {
    tracksLoaded.fetch_add(1, std::memory_order_relaxed);
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 2, outRate);
    ma_decoder        dec;
    ma_result         r = ma_decoder_init_file(path.c_str(), &cfg, &dec);
    if (r != MA_SUCCESS) {
        if (err) *err = std::string("cannot open: ") + ma_result_description(r);
        return nullptr;
    }

    auto* t = new Track();
    t->path       = path;
    t->title      = BaseName(path);
    t->sampleRate = outRate;

    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &total) == MA_SUCCESS && total > 0)
        t->pcm.reserve((size_t)total * 2);

    std::vector<int16_t> chunk((size_t)65536 * 2);
    for (;;) {
        ma_uint64 got = 0;
        r = ma_decoder_read_pcm_frames(&dec, chunk.data(), 65536, &got);
        if (got > 0) t->pcm.insert(t->pcm.end(), chunk.begin(), chunk.begin() + (size_t)got * 2);
        if (r != MA_SUCCESS || got < 65536) break;
    }
    ma_decoder_uninit(&dec);

    t->frames = t->pcm.size() / 2;
    if (t->frames < 1024) {
        if (err) *err = "file too short or unreadable";
        delete t;
        return nullptr;
    }
    BuildPeaks(*t);
    return t;
}

// ------------------------------------------------------------------ demo wav
bool GenerateDemoWav(const std::string& path, double seconds, double bpm, int style,
                     uint32_t rate, std::string* err) {
    const uint64_t frames = (uint64_t)(seconds * rate);
    std::vector<int16_t> pcm((size_t)frames * 2);
    const double beat = 60.0 / bpm;
    static const double progA[4] = {110.00, 87.31, 130.81, 98.00};    // Am F C G
    static const double progB[4] = {146.83, 116.54, 174.61, 130.81};  // Dm Bb F C
    const int      st     = (style == 0) ? 0 : 1;     // key / subdivision family
    const bool     sparse = (style == 2);             // half-time, no pad: visually distinct
    const double*  prog   = (style == 0) ? progA : progB;
    uint32_t rng = 0x12345678u + (uint32_t)style * 7919u;
    double   prevNoise = 0.0;

    for (uint64_t i = 0; i < frames; ++i) {
        const double   tl      = double(i) / double(rate);
        const double   bt      = tl / beat;
        const uint64_t beatIdx = (uint64_t)bt;
        const double   bf      = bt - double(beatIdx);
        const double   tau     = bf * beat;
        const int      bar     = int(beatIdx / 4) & 3;
        const int      bib     = int(beatIdx % 4);

        const double kf = 45.0 + 90.0 * exp(-tau / 0.030);
        double kick = sin(2.0 * kPi * kf * tau) * exp(-tau / 0.16) * (st ? 0.80 : 0.95);
        if (sparse && bib != 0 && bib != 2) kick = 0.0;

        const double sub  = beat * (st ? 0.25 : 0.5);
        const double tauH = fmod(tl, sub);
        rng = rng * 1664525u + 1013904223u;
        const double n  = double((rng >> 9) & 0xFFFF) / 32768.0 - 1.0;
        const double hp = (n - prevNoise) * 0.5;
        prevNoise = n;
        double hat = hp * exp(-tauH / 0.020) * (st ? 0.16 : 0.20);
        if (sparse && bf > 0.25) hat = 0.0;

        double bass = 0.0;
        if (sparse) {
            if (bib == 0) bass = sin(2.0 * kPi * prog[bar] * tau) * exp(-tau / 0.30) * 0.35;
        } else if (bib == 0 || bib == 2) {
            bass = sin(2.0 * kPi * prog[bar] * tau) * exp(-tau / 0.28) * 0.50;
        } else if (st && bib == 3) {
            bass = sin(2.0 * kPi * prog[bar] * 0.5 * tau) * exp(-tau / 0.12) * 0.28;
        }

        const double sw  = 0.55 + 0.45 * sin(2.0 * kPi * tl / 8.0);
        const double pad = sparse ? 0.0
                                  : (sin(2.0 * kPi * prog[bar] * 2.0 * tl)
                                   + 0.70 * sin(2.0 * kPi * prog[bar] * 3.0 * tl)
                                   + 0.40 * sin(2.0 * kPi * prog[bar] * 2.005 * tl)) * 0.075 * sw;

        const double mono = kick * 0.85 + bass + pad + hat;
        double l = mono + hat * 0.35 + pad * 0.25;
        double r = mono - hat * 0.15 + pad * 0.15;
        l = tanh(l * 1.15) * 0.72;
        r = tanh(r * 1.15) * 0.72;
        pcm[i * 2]     = (int16_t)lrint(std::max(-32768.0, std::min(32767.0, l * 32767.0)));
        pcm[i * 2 + 1] = (int16_t)lrint(std::max(-32768.0, std::min(32767.0, r * 32767.0)));
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    const uint32_t dataBytes = (uint32_t)(frames * 2 * 2);
    const uint32_t riffSize  = 36 + dataBytes;
    const uint16_t ch = 2, bits = 16;
    const uint32_t byteRate = rate * ch * bits / 8;
    const uint16_t align = (uint16_t)(ch * bits / 8);
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(riffSize); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(ch); w32(rate); w32(byteRate); w16(align); w16(bits);
    fwrite("data", 1, 4, f); w32(dataBytes);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    return true;
}

// ------------------------------------------------------------------ time-stretch
// WSOLA: overlap-add windows on a fixed synthesis hop, each nudged by +/-D frames so
// its phase matches the natural continuation of the previous window. Pitch comes from
// the source untouched; only the *rate* at which we walk through it changes.
void Stretch::Init() {
    win.assign(N, 0.0f);
    for (int i = 0; i < N; ++i)
        win[i] = 0.25f - 0.25f * cosf(2.0f * kPi * (float)i / (float)N);  // 4x Hann sums to 1
    acc[0].assign(N, 0.0f);
    acc[1].assign(N, 0.0f);
    Reset();
}

void Stretch::Reset() {
    readPos  = Hs;                       // >= Hs makes the next pull refill from scratch
    started  = false;
    havePrev = false;
    tPrev    = 0;
    std::fill(acc[0].begin(), acc[0].end(), 0.0f);
    std::fill(acc[1].begin(), acc[1].end(), 0.0f);
}

void Stretch::Next(const int16_t* pcm, uint64_t frames, double srcPos, float& l, float& r) {
    if (readPos >= Hs) Refill(pcm, frames, srcPos);
    l = acc[0][readPos];
    r = acc[1][readPos];
    ++readPos;
}

void Stretch::Refill(const int16_t* pcm, uint64_t frames, double srcPos) {
    if (started) {
        const int keep = N - Hs;         // exactly one hop was emitted since the last refill
        std::memmove(acc[0].data(), acc[0].data() + Hs, sizeof(float) * keep);
        std::memmove(acc[1].data(), acc[1].data() + Hs, sizeof(float) * keep);
        std::fill(acc[0].begin() + keep, acc[0].end(), 0.0f);
        std::fill(acc[1].begin() + keep, acc[1].end(), 0.0f);
    } else {
        std::fill(acc[0].begin(), acc[0].end(), 0.0f);
        std::fill(acc[1].begin(), acc[1].end(), 0.0f);
        started  = true;
        havePrev = false;
    }

    const int maxTk   = (int)(frames - N);          // caller guarantees frames > N
    const int nominal = (int)llround(srcPos);
    int best = nominal < 0 ? 0 : (nominal > maxTk ? maxTk : nominal);

    if (havePrev) {
        // Reference: the source that would naturally have continued the previous window
        // once one synthesis hop of output had passed.
        int refP = (int)tPrev + Hs;
        if (refP < 0) refP = 0;
        if (refP + L > (int)frames) refP = (int)frames - L;
        float  ref[L];
        double refE = 1e-12;
        for (int i = 0; i < L; ++i) {
            const float m = (pcm[(refP + i) * 2] + pcm[(refP + i) * 2 + 1]) * (0.5f * kInvS16);
            ref[i] = m;
            refE  += double(m) * m;
        }
        double bestScore = -1e30;
        for (int tau = -D; tau <= D; ++tau) {
            int tk = nominal + tau;
            if (tk < 0) tk = 0;
            if (tk > maxTk) tk = maxTk;
            double dot = 0.0, e = 1e-12;
            for (int i = 0; i < L; ++i) {
                const float m = (pcm[(tk + i) * 2] + pcm[(tk + i) * 2 + 1]) * (0.5f * kInvS16);
                dot += double(m) * ref[i];
                e   += double(m) * m;
            }
            const double score = dot / sqrt(refE * e);
            if (score > bestScore) { bestScore = score; best = tk; }
        }
    }

    float* aL = acc[0].data();
    float* aR = acc[1].data();
    for (int i = 0; i < N; ++i) {
        const int f = (best + i) * 2;
        const float w = win[i] * kInvS16;
        aL[i] += w * (float)pcm[f];
        aR[i] += w * (float)pcm[f + 1];
    }
    tPrev    = best;
    havePrev = true;
    readPos  = 0;
}

// ------------------------------------------------------------------ engine
static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    auto* e = static_cast<Engine*>(pDevice->pUserData);
    if (!e) return;
    if (e->periodFrames.load(std::memory_order_relaxed) != frameCount)
        e->periodFrames.store(frameCount, std::memory_order_relaxed);
    e->RenderBlock(static_cast<float*>(pOutput), frameCount);
}

void Engine::InitOffline(uint32_t rate) {
    sampleRate = rate;
    for (int i = 0; i < 2; ++i) {
        deck[i].stretch.Init();
        for (int c = 0; c < 2; ++c)
            mix[i][c].assign(kMaxBlock, 0.0f);
    }
}

bool Engine::Start(std::string* err) {
    uint32_t    nativeRate = 0;
    std::string name;

    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
        ma_device_info* pPlayback = nullptr; ma_uint32 nPlayback = 0;
        ma_device_info* pCapture  = nullptr; ma_uint32 nCapture  = 0;
        if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback, &pCapture, &nCapture) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < nPlayback; ++i) {
                if (pPlayback[i].isDefault) {
                    name = pPlayback[i].name;
                    if (pPlayback[i].nativeDataFormatCount > 0)
                        nativeRate = pPlayback[i].nativeDataFormats[0].sampleRate;
                    break;
                }
            }
        }
        ma_context_uninit(&ctx);
    }
    if (nativeRate == 0) nativeRate = 48000;
    sampleRate = nativeRate;
    InitOffline(sampleRate);

    dev = new ma_device();
    auto tryInit = [&](ma_uint32 period, bool lowLatency, bool proAudio) -> ma_result {
        ma_device_config cfg   = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format    = ma_format_f32;
        cfg.playback.channels  = 2;
        cfg.sampleRate         = sampleRate;
        cfg.periodSizeInFrames = period;
        cfg.performanceProfile = lowLatency ? ma_performance_profile_low_latency
                                            : ma_performance_profile_conservative;
        cfg.noClip             = MA_TRUE;   // we run our own limiter
        cfg.wasapi.noAutoConvertSRC = MA_TRUE;
        if (proAudio) cfg.wasapi.usage = ma_wasapi_usage_pro_audio;
        cfg.dataCallback = DataCallback;
        cfg.pUserData    = this;
        return ma_device_init(nullptr, &cfg, dev);
    };

    ma_result r = tryInit(256, true, true);
    deviceNote  = "low-latency, 256-frame request";
    if (r != MA_SUCCESS) {
        r          = tryInit(0, true, true);
        deviceNote = "low-latency, default period";
    }
    if (r != MA_SUCCESS) {
        r          = tryInit(0, false, false);
        deviceNote = "conservative fallback";
    }
    if (r != MA_SUCCESS) {
        if (err) *err = std::string("ma_device_init: ") + ma_result_description(r);
        delete dev; dev = nullptr;
        return false;
    }

    sampleRate = dev->sampleRate;
    periodFrames.store(dev->playback.internalPeriodSizeInFrames);  // refined by the first callback
    deviceName   = dev->playback.name[0] ? dev->playback.name : (name.empty() ? "(default output)" : name);
    InitOffline(sampleRate);

    if (ma_device_start(dev) != MA_SUCCESS) {
        if (err) *err = "ma_device_start failed";
        ma_device_uninit(dev); delete dev; dev = nullptr;
        return false;
    }
    return true;
}

void Engine::Stop() {
    if (!dev) return;
    ma_device_uninit(dev);
    delete dev;
    dev = nullptr;
}

void Deck::Retire(Track* t) {
    if (!t) return;
    const uint32_t h    = retireHead.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1) % kRetireSlots;
    if (next == retireTail.load(std::memory_order_acquire)) return;  // full: never blocks the audio thread
    retireQ[h].store(t, std::memory_order_release);
    retireHead.store(next, std::memory_order_release);
}

Track* Deck::TakeRetired() {
    const uint32_t tail = retireTail.load(std::memory_order_relaxed);
    if (tail == retireHead.load(std::memory_order_acquire)) return nullptr;
    Track* t = retireQ[tail].load(std::memory_order_acquire);
    retireTail.store((tail + 1) % kRetireSlots, std::memory_order_release);
    return t;
}

bool Deck::RetireEmpty() const {
    return retireTail.load(std::memory_order_acquire) == retireHead.load(std::memory_order_acquire);
}

void Deck::ResetTransport() {
    frac = 0.0; chan = 0.0f; env = 1.0f; envStep = 0.0f; envLeft = 0;
    dk = 1.0f; dkStep = 0.0f; dkLeft = 0; jumpPending = false;
    lastFadeSeq = fadeSeq.load(std::memory_order_relaxed);
    playing.store(false, std::memory_order_relaxed);
    atEnd.store(false, std::memory_order_relaxed);
    pos.store(0, std::memory_order_relaxed);
    loopOn.store(false, std::memory_order_relaxed);
    loopIn.store(UINT64_MAX, std::memory_order_relaxed);
    loopOut.store(UINT64_MAX, std::memory_order_relaxed);
    for (auto& b : bq) b.reset();
    stretch.Reset();
    stretchOn = false;
}

uint32_t Engine::renderChunk(float* out, uint32_t n) {
    float gA = 0.0f, gB = 0.0f;
    CrossfaderGains(xfader.load(std::memory_order_relaxed), &gA, &gB);
    const float deckGain[2] = {gA, gB};
    const float master      = masterGain.load(std::memory_order_relaxed);

    for (int i = 0; i < 2; ++i) {
        Deck&  d = deck[i];
        float* L = mix[i][0].data();
        float* R = mix[i][1].data();
        std::memset(L, 0, sizeof(float) * n);
        std::memset(R, 0, sizeof(float) * n);

        Track* t = d.current.load(std::memory_order_acquire);
        if (!t) {
            d.meter[0].store(0.0f, std::memory_order_relaxed);
            d.meter[1].store(0.0f, std::memory_order_relaxed);
            continue;
        }

        // --- control snapshot -------------------------------------------------
        const float targetChan = dBToLin(d.faderDb.load(std::memory_order_relaxed));
        const float rate       = d.rate.load(std::memory_order_relaxed);
        const bool  play       = d.playing.load(std::memory_order_relaxed);

        const uint32_t fs = d.fadeSeq.load(std::memory_order_acquire);
        if (fs != d.lastFadeSeq) {
            d.lastFadeSeq = fs;
            const float    tt = d.fadeTarget.load(std::memory_order_relaxed);
            const uint32_t nf = d.fadeFrames.load(std::memory_order_relaxed);
            if (nf == 0) { d.env = tt; d.envLeft = 0; }
            else         { d.envStep = (tt - d.env) / (float)nf; d.envLeft = nf; }
        }

        const uint64_t st = d.seekReq.exchange(UINT64_MAX, std::memory_order_acq_rel);
        if (st != UINT64_MAX) {
            d.jumpTarget  = st >= t->frames ? t->frames - 1 : st;
            d.jumpPending = true;
            if (d.dkLeft == 0) { d.dkStep = -1.0f / (float)kDeclickFrames; d.dkLeft = kDeclickFrames; }
        }

        const int64_t scr = d.scrubFrames.exchange(0, std::memory_order_acq_rel);
        const bool scrubbing = scr != 0;
        const bool audible   = play || scrubbing;
        const double adv     = scrubbing ? (double)scr / (double)n : (double)rate;

        // keylock: with the rate away from 1.0 and KEYLOCK on, the source is time-stretched
        // so the pitch stays put; the direct resampler covers everything else (and scrubbing,
        // which is meant to sound like a turntable).
        const bool stretchable = d.keylock.load(std::memory_order_relaxed) && !scrubbing &&
                                 d.stretch.Ready() && t->frames > (uint64_t)Stretch::N;
        const bool useStretch  = stretchable && (rate < 0.999f || rate > 1.001f);
        if (useStretch != d.stretchOn) { d.stretch.Reset(); d.stretchOn = useStretch; }

        // --- EQ coefficients --------------------------------------------------
        float eqv[3];
        for (int b = 0; b < 3; ++b) eqv[b] = d.eqDb[b].load(std::memory_order_relaxed);
        if (eqv[0] != d.eqCache[0] || eqv[1] != d.eqCache[1] || eqv[2] != d.eqCache[2]) {
            d.eqCache[0] = eqv[0]; d.eqCache[1] = eqv[1]; d.eqCache[2] = eqv[2];
            const float fsf = (float)sampleRate;
            MakeLowShelf (d.bq[0], fsf,  200.0f, eqv[0]);
            MakePeak     (d.bq[1], fsf, 1200.0f, 1.0f, eqv[1]);
            MakeHighShelf(d.bq[2], fsf, 4000.0f, eqv[2]);
            d.eqBypass = fabsf(eqv[0]) < 0.01f && fabsf(eqv[1]) < 0.01f && fabsf(eqv[2]) < 0.01f;
        }

        const bool  loopOn = d.loopOn.load(std::memory_order_relaxed);
        const uint64_t loopIn = d.loopIn.load(std::memory_order_relaxed);
        const uint64_t loopOut = d.loopOut.load(std::memory_order_relaxed);
        const double lastFrame = (double)t->frames - 1.0;
        const int16_t* pcm = t->pcm.data();

        float mL = 0.0f, mR = 0.0f;
        for (uint32_t k = 0; k < n; ++k) {
            if (d.dkLeft) {
                d.dk += d.dkStep;
                if (d.dk <= 0.0f) {
                    d.dk = 0.0f;
                    if (d.jumpPending) {
                        d.frac        = (double)d.jumpTarget;
                        d.jumpPending = false;
                        d.atEnd.store(false, std::memory_order_relaxed);
                        d.dkStep      = 1.0f / (float)kDeclickFrames;
                        d.dkLeft      = kDeclickFrames;
                        d.stretch.Reset();           // the read head moved: restage the stretcher
                    } else { d.dkLeft = 0; d.dkStep = 0.0f; }
                } else if (d.dk >= 1.0f) {
                    d.dk = 1.0f; d.dkLeft = 0; d.dkStep = 0.0f;
                }
            }
            if (d.envLeft) {
                d.env += d.envStep;
                if (--d.envLeft == 0) {
                    d.env = d.envStep > 0.0f ? 1.0f : 0.0f;
                    if (d.env <= 0.0f && d.stopWhenFaded.load(std::memory_order_relaxed))
                        d.playing.store(false, std::memory_order_relaxed);
                }
            }

            float sl = 0.0f, sr = 0.0f;
            if (audible) {
                if (useStretch) {
                    d.stretch.Next(pcm, t->frames, d.frac, sl, sr);
                } else {
                    const uint64_t i0 = (uint64_t)d.frac;
                    if (i0 + 1 < t->frames) {
                        const float mu = (float)(d.frac - (double)i0);
                        const int16_t* p = pcm + i0 * 2;
                        sl = (p[0] + (p[2] - p[0]) * mu) * kInvS16;
                        sr = (p[1] + (p[3] - p[1]) * mu) * kInvS16;
                    }
                }
                d.frac += adv;
                if (loopOn && loopOut != UINT64_MAX && loopOut > loopIn && d.frac >= (double)loopOut) {
                    d.frac = (double)loopIn + (d.frac - (double)loopOut);
                    d.stretch.Reset();               // jumped back: restage the stretcher
                }
                if (d.frac >= lastFrame && !d.jumpPending && d.dkLeft == 0) {
                    d.frac = lastFrame;
                    d.playing.store(false, std::memory_order_relaxed);
                    d.atEnd.store(true, std::memory_order_relaxed);
                }
                const double tail = lastFrame - d.frac;
                if (tail < (double)kDeclickFrames) {
                    float tk = (float)(tail / (double)kDeclickFrames);
                    tk = tk < 0.0f ? 0.0f : tk;
                    sl *= tk; sr *= tk;
                }
                if (!d.eqBypass) {
                    sl = d.bq[2].process(d.bq[1].process(d.bq[0].process(sl, 0), 0), 0);
                    sr = d.bq[2].process(d.bq[1].process(d.bq[0].process(sr, 1), 1), 1);
                }
            }
            d.chan += (targetChan - d.chan) * 0.002f;
            const float g = d.chan * d.env * d.dk * deckGain[i];
            L[k] = sl * g;
            R[k] = sr * g;
            mL = std::max(mL, fabsf(L[k]));
            mR = std::max(mR, fabsf(R[k]));
        }
        d.meter[0].store(mL, std::memory_order_relaxed);
        d.meter[1].store(mR, std::memory_order_relaxed);
        d.pos.store((uint64_t)d.frac, std::memory_order_relaxed);
        d.envNow.store(d.env, std::memory_order_relaxed);
    }

    float peak = 0.0f;
    for (uint32_t k = 0; k < n; ++k) {
        float l = (mix[0][0][k] + mix[1][0][k]) * master;
        float r = (mix[0][1][k] + mix[1][1][k]) * master;
        const float a = std::max(fabsf(l), fabsf(r));
        if (a > 1.0f) {
            const float need = 1.0f / a;
            if (need < limGain) limGain = need;
        }
        limGain += (1.0f - limGain) * 0.00002f;
        if (limGain > 1.0f) limGain = 1.0f;
        l *= limGain; r *= limGain;
        l = std::max(-1.0f, std::min(1.0f, l));
        r = std::max(-1.0f, std::min(1.0f, r));
        out[k * 2]     = l;
        out[k * 2 + 1] = r;
        peak = std::max(peak, std::max(fabsf(l), fabsf(r)));
    }
    masterPeak.store(peak, std::memory_order_relaxed);
    return n;
}

void Engine::RenderBlock(float* out, uint32_t frames) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    // Adopt freshly loaded tracks / process ejects; hand old ones back to the UI thread.
    for (int i = 0; i < 2; ++i) {
        Deck& d = deck[i];
        if (d.ejectReq.exchange(false, std::memory_order_acq_rel)) {
            d.Retire(d.current.exchange(nullptr, std::memory_order_acq_rel));
            d.ResetTransport();
        }
        if (Track* nw = d.pending.exchange(nullptr, std::memory_order_acq_rel)) {
            d.Retire(d.current.exchange(nw, std::memory_order_acq_rel));
            d.ResetTransport();
        }
    }

    uint32_t done = 0;
    while (done < frames) {
        const uint32_t chunk = std::min(frames - done, kMaxBlock);
        renderChunk(out + (size_t)done * 2, chunk);
        done += chunk;
    }

    const double dt       = std::chrono::duration<double>(clock::now() - t0).count();
    const double blockSec = double(frames) / double(sampleRate ? sampleRate : 48000);
    const float  load     = float(dt / (blockSec > 0.0 ? blockSec : 1.0));
    cpuLoad.store(cpuLoad.load(std::memory_order_relaxed) * 0.9f + load * 0.1f, std::memory_order_relaxed);
    callbacks.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace dj
