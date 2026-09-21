# djdeck — 双音轨播放器 (Windows)

主页就是播放器：两条音轨、播放指针固定在中央的滚动波形、每条音轨一条独立队列
（顺序 / 列表循环 / 单曲循环 / 随机）、倍速播放（0.50×–2.00×）与音量、文件浏览器与拖放。
混音那一套（3 段 EQ、交叉推子、渐入渐出、自动混音、8 秒循环）收进由顶栏 **MIXER** 按钮
（或 `M` 键）打开的**混音台**浮窗，不占主页空间。
队列、播放模式、浏览器目录与混音台开关会随会话保存，下次启动原样恢复。

Windows 原生：Win32 + Direct3D 11 + Dear ImGui + miniaudio (WASAPI 低延迟共享模式)。

## 构建 / 运行

需要 Git Bash + curl + python（脚本会在首次运行时把可移植的 LLVM-MinGW 工具链下载解压到 `tools/`，
约 190 MB，一次性；仓库里不含工具链二进制）。

```bash
bash build.sh                                  # 首次会自动拉工具链，之后增量编译
build/djdeck.exe                                # 交互式（会恢复上次的队列）
build/djdeck.exe --selftest                     # 音频引擎测试（29 项，退出码 = 失败数）
build/djdeck.exe --selftest --allow-no-device   # 无声卡环境（CI）：设备检查记 SKIP 而不是失败
build/djdeck.exe --playlist-test                # 队列续播端到端：顺序推进 + 单曲循环重播
build/djdeck.exe --gapless-test                 # 换曲无缝端到端：下一首预载 + 直接移交（量测间隔）
build/djdeck.exe --resume-test                  # 断点续播端到端：停在 12 s -> 重新载入 -> 从 12 s 继续
build/djdeck.exe --drop-test                    # 拖放规则测试（播放中拒绝 / 暂停可放且保持暂停）
build/djdeck.exe --jitter-test                  # 播放中波形位置抖动测试（不允许回退）
build/djdeck.exe --autoplay --screenshot out.png --frames 320   # 渲染真实 UI 存 PNG
build/djdeck.exe --autoplay --mixer --screenshot mix.png        # 同上，并打开混音台窗口
build/djdeck.exe --autoplay --no-panels --screenshot player.png # 同上，并收起浏览器/队列（纯波形）
build/djdeck.exe --gen-demo DIR                 # 只生成演示 WAV
build/djdeck.exe --load a.wav b.wav             # 指定两首歌启动
```

首次启动会在 exe 目录生成演示曲子：`demo_a.wav`（四拍）、`demo_b.wav`（十六分踩镲）、
`demo_c.wav`（稀疏半拍，用于演示换歌）。

## 界面布局

```
顶栏  [MIXER] [PANELS ON]  File  Help              focus: DECK A | 设备名 | player
[ Deck A：标题 / LOAD RECENT EJECT / 时间 ]
[ 播放控制：PLAY CUE | SPEED − 1.00× + 0.75× 1× 1.25× 1.5× 2× | VOL | 电平表 ]
[ 缩放波形（播放指针固定在正中，波形自右向左滚动）                        ]
[ 全曲概览条（含视窗框，可点可拖）                                        ]
[ Deck B：同上                                                            ]
[ 文件浏览器 FILES ][ 播放列表 A ][ 播放列表 B ]   ← PANELS 按钮可整块收起，只剩两条波形
状态栏：设备 / 采样率 / 块大小 / CPU / callback
```

**混音台**是独立浮窗（顶栏 MIXER 按钮或 `M`），关掉不影响播放：

```
[ DECK A 曲名 ]  LOOP 8s | FADE IN 4s | FADE OUT 4s | GAIN LOW MID HIGH | 电平表
[ 中间：CROSSFADER / XFADE 自动混音 / fade(s) / MASTER + 主电平 ]
```

## 操作

主页（播放器）

| 操作 | 说明 |
|---|---|
| PLAY / PAUSE、CUE | 播放暂停 / 回到开头并暂停；空白音轨按 PLAY 会提示 nothing loaded |
| SPEED `−` `+` | 每次 5% 步进；点数字回 1.00×；预设 `0.75× 1× 1.25× 1.5× 2×`；键盘 `-` / `=` |
| VOL | 该音轨的通道音量（与混音台的 GAIN 是同一个值） |
| 波形左键拖动 | 抓住波形左右拖 = 跳转播放时间（指针始终居中，抓到的采样点跟随鼠标） |
| 波形右键拖动 | scratch：播放速率跟随拖动速度（分数读取指针 + 插值） |
| 波形滚轮 | 以指针为中心缩放可视窗口（最短 1.5 s） |
| 概览条点击 / 拖动 | 按全曲比例跳转，视窗框同步移动 |
| **拖文件到音轨** | **只有暂停中的音轨接受**；播放中的音轨显示红框 "PLAYING - PAUSED DECKS ONLY" 并拒绝。放下后该音轨保持暂停 |
| 拖文件到播放列表 | 插到该行位置；拖到空白处追加；列表内上下拖动 = 排序；也可跨 A/B 列表拖动 |
| 双击浏览器里的文件 | 载入当前聚焦的音轨（同样只接受暂停的音轨） |
| LOAD / RECENT / EJECT | 原生文件对话框 / 最近文件 / 卸下当前曲目 |
| 快捷键 | `Space` 播放暂停 · `Q` 回零暂停 · `M` 混音台 · `1`/`2` 聚焦音轨 · `←`/`→` ±5 s · `L` 载入 · `X` 自动混音 |

混音台（浮窗，DJ 功能）

| 操作 | 说明 |
|---|---|
| LOOP 8s | 从当前位置开始 8 秒循环 |
| FADE IN / FADE OUT | 按 `fade(s)` 时长渐入 / 渐出（渐出结束自动暂停） |
| GAIN / LOW / MID / HIGH | 通道增益 + 3 段 EQ（旋钮左端 = KILL，−96 dB） |
| CROSSFADER | 等功率交叉推子（中心 −3 dB） |
| XFADE > | 自动混音：当前音轨渐出 + 另一条跳到开头渐入 |
| MASTER + 右表 | 主输出增益与限幅后的峰值 |

## 预载（换曲无缝）与断点续播

**预载**：音轨还剩不到 12 秒时，队列里的下一首就在工作线程上解码好握在手里；
到点后直接把解码结果交给音频线程（`pending`），换曲间隔只剩一个音频块级别，
而不是「解码 + 载入」的全部时间。实测移交延迟 **0.019 s**（`--gapless-test`）。

* 只有真正会继续播下去的场景才预载：音轨在播、没被用户暂停、队列里确实有下一首。
* 队列模式为 `SHUF` 时，一轮结束、需要重新洗牌的那一刻不预载（洗牌只能在真正推进时决定），
  这一次退回普通载入路径。
* 用户手动载入 / EJECT 会立刻丢弃已预载的音轨，不会白占内存。

**断点续播**：每约 1 秒记录一次两条音轨的播放位置（按文件路径），随会话保存。
再次**手动**载入同一个文件（LOAD / RECENT / 双击 / 拖放 / 队列的 play 按钮）时，
若上次停在 3 秒之后且距离结尾还有 5 秒以上，就从那里继续，并在状态栏说明；
队列**自动**推进的那一首永远从头播放。CUE 键回到 0 秒，位置记录随即被覆盖为 0。

## 播放列表与自动续播

* 每条音轨一条独立队列，`> ` 标记当前项，标题栏显示 `[playlist n/N]`。
* 每队列一个播放模式（下拉框选，鼠标悬停有说明）：

| 模式 | 行为 |
|---|---|
| `SEQ` 顺序 | 放完自动下一首，到队列末尾**停止** |
| `ALL` 列表循环 | 放完最后一首回到第一首 |
| `ONE` 单曲循环 | 反复重播当前这首 |
| `SHUF` 随机 | 一轮之内的顺序是随机排列：每首恰好一次，且不会连播同一首；一轮结束重新洗牌 |

* 只有**不是用户暂停**的音轨才会自动续播：按过 PAUSE / CUE、或手动载入（拖放 / LOAD / 双击浏览器）
  都会让该音轨停下等待，直到再次按 PLAY。
* 载入是异步的（worker 线程），所以续播由「载入完成 → 自动播放」两步完成，中途不阻塞音频。

## 会话持久化

`build/djdeck_session.txt`（与 exe 同目录）保存两条队列、两个播放模式、每个文件的播放位置
（最多 400 条）、浏览器目录、混音台与面板开关，退出时写入、启动时恢复；
测试与截图模式（任何 `--` 参数）都不会读写它。
找不到的文件在恢复时会被跳过，所以移动/删除过的曲目不会让队列废掉。

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
src/main.cpp            Win32 + D3D11 + ImGui 前端：顶栏、播放控制、波形、队列、浏览器、混音台浮窗
third_party/imgui/      Dear ImGui（docking-free，2026-09-18 快照 420f179，MIT）
third_party/miniaudio.h 音频 I/O（public domain / MIT-0）
third_party/stb_image_write.h  截图编码（public domain）
.github/workflows/      CI：windows-latest 上构建 + 跑引擎自测
docs/                   截图
tools/                  构建时下载的工具链（已 gitignore）
```

## 验证

| 测试 | 覆盖 |
|---|---|
| `--selftest`（29 项） | 交叉推子隔离与等功率律 · 限幅器不超 0 dBFS · 渐出到静音并自动暂停 · 跳转无爆音（与连续信号自校准）· EQ 增益/KILL · 热换轨时另一轨输出不变且无 dropout · 旧轨道回收 · 四种队列模式的推进规则（含随机的不连播、每轮全覆盖、空队列/单曲边界）· 预载的预判与随后真正推进的结果完全一致（遍历所有模式 × 1–4 首 × 每个位置）· 倍速确实按 0.5×/1×/2× 消耗音频 · 真实 WASAPI 回调速率与 CPU 负载 |
| `--playlist-test` | 端到端两段：`SEQ` 播完第一首自动载入第二首并继续播放；切到 `ONE` 后同一首被自动重播 |
| `--gapless-test` | 端到端：三首队列、连续两次换曲，每次都必须**在上一首结束前就已预载**，且结束到出声之间**没有发生任何解码**（引擎解码计数增量 = 0），并量测结束到出声的间隔（实测 0.020–0.035 s，阈值 0.15 s） |
| `--resume-test` | 端到端：把文件停在 12 s → 记录位置 → 重新载入同一文件 → 确认从 12 s 继续（不是 0）；并校验会话文件的读写往返 |
| `--jitter-test` | 播放中逐帧采样显示位置：0 次回退、单帧前进 ≤ 一帧音频量（修复前实测 23 次回退 / 最大 −0.248 s） |
| `--drop-test` | 暂停音轨接受、播放中音轨拒绝、放下后保持暂停、另一音轨不受影响 |
| CI | `.github/workflows/windows.yml`：构建 + `--selftest --allow-no-device`（万一 runner 没有声卡，设备检查记 SKIP 而不是失败；实测 GitHub 的 runner 有声卡，28 项全部真跑） |
| 截图 | `docs/screenshot.png`（播放器主页）、`docs/mixer.png`（混音台浮窗）、`docs/player.png`（收起面板的纯波形视图）、`docs/drop-rule.png`（拖放规则红/绿提示） |

## 已知取舍

* 载入时整曲解码为 16-bit PCM（45 s ≈ 8.6 MB，实测 0.192 MB/s），长曲子内存线性增长；
  低配设备应换成流式预读 + mmap PCM 缓存。
* 预载会让内存峰值翻倍（当前曲 + 已解码的下一首），且只在这条音轨最后 12 秒内出现；
  长时间连续播放（每小时上千次换曲）时预载线程与解码线程共用队列，不会堆积。
* 断点续播按路径记录，文件改名/搬走就失效；同时它是「退出时写入」，强杀会丢掉最后约 1 秒的记录。
* 峰值列密度固定（最多 16384 列），放大超过约 3 倍会出现柱状台阶；
  更深缩放需按视窗动态重建纹理。
* 倍速是磁带式变速（音高随速度），不是 keylock；`LOOP` 是定长 8 秒，无节拍网格。
* 只解码 miniaudio 内置的四种格式：**WAV / FLAC / MP3 / OGG(Vorbis)**。
  `.m4a`、`.opus` 需要额外的解码后端，因此不在浏览器和文件对话框里列出；
  拖入不支持的文件会在状态栏说明原因。
* 文件浏览器列出当前目录（可进子目录），不做递归扫描、不读音频标签，时长需载入后才知道。
* 不注册 miniaudio 的 `notificationCallback`，所以拔出/切换输出设备时界面不会有提示。

## 第三方

Dear ImGui (MIT) · miniaudio (public domain / MIT-0) · stb_image_write (public domain) ·
LLVM-MinGW 工具链 (Apache-2.0 with LLVM exceptions)。
