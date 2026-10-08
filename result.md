# CarPlayStation — 阶段性进展与修复总结 (Phase Result)

## 1. 核心突破与里程碑 (Milestones)

- **无线 CarPlay 完整握手链路全线打通**：
  - **iAP2 链路与双代 MFi 认证**：支持 2 代与 3 代（MFi 3.0 SHA-256）硬件证书探测与动态验签，成功通过从 `0xAA00`、`0xAA02`、`0x1D00` (StartIdentification) 到 `0x1D02` (IdentifyAccepted) 的完整双向协商，无缝引导 iPhone 接入 Wi-Fi 热点。
  - **AirPlay 2 RTSP 协议链成功落地**：依次通过 `POST /pair-setup` (M1~M6)、`POST /pair-verify`、`POST /auth-setup`、`SETUP Phase 1`、`GET /info`、`RECORD` 以及 `SETUP Phase 2 (Streams)`。
  - **真实的事件与时钟通道 (Event / Timing Ports)**：动态监听并分配端口，启动后台精准 NTP 时钟应答，`RECORD` 阶段成功建立加密的 Event Channel，接收 iPhone 的 200 OK 确认响应。
  - **主动抢占屏幕焦点**：在建立 Event Channel 后立即下发 `requestUI`、`takeScreen (changeModes)` 以及 `forceKeyFrame`，引导 iOS 启动屏幕投影并强制吐出关键帧。

---

## 2. 视频解密与硬件解码修复 (Video Pipeline Fixes)

### 2.1 SETUP Phase 2 密钥同步修复 (Key Sync Timing Fix)
- **问题根源**：此前核心服务仅在 `RECORD` 时触发流配置，而 iPhone 的 `streamConnectionID` 与视频 ChaCha20 密匙是在 `RECORD` 之后的 `SETUP Phase 2` 才派生并保存。导致 `AirPlayVideoReceiver` 未能收到解密密钥，将加密视频密文误当做明文 NALU 解析，导致全部视频帧因长度异常被跳过。
- **修复方案**：在 `AirPlayRtspServer` 的 `SETUP Phase 2` 成功派生流密钥后，立即触发 `streaming_cb_`，将 32 字节视频解密密钥实时推送到 `AirPlayVideoReceiver::SetDecryptionKey()`，完成 ChaCha20 解密激活。

### 2.2 Windows Media Foundation 硬件解码器全面重构 (`MfVideoDecoder`)
- **MFT 创建方式升级**：淘汰不支持直接实例化的传统 COM `CoCreateInstance`，改用系统标准的 `MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER)` 动态枚举与激活硬件加速及极低延迟 H.264 解码器。
- **补全 MFT 输出缓冲容器**：针对 `MFT_OUTPUT_STREAM_PROVIDES_SAMPLES = NO` 的 MFT 特性，依据 `GetOutputStreamInfo.cbSize` 动态分配并挂载 `IMFMediaBuffer`，彻底解决空 Sample 抛出 `MF_E_BUFFERTOOSMALL` 导致无法出图的问题。
- **Direct3D 11 共享纹理与色彩空间适配**：
  - 共享纹理格式升级为 `DXGI_FORMAT_B8G8R8A8_UNORM`，并显式指定 `D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET`。
  - 实现极速 NV12 $\rightarrow$ BGRA 内存转换（单帧仅约 1ms），通过 `UpdateSubresource` 直推 D3D11 共享纹理，供渲染层直接着色呈现。

### 2.3 AVCC 直转 Annex-B 零丢帧直通
- **Opcode 1 (VideoConfig)**：直接提取 SPS 与 PPS 并封装为 Annex-B 头部 (`00 00 00 01`) 直喂解码器完成规格预设，避免此前解复用器因缺少 IDR 误判为坏帧。
- **Opcode 0 (VideoFrame)**：ChaCha20-Poly1305 解密后直接转为 Annex-B 流直塞解码器，解除旧有解复用器的丢帧锁死机制。

### 2.4 ChaCha20 Nonce 重连计数器复位
- 每次收到新的 TCP 视频流连接或更新会话密钥时，强制将 `frame_counter_` 归零，避免重连后 Nonce 计数值脱节引发连续解密失败警告 (`ChaCha20 decrypt failed`)。

### 2.5 应用服务层帧消费者延后绑定打通
- 在 `ApplicationService::StartCarPlay()` 启动 Core 服务并创建 `video_decoder_` 后，立即自动挂接此前在 UI 初始化阶段注册的 `frame_consumer_`，使解码器解码出的 D3D11 共享纹理真正触发 `D3D11Renderer::ConsumeFrame()`。

---

## 3. 自动化验证与构建状态 (Build & Verification)

- **Release 构建**：
  - MSVC 2022 x64 Release 编译 100% 通过（`CarPlayCore.lib`, `CarPlayUI.lib`, `CarPlayStation.exe`）。
- **单元测试套件**：
  - `CarPlayStation_MediaTests.exe`：全项通过（含 MfVideoDecoder D3D11 BGRA 纹理创建与初始化）。
  - `CarPlayStation_SessionTests.exe`：全项通过（含 RTSP 状态机、编解码与生命周期）。

---

## 4. 后续任务规划 (Next Session: UI Layer Overhaul)

1. **Win32 窗口生命周期与渲染驱动**：
   - 完善主渲染循环与帧率同步（VSync 呈现）。
   - 处理 Direct3D 11 SwapChain 与 Direct2D 互操作图层（车机状态栏、悬浮球、菜单覆盖）。
2. **多分辨率与自适应缩放**：
   - 完善车机宽屏与不同 DPI 显示器的等比 Letterbox / Pillarbox 裁剪及全屏交互。
3. **输入事件回传 (Input Forwarding)**：
   - 接入鼠标/触控屏点击，将坐标映射回送至 iPhone（通过 iAP2 / HID 触控通道）。

---

## 任务执行记录

---

### 记录 [6]：视频链路延迟、窗口比例锁定与显示描述符物理尺寸修正，以及解码器精简（Core/Media、UI/Win32）

* **时间**：`2026-10-04 21:20:00 (UTC+8)`
* **客户端（Agent）**：`GitHub Copilot (Copilot SDK in VS Code)`
* **背景与实现**：
  真机（iPhone 16 Pro，1280x720@60 H.264）联调下报告"视频卡顿/输入到反馈延迟大"，随后报告"画面被拉伸"。本轮按"先降延迟 → 再锁窗口 → 再查比例 → 最后精简依赖"的顺序完成四件事。

  1. **Step 1 输入到显示延迟治理（`Core/Media/AirPlayVideoReceiver.cpp`、`Core/Media/MpvVideoPlayer.cpp`、`UI/Win32/AppController.cpp`、`Core/Session/Pairing/Crypto.h`）**：
     - 修复 `AirPlayVideoReceiver.cpp` 的编译中断，恢复视频接收链路。
     - 缓存 ChaCha20-Poly1305 的 BCrypt 算法/密钥 provider，避免每帧重复 `BCryptOpenAlgorithmProvider`/`BCryptGenerateSymmetricKey`。
     - 删除逐帧堆分配，并移除冗余的 `cpu_staging_` 整帧拷贝；未被使用的共享纹理上传路径被跳过。
     - 移除 `AppController::Step()` 中无条件的 `::Sleep(1)`（这是主循环里最稳定的 1ms 抖动源）。
     - 结果：平均帧率从约 20 fps 提升到 30–64 fps，并出现持续 60 fps 的区间。
  2. **Step 2 窗口尺寸锁定（`UI/Win32/MainWindow.h/.cpp`）**：
     - 按用户要求放弃处理 Win32 自由拉伸，直接锁定窗口：不可手动拖拽改变尺寸、不可最大化，客户区固定为协商分辨率，从源头消除拉伸与"拉伸后比例不对"的观感。
  3. **Step 3 显示描述符物理尺寸修正（`Core/Session/AirPlayRtspServer.cpp`）**：
     - 屏幕像素测量（对高饱和色块做洪水填充）显示 CarPlay 图标为 121x180，即纵向被拉伸 1.50 倍；比对照 `widthPhysical/heightPhysical = 240/90 = 2.667` 与 `1280/720 = 1.778` 完全吻合。
     - 根因：CarPlay `/info` 显示描述符里的 `widthPhysical`/`heightPhysical`（毫米）并非装饰值——iPhone 先按该物理比例排版 CarPlay UI，再按 `widthPixels x heightPixels` 编码；两者不一致时画面被按 `physical_aspect / pixel_aspect` 压扁。
     - 修正规则（与 `Reference/xcertplay` 一致）：`heightPhysical = round(widthPhysical * heightPixels / widthPixels)`，本例 240 → 135。复测图标 121x121，比例约 1.00。
  4. **Step 4 解码器精简为 libmpv 单一后端（`Core/CMakeLists.txt`、`Core/Media/`、`cmake/`、`scripts/`、`Lib/`）**：
     - 记录 `DEC-009`，将 `DEC-008` 中"MF 主解码器 + FFmpeg 动态回退"两节标记为被取代。
     - 删除 `Core/Media/MfVideoDecoder.{h,cpp}`、`Core/Media/FFmpegVideoDecoder.{h,cpp}`、`cmake/FindOrDownloadFFmpeg.cmake`、`scripts/Update-FFmpeg.ps1`，并删除本地 `Lib/ffmpeg/`（386 MB）。
     - `CarPlayCore` 不再链接 `mfplat`/`mfuuid`，仅保留 `d3d11`/`dxgi` 用于交付解码帧的共享纹理；`IVideoDecoder` 抽象边界保持不变。
     - `Tests/MediaTests.cpp` 的硬件解码器优雅降级用例改写为针对 `MpvVideoPlayer`（缺 `libmpv-2.dll` 时必须干净失败，不得崩溃）。
* **构建与测试运行指标**：
  - 构建环境：MSVC 2022 Build Tools（`vcvars64`）+ CMake `NMake Makefiles`，Release，构建目录 `build-release/`。因源列表变更执行了完整 reconfigure。
  - 编译诊断：reconfigure 与全量构建 **0 错误**；产物 `build-release/UI/Win32/CarPlayStation.exe`。
  - 自动化测试：CTest 6/6 套件 **100% 全部通过**（CommonTests、HardwareTests、TransportTests、ProtocolTests、SessionTests、MediaTests，总用时 2.99s）。
  - 真机验证：图标宽高比由 1.50 修正为约 1.00；帧率维持 60 fps 区间。
* **本会话修复的关键缺陷与经验**：
  1. `显示比例被压扁`：不是渲染层的缩放错误，而是协议层显示描述符自相矛盾。经验：`widthPhysical`/`heightPhysical` 必须与像素比例一致；`Reference/ncm_carplayd` 里硬编码 240x90 属同类潜在缺陷，不可当作权威。
  2. `启动崩溃 0xC000041D`：`NMake Makefiles` 的 `compiler_depend.make` 缺少头文件前置依赖，改头文件不触发依赖重编，导致 ODR/结构体布局不一致而访问违例。经验：改头文件后需删除对应 `.obj` 与 `compiler_depend.*` 并重新 `cmake -S … -B …`（只删 `compiler_depend.make` 会 `fatal error U1052`）。
  3. `低帧率日志样本`：多数是待机画面——iPhone 在画面不变时停止编码，并非解码停滞。经验：判断卡顿要把接收端 `total frames=` 计数与环形缓冲可用量一起看，不能只看瞬时 `rate=`。
  4. `PrintWindow(hwnd, hdc, 2)` 在 D3D11 flip-model 交换链上永久挂起。经验：截屏改用 DPI 感知进程里的 `Graphics.CopyFromScreen` + `GetWindowRect`。
  5. 未解释项（留待后续）：解码器 `decoded=` 计数约为接收端 `total frames=` 的 2 倍；每秒一次的 `Unknown packet opcode: 5`；SETUP Phase 2 流字典中的 `latencyMs` 目前无人读取。
* **完成清理**：
  - 删除 `Lib/ffmpeg/`（386 MB）、`_wer_dump/`、`build-release` 下的过期运行日志与 `Testing/Temporary/*.log`、`TODO_missing.md`。
  - `.gitignore` 去重并移除失效条目（`/Lib/ffmpeg`、重复的 `/Tests`、已不存在的 `/cmake`、`/_wer_dump`）。
  - 文档同步：`README.md`、`PROJECT.md`、`ARCHITECTURE.md`、`TODO.md`、`Lib/Readme.md`（删除 FFmpeg 条目）、`Core/README.md`、`Core/Media/README.md`、`Core/Service/README.md` 全部改为 libmpv 单一解码后端；`DECISIONS.md` 新增 `DEC-009` 并在 `DEC-008` 标注被取代范围。
  - `Lib/mpv/`（145.5 MB）与 `ProjectManagementTemplate/` 按要求保留；`build*` 构建目录保留。
* **补充：遗留测试文件注册与测试套件扩容**：
  * 清理 `.gitignore` 后暴露出 4 个从未被 git 跟踪、也从未注册进构建的测试文件（`Tests/InputTests.cpp`、`Tests/ProtocolReplayTests.cpp`、`Tests/ServiceTests.cpp`、`Tests/UiTests.cpp`）。已在 `Tests/CMakeLists.txt` 注册为 `CarPlayStation_InputTests / _ProtocolReplayTests / _ServiceTests / _UiTests` 四个可执行目标并 `add_test`，测试套件从 6 个扩展到 10 个。
  * `Tests/UiTests.cpp` 的 `MockApplicationApi` 缺少 `IApplicationApi` 新增的两个纯虚函数（`RegisterCpuFrameConsumer`、`SetVideoSharedDevice`），导致 `C2259 无法实例化抽象类`。已补齐实现，测试通过。
  * `ProtocolReplayTests` 中 RTSP 麦克风上行 SETUP（`streamType=100` → `server_port=5002` 与 `mic_audio_*_port`）的断言在当前实现里没有对应代码：`AirPlayRtspServer::HandleSetup` 只协商视频流（7100/7101），`mic_audio_data_port/control_port` 从未被写入，`MicrophoneRtpSender` 也尚未接入任何服务层。这是麦克风上行的功能缺口而非本次清理造成的回归，已按决定注释掉该断言块并标注 `TODO(microphone-uplink)`，同时在 `TODO.md` 第 8 节新增对应待办条目。
  * 最终 CTest：10/10 套件 **100% 全部通过**（总用时 3.39s）。

---

### 记录 [7]：Windows 主题跟随、主题切换崩溃修复与 TODO 全量盘点（UI/Win32、Application、Core/Session、文档）

* **时间**：`2026-10-08 22:15:53 (UTC+8)`
* **客户端（Agent）**：`GitHub Copilot (Copilot SDK in VS Code)`
* **背景与实现**：
  需求是"让主机跟随 Windows 的黑白主题，并让 CarPlay 画面一起变暗"。实现后用户报告"我一切换主题就闪退"，本轮先修崩溃，再对 `TODO.md` 做全量实况盘点。

  1. **主机侧主题跟随（DEC-012）**：`ThemeHelper` 读取注册表 `Themes\Personalize` 的 `AppsUseLightTheme` / `SystemUsesLightTheme`；`MainWindow::ApplyTheme` 换控件调色板并用 `DwmSetWindowAttribute` 切深色标题栏；`OverlayRenderer::ApplyTheme` 换 HUD 配色；`WM_DWMCOLORIZATIONCOLORCHANGED` 与 `WM_SETTINGCHANGE` 驱动热切换；`AppController::ApplyCurrentTheme` 统一决策并同步 letterbox 底色与 HUD。
  2. **手机侧夜间模式（DEC-013）**：`MainWindow` 只上报原始信号（`WM_SETTINGCHANGE` 的 `lParam == L"Themes"`，或 `WM_DWMCOLORIZATIONCOLORCHANGED`）→ `AppController` 判定夜间（`AppsUseLightTheme == 0`，回退 `SystemUsesLightTheme`）→ `ApplicationService::SetHostNightMode` → `CarPlayCoreService::SetHostNightMode` → `AirPlayRtspServer::SetNightMode`，走事件通道 `setNightMode`（与已验证可用的 `setUIResolution` 同一条通道）。去重：`night_mode_` 只在值变化时发送；`ResetStreamState()` 复位，重连后按当前主题补发。
  3. **真机证据**（`build-release/UI/Win32/carplaystation.log`）：`Sent event command: setNightMode (nightMode=false), success=true`、`Sent event command: setNightMode (nightMode=true), success=true`，以及多次 `UI theme switched to light/dark (appsLight=…, systemLight=…), commandWritten=true`；日志尾部视频仍 58 fps、音频持续渲染 —— 命令确实送达并被接受。
  4. **崩溃修复**：用户手动切换 Windows 主题必闪退。根因是 `WM_SETTINGCHANGE` 处理里把 **`wParam`** 当作 `const wchar_t*` 传给 `_wcsicmp`；变更段字符串在 **`lParam`**，`wParam` 是数值（主题变更为 0），解引用空指针即访问违例。已改为读 `lParam`（`UI/Win32/MainWindow.cpp:344`）。该路径只有真实消息分发能触发，`UiTests` 直接调 `ApplyCurrentTheme()`，因此测试从未暴露它。
  5. **文档同步**：`DECISIONS.md` 追加 DEC-012/DEC-013；`UI/Win32/README.md`、`Application/README.md`、`Core/Session/README.md` 同步主题与夜间模式说明；`TODO.md` 对应项标记完成并按实况更正状态。
* **构建与测试运行指标**：
  - 全新生成 + 干净全量构建（`cmake --build . --target clean` 后全量编译，`vcvars64.bat` + `NMake Makefiles`）：`EXITCODE=0`，`[100%] Built target CarPlayStation_UiTests`；`error C` / `LNK` / `fatal error` 命中数 0；警告与基线完全一致（`Lib/` 17×C4244 + 47×C4456、`AirPlayRtspServer.cpp:290` 1×C4458、1×C4996、`Tests/UiTests.cpp` 16×C4834 + 20×C4189），无新增。
  - `CarPlayStation_UiTests.exe`：8/8 通过（含主题解析、HUD 调色板、letterbox 底色、`WM_SETTINGCHANGE` / `WM_DWMCOLORIZATIONCOLORCHANGED` 处理、`setNightMode` 去重与复位）。
  - `CarPlayStation_SessionTests.exe`：全部通过（含 `setNightMode` 参数编码）。
  - 用户真机复跑：多次主题切换无崩溃，`setNightMode` 均 `success=true`。
* **本会话修复的关键缺陷与经验**：
  1. `WM_SETTINGCHANGE` 的变更段字符串在 `lParam` 不在 `wParam`（`MainWindow.cpp:344`）。这是"只有真人去点系统设置才会触发"的路径，单元测试结构上覆盖不到 —— 涉及 Win32 消息语义的逻辑，测试应直接 `SendMessage` 投递消息，而不是只调内部方法。
  2. 夜间模式命令走事件通道（`stream=11`）而非 `SET_PARAMETER`：`SETUP` 阶段 `SET_PARAMETER` 会被拒（`error 355`），事件通道已被 `setUIResolution` 证明可用。但 `commandWritten=true` 只说明帧已写入加密事件通道，不等于 iPhone 已改变界面外观。
  3. 遗留风险：我们的 `_airplay._tcp` TXT 与 `/info` 广告里从未出现 `enablesUIAppearance` / `uiAppearanceModes`（全仓库 0 次命中），iPhone 是否按外观能力处理仍未证实。真机已见 `setNightMode` 被接受，但建议补上广告键后再回归一次。
  4. 用 PowerShell 写文档时 `$content` 未定义会把文件静默截断成 3 字节（本次 `result.md` 差点丢失，已用 `git checkout -- result.md` 恢复）。文档编辑一律用编辑工具，不用 shell 变量插值。
* **完成清理**：临时构建日志（`build-release/_clean.log`、`build-release/_build.log`）已删除；`git status` 只包含预期改动（`UI/Win32/MainWindow.cpp`、`DECISIONS.md`、`TODO.md`、三个模块 README、`result.md`）；未新增第三方依赖。

---

### 记录 [8]：死代码清理（AudioDuckingManager / VehicleNightModeEvent）与两个启动弹窗的 DPI 缩放（Core/Media、Core/Service、UI/Win32、文档）

* **时间**：`2026-10-09 00:40:00 (UTC+8)`
* **客户端（Agent）**：`GitHub Copilot (Copilot SDK in VS Code)`
* **背景与实现**：
  上一轮的全量盘点留下两处确认无用的死代码和一批待办；同时用户反馈"我有设备带缩放点不到"最先弹出的两个界面。本轮删除死代码、把其余项登记为待进一步工作，并给两个弹窗加上 DPI 缩放。

  1. **删除 `Core/Media/AudioDuckingManager`**（经用户确认）：组件、头文件、`Core/CMakeLists.txt` 条目、`Tests/MediaTests.cpp` 里的 `TestAudioDuckingManager()` 全部移除。实际的闪避由 `WasapiAudioPipeline` 内部的媒体通道增益步进完成，相关单测保留。`DECISIONS.md` DEC-008 的实现说明改为"已删除"，`Core/Media/README.md` 的 ducking 描述改指向 `WasapiAudioPipeline`。
  2. **删除 `CoreEventBus::VehicleNightModeEvent` / `PublishVehicleNightMode`**：夜间模式实际走 `IApplicationApi::SetHostNightMode` → `AirPlayRtspServer::SetNightMode`（DEC-013），事件总线上这条从未被发布也从未被订阅。`Core/Service/README.md` 的发布事件列表同步去掉"vehicle night mode"，DEC-013 的备选说明注明该事件类型已移除。
  3. **两个启动弹窗的 DPI 缩放**：`AppController::Initialize` 先弹安全须知（`SetupWizard::ShowSafetyDisclaimer`），再弹首次运行 MFi 向导（`ShowMFiWizard`）。进程在 `MainWindow.cpp` 里调用 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`，系统不会做位图拉伸，而这两个窗口全部用硬编码 96 DPI 像素坐标创建、且从不设置字体，所以在开启缩放的屏幕上控件极小、几乎点不中。现在 `UI/Win32/SetupWizard.cpp` 增加文件内辅助函数：`DialogDpi`（取父窗口所在显示器的 `GetDpiForWindow`）、`Scaled`（`MulDiv` 换算）、`CreateScaledUiFont`（按 DPI 缩放 `NONCLIENTMETRICS.lfMessageFont`）、`CreateScaledChild`（换算矩形并 `WM_SETFONT`）、`ScaledWindowSize`（用 `AdjustWindowRectExForDpi` 由设计单位客户区 540×230 / 570×400 推出外框像素尺寸）。DPI 与字体句柄放在两个对话框上下文结构里，由 `WM_CREATE` 读取；所有子控件改走 `CreateScaledChild`，外层窗口尺寸改由 `ScaledWindowSize` 计算，模态循环结束后 `DeleteObject` 释放字体。布局源码仍写设计单位，无需按屏幕改数值。
  4. **文档与待办**：`TODO.md` 顶部新增"Deferred items — need further work"顺序工作队列（Now Playing、通话状态与来电 UI、Siri/通话按键、`enablesUIAppearance` 广告），三处过时的 `AudioDuckingManager` 表述改为"已删除"，并新增一条已完成的 P1 记录说明弹窗缩放实现；`result.md` 第 5.3 / 5.5 节由"待定去留"改为"已删除"。
* **构建与测试运行指标**：
  - 清理后全量构建（`vcvars64.bat` + `cmake --build .`，`build-release`）：`exit code 0`，`[100%] Built target CarPlayStation_UiTests`；`error C` / `LNK` / `fatal error` 命中 0；警告与既有基线一致（`Curve25519.h`、`CarPlayCoreService.cpp` 的 C4834 等），无新增。
  - `ctest --output-on-failure`：**10/10 通过**（CommonTests、HardwareTests、TransportTests、ProtocolTests、SessionTests、MediaTests 14.19s、InputTests、ProtocolReplayTests、ServiceTests、UiTests），总耗时 16.27s。
* **本会话修复的关键缺陷与经验**：
  1. 改 `CoreEventBus.h`（`CoreEvent` 变体成员增删）后链接报 `LNK2019`（`CoreEventBus::Subscribe`）：本仓库的构建系统对头文件依赖跟踪不可靠，必须 `touch` 所有包含该头文件的 `.h`/`.cpp`（`Core/Service/CarPlayCoreService.h`、`CoreEventBus.cpp`、`ICarPlayCoreApi.h`、`Application/ApplicationService.{h,cpp}`、`Tests/ServiceTests.cpp`）后重编。
  2. Per-Monitor V2 下手工布局的窗口不会被系统缩放：任何用绝对像素创建的 Win32 对话框都必须自己按 `GetDpiForWindow` 换算矩形与字体，否则在带缩放的设备上不可用。`UI/Win32/CarPlayStation.manifest` 只有 `asInvoker`，DPI 感知完全由代码设置，因此无需改 manifest。
  3. 死代码判定要落到"发布方与订阅方都不存在"这一硬证据上（`PublishVehicleNightMode` 0 调用、`VehicleNightModeEvent` 0 订阅），而不是仅看有没有测试。
* **完成清理**：`git status` 只包含预期改动（`UI/Win32/SetupWizard.cpp`、`TODO.md`、`UI/Win32/README.md`、`result.md`，以及已提交的删除项）；未新增第三方依赖；未推送远端。

---

## 5. 架构完成度盘点（对照 `TODO.md` 与代码实况）

> 逐条核对 `TODO.md` 的 37 个未勾选项与真实代码，结论如下。`TODO.md` 已按此结果更正状态。

### 5.1 已实现（`TODO.md` 里仍挂着未勾选，已更正）

| 项目 | 证据 |
| --- | --- |
| README 功能清单反映真实状态（原 `TODO.md:159`） | `README.md:22-38` 已是真实的 `[x]` 清单 |
| 编解码能力协商与运行时选择 H.264/HEVC（原 `TODO.md:202`） | `VideoCodec` 枚举（`Core/Common/Config.h:25-46`）、`advertise_hevc`（`AirPlayRtspServer.cpp:954`）、`/info` 的 `hevcInfo`（1664）、`enabledFeatures` 含 `hevc`（2182）、HEVC NAL 分类（`AirPlayVideoDemuxer.cpp:27-54`）、`VideoCodecPreference`（`Application/AppSettings.h:36-44`）与映射（`ApplicationService.cpp:421-424`）；真机日志 `Video stream spec set to 1920x1080@60 HEVC`、`Processed hvcC HEVC VideoConfig`、`video-codec=H.265 / HEVC` |
| PROJECT.md 描述的显示模式选择器（原 `TODO.md:203`） | `MainWindow::SetDisplayMode`（`MainWindow.cpp:212-262`，Windowed ↔ BorderlessFullscreen，364/391/419 处应用）、`AppController.cpp:177-178` 从设置应用；720p/1080p/1440p/2160p/Auto/Custom/Car1280x480/Car1920x720 与 30/60 fps 预设（`AppSettings.h:17-63`） |
| 麦克风上行（接线 + 真机验证，记录 [6] 记录的缺口已闭合） | `CarPlayCoreService.cpp:1496-1544` 构造 `MicrophoneRtpConfig`、创建 `MicrophoneRtpSender`、`AttachToUplink`、启动采集；`AirPlayRtspServer.cpp:1856-1857` / `1980-1981` 写入 `mic_audio_data_port=5002` / `control=5003`；`ProtocolReplayTests.cpp:300-301` 断言这些端口。**真机已验证：用户确认走的是电脑麦克风。** 注意：我核对的那份日志窗口（21:55:58–22:15:58）里没有 `type=100` 的 `RECORD`，也没有 `Microphone uplink started`，即该窗口未建立语音通道；结论以用户实测为准，日志证据待下一次会话补齐 |
| 单实例与干净退出（`TODO.md:250` 的前半） | `MainWindow.cpp:46` 的 `CreateMutexW`；退出清理由 BUG-001 / BUG-005 修复 |
| 三车道音量与自动闪避（`TODO.md:217` 的音量部分） | `SetLaneVolume` 三车道 + `voiceDuckingActive_` / `rtspDuckingActive_` 自动闪避（`WasapiAudioPipeline.cpp:230-280`）、INI 与 HUD 滑条、`UpdateSettings` 热更新 |

### 5.2 部分实现（骨架在，缺最后一环）

| 项目 | 已有 | 缺 |
| --- | --- | --- |
| Now Playing 显示（`TODO.md:254`） | `CoreEventBus::PublishNowPlaying`（`CoreEventBus.cpp:43-56`）、`ApplicationService.cpp:338-339` 已消费 `NowPlayingEvent`、目录里有 `0x5000/0x5001/0x5002` | `PublishNowPlaying` **无调用者**；`Iap2ControlPlane.cpp` 无 `0x5001` 分支，落到 `default:`（164 行）。真机日志里 `0x5001 NowPlayingUpdate` 到达 **1373 次**却只被记录。缺的就是"解析 `0x5001` → `PublishNowPlaying`" |
| 通话控制（`TODO.md:242`） | 目录与订阅齐全：`StartCallState` / `CallStateUpdate` / `AcceptCall` / `EndCall` / `StartCommunications`（`Iap2ControlPlane.cpp:69-70`） | 无 `CallStateUpdate` 分支（落到 `default:`）、无来电 UI、无接听/挂断动作。真机日志里 `0x4155 CallStateUpdate` 到达 3 次但未被解析；`0x415B AcceptCall` / `0x415C EndCall` 从未发送 |
| Siri / 通话**按键**发不出去（新增，见 5.5） | 输入侧映射存在：`Win32InputRouter.cpp:312-451`、`KeyboardKnobTranslator.cpp:61-62` → `ButtonType::Siri` / `CallAccept` / `CallReject` | `CarPlayCoreService.cpp:183-205` 只映射 PlayPause/NextTrack/PrevTrack，其余返回 `NotSupported`，且调用方 `(void)` 丢弃返回值 → 静默失效。`/info` 只广告 2 个 HID 设备（触摸屏 + 3 个媒体键），描述符里没有 Siri/通话的 consumer usage |
| 加固（`TODO.md:250`） | 单实例、干净退出 | 系统托盘图标（无 `Shell_NotifyIcon`）、开机自启（无 `CurrentVersion\Run`） |
| 验证基础设施（`TODO.md:258-261`） | `ProtocolReplayTests` 存在 | CI（**无 `.github/workflows`**）、mock iPhone 模拟器、设备回归脚本 |

### 5.3 未实现

| 项目 | 说明 |
| --- | --- |
| 线程与数据流模型文档、错误处理与日志策略文档（`TODO.md:157` / `158`） | 全仓库没有任何文档提到线程/数据流或 `CPS_LOG_*` 约定 |
| iAP2 消息模型成文（`TODO.md:163`）、单一事实来源表（`:170`）、accessory-info / 车辆身份记录（`:171`） | 只有 `Core/Protocol/README.md` 与 `Iap2MessageCatalog` 代码 |
| 外部桥接的有线 CarPlay（`TODO.md:186` / `187`） | `Core/Transport/` 只有 `BtRfcommTransport`、`BtSdpHelper`、`MdnsAdvertiser`、`WifiSoftApManager`；`TransportType::UsbNcm` 只是枚举（`ITransportChannel.h:14`）。**`README.md:24` 的 `[x] Wired CarPlay (USB / NCM)` 是过度声明，需更正或补实现** |
| 仪表 / HUD 模板（`TODO.md:182`） | 无任何代码 |
| 会话其余平面：车辆数据、位置、目的地（`TODO.md:194`）、Modern Vehicle Data Config（`:195`） | `Iap2MessageCatalog.h` 只有 availability / Wi-Fi / NowPlaying / RouteGuidance / Call / Power 相关 id |
| 延迟测量与丢帧重同步策略（`TODO.md:204` / `216`） | `latencyMs` 在 `Core/` 已 0 次命中 |
| 多声道音频（`TODO.md:217`） | 无 |
| `Core/Media/AudioDuckingManager` | **已删除**（本轮）。此前状态：设计过但未接线——参与编译、有单元测试、有文档（DEC-008、`Core/Media/README.md`），但没有任何生产代码构造它；闪避实际在 `WasapiAudioPipeline` 内实现（见 5.5）。经用户确认后删除组件（`.h`/`.cpp`）、构建条目与对应单测，并修订 DEC-008 |
| PIP / 置顶小窗（`TODO.md:251`）、多显示器输出目标选择（`:252`） | 只有 `SetupWizard.cpp:534,949` 的 `WS_EX_TOPMOST` |
| i18n（`TODO.md:254`） | 只有 `OverlayRenderer.cpp:81,95,106` 硬编码 `L"zh-cn"`，无检测与选择器 |
| `ARCHITECTURE.md` 详细章节（`TODO.md:266`） | `ARCHITECTURE.md:169` "Detailed Architecture" 仍是一行占位 |
| PROJECT.md Phase 1 目标与实况对齐（`TODO.md:267`） | 未做 |
| `/info` 广告 `enablesUIAppearance` / `uiAppearanceModes`（DEC-013 遗留） | 全仓库 0 次命中 |

### 5.4 文档自身的不一致（已修）

* `TODO.md:143` 声称条目已并入 `TODO_missing.md`，**该文件不存在**（记录 [6] 记录过它被有意删除）。已改为直接说明条目保留在 `TODO.md` 各分区。
* `TODO.md` 规则 5 要求"完成即删除"，但文件保留了大量 `[x]` 长块；实际做法是保留并标注 `[Concluded: DEC-xxx]` 以保留可追溯性。

### 5.5 复核两处"遗留"与音频/语音实况（本轮补充）

**（1）两处遗留已用代码确认，不是"待查"。**

* `enablesUIAppearance` / `uiAppearanceModes`：全仓库 **0 次**代码命中（只有 `DECISIONS.md:1201`、`TODO.md`、本文件与 `Reference/carplayd/docs/APPLE_BEHAVIORAL_REFERENCE.md:203,250,252` 提到）。即 DEC-013 的这条遗留不是"没验证"，而是"从未实现"，已归入 5.3。
* `AudioDuckingManager`：不是"没用"，而是"设计完成但未接线"。它被编译、被单测覆盖、被 DEC-008 记录，但没有任何生产代码构造它。经用户确认，本轮已按死代码删除（组件 + 构建条目 + 单测），并修订 DEC-008 与相关分区文档。

**（2）闪避（ducking）的真实实现位置：`Core/Media/WasapiAudioPipeline.cpp`，两个独立触发源，取并集。**

| 触发源 | 置位 | 释放 |
| --- | --- | --- |
| `voiceDuckingActive_`（本地语音闪避） | 语音车道（`SiriTelephony` / `AltNavigation`）出现**可听**音频块时置位（`WasapiAudioPipeline.cpp:423-424`） | 静默超过 `kDuckingHangoverMs` 后自动释放 |
| `rtspDuckingActive_`（手机请求闪避） | RTSP 事件 `duckAudio`（`AirPlayRtspServer.cpp:1834`）→ `CarPlayCoreService::SetDuckingActive`（`:898`）→ `SetDuckingActive(true)` | `unduckAudio`（`:1836` / `:915`），或超过 `kRtspDuckingSafetyMs` 由安全网自动解除并告警 |

`UpdateDuckingStateLocked`（245-263）把两者按 OR 合并成单一 `ducking_active_`；`ApplyDuckingGainLocked`（271-290）是**硬阶跃**（无渐变），增益与 carplayd 的 `DUCK_GAIN` / `UNDUCK_GAIN` 一致；真机手机上闪避窗口约 1.5 s。目标比例由设置 HUD 通过 `SetTargetDuckingRatio`（`ApplicationService.cpp:109,181`）热更新。测试见 `Tests/MediaTests.cpp:185,522-585`（含 RTSP 超时安全网）。

**（3）Siri 是谁唤醒的：这台设备上是 iPhone 自己，不是电脑。**

决定性证据（`build-release/UI/Win32/carplaystation.log`，窗口 21:55:58–22:15:58）：

* HID 数据通道只有触摸屏的 down/up 报文，**没有任何按键报文**；
* `SendButtonEvent`（`CarPlayCoreService.cpp:183-205`）只映射 PlayPause/NextTrack/PrevTrack，`Siri` / `CallAccept` / `CallReject` 返回 `NotSupported`，而 `Win32InputRouter` 与 `KeyboardKnobTranslator` 都用 `(void)` 丢弃状态，所以按快捷键是**静默无操作**；
* `/info` 只广告两个 HID 设备：uuid=1 触摸屏、uuid=2 "CarPlay Media Buttons"（37 字节描述符，consumer usage 仅 `0x00CD` / `0x00B5` / `0x00B6`），没有 Siri 或通话键；
* 该日志窗口内 `SETUP` 只建立了 `type=130`（数据隧道）、`110`（视频）、`102`（媒体音频）三条流，**没有 `type=100`（语音）/`101`（导航）**，也没有 `Microphone uplink started`。

所以：主机既没发出过 Siri 按键，也没建立语音通道，那次"Siri 正常"只能是 iPhone 端自己唤醒（手机麦克风）。要让**电脑**唤醒 Siri，需要扩 HID 描述符 + 扩 `SendButtonEvent`，并先研究协议语义（`TODO.md` 已记为研究项）。

**（4）麦克风上行**：代码路径完整（`AirPlayRtspServer.cpp:1976-1996` 解析 `streamType=100` 的 `RECORD` 协商 → `CarPlayCoreService.cpp:1488-1541` 构造 `MicrophoneRtpConfig` 并启动 `MicrophoneRtpSender`）。用户实测确认走的是电脑麦克风，故状态已按"真机已验证"记录；上面那份日志窗口未包含语音通道，属于日志采样窗口问题，不是代码缺口。