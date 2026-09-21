# djdeck — 双音轨 DJ 播放器 demo (Windows)

两条音轨 + 居中播放指针的滚动波形 + 渐入渐出 + 交叠混音 + 每条音轨的播放列表 + 常驻文件浏览器。
Windows 原生：Win32 + Direct3D 11 + Dear ImGui + miniaudio (WASAPI 低延迟共享模式)。

## 构建 / 运行

需要 Git Bash + curl + python（脚本会在首次运行时把可移植的 LLVM-MinGW 工具链下载解压到 `tools/`，
约 190 MB，一次性；仓库里不含工具链二进制）。

```bash
bash build.sh                                  # 首次会自动拉工具链，之后增量编译
build/djdeck.exe                                # 交互式
build/djdeck.exe --selftest                     # 音频引擎测试（19 项，退出码 = 失败数）
build/djdeck.exe --playlist-test                # 播放列表自动续播测试
build/djdeck.exe --drop-test                    # 拖放规则测试（播放中拒绝 / 暂停可放且保持暂停）
build/djdeck.exe --autoswap --browser DIR --screenshot out.png --frames 320   # 渲染真实 UI 存 PNG
build/djdeck.exe --gen-demo DIR                 # 只生成演示 WAV
build/djdeck.exe --load a.wav b.wav             # 指定两首歌启动
```

首次启动会在 exe 目录生成演示曲子：`demo_a.wav`（四拍）、`demo_b.wav`（十六分踩镲）、
`demo_c.wav`（稀疏半拍，用于演示换歌）。

## 界面布局

```
[ Deck A：标题 / LOAD RECENT EJECT / 时间 ]  ← 播放指针固定在正中，波形自右向左滚动
[  缩放波形（可拖）                        ]
[  全曲概览条（含视窗框，可点可拖）        ]
[ Deck B：同上 ]
[ 混音面板：PLAY CUE LOOP auto / FADE IN OUT / GAIN LOW MID HIGH / PITCH LEVEL / 交叉推子 / MASTER ]
[ 文件浏览器 FILES ][ 播放列表 A ][ 播放列表 B ]
[ 状态栏：设备 / 采样率 / 块大小 / CPU / callback ]
```

## 操作

| 操作 | 说明 |
|---|---|
| 波形左键拖动 | 抓住波形左右拖 = 跳转播放时间（指针始终居中，抓到的采样点跟随鼠标） |
| 波形右键拖动 | scratch：播放速率跟随拖动速度（分数读取指针 + 插值） |
| 波形滚轮 | 以指针为中心缩放可视窗口（最短 1.5 s） |
| 概览条点击 / 拖动 | 按全曲比例跳转，视窗框同步移动 |
| **拖文件到音轨** | **只有暂停中的音轨接受**；播放中的音轨显示红框 "PLAYING - PAUSED DECKS ONLY" 并拒绝。放下后该音轨保持暂停 |
| 拖文件到播放列表 | 插到该行位置；拖到空白处追加；列表内上下拖动 = 排序；也可跨 A/B 列表拖动 |
| 双击浏览器里的文件 | 载入当前聚焦的音轨（同样只接受暂停的音轨） |
| LOAD / RECENT | 原生文件对话框 / 最近文件 |
| PLAY / CUE / LOOP | LOOP 为 8 秒循环（本 demo 无节拍网格） |
| FADE IN/OUT + fade(s) | 渐入 / 渐出，渐出结束自动暂停 |
| XFADE > | 自动混音：当前 deck 渐出 + 另一个 deck 跳到开头渐入 |
| `auto` 勾选 | 该音轨播放列表自动续播开关 |
| 播放列表 play / remove / clear | 播放选中项（异步载入后自动开始）/ 删除 / 清空 |
| GAIN / LOW / MID / HIGH | 推子 + 3 段 EQ（左端 = KILL，-96 dB） |
| PITCH | ±8% 变速（重采样，非 keylock） |
| MASTER / CROSSFADER | 等功率交叉推子（中心 -3 dB）+ 主输出限幅 |
| 快捷键 | `Space` 播放/暂停 · `Q` 回到开头并暂停 · `1`/`2` 选择 deck · `←`/`→` ±5 s · `L` 载入 · `X` 自动混音 |

## 播放列表与自动续播

* 每条音轨一个独立队列，`> ` 标记当前项，标题栏显示 `[playlist n/N]`。
* 一首曲子播完（引擎置 `atEnd`）时，**只要这条音轨不是被用户暂停的**且 `auto` 打开，
  就自动载入下一首并继续播放；用户按过 PAUSE/CUE 则停下不续播。
* 手动载入（拖放/LOAD/双击浏览器）会把该音轨标记为「用户暂停」，不会自动续播，直到再次按 PLAY。
* 载入是异步的（worker 线程），所以续播由「载入完成 → 自动播放」两步完成，中途不阻塞音频。

## 架构

```
音频线程 (WASAPI, 256 帧 ≈ 5.3 ms)          UI 线程 (60 fps)
  Deck::pending  ──adopt──►  Deck::current ──► 波形纹理 / 位置 / 峰值表
  Deck::retireQ  ◄──retire──  旧 Track            └─► FreeTrack
  原子参数 (seek/scrub/rate/eq/fade/xfader)  ◄── 旋钮/推子/拖动/拖放
```

* **音频回调零分配、零加锁、零 IO**；`Engine::RenderBlock()` 可离线调用，`--selftest` 就是这么跑的。
* **滚动波形**：峰值纹理固定，每帧只改 UV 窗口；位置在 UI 侧按 `DeltaTime` 插值，避免 5 ms 块进位的抖动。
* **拖动**：左键把位移换算成秒 → `RequestSeek`（音频线程 128 帧淡出→跳→淡入，无爆音）；
  右键把位移换算成速率 → `scrubFrames`，音频线程按块摊成播放速率。
* **换歌不打断另一条轨道**：解码在工作线程，产物经 `std::atomic<Track*>` 发布，
  旧轨道进 4 槽 SPSC retire 环由 UI 释放。
* **拖放规则**只有一处实现（`TryDropOnDeck`）：`playing` 时拒绝并提示，否则载入并保持暂停。

## Windows 音频配置（关键）

```c
cfg.performanceProfile      = ma_performance_profile_low_latency;
cfg.periodSizeInFrames      = 256;                          // 5.3 ms @48k
cfg.wasapi.usage            = ma_wasapi_usage_pro_audio;    // Avrt/MMCSS 线程特征
cfg.wasapi.noAutoConvertSRC = MA_TRUE;                      // 保住 IAudioClient3 低延迟共享模式
cfg.sampleRate              = 设备原生采样率;                 // 不触发 WASAPI 重采样
```

启动失败会自动降级（默认周期 → conservative），最后才报错。
miniaudio 没有 ASIO 后端；需要接专业声卡再换 PortAudio/RtAudio/JUCE。

## 仓库结构

```
build.sh                构建脚本（首次自动拉工具链）
src/dj.h  dj.cpp        音频引擎：解码/峰值、deck 状态机、混音、限幅、EQ
src/main.cpp            Win32 + D3D11 + ImGui 前端：波形、混音面板、播放列表、文件浏览器
third_party/imgui/      Dear ImGui（docking-free，2026-09-18 快照 420f179，MIT）
third_party/miniaudio.h 音频 I/O（public domain / MIT-0）
third_party/stb_image_write.h  截图编码（public domain）
docs/                   截图
tools/                  构建时下载的工具链（已 gitignore）
```

## 验证

| 测试 | 覆盖 |
|---|---|
| `--selftest`（19 项） | 交叉推子隔离与等功率律 · 限幅器不超 0 dBFS · 渐出到静音并自动暂停 · 跳转无爆音（与连续信号自校准）· EQ 增益/KILL · 热换轨时另一轨输出不变且无 dropout · 旧轨道回收 · 真实 WASAPI 回调速率与 CPU 负载 |
| `--playlist-test` | 两首 6 s 曲子，第一首播完自动载入第二首并继续播放 |
| `--jitter-test` | 播放中逐帧采样显示位置：0 次回退、单帧前进 ≤ 一帧音频量（修复前实测 23 次回退 / 最大 −0.248 s） |
| `--drop-test` | 暂停音轨接受、播放中音轨拒绝、放下后保持暂停、另一音轨不受影响 |
| 截图 | `docs/screenshot.png`（布局）、`docs/drop-rule.png`（拖放规则红/绿提示） |

## 已知取舍

* 载入时整曲解码为 16-bit PCM（45 s ≈ 8.6 MB），长曲子内存线性增长；
  低配设备应换成流式预读 + mmap PCM 缓存。
* 峰值列密度固定（最多 16384 列），放大超过约 3 倍会出现柱状台阶；
  更深缩放需按视窗动态重建纹理。
* 无节拍网格 / 同步 / keylock：PITCH 是磁带式变速；LOOP 是定长 8 秒。
* 文件浏览器列出当前目录（可进子目录），不做递归扫描、不读音频标签，时长需载入后才知道。

## 第三方

Dear ImGui (MIT) · miniaudio (public domain / MIT-0) · stb_image_write (public domain) ·
LLVM-MinGW 工具链 (Apache-2.0 with LLVM exceptions)。
