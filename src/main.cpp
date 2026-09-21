// djdeck - two-deck DJ player demo.
// Windows: Win32 + Direct3D 11 + Dear ImGui (docking-free) + miniaudio (WASAPI).
//
//   djdeck                     interactive
//   djdeck --selftest          headless audio-engine test suite (exit code = failures)
//   djdeck --screenshot f.png  render the real UI, save a PNG, exit
//   djdeck --gendemo DIR       write demo_a.wav / demo_b.wav and exit
//   djdeck --load A.wav B.wav  start with two specific files
#include <windows.h>
#include <d3d11.h>
#include <commdlg.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "dj.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ------------------------------------------------------------------ d3d
static ID3D11Device*           g_dev   = nullptr;
static ID3D11DeviceContext*    g_ctx   = nullptr;
static IDXGISwapChain*         g_swap  = nullptr;
static ID3D11RenderTargetView* g_rtv   = nullptr;
static UINT                    g_pendingW = 0, g_pendingH = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}
static void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx)  { g_ctx->Release();  g_ctx  = nullptr; }
    if (g_dev)  { g_dev->Release();  g_dev  = nullptr; }
}
static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount        = 2;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL want[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL        got     = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               want, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev,
                                               &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // e.g. no GPU / remote session: fall back to WARP
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           want, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev,
                                           &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) { g_pendingW = LOWORD(lParam); g_pendingH = HIWORD(lParam); }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // disable ALT menu
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool SaveBackbufferPNG(const char* path) {
    ID3D11Texture2D* back = nullptr;
    if (!g_swap || FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.BindFlags      = 0;
    d.Usage          = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags      = 0;
    d.ArraySize      = 1;
    d.MipLevels      = 1;
    ID3D11Texture2D* stage = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &stage))) { back->Release(); return false; }
    g_ctx->CopyResource(stage, back);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    if (SUCCEEDED(g_ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        ok = stbi_write_png(path, (int)d.Width, (int)d.Height, 4, m.pData, (int)m.RowPitch) != 0;
        g_ctx->Unmap(stage, 0);
    }
    stage->Release();
    back->Release();
    return ok;
}

// ------------------------------------------------------------------ app state
static dj::Engine g_eng;
static bool       g_deviceOk = false;

struct DeckUI {
    double viewLen    = 0.0;   // visible window in seconds (0 => whole track), centred on the playhead
    double smoothPos  = 0.0;   // displayed position in seconds
    double posAnchor  = 0.0;   // last position reported by the audio thread
    double anchorTime = 0.0;   // ImGui time at which posAnchor last changed
    bool   dragging   = false; // a waveform drag owns the position this frame
    float  meter[2]  = {0, 0};
    float  hold[2]   = {0, 0};
    double holdT[2]  = {0, 0};

    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    const dj::Track*          texOwner = nullptr;
    int texW = 0;

    void ReleaseTex() {
        if (srv) { srv->Release(); srv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
        texOwner = nullptr;
        texW = 0;
    }
};
static DeckUI g_ui[2];

struct Loader {
    std::thread        th;
    std::atomic<bool>  busy{false};
    std::atomic<bool>  cancel{false};
    std::string        path;
    int                deck = 0;
};
static Loader g_loader[2];

static std::mutex              g_logMutex;
static char                    g_status[512] = "ready";
static std::vector<std::string> g_recent;
static int                     g_focus = 0;
static float                   g_fadeSecs = 4.0f;
static bool                    g_quit = false;
static std::string             g_exeDir;
static bool                    g_demoDrag = false;   // screenshot hook: render the drag overlay
static bool                    g_showMixer  = false;  // secondary panel, opened from the top bar (M)
static bool                    g_showPanels = true;   // browser + playlists strip (player page furniture)

// ---- playlists (one queue per deck)
// The deck owns playback; a playlist only decides what to play next when a track
// ends on a deck nobody paused. Modes are per queue, not global.
enum PlayMode { PlaySeq = 0, PlayAll, PlayOne, PlayShuffle };
static constexpr int kPlayModeCount = 4;
static const char* kPlayModeName[kPlayModeCount]  = {"SEQ", "ALL", "ONE", "SHUF"};
static const char* kPlayModeTip[kPlayModeCount]   = {
    "顺序播放:放完自动下一首,到队列末尾停止",
    "列表循环:放完最后一首回到第一首",
    "单曲循环:反复播放当前这首",
    "随机播放:一轮内不重复,且不会连播同一首",
};

struct Playlist {
    std::vector<std::string> items;
    int      current  = -1;          // item currently on the deck (-1 = not from the list)
    int      selected = -1;
    PlayMode mode     = PlaySeq;
    // shuffle bookkeeping: `order` is a permutation of the item indices, consumed
    // once per cycle, so a pass never repeats a track and never repeats back-to-back
    std::vector<int> order;
    int              orderPos = 0;
    uint32_t         rng      = 0x2545F491u;
};
static Playlist            g_pl[2];
// The deck must not auto-advance: set when the user pauses/loads/ejects the deck and
// when a queue runs out, cleared whenever playback is (re)started.
static std::atomic<bool>   g_halted[2] = {{true}, {true}};
static std::atomic<bool>   g_autoPending[2] = {{false}, {false}};  // start playing once the auto-load lands
// The track the UI is waiting for before it may (re)start a deck. Set right before a
// loader publishes, so a start can never land on the track that is on its way out.
static std::atomic<const dj::Track*> g_awaiting[2] = {{nullptr}, {nullptr}};
// Where to seek once the newly loaded track is really on the deck (-1 = start at 0).
static std::atomic<double> g_resumeAt[2] = {{-1.0}, {-1.0}};
// Remembered play positions (path -> seconds), captured while playing, saved on exit.
static std::map<std::string, double> g_positions;

// Deterministic LCG: shuffle order must be reproducible for the tests.
static inline uint32_t RngNext(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 9; }

// Rebuild the shuffle order (Fisher-Yates). `avoidFirst` is never placed at the
// front, which is what stops a cycle from starting with the track that just played.
static void ShuffleReset(Playlist& pl, int avoidFirst) {
    pl.order.resize(pl.items.size());
    for (int k = 0; k < (int)pl.order.size(); ++k) pl.order[k] = k;
    for (int k = (int)pl.order.size() - 1; k > 0; --k)
        std::swap(pl.order[k], pl.order[(int)(RngNext(pl.rng) % (uint32_t)(k + 1))]);
    if (avoidFirst >= 0 && pl.order.size() > 1 && pl.order[0] == avoidFirst)
        std::swap(pl.order[0], pl.order[1 + (int)(RngNext(pl.rng) % (uint32_t)(pl.order.size() - 1))]);
    pl.orderPos = 0;
}

// The index that follows `current` in this queue's mode, or -1 when playback stops.
// `commit` consumes the shuffle cursor: advance with true, peek with false. A peek
// never rebuilds a stale order and never touches the RNG, so predicting the next item
// can not change which item that will be.
static int PlaylistAdvance(Playlist& pl, bool commit) {
    const int n = (int)pl.items.size();
    if (n == 0) return -1;
    switch (pl.mode) {
        case PlaySeq:     return (pl.current + 1 < n) ? pl.current + 1 : -1;
        case PlayAll:     return (pl.current + 1) % n;
        case PlayOne:     return pl.current >= 0 ? pl.current : -1;
        case PlayShuffle:
            if ((int)pl.order.size() != n) {
                if (!commit) return -1;                  // stale order: only a real advance may rebuild
                ShuffleReset(pl, -1);
            }
            if (pl.orderPos >= (int)pl.order.size()) {
                if (!commit) return -1;                  // cycle boundary: the rebuild needs committing
                ShuffleReset(pl, pl.current);
            }
            return pl.order[commit ? pl.orderPos++ : pl.orderPos];
    }
    return -1;
}
static int PlaylistNext(Playlist& pl) { return PlaylistAdvance(pl, true); }
static int PlaylistPeek(Playlist& pl) { return PlaylistAdvance(pl, false); }

// Keep the shuffle cursor in step when the user starts a row by hand.
static void ShuffleNoteManual(Playlist& pl, int index) {
    if (pl.mode != PlayShuffle) return;
    if ((int)pl.order.size() == (int)pl.items.size()) {
        for (int k = 0; k < (int)pl.order.size(); ++k)
            if (pl.order[k] == index) { pl.orderPos = k + 1; return; }
    }
    ShuffleReset(pl, index);
}

// ---- file browser
struct BrowserEntry {
    std::string name, path;
    bool          isDir = false;
    unsigned long long size = 0;
};
struct Browser {
    std::string               dir;
    std::vector<BrowserEntry> entries;
    int                       selected = -1;
    bool                      needScan = true;
};
static Browser g_browser;

// ---- drag & drop payloads
static const char* kPayloadPath = "DJDECK_PATH";
static const char* kPayloadRow  = "DJDECK_PLROW";
struct PlRowPayload { int deck = 0; int index = 0; };

static std::string BaseNameOf(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static void Say(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        snprintf(g_status, sizeof(g_status), "%s", buf);
        printf("[djdeck] %s\n", buf);
        fflush(stdout);
    }
}

static std::string ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf);
    size_t s = w.find_last_of(L"\\/");
    std::wstring dir = (s == std::wstring::npos) ? L"." : w.substr(0, s);
    std::string out;
    out.assign(dir.begin(), dir.end());  // ASCII-safe enough for a demo path
    return out;
}

static bool FileExists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void FmtTime(double s, char* out, size_t n) {
    if (s < 0) s = 0;
    int    m   = (int)(s / 60.0);
    double rem = s - m * 60.0;
    snprintf(out, n, "%d:%06.3f", m, rem);
}

// ------------------------------------------------------------------ file browser
static bool DirExists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Exactly what the built-in decoders read: WAV / FLAC / MP3 / Vorbis.
// (Opus and M4A/AAC would need a custom decoding backend, so they are not listed.)
static bool HasAudioExt(const std::string& n) {
    static const char* kExt[] = {".wav", ".mp3", ".flac", ".ogg"};
    const size_t d = n.find_last_of('.');
    if (d == std::string::npos) return false;
    std::string e = n.substr(d);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (const char* x : kExt) if (e == x) return true;
    return false;
}

static std::string LowerOf(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

static void ScanBrowser() {
    g_browser.entries.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_browser.dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!isDir && !HasAudioExt(name)) continue;
            BrowserEntry e;
            e.name  = name;
            e.isDir = isDir;
            e.path  = g_browser.dir + "\\" + name;
            e.size  = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            g_browser.entries.push_back(e);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(g_browser.entries.begin(), g_browser.entries.end(),
              [](const BrowserEntry& x, const BrowserEntry& y) {
                  if (x.isDir != y.isDir) return x.isDir;
                  return LowerOf(x.name) < LowerOf(y.name);
              });
    g_browser.selected = -1;
    g_browser.needScan = false;
}

static void InitBrowser() {
    const char* up = getenv("USERPROFILE");
    std::string music = up ? (std::string(up) + "\\Music") : g_exeDir;
    g_browser.dir      = DirExists(music) ? music : g_exeDir;
    g_browser.needScan = true;
}

// ------------------------------------------------------------------ file picker
static bool PickFile(std::string* out) {
    wchar_t buf[2048] = {};
    OPENFILENAMEW ofn  = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetActiveWindow();
    ofn.lpstrFilter = L"Audio (*.wav;*.mp3;*.flac;*.ogg)\0*.wav;*.mp3;*.flac;*.ogg\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = 2048;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::wstring w(buf);
    out->assign(w.begin(), w.end());
    return true;
}

static void AddRecent(const std::string& p) {
    auto it = std::find(g_recent.begin(), g_recent.end(), p);
    if (it != g_recent.end()) g_recent.erase(it);
    g_recent.insert(g_recent.begin(), p);
    if (g_recent.size() > 10) g_recent.resize(10);
    FILE* f = fopen((g_exeDir + "\\djdeck_recent.txt").c_str(), "w");
    if (f) {
        for (auto& r : g_recent) fprintf(f, "%s\n", r.c_str());
        fclose(f);
    }
}
static void LoadRecents() {
    FILE* f = fopen((g_exeDir + "\\djdeck_recent.txt").c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) g_recent.push_back(line);
    }
    fclose(f);
}

// ------------------------------------------------------------------ session
// Remembers the queues, their play modes, the browser folder and the mixer state
// (djdeck_session.txt next to the exe). The audio settings stay untouched: this is
// about not losing the queue when the window closes.
static void SaveSession() {
    FILE* f = fopen((g_exeDir + "\\djdeck_session.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "mode %d %d\n", (int)g_pl[0].mode, (int)g_pl[1].mode);
    fprintf(f, "mixer %d\n", g_showMixer ? 1 : 0);
    fprintf(f, "panels %d\n", g_showPanels ? 1 : 0);
    if (!g_browser.dir.empty()) fprintf(f, "browser %s\n", g_browser.dir.c_str());
    for (int i = 0; i < 2; ++i)
        for (const auto& p : g_pl[i].items) fprintf(f, "item %c %s\n", 'A' + i, p.c_str());
    int written = 0;                                  // most recent plays first would need order; cap instead
    for (const auto& kv : g_positions) {
        if (written++ >= 400) break;
        fprintf(f, "pos %.2f %s\n", kv.second, kv.first.c_str());
    }
    fclose(f);
}

static void LoadSession() {
    FILE* f = fopen((g_exeDir + "\\djdeck_session.txt").c_str(), "r");
    if (!f) return;
    char line[1024];
    int  nA = 0, nB = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (strncmp(line, "mode ", 5) == 0) {
            int a = 0, b = 0;
            if (sscanf(line + 5, "%d %d", &a, &b) == 2) {
                g_pl[0].mode = (PlayMode)std::clamp(a, 0, (int)kPlayModeCount - 1);
                g_pl[1].mode = (PlayMode)std::clamp(b, 0, (int)kPlayModeCount - 1);
            }
        } else if (strncmp(line, "mixer ", 6) == 0) {
            g_showMixer = atoi(line + 6) != 0;
        } else if (strncmp(line, "panels ", 7) == 0) {
            g_showPanels = atoi(line + 7) != 0;
        } else if (strncmp(line, "browser ", 8) == 0) {
            if (DirExists(line + 8)) { g_browser.dir = line + 8; g_browser.needScan = true; }
        } else if (strncmp(line, "item A ", 7) == 0) {
            if (FileExists(line + 7)) { g_pl[0].items.push_back(line + 7); ++nA; }
        } else if (strncmp(line, "item B ", 7) == 0) {
            if (FileExists(line + 7)) { g_pl[1].items.push_back(line + 7); ++nB; }
        } else if (strncmp(line, "pos ", 4) == 0) {
            double      secs = 0.0;
            int         used = 0;
            if (sscanf(line + 4, "%lf %n", &secs, &used) == 1 && used > 0 && line[4 + used] &&
                g_positions.size() < 400)
                g_positions[line + 4 + used] = secs;
        }
    }
    fclose(f);
    if (nA || nB) Say("session restored: %d + %d queued tracks", nA, nB);
}

// ------------------------------------------------------------------ positions
// Where each track was left: sampled about once a second while a deck holds a real
// file, saved with the session and applied on the next deliberate load of that file.
static double g_captureT = 0.0;

static void CapturePositions(bool force = false) {
    const double now = dj::NowSeconds();
    if (!force && now - g_captureT < 1.0) return;
    g_captureT = now;
    for (int i = 0; i < 2; ++i) {
        dj::Deck&        d = g_eng.deck[i];
        const dj::Track* t = d.current.load(std::memory_order_acquire);
        if (!t || t->path.empty() || t->path[0] == '<') continue;   // synthetic tracks have no path
        const double pos = double(d.pos.load(std::memory_order_relaxed)) / t->sampleRate;
        const double dur = t->seconds();
        g_positions[t->path] = (pos > dur - 5.0) ? 0.0 : pos;       // played to the end -> start over
    }
}

// ------------------------------------------------------------------ preload
// The item the queue will play next is decoded *before* the current one ends, so the
// handover costs one audio block instead of a whole decode. The staged track is owned
// by the UI thread; it only reaches the audio thread when the deck actually ends.
static constexpr double kPreloadLead = 12.0;   // start staging this many seconds before the end

struct Staged {
    dj::Track*  track   = nullptr;   // decoded and waiting, UI-owned
    int         plIndex = -1;        // queue row it was decoded for
    std::string path;
};
static Staged g_staged[2];

struct Preload {
    std::thread             th;
    std::atomic<bool>       busy{false};
    std::atomic<bool>       cancel{false};
    std::atomic<dj::Track*> result{nullptr};
    int                     plIndex = -1;
    std::string             path;
};
static Preload g_pre[2];

// Drop the staged track and stop staging: the preload thread is never joined here
// (that would stall the UI for the length of a decode), it just publishes into a slot
// that the hand-off will free.
static void DiscardStaged(int deck) {
    g_pre[deck].cancel.store(true);
    if (g_staged[deck].track) {
        dj::FreeTrack(g_staged[deck].track);
        g_staged[deck].track = nullptr;
    }
    g_staged[deck].plIndex = -1;
}

// Called once per frame: hand a finished decode over, then arm a new one if the deck
// is close enough to the end of its track to need it.
static void UpdatePreload() {
    for (int i = 0; i < 2; ++i) {
        Preload&         P = g_pre[i];
        dj::Deck&        d = g_eng.deck[i];
        const dj::Track* cur = d.current.load(std::memory_order_acquire);

        if (!P.busy.load()) {
            if (dj::Track* t = P.result.exchange(nullptr)) {
                if (P.cancel.load()) {
                    dj::FreeTrack(t);
                } else {
                    if (g_staged[i].track) dj::FreeTrack(g_staged[i].track);
                    g_staged[i].track   = t;
                    g_staged[i].plIndex = P.plIndex;
                    g_staged[i].path    = P.path;
                    Say("deck %c: preloaded next  %s", 'A' + i, BaseNameOf(P.path).c_str());
                }
            }
        }
        const bool canStage = cur && d.playing.load(std::memory_order_relaxed) && !g_halted[i].load() &&
                              !g_loader[i].busy.load() && !P.busy.load() &&
                              !g_staged[i].track && g_staged[i].plIndex < 0;
        if (!canStage) continue;

        const int next = PlaylistPeek(g_pl[i]);
        if (next < 0 || next >= (int)g_pl[i].items.size()) continue;
        const double dur  = cur->seconds();
        const double pos  = double(d.pos.load(std::memory_order_relaxed)) / cur->sampleRate;
        const double rate = std::max(0.1, double(d.rate.load(std::memory_order_relaxed)));
        if ((dur - pos) / rate > kPreloadLead) continue;

        if (P.th.joinable()) P.th.join();                    // the previous decode is finished
        if (dj::Track* stale = P.result.exchange(nullptr)) dj::FreeTrack(stale);   // and irrelevant
        P.cancel.store(false);
        P.busy.store(true);
        P.plIndex = next;
        P.path    = g_pl[i].items[next];
        P.th = std::thread([i]() {
            Preload&    my = g_pre[i];
            std::string err;
            dj::Track*  t = dj::LoadTrack(my.path, g_eng.sampleRate, &err);
            if (!t) Say("deck %c: preload of %s failed (%s)", 'A' + i, BaseNameOf(my.path).c_str(), err.c_str());
            else    my.result.store(t);
            my.busy.store(false);
        });
    }
}

// ------------------------------------------------------------------ loading
static void StartLoad(int deckIdx, const std::string& path, bool autoAdvance = false) {
    Loader& L = g_loader[deckIdx];
    if (L.th.joinable()) {
        if (L.busy.load()) { Say("deck %c is still loading - please wait", 'A' + deckIdx); return; }
        L.th.join();
    }
    L.cancel.store(false);
    L.busy.store(true);
    if (!FileExists(path)) {
        L.busy.store(false);
        Say("deck %c: %s not found", 'A' + deckIdx, BaseNameOf(path).c_str());
        return;
    }
    L.path = path;
    L.deck = deckIdx;
    AddRecent(path);
    // A manual load always leaves the deck paused; a playlist advance takes over playing.
    g_autoPending[deckIdx].store(autoAdvance);
    if (!autoAdvance) g_halted[deckIdx].store(true);
    DiscardStaged(deckIdx);              // whatever was preloaded is no longer next
    g_awaiting[deckIdx].store(nullptr);  // only the loader may re-arm this now
    // Pick up where this file was left - but only for a deliberate load, never for a
    // queue advance (a queued track plays from the top).
    double resume = -1.0;
    if (!autoAdvance) {
        auto it = g_positions.find(path);
        if (it != g_positions.end() && it->second > 3.0) resume = it->second;
    }
    g_resumeAt[deckIdx].store(resume);
    int idx = -1;
    for (int k = 0; k < (int)g_pl[deckIdx].items.size(); ++k)
        if (g_pl[deckIdx].items[k] == path) { idx = k; break; }
    g_pl[deckIdx].current = idx;
    if (!autoAdvance && idx >= 0) ShuffleNoteManual(g_pl[deckIdx], idx);
    Say("deck %c: loading %s ...", 'A' + deckIdx, BaseNameOf(path).c_str());

    L.th = std::thread([deckIdx, path]() {
        Loader&   my = g_loader[deckIdx];
        std::string err;
        const double t0 = dj::NowSeconds();
        dj::Track* t = dj::LoadTrack(path, g_eng.sampleRate, &err);
        if (!t) {
            // Say why, and name the formats the decoders can actually read.
            const char* hint = HasAudioExt(path) ? "" : "  - only WAV/FLAC/MP3/OGG are decodable";
            Say("deck %c: FAILED to load %s (%s)%s", 'A' + deckIdx, BaseNameOf(path).c_str(),
                err.c_str(), hint);
            my.busy.store(false);
            g_autoPending[deckIdx].store(false);
            return;
        }
        // Never drop a track on the floor: wait for the UI to free the previous one.
        for (int spin = 0; spin < 5000 && !g_eng.deck[deckIdx].RetireEmpty(); ++spin)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (my.cancel.load()) { dj::FreeTrack(t); my.busy.store(false); return; }
        const double      secs  = t->seconds();
        const double      mb    = double(t->pcm.size() * 2) / 1e6;
        const std::string title = t->title;
        g_awaiting[deckIdx].store(t);            // before busy=false: the UI may only start once this is current
        g_eng.deck[deckIdx].pending.store(t, std::memory_order_release);
        Say("deck %c: %s loaded in %.2fs (%.1f MB, %.1f s)", 'A' + deckIdx, title.c_str(),
            dj::NowSeconds() - t0, mb, secs);
        my.busy.store(false);
    });
}

// The one rule for putting a file onto a deck: the deck must be paused, and it stays paused.
static bool TryDropOnDeck(int deck, const std::string& path) {
    if (g_eng.deck[deck].playing.load(std::memory_order_relaxed)) {
        Say("deck %c: refused %s - paused decks only", 'A' + deck, BaseNameOf(path).c_str());
        return false;
    }
    StartLoad(deck, path);
    Say("deck %c: %s loaded - deck stays paused", 'A' + deck, BaseNameOf(path).c_str());
    return true;
}

static void CollectRetired() {
    for (int i = 0; i < 2; ++i)
        while (dj::Track* old = g_eng.deck[i].TakeRetired()) dj::FreeTrack(old);
}

// ------------------------------------------------------------------ transport
// The player controls live in one place: deck row, keyboard shortcuts and the
// playlist advance all go through these.
constexpr float kRateMin = 0.5f, kRateMax = 2.0f;   // tape-style resampling range
constexpr float kTransportH = 40.0f;                // deck transport row (buttons are 30 high)

static void SetDeckRate(int i, float rate) {
    rate = std::clamp(rate, kRateMin, kRateMax);
    g_eng.deck[i].rate.store(rate);
    Say("deck %c: speed %.2fx", 'A' + i, double(rate));
}

static void TogglePlay(int i) {
    dj::Deck& d = g_eng.deck[i];
    if (!d.current.load(std::memory_order_acquire)) {
        Say("deck %c: nothing loaded", 'A' + i);
        return;
    }
    if (d.playing.load(std::memory_order_relaxed)) {
        d.playing.store(false);
        g_halted[i].store(true);
    } else {
        if (d.atEnd.load(std::memory_order_relaxed)) d.RequestSeek(0.0);   // replay from the top
        d.ArmFade(1.0f, 0, false);
        d.playing.store(true);
        g_halted[i].store(false);
    }
}

static void CueDeck(int i) {
    g_eng.deck[i].RequestSeek(0.0);
    g_eng.deck[i].playing.store(false);
    g_halted[i].store(true);
}

// ------------------------------------------------------------------ textures
static void EnsureWaveTexture(int i, const dj::Track* t) {
    DeckUI& u = g_ui[i];
    if (u.texOwner == t && u.srv) return;
    u.ReleaseTex();
    if (!t || t->peakW <= 0 || !g_dev) return;

    std::vector<uint8_t> rgba;
    const uint8_t col[2][3] = {{120, 200, 255}, {255, 170, 110}};  // deck A blue / deck B amber
    dj::BuildWaveformRGBA(*t, rgba, dj::kWaveImageH, col[i][0], col[i][1], col[i][2]);
    if (rgba.empty()) return;

    D3D11_TEXTURE2D_DESC d = {};
    d.Width            = (UINT)t->peakW;
    d.Height           = (UINT)dj::kWaveImageH;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_IMMUTABLE;
    d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub = {};
    sub.pSysMem     = rgba.data();
    sub.SysMemPitch = (UINT)t->peakW * 4;
    if (FAILED(g_dev->CreateTexture2D(&d, &sub, &u.tex))) return;
    if (FAILED(g_dev->CreateShaderResourceView(u.tex, nullptr, &u.srv))) { u.ReleaseTex(); return; }
    u.texOwner = t;
    u.texW     = t->peakW;
}

// ------------------------------------------------------------------ widgets
static constexpr float kPiF = 3.14159265358979f;

static void PanelBg(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float r = 8.0f) {
    dl->AddRectFilled(a, b, c, r);
}

// Knob with the name above and the value below the dial: the whole block is
// exactly (2 * text line + size) tall, which is what the mixer layout reserves.
static bool Knob(const char* id, const char* label, const char* valueText, float* v,
                 float vmin, float vmax, float size, ImU32 accent) {
    ImGui::PushID(id);
    ImDrawList*  dl    = ImGui::GetWindowDrawList();
    const float  lineH = ImGui::GetTextLineHeight();
    const ImVec2 p     = ImGui::GetCursorScreenPos();

    if (label) {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y), IM_COL32(142, 148, 162, 255), label);
    }

    const ImVec2 dial(p.x, p.y + lineH);
    ImGui::SetCursorScreenPos(dial);
    ImGui::InvisibleButton("##k", ImVec2(size, size));
    const bool active = ImGui::IsItemActive();
    const bool hover  = ImGui::IsItemHovered();
    ImGuiIO&   io     = ImGui::GetIO();
    if (active)                              *v = std::clamp(*v - io.MouseDelta.y * (vmax - vmin) * 0.006f, vmin, vmax);
    else if (hover && io.MouseWheel != 0.0f) *v = std::clamp(*v + io.MouseWheel * (vmax - vmin) * 0.03f, vmin, vmax);
    if (hover || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    const ImVec2 c(dial.x + size * 0.5f, dial.y + size * 0.5f);
    const float  r  = size * 0.40f;
    const float  a0 = kPiF * 0.75f, a1 = kPiF * 2.25f;
    const float  t  = (vmax > vmin) ? (*v - vmin) / (vmax - vmin) : 0.0f;
    const float  a  = a0 + t * (a1 - a0);
    dl->AddCircleFilled(c, r + 2.0f, IM_COL32(26, 27, 32, 255), 32);
    dl->AddCircle(c, r + 2.0f, IM_COL32(52, 54, 62, 255), 32, 1.0f);
    dl->PathArcTo(c, r, a0, a1, 32);
    dl->PathStroke(IM_COL32(62, 64, 74, 255), 0, 3.0f);
    if (t > 0.004f) {
        dl->PathArcTo(c, r, a0, a, 32);
        dl->PathStroke(accent, 0, 3.0f);
    }
    dl->AddLine(c, ImVec2(c.x + cosf(a) * (r - 3.0f), c.y + sinf(a) * (r - 3.0f)),
                IM_COL32(238, 240, 246, 255), 2.0f);

    if (valueText) {
        const ImVec2 ts = ImGui::CalcTextSize(valueText);
        dl->AddText(ImVec2(dial.x + (size - ts.x) * 0.5f, dial.y + size + 1.0f),
                    IM_COL32(224, 228, 236, 255), valueText);
    }
    // reserve the whole block so the caller's SameLine()/wrapping sees the full footprint
    ImGui::SetCursorScreenPos(ImVec2(p.x, dial.y + size + lineH));
    ImGui::PopID();
    return active;
}

static bool HFader(const char* id, float* v, ImVec2 size, const char* l, const char* r) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##f", size);
    const bool active = ImGui::IsItemActive();
    const bool hover  = ImGui::IsItemHovered();
    if (active) *v = std::clamp((ImGui::GetIO().MousePos.x - p.x) / size.x, 0.0f, 1.0f);
    if (hover && ImGui::GetIO().MouseWheel != 0.0f) *v = std::clamp(*v + ImGui::GetIO().MouseWheel * 0.03f, 0.0f, 1.0f);
    if (hover || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + size.y * 0.5f;
    const float h  = size.y * 0.22f;
    dl->AddRectFilled(ImVec2(p.x, cy - h * 0.5f), ImVec2(p.x + size.x, cy + h * 0.5f),
                      IM_COL32(30, 31, 37, 255), h * 0.5f);
    const float hx = p.x + size.x * (*v);
    dl->AddRectFilled(ImVec2(p.x, cy - h * 0.5f), ImVec2(hx, cy + h * 0.5f), IM_COL32(56, 92, 140, 255), h * 0.5f);
    dl->AddRectFilled(ImVec2(hx - 7.0f, p.y), ImVec2(hx + 7.0f, p.y + size.y),
                      active ? IM_COL32(240, 244, 250, 255) : IM_COL32(206, 212, 224, 255), 3.0f);
    // endpoint labels go under the track: the line above belongs to the section label
    dl->AddText(ImVec2(p.x, p.y + size.y + 2.0f), IM_COL32(140, 146, 160, 255), l);
    const ImVec2 ts = ImGui::CalcTextSize(r);
    dl->AddText(ImVec2(p.x + size.x - ts.x, p.y + size.y + 2.0f),
                IM_COL32(140, 146, 160, 255), r);
    ImGui::PopID();
    return active;
}

static void LevelMeter(const char* id, float l, float r, float hl, float hr, ImVec2 size) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float  w = (size.x - 3.0f) * 0.5f;
    for (int c = 0; c < 2; ++c) {
        const float v = std::clamp(c == 0 ? l : r, 0.0f, 1.0f);
        const ImVec2 a(p.x + c * (w + 3.0f), p.y);
        const ImVec2 b(a.x + w, p.y + size.y);
        dl->AddRectFilled(a, b, IM_COL32(34, 36, 44, 255), 2.0f);
        const float h  = size.y * v;
        const ImVec2 f(a.x, b.y - h);
        ImU32 col = v > 0.94f ? IM_COL32(235, 90, 80, 255)
                  : v > 0.72f ? IM_COL32(240, 200, 90, 255)
                              : IM_COL32(120, 220, 140, 255);
        dl->AddRectFilled(f, b, col, 2.0f);
        // segment lines
        for (int k = 1; k < 10; ++k) {
            const float y = b.y - size.y * (k / 10.0f);
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(0, 0, 0, 90), 1.0f);
        }
        const float hv = std::clamp(c == 0 ? hl : hr, 0.0f, 1.0f);
        if (hv > 0.01f) {
            const float y = b.y - size.y * hv;
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 180), 1.5f);
        }
    }
    ImGui::PopID();
}

// ------------------------------------------------------------------ waveform
struct WaveHit {
    bool  clicked = false;   // absolute jump requested
    bool  scrubbed = false;
    bool  zoomed = false;
};

// Keeps the displayed position in step with the audio clock, called exactly ONCE
// per deck per frame. The audio position advances in block-sized steps (~5.3 ms),
// so instead of free-running an integrator (which drifts against the audio clock
// and then snaps back - visible as the waveform jumping backwards) we extrapolate
// from the last reported position using the wall clock. Every new audio block
// re-anchors, so the two clocks can never drift apart.
static void UpdateDeckPosition(int i) {
    DeckUI& u = g_ui[i];
    dj::Deck&        d = g_eng.deck[i];
    const dj::Track* t = d.current.load(std::memory_order_acquire);
    const double now = ImGui::GetTime();
    if (!t) {
        u.smoothPos = u.posAnchor = 0.0;
        u.anchorTime = now;
        return;
    }
    const double posA    = double(d.pos.load(std::memory_order_relaxed)) / t->sampleRate;
    const double rate    = double(d.rate.load(std::memory_order_relaxed));
    const bool   playing = d.playing.load(std::memory_order_relaxed);

    if (u.dragging) {                    // the drag owns the position
        u.posAnchor  = posA;
        u.anchorTime = now;
        return;
    }
    if (posA != u.posAnchor) {           // a new audio block landed -> re-anchor
        u.posAnchor  = posA;
        u.anchorTime = now;
    }
    if (playing) {
        const double extra = std::min(now - u.anchorTime, 0.05);   // clamp for a stalled device
        const double shown = u.posAnchor + extra * rate;
        const double back  = u.smoothPos - shown;
        if (back > 0.05)          u.smoothPos = u.posAnchor;   // real discontinuity (backward seek)
        else if (shown > u.smoothPos) u.smoothPos = shown;     // normal advance
        // else: hold for this frame (sub-frame overshoot); never step backwards
    } else {
        u.smoothPos = posA;
    }
}

// Overlay shown while a file is being dragged: paused decks accept, playing decks refuse.
static void DrawDropHint(ImDrawList* dl, ImVec2 a, ImVec2 b, bool playing) {
    const ImU32 bg = playing ? IM_COL32(150, 48, 40, 70) : IM_COL32(40, 130, 88, 70);
    dl->AddRectFilled(a, b, bg, 7.0f);
    dl->AddRect(a, b, playing ? IM_COL32(235, 90, 80, 210) : IM_COL32(110, 225, 160, 210), 7.0f, 0, 2.0f);
    const char*  msg = playing ? "PLAYING - PAUSED DECKS ONLY" : "DROP TO LOAD   (deck stays paused)";
    const ImVec2 ts  = ImGui::CalcTextSize(msg);
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - 9.0f, c.y - ts.y * 0.5f - 4.0f),
                      ImVec2(c.x + ts.x * 0.5f + 9.0f, c.y + ts.y * 0.5f + 4.0f),
                      IM_COL32(12, 13, 16, 225), 4.0f);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(240, 242, 248, 255), msg);
}

// Scrolling waveform: the playhead is pinned to the centre and the audio scrolls
// right-to-left under it. Dragging grabs the waveform (the sample under the cursor
// stays under the cursor); the overview strip below shows the whole track.
static WaveHit DrawWaveform(int i, ImVec2 topLeft, ImVec2 size, bool overview) {
    WaveHit hit;
    DeckUI&          u  = g_ui[i];
    dj::Deck&        d  = g_eng.deck[i];
    const dj::Track* t  = d.current.load(std::memory_order_acquire);
    const bool playing  = d.playing.load(std::memory_order_relaxed);

    ImGui::PushID(overview ? (i + 100) : i);
    ImGui::SetCursorScreenPos(topLeft);
    ImGui::InvisibleButton("##wave", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool  active = ImGui::IsItemActive();
    const bool  hover  = ImGui::IsItemHovered();
    ImGuiIO&    io     = ImGui::GetIO();
    ImDrawList* dl     = ImGui::GetWindowDrawList();
    const ImVec2 p1(topLeft.x + size.x, topLeft.y + size.y);
    PanelBg(dl, topLeft, p1, overview ? IM_COL32(15, 16, 20, 255) : IM_COL32(19, 20, 25, 255),
            overview ? 4.0f : 7.0f);

    const ImGuiPayload* drag = g_demoDrag ? (const ImGuiPayload*)(uintptr_t)1 : ImGui::GetDragDropPayload();
    const bool draggingPath  = drag && (g_demoDrag || drag->IsDataType(kPayloadPath));

    // drop target: a paused deck accepts a file and stays paused
    if (!overview) {
        if (ImGui::BeginDragDropTarget()) {
            if (!playing) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadPath)) {
                    const std::string path((const char*)p->Data, (size_t)p->DataSize - 1);
                    TryDropOnDeck(i, path);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    const double dur = t ? t->seconds() : 0.0;
    if (!t || dur <= 0.0) {
        const char*  msg = t ? "no audio" : "empty deck  -  drag a file here (paused) or press LOAD";
        const ImVec2 ts  = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(topLeft.x + (size.x - ts.x) * 0.5f, topLeft.y + (size.y - ts.y) * 0.5f),
                    IM_COL32(96, 100, 112, 255), msg);
        if (!overview && draggingPath) DrawDropHint(dl, topLeft, p1, playing);
        ImGui::PopID();
        return hit;
    }

    // ---- view window (centred on the playhead). The position itself is maintained
    //      once per frame by UpdateDeckPosition(): neither this call nor the overview
    //      call below ever advances the clock, which is what used to make the two
    //      views fight each other and jump backwards.
    double vLen = u.viewLen > 0.0 ? u.viewLen : dur;
    vLen = std::clamp(vLen, std::min(1.0, dur), dur);
    const double pxPerSec = double(size.x) / vLen;

    if (active && !overview) {
        u.dragging = true;
        if (io.MouseDown[1]) {                                   // right drag = scratch (pitch follows the hand)
            const double dSec = -double(io.MouseDelta.x) / pxPerSec;
            d.scrubFrames.fetch_add((int64_t)llround(dSec * t->sampleRate), std::memory_order_acq_rel);
            u.smoothPos = std::clamp(u.smoothPos + dSec * 0.25, 0.0, dur);
            hit.scrubbed = true;
        } else if (io.MouseDown[0]) {                            // left drag = jump (grab the waveform)
            const double target = std::clamp(u.smoothPos - double(io.MouseDelta.x) / pxPerSec, 0.0, dur);
            d.RequestSeek(target);
            u.smoothPos = target;
            hit.clicked = true;
        }
    } else if (!overview) {
        u.dragging = false;
    }
    if (active && overview) {                                    // overview drag = absolute jump
        const double tt = std::clamp(double(io.MousePos.x - topLeft.x) / size.x, 0.0, 1.0);
        d.RequestSeek(tt * dur);
        u.smoothPos = tt * dur;
        hit.clicked = true;
    }
    const double pos = std::clamp(u.smoothPos, 0.0, dur);
    if (!overview && hover && io.MouseWheel != 0.0f) {
        u.viewLen = std::clamp(vLen * (io.MouseWheel > 0.0f ? 0.78f : 1.28f), std::min(1.5, dur), dur);
        hit.zoomed = true;
    }
    if (hover) ImGui::SetMouseCursor(io.MouseDown[1] ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Hand);

    const double a  = std::max(0.0, pos - vLen * 0.5);
    const double b  = std::min(dur, pos + vLen * 0.5);
    const float  cx = topLeft.x + size.x * 0.5f;

    // ---- grid
    if (!overview) {
        const double step = vLen > 240.0 ? 60.0 : vLen > 80.0 ? 30.0 : vLen > 30.0 ? 10.0 : 5.0;
        for (double g = ceil(a / step) * step; g <= b; g += step) {
            const float x = cx + float((g - pos) * pxPerSec);
            dl->AddLine(ImVec2(x, topLeft.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 14), 1.0f);
        }
    }

    // ---- waveform image (UV window follows the playhead)
    if (u.srv && u.texOwner == t) {
        const float xa = cx + float((a - pos) * pxPerSec);
        const float xb = cx + float((b - pos) * pxPerSec);
        dl->AddImage((ImTextureID)(uintptr_t)u.srv, ImVec2(xa, topLeft.y), ImVec2(xb, p1.y),
                     ImVec2((float)(a / dur), 0.0f), ImVec2((float)(b / dur), 1.0f), IM_COL32_WHITE);
    }

    // ---- playhead (fixed) or moving marker on the overview
    if (overview) {
        const float px = topLeft.x + float(pos / dur) * size.x;
        dl->AddRectFilled(ImVec2(px - 1.0f, topLeft.y), ImVec2(px + 1.0f, p1.y), IM_COL32(255, 245, 235, 225));
        if (vLen < dur - 0.001) {
            const float va = topLeft.x + float(a / dur) * size.x;
            const float vb = topLeft.x + float(b / dur) * size.x;
            dl->AddRectFilled(ImVec2(va, topLeft.y), ImVec2(vb, p1.y), IM_COL32(120, 200, 255, 34));
            dl->AddRect(ImVec2(va, topLeft.y), ImVec2(vb, p1.y), IM_COL32(150, 210, 255, 150), 2.0f);
        }
    } else {
        dl->AddRectFilled(ImVec2(cx - 1.0f, topLeft.y), ImVec2(cx + 1.0f, p1.y), IM_COL32(255, 245, 235, 240));
        dl->AddTriangleFilled(ImVec2(cx - 6.0f, topLeft.y), ImVec2(cx + 6.0f, topLeft.y),
                              ImVec2(cx, topLeft.y + 8.0f), IM_COL32(255, 245, 235, 255));
        if (d.atEnd.load(std::memory_order_relaxed))
            dl->AddText(ImVec2(cx + 10.0f, topLeft.y + 4.0f), IM_COL32(255, 190, 120, 220), "END");
        if (draggingPath) DrawDropHint(dl, topLeft, p1, playing);
    }

    if (hover && !overview) {
        char buf[64];
        FmtTime(std::clamp(pos + double(io.MousePos.x - cx) / pxPerSec, 0.0, dur), buf, sizeof(buf));
        ImGui::SetTooltip("%s", buf);
    }
    ImGui::PopID();
    return hit;
}

// ------------------------------------------------------------------ deck panel
// Everything in a deck block except the waveform window: padding, header, the
// transport row, the gap and the overview strip. The layout above shares this.
static float DeckChromeH(float hdrH, float waveH, float overviewH) {
    return 20.0f + hdrH + 8.0f + kTransportH + waveH + 4.0f + overviewH;
}

// The player controls: play/pause, cue, speed (0.5x - 2.0x), volume, level.
static void DrawDeckTransport(int i, ImVec2 topLeft, float width) {
    DeckUI&   u        = g_ui[i];
    dj::Deck& d        = g_eng.deck[i];
    const bool playing = d.playing.load(std::memory_order_relaxed);
    const float btnH   = 30.0f;
    ImGui::SetCursorScreenPos(topLeft);
    ImGui::PushID(i + 800);

    ImGui::PushStyleColor(ImGuiCol_Button, playing ? IM_COL32(200, 70, 60, 255) : IM_COL32(52, 110, 70, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          playing ? IM_COL32(225, 85, 72, 255) : IM_COL32(64, 130, 84, 255));
    if (ImGui::Button(playing ? "PAUSE" : "PLAY", ImVec2(92, btnH))) TogglePlay(i);
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("CUE", ImVec2(64, btnH))) CueDeck(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("回到开头并暂停");

    // speed: tape-style resampling, so the pitch follows the rate
    ImGui::SameLine(0.0f, 18.0f);
    float rate = d.rate.load(std::memory_order_relaxed);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("SPEED");
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("-##spd", ImVec2(28, btnH))) SetDeckRate(i, rate - 0.05f);
    ImGui::SameLine(0.0f, 4.0f);
    char rtxt[24];
    snprintf(rtxt, sizeof(rtxt), "%.2fx", double(rate));
    ImGui::Button(rtxt, ImVec2(72, btnH));
    if (ImGui::IsItemClicked()) SetDeckRate(i, 1.0f);        // the readout doubles as "reset"
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("点击回到 1.00x  ( - / = 调整当前音轨 )");
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("+##spd", ImVec2(28, btnH))) SetDeckRate(i, rate + 0.05f);

    static const float kPreset[5]  = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
    static const char* kPresetLbl[5] = {"0.75x", "1x", "1.25x", "1.5x", "2x"};
    for (int k = 0; k < 5; ++k) {
        ImGui::SameLine(0.0f, 6.0f);
        const bool on = fabsf(rate - kPreset[k]) < 0.001f;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 110, 165, 255));
        if (ImGui::Button(kPresetLbl[k], ImVec2(48, btnH))) SetDeckRate(i, kPreset[k]);
        if (on) ImGui::PopStyleColor();
    }

    // volume: the same channel fader the mixer window shows as GAIN
    ImGui::SameLine(0.0f, 18.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("VOL");
    ImGui::SameLine(0.0f, 8.0f);
    float db = d.faderDb.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::SliderFloat("##vol", &db, -40.0f, 6.0f, db <= -39.5f ? "-inf dB" : "%+.0f dB"))
        d.faderDb.store(db);

    ImGui::SameLine(0.0f, 14.0f);
    LevelMeter("dmeter", u.meter[0], u.meter[1], u.hold[0], u.hold[1], ImVec2(56, btnH - 6.0f));
    ImGui::PopID();
}

static void DrawDeck(int i, ImVec2 topLeft, float width, float waveH, float overviewH) {
    DeckUI&          u = g_ui[i];
    dj::Deck&        d = g_eng.deck[i];
    const dj::Track* t = d.current.load(std::memory_order_acquire);
    ImDrawList*      dl = ImGui::GetWindowDrawList();
    Loader&          L  = g_loader[i];

    const float pad  = 10.0f;
    const float hdrH = ImGui::GetFrameHeight();
    ImVec2      a(topLeft.x, topLeft.y);
    ImVec2      b(topLeft.x + width, topLeft.y + DeckChromeH(hdrH, waveH, overviewH));
    PanelBg(dl, a, b, IM_COL32(26, 27, 33, 255), 10.0f);

    // -------- header
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad));
    ImGui::PushID(i + 500);
    char deckName[8];
    snprintf(deckName, sizeof(deckName), "DECK %c", 'A' + i);
    ImGui::PushStyleColor(ImGuiCol_Text, i == 0 ? IM_COL32(150, 200, 255, 255) : IM_COL32(255, 180, 130, 255));
    ImGui::TextUnformatted(deckName);
    ImGui::PopStyleColor();
    ImGui::SameLine();

    char titleBuf[256];
    if (L.busy.load())        snprintf(titleBuf, sizeof(titleBuf), "loading...");
    else if (t)               snprintf(titleBuf, sizeof(titleBuf), "%s", t->title.c_str());
    else                      snprintf(titleBuf, sizeof(titleBuf), "(empty)");
    ImGui::TextDisabled("%s", titleBuf);
    ImGui::SameLine();
    if (g_pl[i].current >= 0) {
        ImGui::TextDisabled("[playlist %d/%d]", g_pl[i].current + 1, (int)g_pl[i].items.size());
        ImGui::SameLine();
    }

    const float rightButtonsW = 430.0f;
    ImGui::SetCursorScreenPos(ImVec2(a.x + width - pad - rightButtonsW, a.y + pad - 2.0f));

    if (ImGui::Button("LOAD", ImVec2(70, 0))) {
        std::string p;
        if (PickFile(&p)) StartLoad(i, p);
    }
    ImGui::SameLine();
    if (ImGui::Button("RECENT", ImVec2(78, 0))) ImGui::OpenPopup("##recent");
    if (ImGui::BeginPopup("##recent")) {
        for (auto& r : g_recent) {
            const char* slash = strrchr(r.c_str(), '\\');
            if (ImGui::Selectable(slash ? slash + 1 : r.c_str())) StartLoad(i, r);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.c_str());
        }
        if (g_recent.empty()) ImGui::TextDisabled("no recent files");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("EJECT", ImVec2(70, 0))) {
        d.ejectReq.store(true);
        DiscardStaged(i);
        g_awaiting[i].store(nullptr);
        g_resumeAt[i].store(-1.0);
        u.viewLen   = 0.0;
        u.smoothPos = 0.0;
        g_halted[i].store(true);
        g_pl[i].current = -1;
        Say("deck %c: ejected", 'A' + i);
    }
    ImGui::SameLine();
    const double pos = t ? double(d.pos.load(std::memory_order_relaxed)) / t->sampleRate : 0.0;
    char tbuf[32], ttot[32];
    FmtTime(pos, tbuf, sizeof(tbuf));
    FmtTime(t ? t->seconds() : 0.0, ttot, sizeof(ttot));
    ImGui::Text("%s / %s", tbuf, ttot);
    ImGui::PopID();

    if (L.busy.load()) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + hdrH + pad));
        ImGui::PushItemWidth(width - pad * 2.0f);
        ImGui::ProgressBar(-1.0f * fabsf(sinf((float)dj::NowSeconds() * 3.0f)), ImVec2(-1, 4), "");
        ImGui::PopItemWidth();
    }

    // -------- metering ballistics (the transport row shows the result)
    const float  mLv = d.meter[0].load(std::memory_order_relaxed);
    const float  mRv = d.meter[1].load(std::memory_order_relaxed);
    const double now = dj::NowSeconds();
    for (int c = 0; c < 2; ++c) {
        const float v = c == 0 ? mLv : mRv;
        u.meter[c] = std::max(v, u.meter[c] * 0.90f);
        if (v >= u.hold[c]) { u.hold[c] = v; u.holdT[c] = now; }
        else if (now - u.holdT[c] > 0.9) u.hold[c] *= 0.92f;
    }

    // -------- transport row: the player controls live on the deck, not in the mixer
    const float ty = a.y + hdrH + pad + (L.busy.load() ? 8.0f : 0.0f);
    DrawDeckTransport(i, ImVec2(a.x + pad, ty), width - pad * 2.0f);

    // -------- waveform + overview  (position is advanced exactly once per deck per frame)
    UpdateDeckPosition(i);
    const ImVec2 wavePos(a.x + pad, ty + kTransportH);
    const ImVec2 waveSize(width - pad * 2.0f, waveH);
    DrawWaveform(i, wavePos, waveSize, false);
    DrawWaveform(i, ImVec2(wavePos.x, wavePos.y + waveH + 4.0f),
                 ImVec2(width - pad * 2.0f, overviewH), true);
}

// ------------------------------------------------------------------ mixer panel
// Secondary window (top-bar button / M), flat grid per strip so nothing overlaps:
//   tag row      DECK A  <track title>
//   y0+0   (30)  LOOP 8s | FADE IN n s | FADE OUT n s
//   y0+42        GAIN + LOW/MID/HIGH knobs (label / 48 px dial / value), LEVEL meter right
// The player-side controls (play, cue, speed, volume) live on the deck itself.
static constexpr float kStripW = 384.0f;
static constexpr float kMixerH = 248.0f;

static void DrawMixer(ImVec2 topLeft, float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = topLeft;
    const ImVec2 b(topLeft.x + width, topLeft.y + height);
    PanelBg(dl, a, b, IM_COL32(26, 27, 33, 255), 10.0f);

    const float pad     = 14.0f;
    const float stripW  = kStripW;
    const float centerW = std::max(200.0f, width - stripW * 2.0f - pad * 2.0f);
    const float lineH   = ImGui::GetTextLineHeight();
    const float knobSz  = 48.0f;
    const float knobGap = 10.0f;

    auto place = [](float x, float y) { ImGui::SetCursorScreenPos(ImVec2(x, y)); };

    for (int i = 0; i < 2; ++i) {
        dj::Deck&        d  = g_eng.deck[i];
        const dj::Track* t  = d.current.load(std::memory_order_acquire);
        const float      x0 = (i == 0) ? a.x + pad : b.x - pad - stripW;
        const float      y0 = a.y + pad + 22.0f;
        ImGui::PushID(i + 900);

        // ---- strip tag: which deck, which track
        place(x0, a.y + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, i == 0 ? IM_COL32(150, 200, 255, 255) : IM_COL32(255, 180, 130, 255));
        ImGui::TextUnformatted(i == 0 ? "DECK A" : "DECK B");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("%.30s", t ? t->title.c_str() : "(empty)");

        // ---- row 0: the DJ-only transport bits (play/cue/speed/volume live on the deck)
        place(x0, y0);
        const bool loopOn = d.loopOn.load();
        if (loopOn) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 110, 165, 255));
        if (ImGui::Button(loopOn ? "LOOP 8s ON" : "LOOP 8s", ImVec2(100, 30))) {
            const bool on = !loopOn;
            if (on && t) {
                const uint64_t p   = d.pos.load();
                const uint64_t len = t->sampleRate * 8;   // 8-second loop (no beatgrid in this demo)
                d.loopIn.store(p);
                d.loopOut.store(std::min<uint64_t>(t->frames, p + len));
            }
            d.loopOn.store(on);
        }
        if (loopOn) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("从当前位置开始 8 秒循环");
        ImGui::SameLine(0.0f, 8.0f);
        char fl[24], fr[24];
        snprintf(fl, sizeof(fl), "FADE IN %.0fs", g_fadeSecs);
        snprintf(fr, sizeof(fr), "FADE OUT %.0fs", g_fadeSecs);
        if (ImGui::Button(fl, ImVec2(130, 30)))
            d.ArmFade(1.0f, (uint32_t)(g_fadeSecs * g_eng.sampleRate), false);
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button(fr, ImVec2(130, 30)))
            d.ArmFade(0.0f, (uint32_t)(g_fadeSecs * g_eng.sampleRate), true);

        // ---- row 1: channel trim + 3-band EQ, level meter on the right
        const float row2 = y0 + 42.0f;
        place(x0, row2);
        {
            float gain = d.faderDb.load();
            char  txt[24];
            snprintf(txt, sizeof(txt), gain <= -39.5f ? "-inf" : "%+.0f dB", gain);
            if (Knob("gain", "GAIN", txt, &gain, -40.0f, 6.0f, knobSz, IM_COL32(150, 200, 255, 255)))
                d.faderDb.store(gain);
        }
        static const char* eqLabels[3] = {"LOW", "MID", "HIGH"};
        for (int k = 0; k < 3; ++k) {
            place(x0 + (knobSz + knobGap) * (k + 1), row2);
            float v = d.eqDb[k].load();
            char  txt[24];
            snprintf(txt, sizeof(txt), v <= -23.5f ? "KILL" : "%+.0f dB", v);
            char id[16];
            snprintf(id, sizeof(id), "eq%d", k);
            if (Knob(id, eqLabels[k], txt, &v, -24.0f, 6.0f, knobSz, IM_COL32(180, 220, 255, 255)))
                d.eqDb[k].store(v <= -23.9f ? -96.0f : v);
        }
        place(x0 + stripW - 62.0f, row2);
        ImGui::TextDisabled("LEVEL");
        place(x0 + stripW - 62.0f, row2 + lineH);
        LevelMeter("meter", g_ui[i].meter[0], g_ui[i].meter[1],
                   g_ui[i].hold[0], g_ui[i].hold[1], ImVec2(46, 92));
        ImGui::PopID();
    }

    // ---- centre column: crossfader, auto-mix, fade time, master
    {
        const float cx = a.x + pad + stripW + std::max(0.0f, (centerW - 300.0f) * 0.5f);
        float       y  = a.y + pad;
        ImGui::PushID(777);
        place(cx, y);
        ImGui::TextDisabled("CROSSFADER");
        place(cx + 190.0f, y - 2.0f);
        if (ImGui::SmallButton("XFADE >")) {
            const int from = g_eng.xfader.load() < 0.5f ? 0 : 1;
            const int to   = 1 - from;
            dj::Deck& dt   = g_eng.deck[to];
            const dj::Track* tt = dt.current.load();
            if (!tt) Say("nothing loaded on deck %c", 'A' + to);
            else {
                if (dt.atEnd.load() || dt.pos.load() + 1000 >= tt->frames) dt.RequestSeek(0.0);
                dt.ArmFade(1.0f, (uint32_t)(g_fadeSecs * 1.5f * g_eng.sampleRate), false);
                dt.playing.store(true);
                g_halted[to].store(false);
                g_eng.deck[from].ArmFade(0.0f, (uint32_t)(g_fadeSecs * 1.5f * g_eng.sampleRate), true);
                Say("auto-mix: %c -> %c over %.1fs", 'A' + from, 'A' + to, g_fadeSecs * 1.5f);
            }
        }
        y += 22.0f;
        place(cx, y);
        float xf = g_eng.xfader.load();
        if (HFader("xf", &xf, ImVec2(240, 34), "A", "B")) g_eng.xfader.store(xf);

        y += 34.0f + 26.0f;                          // track + the A/B label line under it
        place(cx, y);
        float fadeSec = g_fadeSecs;
        ImGui::PushItemWidth(150.0f);
        if (ImGui::SliderFloat("fade (s)", &fadeSec, 1.0f, 16.0f, "%.0f")) g_fadeSecs = fadeSec;
        ImGui::PopItemWidth();

        y += 42.0f;
        place(cx, y);
        float mg = g_eng.masterGain.load();
        char  mtxt[24];
        snprintf(mtxt, sizeof(mtxt), "%.0f%%", mg * 100.0f);
        if (Knob("master", "MASTER", mtxt, &mg, 0.0f, 1.2f, 52.0f, IM_COL32(255, 220, 150, 255)))
            g_eng.masterGain.store(mg);
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::BeginGroup();
        const float mp = g_eng.masterPeak.load();
        LevelMeter("mmeter", mp, mp, mp, mp, ImVec2(50, 70));
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

// ------------------------------------------------------------------ mixer window
// The mixer is a secondary panel now: closed by default, opened from the top bar
// (or M), floating above the player page. Its own window keeps the main page clean
// and leaves the full height to the waveforms.
static constexpr float kMixerW = kStripW * 2.0f + 348.0f;   // two strips + the centre column

static void DrawMixerWindow() {
    if (!g_showMixer) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(kMixerW, kMixerH + 46.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(std::max(0.0f, io.DisplaySize.x - kMixerW - 40.0f),
                                   std::max(0.0f, io.DisplaySize.y - kMixerH - 150.0f)),
                            ImGuiCond_FirstUseEver);
    if (ImGui::Begin("MIXER  混音台", &g_showMixer,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const ImVec2 o = ImGui::GetCursorScreenPos();
        DrawMixer(o, kMixerW, kMixerH);
        ImGui::Dummy(ImVec2(kMixerW, kMixerH));
    }
    ImGui::End();
}

// ------------------------------------------------------------------ file browser
static void DrawBrowserPanel(ImVec2 topLeft, ImVec2 size) {
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = topLeft;
    const ImVec2 b(topLeft.x + size.x, topLeft.y + size.y);
    PanelBg(dl, a, b, IM_COL32(26, 27, 33, 255), 10.0f);
    if (g_browser.needScan) ScanBrowser();

    const float pad   = 10.0f;
    const float footH = ImGui::GetFrameHeight() + 10.0f;
    ImGui::PushID(4242);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad));
    ImGui::TextDisabled("FILES");
    ImGui::SameLine();
    if (ImGui::SmallButton("up")) {
        const size_t s = g_browser.dir.find_last_of('\\');
        if (s != std::string::npos && s >= 3) {
            g_browser.dir      = g_browser.dir.substr(0, s);
            g_browser.needScan = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh")) g_browser.needScan = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_browser.dir.c_str());

    const float listTop = a.y + pad + ImGui::GetFrameHeight() + 6.0f;
    const float listH   = std::max(40.0f, b.y - footH - listTop);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, listTop));
    ImGui::BeginChild("##files", ImVec2(size.x - pad * 2.0f, listH), true);
    if (g_browser.entries.empty()) ImGui::TextDisabled("no audio files here");
    for (int i = 0; i < (int)g_browser.entries.size(); ++i) {
        BrowserEntry& e = g_browser.entries[i];
        char          label[320];
        if (e.isDir) snprintf(label, sizeof(label), "%-40s  <dir>", e.name.c_str());
        else         snprintf(label, sizeof(label), "%-40s %6.1f MB", e.name.c_str(), double(e.size) / 1e6);
        ImGui::PushID(i);
        if (ImGui::Selectable(label, g_browser.selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            g_browser.selected = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.isDir) {
                    g_browser.dir      = e.path;
                    g_browser.needScan = true;
                } else {
                    TryDropOnDeck(g_focus, e.path);
                }
            }
        }
        if (!e.isDir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload(kPayloadPath, e.path.c_str(), e.path.size() + 1);
            ImGui::Text("load  %s", e.name.c_str());
            ImGui::TextDisabled("drop on a paused deck, or into a playlist");
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n\ndrag onto a paused deck, or into a playlist", e.path.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, b.y - footH + 3.0f));
    const bool has = g_browser.selected >= 0 && g_browser.selected < (int)g_browser.entries.size() &&
                     !g_browser.entries[g_browser.selected].isDir;
    ImGui::BeginDisabled(!has);
    if (ImGui::Button("+ queue A")) g_pl[0].items.push_back(g_browser.entries[g_browser.selected].path);
    ImGui::SameLine();
    if (ImGui::Button("+ queue B")) g_pl[1].items.push_back(g_browser.entries[g_browser.selected].path);
    ImGui::SameLine();
    if (ImGui::Button("load paused")) TryDropOnDeck(g_focus, g_browser.entries[g_browser.selected].path);
    ImGui::EndDisabled();
    ImGui::PopID();
}

// ------------------------------------------------------------------ playlist
static void DrawPlaylistPanel(int i, ImVec2 topLeft, ImVec2 size) {
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = topLeft;
    const ImVec2 b(topLeft.x + size.x, topLeft.y + size.y);
    PanelBg(dl, a, b, IM_COL32(26, 27, 33, 255), 10.0f);
    Playlist& pl = g_pl[i];
    dj::Deck& d  = g_eng.deck[i];

    const float pad   = 10.0f;
    const float footH = ImGui::GetFrameHeight() + 10.0f;
    ImGui::PushID(5000 + i);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad));
    ImGui::PushStyleColor(ImGuiCol_Text, i == 0 ? IM_COL32(150, 200, 255, 255) : IM_COL32(255, 180, 130, 255));
    ImGui::Text("PLAYLIST %c", 'A' + i);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 12.0f);
    {   // how this queue continues when a track ends on a deck nobody paused
        int m = (int)pl.mode;
        ImGui::SetNextItemWidth(92.0f);
        if (ImGui::Combo("##mode", &m, kPlayModeName, kPlayModeCount)) pl.mode = (PlayMode)m;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPlayModeTip[(int)pl.mode]);
    }
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextDisabled("%d tracks", (int)pl.items.size());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("把文件拖到这里加入队列;列表内拖动可排序");

    const float listTop = a.y + pad + ImGui::GetFrameHeight() + 6.0f;
    const float listH   = std::max(40.0f, b.y - footH - listTop);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, listTop));
    ImGui::BeginChild("##pl", ImVec2(size.x - pad * 2.0f, listH), true);

    int mvSrcDeck = -1, mvSrcRow = -1, mvDstRow = -1;
    if (pl.items.empty()) ImGui::TextDisabled("queue empty - drag audio files here");
    for (int r = 0; r < (int)pl.items.size(); ++r) {
        const bool cur = (r == pl.current);
        char       label[320];
        snprintf(label, sizeof(label), "%s %2d  %s", cur ? ">" : " ", r + 1,
                 BaseNameOf(pl.items[r]).c_str());
        ImGui::PushID(r);
        if (ImGui::Selectable(label, pl.selected == r)) pl.selected = r;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            PlRowPayload row;
            row.deck  = i;
            row.index = r;
            ImGui::SetDragDropPayload(kPayloadRow, &row, sizeof(row));
            ImGui::Text("move  %s", BaseNameOf(pl.items[r]).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadPath))
                pl.items.insert(pl.items.begin() + r, std::string((const char*)p->Data, (size_t)p->DataSize - 1));
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadRow)) {
                const auto* rr = (const PlRowPayload*)p->Data;
                mvSrcDeck = rr->deck;
                mvSrcRow  = rr->index;
                mvDstRow  = r;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pl.items[r].c_str());
        ImGui::PopID();
    }
    ImGui::PushID("tail");
    ImGui::Dummy(ImVec2(10.0f, 16.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadPath))
            pl.items.push_back(std::string((const char*)p->Data, (size_t)p->DataSize - 1));
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadRow)) {
            const auto* rr = (const PlRowPayload*)p->Data;
            mvSrcDeck = rr->deck;
            mvSrcRow  = rr->index;
            mvDstRow  = (int)pl.items.size();
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    ImGui::EndChild();

    if (mvSrcDeck >= 0 && mvSrcRow >= 0 && mvSrcRow < (int)g_pl[mvSrcDeck].items.size()) {
        const std::string v = g_pl[mvSrcDeck].items[mvSrcRow];
        g_pl[mvSrcDeck].items.erase(g_pl[mvSrcDeck].items.begin() + mvSrcRow);
        int dst = mvDstRow;
        if (mvSrcDeck == i && mvSrcRow < dst) dst--;
        dst = std::clamp(dst, 0, (int)pl.items.size());
        pl.items.insert(pl.items.begin() + dst, v);
        if (mvSrcDeck == i && pl.current == mvSrcRow) pl.current = dst;
        Say("playlist %c: moved to #%d", 'A' + i, dst + 1);
    }

    const bool sel = pl.selected >= 0 && pl.selected < (int)pl.items.size();
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, b.y - footH + 3.0f));
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("play")) {
        pl.current = pl.selected;
        ShuffleNoteManual(pl, pl.selected);
        StartLoad(i, pl.items[pl.selected]);
        g_autoPending[i].store(true);          // loading is async: start it as soon as it lands
        g_halted[i].store(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("remove")) {
        pl.items.erase(pl.items.begin() + pl.selected);
        if (pl.current >= (int)pl.items.size()) pl.current = -1;
        pl.selected = std::min(pl.selected, (int)pl.items.size() - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("clear")) {
        pl.items.clear();
        pl.current  = -1;
        pl.selected = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(d.atEnd.load() ? "ended" : (d.playing.load() ? "playing" : "paused"));
    ImGui::PopID();
}

// ------------------------------------------------------------------ status bar
static void DrawStatusBar(ImVec2 topLeft, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h  = ImGui::GetTextLineHeight() + 10.0f;
    PanelBg(dl, topLeft, ImVec2(topLeft.x + width, topLeft.y + h), IM_COL32(21, 22, 27, 255), 6.0f);
    ImGui::SetCursorScreenPos(ImVec2(topLeft.x + 10.0f, topLeft.y + 5.0f));
    ImGui::TextDisabled("%s", g_status);
    ImGui::SameLine();
    ImGui::TextDisabled("| %s | %.0f Hz | %u frames (%.1f ms) | cpu %.1f%% | callbacks %llu",
                        g_deviceOk ? g_eng.deviceName.c_str() : "NO AUDIO DEVICE",
                        double(g_eng.sampleRate), g_eng.periodFrames.load(),
                        g_eng.sampleRate ? 1000.0 * g_eng.periodFrames.load() / g_eng.sampleRate : 0.0,
                        double(g_eng.cpuLoad.load()) * 100.0,
                        (unsigned long long)g_eng.callbacks.load());
}

// ------------------------------------------------------------------ playlist advance
// "If the deck is not paused, keep going through the queue - the queue's mode says how."
static void UpdatePlaylists() {
    for (int i = 0; i < 2; ++i) {
        dj::Deck& d  = g_eng.deck[i];
        Playlist& pl = g_pl[i];

        // ---- a remembered position is applied only once the new track really is on the
        //      deck: seeking before the swap would seek the outgoing track
        const double resume = g_resumeAt[i].load();
        if (resume >= 0.0 && !g_loader[i].busy.load()) {
            const dj::Track* want = g_awaiting[i].load();
            const dj::Track* cur  = d.current.load(std::memory_order_acquire);
            if (cur && (!want || cur == want)) {
                const double at = std::min(resume, std::max(0.0, cur->seconds() - 1.0));
                d.RequestSeek(at);
                g_ui[i].smoothPos = at;
                g_resumeAt[i].store(-1.0);
                Say("deck %c: resuming %s at %.1f s", 'A' + i, BaseNameOf(cur->title).c_str(), at);
            }
        }

        // ---- an auto-advanced (or hand-started) track begins as soon as it is current:
        //      waiting for `g_awaiting` is what keeps a stale start off the outgoing track
        if (g_autoPending[i].load() && !g_loader[i].busy.load()) {
            const dj::Track* want = g_awaiting[i].load();
            const dj::Track* cur  = d.current.load(std::memory_order_acquire);
            if (cur && (!want || cur == want) && !d.playing.load(std::memory_order_relaxed)) {
                d.ArmFade(1.0f, 0, false);
                d.playing.store(true);
                g_halted[i].store(false);
                g_autoPending[i].store(false);
                g_awaiting[i].store(nullptr);
                Say("deck %c: playing playlist %d/%d  %s", 'A' + i, pl.current + 1, (int)pl.items.size(),
                    pl.current >= 0 ? BaseNameOf(pl.items[pl.current]).c_str() : "?");
            }
        }

        // ---- track ended while the deck was not user-paused -> the queue decides what is next
        if (d.atEnd.load() && !g_halted[i].load() && !g_loader[i].busy.load() && !g_autoPending[i].load()) {
            const int next = PlaylistNext(pl);
            if (next >= 0) {
                const bool same = (next == pl.current);
                pl.current  = next;                    // repeat-one keeps the same item
                pl.selected = next;
                if (g_staged[i].track && g_staged[i].plIndex == next) {
                    // already decoded: hand it over and the gap is one audio block
                    dj::Track* t = g_staged[i].track;
                    g_staged[i].track   = nullptr;
                    g_staged[i].plIndex = -1;
                    g_resumeAt[i].store(-1.0);         // a queue item plays from the top
                    g_awaiting[i].store(t);
                    d.pending.store(t, std::memory_order_release);
                    g_autoPending[i].store(true);
                    Say("playlist %c: %s %d/%d  %s (preloaded, no gap)", 'A' + i, same ? "repeat" : "next",
                        next + 1, (int)pl.items.size(), BaseNameOf(pl.items[next]).c_str());
                } else {
                    DiscardStaged(i);
                    Say("playlist %c: %s %d/%d  %s", 'A' + i, same ? "repeat" : "next", next + 1,
                        (int)pl.items.size(), BaseNameOf(pl.items[next]).c_str());
                    StartLoad(i, pl.items[next], true);
                }
            } else {
                // nothing left to play: park the deck (this also stops us re-entering here)
                g_halted[i].store(true);
                DiscardStaged(i);
                Say("playlist %c: end of queue (%d/%d, %s)", 'A' + i,
                    pl.current + 1, (int)pl.items.size(), kPlayModeName[(int)pl.mode]);
            }
        }
    }
}

// ------------------------------------------------------------------ app frame
static void DrawUI() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("djdeck", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (ImGui::BeginMenuBar()) {
        ImGui::TextColored(ImVec4(0.62f, 0.78f, 1.00f, 1.0f), "djdeck");
        ImGui::SameLine(0.0f, 12.0f);
        // the top-bar buttons: the player page is the default view
        if (ImGui::Button(g_showMixer ? "MIXER  ON" : "MIXER")) g_showMixer = !g_showMixer;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("打开/关闭混音台窗口 (M)");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button(g_showPanels ? "PANELS  ON" : "PANELS")) g_showPanels = !g_showPanels;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("文件浏览器 + 播放列表");
        ImGui::SameLine(0.0f, 14.0f);

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load into deck A...", "L")) {
                std::string p;
                if (PickFile(&p)) StartLoad(0, p);
            }
            if (ImGui::MenuItem("Load into deck B...")) {
                std::string p;
                if (PickFile(&p)) StartLoad(1, p);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load demo tracks")) {
                std::string err;
                const std::string pa = g_exeDir + "\\demo_a.wav", pb = g_exeDir + "\\demo_b.wav";
                if (!FileExists(pa)) dj::GenerateDemoWav(pa, 45.0, 124.0, 0, g_eng.sampleRate, &err);
                if (!FileExists(pb)) dj::GenerateDemoWav(pb, 45.0, 124.0, 1, g_eng.sampleRate, &err);
                StartLoad(0, pa);
                StartLoad(1, pb);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) g_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("Space play/pause   Q cue   M mixer   1/2 focus", nullptr, false, false);
            ImGui::MenuItem("left/right seek 5 s   - / = speed   L load   X xfade", nullptr, false, false);
            ImGui::MenuItem("drag waveform = seek, right-drag = scrub, wheel = zoom", nullptr, false, false);
            ImGui::MenuItem("playlist modes: SEQ / ALL / ONE / SHUF", nullptr, false, false);
            ImGui::EndMenu();
        }

        // right-aligned: which deck the keyboard talks to
        char right[128];
        snprintf(right, sizeof(right), "focus: DECK %c   |   %s   |   %s", 'A' + g_focus,
                 g_deviceOk ? g_eng.deviceName.c_str() : "NO AUDIO DEVICE",
                 g_showMixer ? "mixer open" : "player");
        const float rw = ImGui::CalcTextSize(right).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - rw - 16.0f));
        ImGui::TextDisabled("%s", right);
        ImGui::EndMenuBar();
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const float pad       = 10.0f;
    const float statusH   = ImGui::GetTextLineHeight() + 10.0f;
    const float hdrH      = ImGui::GetFrameHeight();
    const float overviewH = 26.0f;
    const float bottomH   = g_showPanels ? std::clamp(avail.y * 0.26f, 160.0f, 280.0f) : 0.0f;
    const float vpads     = pad * (g_showPanels ? 4.0f : 3.0f);

    const float deckBlockH = (avail.y - statusH - bottomH - vpads) * 0.5f;
    const float waveH      = std::max(56.0f, deckBlockH - DeckChromeH(hdrH, 0.0f, overviewH));

    float y = origin.y;
    DrawDeck(0, ImVec2(origin.x, y), avail.x, waveH, overviewH);
    y += deckBlockH + pad;
    DrawDeck(1, ImVec2(origin.x, y), avail.x, waveH, overviewH);
    y += deckBlockH + pad;

    if (g_showPanels) {
        const float inner = avail.x - pad * 2.0f;
        const float bw    = std::max(280.0f, inner * 0.42f);
        const float pw    = std::max(190.0f, (inner - bw - pad * 2.0f) * 0.5f);
        DrawBrowserPanel(ImVec2(origin.x, y), ImVec2(bw, bottomH));
        DrawPlaylistPanel(0, ImVec2(origin.x + bw + pad, y), ImVec2(pw, bottomH));
        DrawPlaylistPanel(1, ImVec2(origin.x + bw + pw + pad * 2.0f, y), ImVec2(pw, bottomH));
        y += bottomH + pad * 0.5f;
    }

    DrawStatusBar(ImVec2(origin.x, y), avail.x);

    ImGui::Dummy(ImVec2(avail.x, y - origin.y + statusH));
    ImGui::End();

    // secondary panel: drawn after the main window so it floats on top of it
    DrawMixerWindow();
}

static void HandleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) TogglePlay(g_focus);
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) g_focus = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) g_focus = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) g_showMixer = !g_showMixer;
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) CueDeck(g_focus);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, false))
        SetDeckRate(g_focus, g_eng.deck[g_focus].rate.load(std::memory_order_relaxed) - 0.05f);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal, false))
        SetDeckRate(g_focus, g_eng.deck[g_focus].rate.load(std::memory_order_relaxed) + 0.05f);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        dj::Deck&        d = g_eng.deck[g_focus];
        const dj::Track* t = d.current.load(std::memory_order_acquire);
        if (t) {
            const int64_t step = (int64_t)t->sampleRate * 5;
            const int64_t want = (int64_t)d.pos.load() +
                                 (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) ? step : -step);
            d.seekReq.store((uint64_t)std::clamp<int64_t>(want, 0, (int64_t)t->frames - 1));
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        std::string p;
        if (PickFile(&p)) StartLoad(g_focus, p);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        const int from = g_focus, to = 1 - from;
        dj::Deck& dt = g_eng.deck[to];
        if (dt.current.load()) {
            dt.ArmFade(1.0f, (uint32_t)(g_fadeSecs * 1.5f * g_eng.sampleRate), false);
            dt.playing.store(true);
            g_eng.deck[from].ArmFade(0.0f, (uint32_t)(g_fadeSecs * 1.5f * g_eng.sampleRate), true);
        }
    }
}

// ------------------------------------------------------------------ self test
static double RmsOf(const std::vector<float>& b, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > b.size()) to = b.size();
    double s = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to; ++i) { s += double(b[i]) * b[i]; ++n; }
    return n ? sqrt(s / double(n)) : 0.0;
}
static double MaxStep(const std::vector<float>& b, size_t ch = 0) {
    double m = 0.0;
    for (size_t i = ch + 2; i < b.size(); i += 2) m = std::max(m, fabs(double(b[i]) - double(b[i - 2])));
    return m;
}

static int RunSelfTest(bool allowNoDevice = false) {
    int fails = 0, checks = 0, skipped = 0;
    auto check = [&](bool ok, const char* what, const char* fmt = nullptr, ...) {
        ++checks;
        char detail[256] = "";
        if (fmt) {
            va_list ap; va_start(ap, fmt); vsnprintf(detail, sizeof(detail), fmt, ap); va_end(ap);
        }
        printf("[%s] %-46s %s\n", ok ? " OK " : "FAIL", what, detail);
        if (!ok) ++fails;
    };

    // ---- offline engine -------------------------------------------------
    dj::Engine e;
    e.InitOffline(48000);
    e.masterGain.store(1.0f);
    std::vector<float> buf(1024 * 2);

    dj::Track* A = dj::MakeSineTrack(4.0, 1000.0, 48000, 0.5f);
    dj::Track* B = dj::MakeSineTrack(4.0, 1000.0, 48000, 0.5f);   // same tone: makes mixing math checkable
    e.deck[0].pending.store(A);
    e.deck[1].pending.store(B);
    e.RenderBlock(buf.data(), 1024);           // adopt

    e.xfader.store(0.0f);
    e.deck[0].playing.store(true);
    for (int i = 0; i < 40; ++i) e.RenderBlock(buf.data(), 1024);
    const double rmsA = RmsOf(buf);
    check(rmsA > 0.25 && rmsA < 0.45, "deck A audible with the crossfader at A",
          "rms=%.4f", rmsA);

    e.xfader.store(1.0f);                       // A muted, B still stopped -> silence
    for (int i = 0; i < 40; ++i) e.RenderBlock(buf.data(), 1024);
    const double rmsMuted = RmsOf(buf);
    check(rmsMuted < 1e-3, "crossfader fully mutes the far deck", "rms=%.6f", rmsMuted);

    e.xfader.store(0.5f);
    for (int i = 0; i < 40; ++i) e.RenderBlock(buf.data(), 1024);
    const double rmsHalf = RmsOf(buf);
    check(fabs(rmsHalf / rmsA - 0.7071) < 0.03, "constant-power law at centre (-3 dB)",
          "ratio=%.4f (expect 0.7071)", rmsHalf / rmsA);

    // ---- limiter: both decks at +6 dB, crossfader centred => well past 0 dBFS
    e.deck[0].faderDb.store(6.0f);
    e.deck[1].faderDb.store(6.0f);
    e.deck[1].playing.store(true);
    double peak = 0.0;
    for (int i = 0; i < 60; ++i) {
        e.RenderBlock(buf.data(), 1024);
        for (float v : buf) peak = std::max(peak, fabs(double(v)));
    }
    check(peak <= 1.0001 && peak > 0.90, "master limiter holds the output at 0 dBFS", "peak=%.4f", peak);
    e.deck[0].faderDb.store(0.0f);
    e.deck[1].faderDb.store(0.0f);
    e.deck[1].playing.store(false);

    // ---- fade out then auto-pause (deck A alone)
    e.xfader.store(0.0f);
    e.deck[0].ArmFade(1.0f, 0, false);
    e.deck[0].playing.store(true);
    for (int i = 0; i < 4; ++i) e.RenderBlock(buf.data(), 1024);
    e.deck[0].ArmFade(0.0f, 4800, true);        // 100 ms
    for (int i = 0; i < 20; ++i) e.RenderBlock(buf.data(), 1024);
    const double rmsFaded = RmsOf(buf);
    check(rmsFaded < 0.03, "fade-out reaches silence", "rms=%.5f", rmsFaded);
    check(!e.deck[0].playing.load(), "fade-out auto-pauses the deck", "");

    // ---- seek is click-free (self-calibrating against the continuous signal)
    e.deck[0].ArmFade(1.0f, 0, false);
    e.deck[0].RequestSeek(0.0);
    e.deck[0].playing.store(true);
    for (int i = 0; i < 10; ++i) e.RenderBlock(buf.data(), 1024);
    std::vector<float> cont;
    for (int i = 0; i < 12; ++i) { e.RenderBlock(buf.data(), 1024); cont.insert(cont.end(), buf.begin(), buf.end()); }
    const double stepCont = MaxStep(cont);
    e.deck[0].RequestSeek(1.0001);              // an awkward phase: a hard jump would click
    std::vector<float> seekBuf;
    for (int i = 0; i < 12; ++i) { e.RenderBlock(buf.data(), 1024); seekBuf.insert(seekBuf.end(), buf.begin(), buf.end()); }
    const double stepSeek = MaxStep(seekBuf);
    check(stepSeek < stepCont * 1.6, "seek is ramped (no click)",
          "continuous max step=%.4f, around seek=%.4f", stepCont, stepSeek);
    check(stepCont * 1.6 < 0.25, "the click test can actually detect a click",
          "threshold=%.4f, a hard jump would be up to 0.50", stepCont * 1.6);
    const uint64_t posAfter = e.deck[0].pos.load();
    check(posAfter > 48000 && posAfter < 48000 + 1024 * 16, "seek landed near 1.0 s",
          "pos=%llu frames", (unsigned long long)posAfter);

    // ---- EQ
    dj::Track* L = dj::MakeSineTrack(2.0, 60.0, 48000, 0.25f);
    dj::Track* H = dj::MakeSineTrack(2.0, 8000.0, 48000, 0.25f);
    auto rmsOfTrack = [&](dj::Track* t, float lowDb, float highDb) {
        dj::Engine en;
        en.InitOffline(48000);
        en.masterGain.store(1.0f);
        en.xfader.store(0.0f);
        en.deck[0].pending.store(t);
        en.deck[0].eqDb[0].store(lowDb);
        en.deck[0].eqDb[2].store(highDb);
        std::vector<float> b(1024 * 2);
        en.RenderBlock(b.data(), 1024);            // adopt first: loading leaves a deck paused
        en.deck[0].playing.store(true);
        for (int i = 0; i < 30; ++i) en.RenderBlock(b.data(), 1024);
        return RmsOf(b);
    };
    const double lowFlat = rmsOfTrack(L, 0.0f, 0.0f);
    const double lowBoost = rmsOfTrack(L, 6.0f, 0.0f);
    check(fabs(lowBoost / lowFlat - 2.0) < 0.25, "EQ low shelf +6 dB doubles 60 Hz",
          "ratio=%.3f", lowBoost / lowFlat);
    const double lowKill = rmsOfTrack(L, -24.0f, 0.0f);
    check(lowKill / lowFlat < 0.1, "EQ low kill removes 60 Hz", "ratio=%.4f", lowKill / lowFlat);
    const double highFlat = rmsOfTrack(H, 0.0f, 0.0f);
    const double highKill = rmsOfTrack(H, 0.0f, -24.0f);
    check(highKill / highFlat < 0.1, "EQ high kill removes 8 kHz", "ratio=%.4f", highKill / highFlat);

    // ---- hot swap deck B while deck A keeps playing
    {
        dj::Engine en;
        en.InitOffline(48000);
        en.masterGain.store(1.0f);
        en.xfader.store(0.0f);                     // deck A only
        dj::Track* a1 = dj::MakeSineTrack(6.0, 1000.0, 48000, 0.4f);
        dj::Track* b1 = dj::MakeSineTrack(6.0, 300.0, 48000, 0.4f);
        en.deck[0].pending.store(a1);
        en.deck[1].pending.store(b1);
        std::vector<float> b(1024 * 2);
        en.RenderBlock(b.data(), 1024);            // adopt first: loading leaves a deck paused
        en.deck[0].playing.store(true);
        for (int i = 0; i < 30; ++i) en.RenderBlock(b.data(), 1024);
        const double before = RmsOf(b);

        dj::Track* b2 = dj::MakeSineTrack(6.0, 700.0, 48000, 0.4f);   // "user loaded another song into B"
        en.deck[1].pending.store(b2);
        double worst = 1e9;
        for (int i = 0; i < 60; ++i) {
            en.RenderBlock(b.data(), 1024);
            for (float v : b) if (!std::isfinite(v)) { worst = -1.0; break; }
            worst = std::min(worst, RmsOf(b));
        }
        const double after = RmsOf(b);
        check(en.deck[1].TakeRetired() == b1, "old deck-B track is handed back for freeing", "");
        check(fabs(after / before - 1.0) < 0.02, "deck A output unaffected by the swap",
              "before=%.4f after=%.4f", before, after);
        check(worst > 0.2, "no dropout / non-finite samples during the swap", "min block rms=%.4f", worst);
        en.xfader.store(1.0f);                     // now listen to deck B: it must be the new track
        en.deck[1].playing.store(true);
        for (int i = 0; i < 20; ++i) en.RenderBlock(b.data(), 1024);
        check(RmsOf(b) > 0.2, "deck B plays the newly loaded track", "rms=%.4f", RmsOf(b));
        if (dj::Track* r = en.deck[1].current.exchange(nullptr)) dj::FreeTrack(r);
        if (dj::Track* r = en.deck[0].current.exchange(nullptr)) dj::FreeTrack(r);
    }
    dj::FreeTrack(L); dj::FreeTrack(H);
    if (dj::Track* r = e.deck[0].current.exchange(nullptr)) dj::FreeTrack(r);
    if (dj::Track* r = e.deck[1].current.exchange(nullptr)) dj::FreeTrack(r);
    while (dj::Track* r = e.deck[0].TakeRetired()) dj::FreeTrack(r);
    while (dj::Track* r = e.deck[1].TakeRetired()) dj::FreeTrack(r);

    // ---- queue modes (pure selection logic: no audio, so every boundary is cheap to cover)
    {
        auto mk = [](int n, PlayMode m, int cur) {
            Playlist p;
            p.mode    = m;
            p.current = cur;
            p.rng     = 20260921u;                     // fixed seed: the shuffle order is reproducible
            for (int k = 0; k < n; ++k) p.items.push_back(std::string("track") + char('0' + k));
            return p;
        };
        {
            Playlist p = mk(3, PlaySeq, 0);
            const int a = PlaylistNext(p); p.current = a;
            const int b = PlaylistNext(p); p.current = b;
            const int c = PlaylistNext(p);
            check(a == 1 && b == 2 && c == -1, "SEQ walks to the end and then stops",
                  "0->%d->%d->%d", a, b, c);
        }
        {
            Playlist p = mk(3, PlayAll, 2);
            check(PlaylistNext(p) == 0, "ALL wraps around the end", "2->0");
        }
        {
            Playlist p = mk(2, PlayOne, 1);
            const int a = PlaylistNext(p); p.current = a;
            const int b = PlaylistNext(p);
            check(a == 1 && b == 1, "ONE stays on the same item", "1->%d->%d", a, b);
        }
        {
            Playlist p = mk(5, PlayShuffle, -1);
            int  prev = -1, repeats = 0, visits[5] = {0, 0, 0, 0, 0};
            bool bad  = false;
            for (int step = 0; step < 5 * 100; ++step) {
                const int nx = PlaylistNext(p);
                if (nx < 0 || nx >= 5) { bad = true; break; }
                if (nx == prev) ++repeats;
                ++visits[nx];
                prev = p.current = nx;
            }
            bool even = !bad;
            for (int k = 0; k < 5; ++k) if (visits[k] != 100) even = false;
            check(!bad && repeats == 0, "SHUF never plays the same track twice in a row",
                  "%d immediate repeats in 500 picks", repeats);
            check(even, "SHUF plays every track once per cycle", "%d %d %d %d %d",
                  visits[0], visits[1], visits[2], visits[3], visits[4]);
        }
        Playlist one  = mk(1, PlaySeq, 0);
        Playlist none = mk(0, PlayAll, -1);
        check(PlaylistNext(one) == -1, "a one-item queue stops in SEQ", nullptr);
        check(PlaylistNext(none) == -1, "an empty queue never advances", nullptr);

        // what the preload predicts must be exactly what the advance then plays
        {
            bool agree = true;
            for (int mode = 0; mode < kPlayModeCount && agree; ++mode) {
                for (int n = 1; n <= 4 && agree; ++n) {
                    for (int cur = -1; cur < n && agree; ++cur) {
                        Playlist a = mk(n, (PlayMode)mode, cur);
                        Playlist b = mk(n, (PlayMode)mode, cur);
                        if (a.mode == PlayShuffle) {          // prime the order: a peek never rebuilds one
                            ShuffleReset(a, -1);
                            ShuffleReset(b, -1);
                        }
                        if (PlaylistPeek(a) != PlaylistNext(b)) agree = false;
                    }
                }
            }
            check(agree, "preload peek == the advance that follows it (all modes)", nullptr);
        }
    }

    // ---- playback speed: tape-style resampling, 0.50x - 2.00x
    {
        auto consumed = [&](float rate) {
            dj::Engine en;
            en.InitOffline(48000);
            en.masterGain.store(1.0f);
            en.xfader.store(0.0f);
            en.deck[0].pending.store(dj::MakeSineTrack(120.0, 440.0, 48000, 0.4f));
            std::vector<float> b(1024 * 2);
            en.RenderBlock(b.data(), 1024);            // adopt first: a load leaves the deck paused
            en.deck[0].rate.store(rate);
            en.deck[0].ArmFade(1.0f, 0, false);
            en.deck[0].playing.store(true);
            for (int i = 0; i < 100; ++i) en.RenderBlock(b.data(), 1024);
            const double frames = double(en.deck[0].pos.load());
            if (dj::Track* r = en.deck[0].current.exchange(nullptr)) dj::FreeTrack(r);
            while (dj::Track* r = en.deck[0].TakeRetired()) dj::FreeTrack(r);
            return frames;
        };
        const double f1 = consumed(1.0f), f2 = consumed(2.0f), fh = consumed(0.5f);
        check(fabs(f2 / f1 - 2.0) < 0.02, "2.00x consumes audio twice as fast",
              "%.0f vs %.0f frames", f2, f1);
        check(fabs(fh / f1 - 0.5) < 0.02, "0.50x consumes audio half as fast",
              "%.0f vs %.0f frames", fh, f1);
    }

    // ---- real device ------------------------------------------------------
    dj::Engine live;
    std::string err;
    if (live.Start(&err)) {
        const uint64_t c0 = live.callbacks.load();
        const double   t0 = dj::NowSeconds();
        dj::Track* s = dj::MakeSineTrack(2.0, 440.0, live.sampleRate, 0.3f);
        live.deck[0].pending.store(s);
        live.deck[0].playing.store(true);
        live.xfader.store(0.0f);
        Sleep(1500);
        const uint64_t c1 = live.callbacks.load();
        const double   dt = dj::NowSeconds() - t0;
        const double   expect = dt * live.sampleRate / double(live.periodFrames.load() ? live.periodFrames.load() : 256);
        check(c1 > c0, "WASAPI device delivers callbacks", "%llu callbacks in %.2fs",
              (unsigned long long)(c1 - c0), dt);
        check(c1 - c0 > expect * 0.9, "callback rate matches the period size",
              "got %llu, expected ~%.0f", (unsigned long long)(c1 - c0), expect);
        check(live.cpuLoad.load() < 0.5f, "audio thread CPU load is sane", "%.2f%%",
              double(live.cpuLoad.load()) * 100.0);
        live.Stop();
        if (dj::Track* r = live.deck[0].current.exchange(nullptr)) dj::FreeTrack(r);
    } else if (allowNoDevice) {
        // Headless machines (CI runners) have no audio endpoint: say so instead of
        // failing. Opt-in only, so a broken device still fails on a real machine.
        ++skipped;
        printf("[SKIP] %-46s no audio device (%s)\n", "WASAPI device opens", err.c_str());
    } else {
        check(false, "WASAPI device opens", "%s", err.c_str());
    }

    printf("\n%d checks, %d failed, %d skipped\n", checks, fails, skipped);
    return fails;
}

// ------------------------------------------------------------------ main
static void SetupStyle(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f; s.FrameRounding = 5.0f; s.GrabRounding = 5.0f;
    s.PopupRounding = 6.0f;  s.ScrollbarRounding = 8.0f; s.TabRounding = 5.0f;
    s.WindowBorderSize = 0.0f; s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(12, 10); s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(9, 7);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.078f, 0.082f, 0.095f, 1.00f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.10f, 0.105f, 0.12f, 1.00f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.11f, 0.115f, 0.13f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(0.20f, 0.21f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.145f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.24f, 0.27f, 0.32f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.25f, 0.28f, 0.33f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.31f, 0.35f, 0.42f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.20f, 0.23f, 0.28f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.55f, 0.70f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.70f, 0.82f, 1.00f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.55f, 0.57f, 0.62f, 1.00f);
    c[ImGuiCol_MenuBarBg]       = ImVec4(0.10f, 0.105f, 0.12f, 1.00f);
    s.ScaleAllSizes(scale);
}

static bool OwnsConsole() {
    DWORD pids[4] = {};
    return GetConsoleProcessList(pids, 4) <= 1;
}

int main(int argc, char** argv) {
    g_exeDir = ExeDir();
    const char* shotPath = nullptr;
    std::vector<std::string> initial;
    int  shotFrames = 60;
    bool selfTest = false, genDemo = false, autoplay = false, autoswap = false;
    bool playlistTest = false, dropTest = false, jitterTest = false;
    bool gaplessTest = false, resumeTest = false;
    bool allowNoDevice = false, mixerOpen = false, noPanels = false;
    std::string browserDir;
    std::string resumePath;                  // --resume-test: the file it parks and reloads

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest")              selfTest = true;
        else if (a == "--gen-demo" && i + 1 < argc) { genDemo = true; g_exeDir = argv[++i]; }
        else if (a == "--screenshot" && i + 1 < argc) shotPath = argv[++i];
        else if (a == "--frames" && i + 1 < argc)     shotFrames = atoi(argv[++i]);
        else if (a == "--browser" && i + 1 < argc)    browserDir = argv[++i];
        else if (a == "--autoplay")                    autoplay = true;
        else if (a == "--autoswap")                  { autoplay = true; autoswap = true; }
        else if (a == "--demo-drag")                   g_demoDrag = true;
        else if (a == "--mixer")                       mixerOpen = true;   // screenshot hook
        else if (a == "--no-panels")                   noPanels = true;    // screenshot hook
        else if (a == "--allow-no-device")             allowNoDevice = true;
        else if (a == "--playlist-test")             { playlistTest = true; autoplay = false; }
        else if (a == "--drop-test")                 { dropTest = true; autoplay = true; }
        else if (a == "--jitter-test")               { jitterTest = true; autoplay = true; }
        else if (a == "--gapless-test")              { gaplessTest = true; autoplay = false; }
        else if (a == "--resume-test")               { resumeTest = true; autoplay = false; }
        else if (a.rfind("--", 0) != 0)                initial.push_back(a);
    }

    // Screenshot and test runs own their own state: they must never load or save the
    // user's session (they would otherwise queue pt1.wav / demo_c.wav into it).
    const bool ephemeral = shotPath || autoplay || playlistTest || dropTest || jitterTest ||
                           gaplessTest || resumeTest || !initial.empty();

    if (selfTest) { SetConsoleOutputCP(CP_UTF8); return RunSelfTest(allowNoDevice); }

    if (genDemo) {
        std::string err;
        const bool ok = dj::GenerateDemoWav(g_exeDir + "\\demo_a.wav", 45.0, 124.0, 0, 48000, &err) &&
                        dj::GenerateDemoWav(g_exeDir + "\\demo_b.wav", 45.0, 124.0, 1, 48000, &err);
        printf("%s %s\n", ok ? "wrote demo_a.wav / demo_b.wav to" : "failed:", g_exeDir.c_str());
        return ok ? 0 : 1;
    }

    std::string err;
    if (g_eng.Start(&err)) {
        g_deviceOk = true;
        Say("audio: %s, %.0f Hz, %s", g_eng.deviceName.c_str(), double(g_eng.sampleRate),
            g_eng.deviceNote.c_str());
    } else {
        Say("AUDIO DEVICE FAILED: %s (UI runs, playback disabled)", err.c_str());
    }

    LoadRecents();
    InitBrowser();
    if (!browserDir.empty() && DirExists(browserDir)) {
        g_browser.dir      = browserDir;
        g_browser.needScan = true;
    }
    // Restore the queues only for an interactive start: every test mode (and the
    // screenshot runs) sets its own state and must not inherit one.
    if (!ephemeral) LoadSession();
    if (mixerOpen) g_showMixer = true;
    if (noPanels)  g_showPanels = false;

    std::string dropPath;
    if (dropTest) {
        std::string       err;
        const std::string pc = g_exeDir + "\\demo_c.wav";
        if (!FileExists(pc)) dj::GenerateDemoWav(pc, 20.0, 128.0, 2, g_eng.sampleRate, &err);
        dropPath = pc;
        Say("drop test: deck A plays, then a file is dropped on B (paused) and on A (playing)");
    }

    if (playlistTest) {
        std::string       err;
        const std::string p1 = g_exeDir + "\\pt1.wav", p2 = g_exeDir + "\\pt2.wav";
        if (!FileExists(p1)) dj::GenerateDemoWav(p1, 6.0, 124.0, 0, g_eng.sampleRate, &err);
        if (!FileExists(p2)) dj::GenerateDemoWav(p2, 6.0, 124.0, 2, g_eng.sampleRate, &err);
        g_pl[0].items    = {p1, p2};
        g_pl[0].current  = 0;
        g_pl[0].mode     = PlaySeq;
        g_halted[0].store(false);
        StartLoad(0, p1, true);
        Say("playlist test: two 6 s tracks, expecting SEQ advance then a ONE repeat");
    }

    if (gaplessTest) {
        std::string       err;
        const std::string q1 = g_exeDir + "\\gp1.wav", q2 = g_exeDir + "\\gp2.wav";
        if (!FileExists(q1)) dj::GenerateDemoWav(q1, 6.0, 124.0, 0, g_eng.sampleRate, &err);
        if (!FileExists(q2)) dj::GenerateDemoWav(q2, 6.0, 124.0, 2, g_eng.sampleRate, &err);
        g_pl[0].items   = {q1, q2};
        g_pl[0].current = 0;
        g_pl[0].mode    = PlaySeq;
        g_halted[0].store(false);
        StartLoad(0, q1, true);
        Say("gapless test: playing gp1, expecting gp2 preloaded and handed over without a decode");
    }

    if (resumeTest) {
        std::string       err;
        const std::string rp = g_exeDir + "\\demo_a.wav";
        if (!FileExists(rp)) dj::GenerateDemoWav(rp, 45.0, 124.0, 0, g_eng.sampleRate, &err);
        resumePath = rp;
        StartLoad(0, rp);                     // a deliberate load: starts at 0 and stays paused
        Say("resume test: park demo_a at 12 s, load it again, expect it to resume there");
    }

    if (shotPath || autoplay || !initial.empty()) {
        std::string e2;
        const std::string pa = g_exeDir + "\\demo_a.wav", pb = g_exeDir + "\\demo_b.wav";
        if (!FileExists(pa) && !dj::GenerateDemoWav(pa, 45.0, 124.0, 0, g_eng.sampleRate, &e2))
            Say("demo_a.wav generation failed: %s", e2.c_str());
        if (!FileExists(pb) && !dj::GenerateDemoWav(pb, 45.0, 124.0, 1, g_eng.sampleRate, &e2))
            Say("demo_b.wav generation failed: %s", e2.c_str());
        initial.push_back(pa);
        initial.push_back(pb);
        if (g_pl[0].items.empty()) { g_pl[0].items = {pa, pb}; }
        if (g_pl[1].items.empty()) { g_pl[1].items = {pb, pa}; }
    }

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr,
                      nullptr, nullptr, L"djdeck", nullptr};
    RegisterClassExW(&wc);
    ImGui_ImplWin32_EnableDpiAwareness();
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"djdeck - two-deck player", WS_OVERLAPPEDWINDOW,
                              40, 30, 1560, 1020, nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Say("D3D11 init failed");
        return 2;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    if (!shotPath && OwnsConsole()) ShowWindow(GetConsoleWindow(), SW_HIDE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    const float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    SetupStyle(dpiScale);
    // Latin UI font, then merge a CJK font so non-English audio device names render
    // (Segoe UI carries no CJK glyphs, and ImGui has no OS font fallback).
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.5f * dpiScale);
    if (!font) font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 17.0f * dpiScale);
    if (!font) font = io.Fonts->AddFontDefault();
    if (font) {
        ImFontConfig merge;
        merge.MergeMode = true;
        static const char* kCjkFonts[] = {"C:\\Windows\\Fonts\\msyh.ttc",
                                          "C:\\Windows\\Fonts\\msyh.ttf",
                                          "C:\\Windows\\Fonts\\simsun.ttc",
                                          "C:\\Windows\\Fonts\\malgun.ttf"};
        for (const char* p : kCjkFonts) {
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
            if (io.Fonts->AddFontFromFileTTF(p, 17.0f * dpiScale, &merge,
                                             io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
                break;
        }
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    for (size_t i = 0; i < initial.size() && i < 2; ++i) StartLoad((int)i, initial[i]);
    if (!initial.empty() || autoplay) autoplay = true;

    int  frame = 0;
    bool done  = false;
    double plTestT0 = dj::NowSeconds();
    int    plTestResult = -1;
    int    plStage   = 0;
    bool   plSawEnd  = false;
    bool   dropAccepted = false, dropRefused = false;
    int    dropResult = -1;
    int    gapStage = 0, gapResult = -1;
    bool   gapArmed = false, gapPreloaded = false, gapMissedEnd = false;
    const dj::Track* gapPrev = nullptr, *gapArmedPtr = nullptr;
    double gapT0 = 0.0, gapStart = dj::NowSeconds();
    int    resumeStage = 0, resumeResult = -1;
    double resumePos = -1.0, resumeStart = dj::NowSeconds();
    double jPrev[2] = {-1.0, -1.0}, jWorstBack[2] = {0.0, 0.0}, jMaxFwd[2] = {0.0, 0.0};
    int    jBack[2] = {0, 0}, jFrames = 0, jitterResult = -1;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || g_quit) break;

        if (g_pendingW && g_pendingH) {
            CleanupRenderTarget();
            g_swap->ResizeBuffers(0, g_pendingW, g_pendingH, DXGI_FORMAT_UNKNOWN, 0);
            g_pendingW = g_pendingH = 0;
            CreateRenderTarget();
        }

        CollectRetired();
        CapturePositions();
        UpdatePlaylists();
        UpdatePreload();
        for (int i = 0; i < 2; ++i) EnsureWaveTexture(i, g_eng.deck[i].current.load(std::memory_order_acquire));

        // auto-start playback once the demo tracks are in (screenshot / autoplay mode)
        if (autoplay && frame > 20 && !g_loader[0].busy.load() && !g_loader[1].busy.load() &&
            g_eng.deck[0].current.load() && g_eng.deck[1].current.load()) {
            g_eng.deck[0].RequestSeek(12.0);
            g_eng.deck[1].RequestSeek(6.0);
            g_eng.deck[0].ArmFade(1.0f, 0, false);
            g_eng.deck[1].ArmFade(1.0f, 0, false);
            g_eng.deck[0].playing.store(true);
            g_eng.deck[1].playing.store(true);
            g_eng.xfader.store(0.5f);
            g_ui[0].viewLen   = 20.0;          // deck A: zoomed window (the playhead stays centred)
            g_ui[0].smoothPos = 12.0;
            autoplay = false;
        }

        // scripted proof: replace deck B's song while deck A keeps playing, mid-flight
        if (autoswap && !autoplay && frame == 120) {
            const std::string pc = g_exeDir + "\\demo_c.wav";
            std::string e3;
            if (!FileExists(pc) &&
                !dj::GenerateDemoWav(pc, 45.0, 128.0, 2, g_eng.sampleRate, &e3))
                Say("demo_c.wav generation failed: %s", e3.c_str());
            Say("script: replacing deck B while deck A plays (frame %d)", frame);
            StartLoad(1, pc);
            autoswap = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        HandleShortcuts();
        DrawUI();
        ImGui::Render();
        const float clear[4] = {0.06f, 0.062f, 0.072f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (playlistTest && plTestResult < 0) {
            if (plStage == 0) {
                // stage 0: SEQ advances 1/2 -> 2/2 and keeps playing
                if (g_pl[0].current == 1 && g_eng.deck[0].playing.load()) {
                    printf("[djdeck] PLAYLIST SEQ ADVANCE: OK - advanced to item 2/2 and playing (%s)\n",
                           BaseNameOf(g_pl[0].items[1]).c_str());
                    g_pl[0].mode = PlayOne;                 // stage 1: the same item must replay
                    plStage      = 1;
                    plSawEnd     = false;
                    plTestT0     = dj::NowSeconds();
                    Say("playlist test: mode ONE, the same item must replay by itself");
                } else if (dj::NowSeconds() - plTestT0 > 25.0) {
                    printf("[djdeck] PLAYLIST SEQ ADVANCE: FAIL - current=%d playing=%d atEnd=%d\n",
                           g_pl[0].current, (int)g_eng.deck[0].playing.load(),
                           (int)g_eng.deck[0].atEnd.load());
                    plTestResult = 1;
                    g_quit       = true;
                }
            } else {
                // stage 1: ONE reloads the same item - watch the end latch rise, clear, then play
                if (g_eng.deck[0].atEnd.load()) plSawEnd = true;
                if (plSawEnd && g_pl[0].current == 1 && !g_eng.deck[0].atEnd.load() &&
                    g_eng.deck[0].playing.load()) {
                    printf("[djdeck] PLAYLIST ONE REPEAT: OK - item 2/2 replayed automatically (%s)\n",
                           BaseNameOf(g_pl[0].items[1]).c_str());
                    plTestResult = 0;
                    g_quit       = true;
                } else if (dj::NowSeconds() - plTestT0 > 25.0) {
                    printf("[djdeck] PLAYLIST ONE REPEAT: FAIL - current=%d playing=%d atEnd=%d sawEnd=%d\n",
                           g_pl[0].current, (int)g_eng.deck[0].playing.load(),
                           (int)g_eng.deck[0].atEnd.load(), (int)plSawEnd);
                    plTestResult = 1;
                    g_quit       = true;
                }
            }
        }
        if (dropTest && dropResult < 0) {
            if (frame == 90) {                                   // user pauses deck B first
                g_eng.deck[1].playing.store(false);
                g_halted[1].store(true);
            }
            if (frame == 120) {
                dropAccepted = TryDropOnDeck(1, dropPath);   // deck B is paused -> accepted
                dropRefused  = TryDropOnDeck(0, dropPath);   // deck A is playing -> refused
            }
            if (frame >= 200) {
                const dj::Track* tb = g_eng.deck[1].current.load();
                const std::string title = tb ? tb->title : std::string("(none)");
                const bool ok = dropAccepted && !dropRefused &&
                                !g_eng.deck[1].playing.load() &&               // stayed paused
                                title.find("demo_c") != std::string::npos &&    // the dropped file landed
                                g_eng.deck[0].playing.load();                  // A never stopped
                printf("[djdeck] DROP RULE: %s | paused deck accepted=%d | playing deck refused=%d | "
                       "deck B paused after load=%d | deck A still playing=%d | deck B track=%s\n",
                       ok ? "OK" : "FAIL", (int)dropAccepted, (int)dropRefused,
                       (int)!g_eng.deck[1].playing.load(), (int)g_eng.deck[0].playing.load(), title.c_str());
                dropResult = ok ? 0 : 1;
                g_quit     = true;
            }
        }
        if (jitterTest && frame > 60 && frame <= 420) {
            for (int i = 0; i < 2; ++i) {
                const double p = g_ui[i].smoothPos;
                if (jPrev[i] >= 0.0) {
                    const double d = p - jPrev[i];
                    if (d < -1e-9) { ++jBack[i]; jWorstBack[i] = std::min(jWorstBack[i], d); }
                    jMaxFwd[i] = std::max(jMaxFwd[i], d);
                }
                jPrev[i] = p;
            }
            ++jFrames;
        }
        if (jitterTest && jitterResult < 0 && frame > 420) {
            const bool ok = jBack[0] == 0 && jBack[1] == 0 && jMaxFwd[0] < 0.10 && jMaxFwd[1] < 0.10;
            printf("[djdeck] WAVEFORM JITTER: %s | frames=%d | A: back=%d worst=%.4fs maxfwd=%.4fs | "
                   "B: back=%d worst=%.4fs maxfwd=%.4fs\n",
                   ok ? "OK" : "FAIL", jFrames, jBack[0], jWorstBack[0], jMaxFwd[0],
                   jBack[1], jWorstBack[1], jMaxFwd[1]);
            jitterResult = ok ? 0 : 1;
            g_quit       = true;
        }
        // ---- gapless handover: the next item must arrive decoded, without a decode pause
        if (gaplessTest && gapResult < 0) {
            dj::Deck&        d0  = g_eng.deck[0];
            const dj::Track* cur = d0.current.load(std::memory_order_acquire);
            if (gapStage == 0 && cur && d0.playing.load()) {
                gapPrev  = cur;
                gapStart = dj::NowSeconds();
                gapStage = 1;
            } else if (gapStage == 1) {
                if (g_staged[0].track) { gapArmed = true; gapArmedPtr = g_staged[0].track; }
                if (d0.atEnd.load())            { gapT0 = dj::NowSeconds(); gapStage = 2; }
                else if (cur && cur != gapPrev) { gapT0 = dj::NowSeconds(); gapMissedEnd = true; gapStage = 3; }
            } else if (gapStage == 2 && cur && cur != gapPrev) {
                gapPreloaded = gapArmedPtr && cur == gapArmedPtr;
                gapStage     = 3;
            }
            if (gapStage == 3 && cur && cur != gapPrev && d0.playing.load()) {
                const double gap = dj::NowSeconds() - gapT0;
                const bool   ok  = gapArmed && gapPreloaded && gap <= 0.15 && g_pl[0].current == 1;
                printf("[djdeck] GAPLESS HANDOVER: %s | preload armed=%d | handover used the decoded "
                       "track=%d | gap=%.3fs%s | queue now %d/%d\n",
                       ok ? "OK" : "FAIL", (int)gapArmed, (int)gapPreloaded, gap,
                       gapMissedEnd ? " (end latch coalesced into one frame)" : "",
                       g_pl[0].current + 1, (int)g_pl[0].items.size());
                gapResult = ok ? 0 : 1;
                g_quit    = true;
            } else if (gapResult < 0 && dj::NowSeconds() - gapStart > 30.0) {
                printf("[djdeck] GAPLESS HANDOVER: FAIL (timeout) | stage=%d armed=%d queue=%d/%d\n",
                       gapStage, (int)gapArmed, g_pl[0].current + 1, (int)g_pl[0].items.size());
                gapResult = 1;
                g_quit    = true;
            }
        }

        // ---- resume: park a track, load it again, expect it to pick up where it stopped
        if (resumeTest && resumeResult < 0) {
            dj::Deck&        d0   = g_eng.deck[0];
            const dj::Track* cur  = d0.current.load(std::memory_order_acquire);
            const dj::Track* want = g_awaiting[0].load();
            const bool       adopted = cur && (!want || cur == want) && !g_loader[0].busy.load();
            const double     pos = cur ? double(d0.pos.load(std::memory_order_relaxed)) / cur->sampleRate : 0.0;
            if (resumeStage == 0 && adopted && pos < 1.0) {
                d0.RequestSeek(12.0);                    // the user parks it at 12 s
                g_ui[0].smoothPos = 12.0;
                resumeStage = 1;
            } else if (resumeStage == 1 && adopted && fabs(pos - 12.0) < 0.6) {
                CapturePositions(true);
                auto it = g_positions.find(resumePath);
                resumePos = (it != g_positions.end()) ? it->second : -1.0;
                StartLoad(0, resumePath);                // load the very same file again
                resumeStage = 2;
            } else if (resumeStage == 2 && adopted && g_resumeAt[0].load() < 0.0 && fabs(pos - 12.0) < 1.0) {
                // the resume seek has been applied to the new track: check the session format too
                const std::string sess = g_exeDir + "\\djdeck_session.txt";
                const std::string bak  = sess + ".bak";
                remove(bak.c_str());
                const bool had = FileExists(sess);
                if (had) rename(sess.c_str(), bak.c_str());
                SaveSession();
                g_positions.clear();
                LoadSession();
                auto it2 = g_positions.find(resumePath);
                const double back = (it2 != g_positions.end()) ? it2->second : -1.0;
                if (had) { remove(sess.c_str()); rename(bak.c_str(), sess.c_str()); }
                else     { remove(sess.c_str()); }
                const bool ok = resumePos > 11.0 && resumePos < 13.0 && fabs(back - resumePos) < 0.01;
                printf("[djdeck] RESUME: %s | captured=%.2fs | reload played from %.2fs | "
                       "session file round-trip=%.2fs\n", ok ? "OK" : "FAIL", resumePos, pos, back);
                resumeResult = ok ? 0 : 1;
                g_quit       = true;
            } else if (resumeResult < 0 && dj::NowSeconds() - resumeStart > 30.0) {
                printf("[djdeck] RESUME: FAIL (timeout) | stage=%d pos=%.2f adopted=%d resumeAt=%.1f\n",
                       resumeStage, pos, (int)adopted, g_resumeAt[0].load());
                resumeResult = 1;
                g_quit       = true;
            }
        }

        ++frame;
        if (shotPath && frame >= shotFrames) {
            const bool ok = SaveBackbufferPNG(shotPath);
            printf("[djdeck] screenshot %s: %s\n", ok ? "saved" : "FAILED", shotPath);
            g_quit = true;
        }
        g_swap->Present(1, 0);
    }

    g_quit = true;
    for (int i = 0; i < 2; ++i) {
        g_loader[i].cancel.store(true);
        if (g_loader[i].th.joinable()) g_loader[i].th.join();
        g_pre[i].cancel.store(true);
        if (g_pre[i].th.joinable()) g_pre[i].th.join();
        DiscardStaged(i);
        if (dj::Track* t = g_pre[i].result.exchange(nullptr)) dj::FreeTrack(t);
    }
    if (!ephemeral) {
        CapturePositions(true);             // one last sample of where each deck was left
        SaveSession();                      // queues, modes, positions, browser folder, view state
    }
    g_eng.Stop();
    for (int i = 0; i < 2; ++i) {
        while (dj::Track* r = g_eng.deck[i].TakeRetired()) dj::FreeTrack(r);
        if (dj::Track* r = g_eng.deck[i].current.exchange(nullptr)) dj::FreeTrack(r);
        if (dj::Track* r = g_eng.deck[i].pending.exchange(nullptr)) dj::FreeTrack(r);
    }
    for (int i = 0; i < 2; ++i) g_ui[i].ReleaseTex();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (gapResult >= 0)    return gapResult;
    if (resumeResult >= 0) return resumeResult;
    if (dropResult >= 0)   return dropResult;
    if (jitterResult >= 0) return jitterResult;
    return plTestResult >= 0 ? plTestResult : 0;
}
