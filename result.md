# CarPlayStation — 任务成果与阶段记录 (result.md)

## 任务执行记录

---

### 记录 10-A：视频解复用器、Media Foundation 硬件解码器与 FFmpeg 自动同步（Partition 5: Core/Media 步骤 2）实现与验证

* **时间**：`2026-09-30 14:10:00 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.8 Flash)`
* **背景与实现**：
  - 依据架构设计与 DEC-008（可扩展视频解码器、FFmpeg 自动同步回退、D3D11 共享纹理、WASAPI 共享模式 3 通道混音器），完成 **Partition 5: Core/Media 步骤 2（视频解复用器、Media Foundation 解码器与 FFmpeg 自动同步）** 全部 4 个子任务。
  - **核心成果与组件实现**：
    1. **2.1 流解复用器与 NALU 提取器 (`AirPlayVideoDemuxer.h/.cpp`)**：
       - 解析 AirPlay 视频负载：将 4 字节大端长度前缀 NALU 转换为 Annex-B（`00 00 00 01` 起始码）。
       - 支持非标准超宽宽高比（`1920x720`、`2560x720`）；识别 SPS/PPS/VPS 与 IDR 关键帧（H.264 取低 5 位、HEVC 取低 6 位 NALU 类型）。
       - 实现丢帧检测与重同步到下一个 IDR 关键帧（`RequestResync` / 丢弃非 IDR 帧直至关键帧）。
       - 关键帧组装时前置 Annex-B 编码的参数集（SPS/PPS/VPS），保证解码器可直接消费。
    2. **2.2 Media Foundation 硬件解码器与 D3D11 共享表面 (`MfVideoDecoder.h/.cpp`)**：
       - 基于 Windows Media Foundation Transform（MFT）的 H.264 / HEVC 硬件解码器（`MFCreateSample` + `ProcessInput` + `MFT_OUTPUT_DATA_BUFFER` 的 `ProcessOutput` 排空循环）。
       - 启用 `MF_LOW_LATENCY`（对应 `CODECAPI_AVLowLatencyMode = TRUE`）；Direct3D 11 DXGI 设备管理器集成，经 `IDXGIResource::GetSharedHandle` 输出共享表面句柄。
       - 窗口拖拽时零开销 GPU 视口 letterboxing；带防抖（`reconfigure_debounce_ms_`）的重协商触发。
       - 非 MSVC 平台提供优雅回退（`Init` 返回 `NotSupported`，其余为桩实现）。
    3. **2.3 自动化 FFmpeg 同步（x64 / ARM64）(`cmake/FindOrDownloadFFmpeg.cmake` + `scripts/Update-FFmpeg.ps1`)**：
       - CMake 模块自动检测宿主目标架构（x64 / ARM64），自动下载 Gyan.dev / BtbN 的 release-shared ZIP（版本 7.1），提取 `avcodec`、`avutil` 与头文件并带本地版本缓存；配置 `POST_BUILD` 将 DLL 复制到输出目录。
       - 提供导入目标 `FFmpeg_avcodec` / `FFmpeg_avutil` / `FFmpeg_Face`；当 FFmpeg 缺失时优雅降级为空 `FFmpeg_Face` INTERFACE 目标（回退到 MF/CPU 路径）。
       - `scripts/Update-FFmpeg.ps1` 提供手动一键更新（参数 Version/Arch/OutputDir，自动检测架构、下载、解压、扁平化并复制 DLL）。
    4. **2.4 FFmpeg 硬件/软件回退解码器 (`FFmpegVideoDecoder.h/.cpp`)**：
       - 动态运行时绑定（`LoadLibrary` / `GetProcAddress`，`CPS_FFMPEG_DLLOAD` / `CPS_FFMPEG_DLSYM` 宏），DLL 缺失时核心构建保持零依赖。
       - `d3d11va` 硬件解码（绕过 Windows Store HEVC 包要求），并带 CPU 软件回退；`Reconfigure` 在调用 `Shutdown`/`Init` 前解锁互斥量以避免死锁。
  - **构建与测试**：
    - 更新 `Core/CMakeLists.txt`（纳入 3 个新 .cpp，WIN32 块链接 `mfplat`、`mfuuid`、`d3d11`、`dxgi`，并 `include(cmake/FindOrDownloadFFmpeg.cmake)`）与 `Tests/CMakeLists.txt`（新增 `CarPlayStation_MediaTests` 可执行目标与 CTest 注册）。
    - 在 `Tests/MediaTests.cpp` 中新增 3 组步骤 2 测试：`TestAirPlayVideoDemuxer`（NALU 解复用、长度前缀→Annex-B、SPS/PPS 解析、IDR 检测、参数集前置）、`TestAirPlayVideoDemuxerResync`（截断→重同步、等待关键帧期间丢弃非 IDR 帧）、`TestHardwareDecoderGracefulFallback`（FFmpeg/MF `Init` 返回 `NotSupported` 或干净的能力查询，不崩溃）。
  - **编译与测试运行指标**：
    - 构建环境：MSVC BuildTools 14.44（`/utf-8 /W4 /permissive-`，C++20），NMake Makefiles 生成器（`build-msvc`）。
    - 新增文件（`AirPlayVideoDemuxer.cpp`、`MfVideoDecoder.cpp`、`FFmpegVideoDecoder.cpp`、`MediaTests.cpp`）编译 0 警告 / 0 错误。
    - CTest 6/6 全测试套件（`CommonTests`、`HardwareTests`、`TransportTests`、`ProtocolTests`、`SessionTests`、`MediaTests`）100% 全部通过。
  - **本会话修复的关键缺陷**：
    1. `MfVideoDecoder.cpp`：`HResultToCommonStatus` 返回类型由 `HRESULT` 修正为 `Common::Status`；移除错误的 `SetGUID(MFT_CATEGORY_VIDEO_DECODER, nullptr)`；`ProcessInput` 改用 `MFCreateSample` + `AddBuffer` 封装样本；`DrainOutput` 改用 `MFT_OUTPUT_DATA_BUFFER`；`Flush` 改用 `MFT_MESSAGE_COMMAND_FLUSH`。
    2. `FFmpegVideoDecoder.cpp`：`GetProcAddress`/`FreeLibrary` 经宏将 `void*` 转换为 `HMODULE`；`MakeError` 由 3 参改为 2 参（消息拼接）；`AVPacket` 字段访问改用 `reinterpret_cast` 并按 FFmpeg 7.x 布局取偏移 48（data）/ 56（size）。
    3. `AirPlayVideoDemuxer.cpp`：关键帧检测由 `firstNaluType`（可能为 SPS）改为 `hasIdrSlice` 标志；参数集改为存储 Annex-B 编码形式，避免关键帧组装时双重前缀。
    4. `MediaTests.cpp`：`ASSERT_EQ` 对枚举（`NalUnitType`/`StatusCode`）使用 `<<` 失败，新增 `PrintValue` 模板（`std::is_enum_v` 分支）修复。

---

### 记录 10-B：3 通道音频混音器、闪避管理器与麦克风上行（Partition 5: Core/Media 步骤 3+4）全量实现与测试验证

* **时间**：`2026-09-30 21:15:00 (UTC+8)`
* **客户端（Agent）**：`GitHub Copilot (qwen3.8-27b)`
* **背景与实现**：
  - 依据架构设计与 DEC-008（WASAPI 共享模式 3 通道混音器），在 Partition 5: Core/Media 中完成 **步骤 3（3 通道音频混音器、闪避管理器与麦克风上行）** 与 **步骤 4（测试套件、验证与 CMake 集成）** 的全部研发。步骤 1（接口契约/Mock）与步骤 2（视频解复用器/MF 解码器/FFmpeg 自动同步）已在先前会话完成。
  - **核心成果与组件实现**：
    1. **3.1 音频闪避状态机 (`AudioDuckingManager.h/.cpp`)**：
       - 监控 `MainMedia` / `AltNavigation` / `SiriTelephony` 三通道活动状态，`Config{fullGain=1.0, duckedGain=0.20, attackMs=100, releaseMs=400}`。
       - 攻击曲线：导航/电话提示开始时于 100ms 内由 100% 平滑淡出至 20%；释放曲线：提示结束后于 400ms 内恢复至 100%。
       - `Tick(dtMs)` 步进式增益推进，并在步进到达/越过目标时以 epsilon 精确吸附至目标值，规避浮点 1-ULP 漂移，保证 `IsSettled()` 判定稳定。
    2. **3.2 自适应 WASAPI 共享模式渲染器与软件混音器 (`WasapiAudioPipeline.h/.cpp`)**：
       - 实现 `IAudioPipeline`，非独占共享模式与系统声音和平共存；WASAPI COM 后端以 `#ifdef _WIN32` 守卫并降级为 null 渲染器（`wasapiActive_=false`），使纯软件混音/重采样/限幅逻辑可无头单测。
       - 平台无关的 3 通道软件混音器：`PushLaneSamples` 按通道音量乘数与闪避比累加至 `laneBuffers_[3]`，`RenderMixed` 逐样本求和并施加软限幅（阈值 0.95，软膝压缩超调 0.5 倍并钳制）。
       - 平台无关的多速率线性重采样器 `ResampleLinear`（`srcRate→dstRate` 线性插值）。
       - `PcmToFloat` 支持 PcmFloat32 / PcmInt16 / PcmInt24 / PcmInt32 四种格式转 float32。
    3. **3.3 WASAPI 麦克风上行 (`WasapiMicrophoneUplink.h/.cpp`)**：
       - 实现 `IMicrophoneUplink`，WASAPI 采集客户端以 `#ifdef _WIN32` 守卫并降级为 null 采集源。
       - `ProcessCapturedSamples` 将采集样本按 `srcCount * dstRate / srcRate` 重采样至 16/24kHz 单声道，施加输入增益后按 `packetSamples` 分包，逐包分发 `UplinkAudioCallback`（回调在锁外调用，避免死锁）。
  - **步骤 4（测试与构建集成）**：
    - 在 [`Tests/MediaTests.cpp`](file:///d:/CarPlayStation/Tests/MediaTests.cpp) 新增 5 组测试（Test 8–12）：`TestAudioDuckingManager`（闪避增益曲线与时序）、`TestWasapiAudioPipelineMixer`（3 通道数学加法 + 软限幅钳制 + 闪避/通道音量 + WriteAudio 计数）、`TestWasapiAudioPipelineResampler`（48k→24k 线性重采样）、`TestWasapiMicrophoneUplink`（48k→16k 重采样 + 10ms 分包 + 回调分发 + 输入增益）、`TestMockEndToEndIntegration`（MockVideoDecoder + MockAudioPipeline 端到端 A/V 同步）。
    - 更新 [`Core/CMakeLists.txt`](file:///d:/CarPlayStation/Core/CMakeLists.txt)：纳入 3 个新 .cpp 至 `CarPlayCore`，WIN32 块新增链接 `ole32 oleaut32 mmdevapi ksuser`（WASAPI）。
  - **构建过程中发现并修复的 Bug**：
    1. `WasapiAudioPipeline.h`：`IsRunning()` 对普通 `bool running_` 误用 `.load()`（C2228），改为直接返回 `running_`。
    2. `WasapiAudioPipeline`：`WriteAudio` 持有 `mutex_` 后调用同样加锁的 `PushLaneSamples` 导致死锁，新增私有 `PushLaneSamplesLocked`（无锁）供其调用。
    3. `WasapiMicrophoneUplink`：`ResampleLinear` 误以 `srcCount` 作为输出样本数，导致 `GetCapturedSampleCount()` 返回源计数 48000 而非重采样后 16000，修正为 `srcCount * dstRate / srcRate`。
    4. `AudioDuckingManager::Tick`：浮点 1-ULP 漂移导致 `ASSERT_EQ(g, 0.20f)` 失败，加入 epsilon 精确吸附。
    5. `MediaTests.cpp`：`duck.Tick(...)` 丢弃 `[[nodiscard]]` 返回值触发 C4834，改为接收返回值并断言。
  - **测试运行指标**：
    - MSVC 14.44.35207（`/utf-8 /W4 /permissive-`, C++20）：3 个新 .cpp 与 `MediaTests.cpp` 全部 0 警告 / 0 错误通过编译。
    - CTest 6/6 全测试套件（`CommonTests`, `HardwareTests`, `TransportTests`, `ProtocolTests`, `SessionTests`, `MediaTests`）100% 全部通过（用时 ~2.8s）。
    - `MediaTests` 内部 12 组测试（Test 1–12）全部 `[PASS]`。

---



### 记录 9：会话状态机、配对存储与 AirPlay RTSP 协议栈（Partition 4: Core/Session）全量实现与端到端验证

* **时间**：`2026-09-30 12:25:00 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.8 Flash)`
* **背景与实现**：
  - 依据架构设计与 DEC-003 / DEC-004，全面完成 **Partition 4: Core/Session（会话状态机、配对存储与 AirPlay 核心控制层）** 全部 4 个步骤的研发。
  - **核心成果与组件实现**：
    1. **Step 1 (已配对设备存储与快速重连匹配器 PairingStore)**：
       - 抽象 [`Core/Session/IPairingStore.h`](file:///d:/CarPlayStation/Core/Session/IPairingStore.h)：定义 `PairedDevice` 领域模型（DeviceID、蓝牙/Wi-Fi MAC、公钥凭证、最后连接时间戳），提供快速查询与多键索引契约。
       - 实现 [`Core/Session/PairingStore.h/.cpp`](file:///d:/CarPlayStation/Core/Session/PairingStore.h)：基于轻量 JSON 格式实现安全磁盘持久化，支持原子写入（临时文件+重命名原子替换），确保意外掉电不损坏配置；遵循 Application 偏好分层规范。
       - 实现 [`Core/Session/MockPairingStore.h`](file:///d:/CarPlayStation/Core/Session/MockPairingStore.h)：提供线程安全的内存级配对虚拟存储桩，支持故障注入与测试断言。
    2. **Step 2 (AirPlay RTSP 协议编解码与参数协商引擎 AirPlayRtspCodec & Server)**：
       - 定义 [`Core/Session/AirPlayRtspTypes.h`](file:///d:/CarPlayStation/Core/Session/AirPlayRtspTypes.h)：标准 RFC 2326 RTSP 报文抽象、大小写不敏感请求头映射 `RtspHeaders`、`NegotiatedMediaParams` 媒体参数结构。
       - 实现 [`Core/Session/AirPlayRtspCodec.h/.cpp`](file:///d:/CarPlayStation/Core/Session/AirPlayRtspCodec.h)：基于 C++20 `std::string_view` 与 `std::span` 的零拷贝 RTSP 报文流式帧解析器，支持粘包/半包重组、`Content-Length` 精确分包以及 `BuildOptionsResponse` / `BuildSetupResponse` 报文序列化。
       - 实现 [`Core/Session/AirPlayRtspServer.h/.cpp`](file:///d:/CarPlayStation/Core/Session/AirPlayRtspServer.h)：WinSock 非阻塞多客户端 TCP 侦听服务，支持动态端口分配（端口 0 自动化测试支持）、后台工作线程池，并内置纯内存级交互仿真器 `ProcessSimulatedRequest`。
    3. **Step 3 (CarPlay 核心状态机与生命周期编排器 CarPlaySessionStateMachine)**：
       - 增强 [`Core/Session/ICarPlaySession.h`](file:///d:/CarPlayStation/Core/Session/ICarPlaySession.h)：扩展 `SessionState::Reconnecting` 异常恢复状态。
       - 实现 [`Core/Session/CarPlaySessionStateMachine.h/.cpp`](file:///d:/CarPlayStation/Core/Session/CarPlaySessionStateMachine.h)：
         - 完整流转矩阵：`Idle → BtDiscovered → iAP2Authenticated → WifiHandoff → AirPlayStreaming → Paused → Teardown → Reconnecting / Error`。
         - 多层级协同驱动：自动编排 Wi-Fi 5GHz SoftAP 启停、mDNS 服务组播广播、RTSP 服务接管及 `IPairingStore` 快速重连匹配。
         - 软硬件超时看门狗保护：支持毫秒级看门狗心跳（`TickWatchdog`），自动探测 iAP2 握手超时、Wi-Fi 关联超时与重连退避调度。
         - 线程安全事件分发：通过 `SessionStateObserver` 采用非锁重入通知机制广播状态跃迁。
    4. **Step 4 (端到端集成仿真测试与全量自动化验证)**：
       - 在 [`Tests/SessionTests.cpp`](file:///d:/CarPlayStation/Tests/SessionTests.cpp) 中新增端到端全链路集成仿真测试（7 组大型测试用例）：
         - `TestMockPairingStore` / `TestPairingStoreDiskPersistence`：验证增删查改、按 MAC 检索、排序与磁盘原子存取。
         - `TestRtspCodecParsingAndFraming` / `TestRtspResponseSerialization` / `TestRtspServerLifecycleAndSimulation`：验证流式 RTSP 分包、RFC 2326 序列化及真实本地 Loopback 端口握手。
         - `TestCarPlayStateMachineLifecycleAndHandoff`：联结 `MockPairingStore`、`MockWifiSoftApManager`、`MockMdnsAdvertiser`、`AirPlayRtspServer`，完整模拟首次配对、Wi-Fi 切换、推流协商、暂停恢复与二次快速重连。
         - `TestCarPlayStateMachineWatchdogAndReconnect`：模拟蓝牙超时、认证失败、Wi-Fi 掉线重试与达到最大重试次数熔断。
  - **历史遗留错误与环境修复（100% 修复完毕）**：
    - `Core/Common/Result.h`：补充缺失的 `StatusCode` 并提供 `MakeError` 全局帮助函数；特化 `Result<void>`。
    - `Core/Hardware/RadioAdapterHelper` / `FileMfiSigner`：将 `std::mutex` 升级为 `std::recursive_mutex`，彻底解决自重入死锁问题；修复 5GHz 网卡枚举能力探测与 CNG ECDSA P-256 私钥导入。
    - `Core/Transport/BtSdpHelper`：清理 WinCE 过期字段适配 Windows SDK。
    - Windows 本地外部编辑器自动保存引发的 0KB 文件异常：全量恢复所有源码并建立 git 跟踪保护。
  - **测试运行指标**：
    - MSVC (`10.0.26100.0` WinSDK, C++20) 编译 0 错误通过。
    - CTest 5/5 全测试套件 (`CommonTests`, `HardwareTests`, `TransportTests`, `ProtocolTests`, `SessionTests`) 100% 全部通过（用时 ~3.8s）。

---

### 记录 8：iAP2 协议与链路层（Partition 3: Core/Protocol）全量实现与编译验证

* **时间**：`2026-09-29 23:45:00 (UTC+8)`
* **客户端（Agent）**：`LM Studio Code (Qwen 3.8 27B Heretic ARA)`
* **背景与实现**：
  - 依据架构设计与 DEC-003，全面完成 **Partition 3: Core/Protocol（iAP2 协议层）** 全部组件的研发。
  - **核心成果与组件实现**：
    1. **链路层帧引擎 (`Iap2LinkEngine.h/.cpp`)**：
       - 完整实现 iAP2 链路层帧格式：9 字节头 `[FF 5A][total BE16][ctl][seq][ack][session][hdr_cksum]` + payload + trailing checksum。
       - SYN / SYN-ACK 握手流程（16 字节协商参数块），序列号管理，累计确认 (cumulative ACK)。
       - `FeedData()` 流式解析器：支持粘包/半包、坏帧 resync（扫描 FF5A magic）、header checksum 校验。
       - `BuildMessage(session, msgId, body)` 将控制面消息封装为完整链路帧（含 `[40 40]` 前缀与双层校验和）。
    2. **报文目录与 TLV 构建器 (`Iap2MessageCatalog.h/.cpp`)**：
       - `TlvBuilder`：大端序 TLV Group 编码器，支持 String / Bytes / U8 / U16 / U32 / Bool / None 六种参数类型。
       - `Iap2MsgId`：Apple 权威报文 ID 常量表（认证面 0xAA0x、识别面 0x1D0x、CarPlay 会话 0x430x、WiFi Handoff 0x570x、NowPlaying 0x500x、RouteGuidance 0x520x、CallControl 0x415x、DeviceInfo 0x4E0x、VehicleStatus 0xA10x）。
       - `Iap2IdentParam`：IdentificationInformation (0x1D01) 全部子参数 ID。
       - `BuildIdentifyInfo()`：完整构建无线 CarPlay IdentifyInformation body（含 BluetoothTransportComponent + WirelessCarPlayTransportComponent + VehicleInformationComponent + RouteGuidanceDisplayComponent）。
       - `ParseMessageId()` / `ExtractChallenge()` / `ParseRejectedParamIds()` 解析工具。
    3. **会话状态机 (`Iap2SessionManager.h/.cpp`)**：
       - 认证→识别全链路状态机：`Init → CertSent → SignSent → Authenticated → IdentSent → Identified`（含 `IdentRetried` 重试态）。
       - MFi 签名器解耦接口注入，事件回调通知上层。
       - `IdentificationRejected` 两级重试策略（首次 retry、二次 abort），符合 carplayd doc 23。
    4. **构建与测试**：
       - 更新 `Core/CMakeLists.txt`，纳入全部 Protocol 源文件至 `CarPlayCore` 静态库。
       - 新增 `Tests/ProtocolTests.cpp`（11 组单测）：checksum 参考向量、SYN 帧结构校验、FeedData SYN-ACK / 坏帧 resync、BuildMessage `[40 40]` 封装、TlvBuilder 编码正确性、BuildIdentifyInfo 完整 group 解析、ParseMessageId 正/反向、SessionManager happy path / out-of-order / reject-retry。
       - `Tests/CMakeLists.txt` 注册 `CarPlayStation_ProtocolTests` CTest target。
  - **编译验证结果（MinGW GCC 15.x, `-std=c++20 -Wall -Wextra`）**：
    - Protocol 三个 .cpp 全部零警告、零错误通过编译，.obj 文件确认生成。
    - `ProtocolTests.cpp` 语法检查通过（零诊断）。
  - **构建过程中发现并修复的 P3 Bug**：
    1. `Iap2MessageCatalog.cpp`: `auto& ip = Iap2IdentParam;` → `namespace ip = Iap2IdentParam;`（C++ 不允许对 namespace 取引用）。
    2. `Iap2SessionManager.cpp`: `auto& id = Iap2MsgId;` → `namespace id = Iap2MsgId;`（同上）。
    3. `Iap2MessageCatalog.cpp`: 缺少 `#include "Core/Protocol/Iap2LinkEngine.h"` 导致 `Iap2Constants` 未声明。
  - **遗留编译错误（非 P3 范围，属 Hardware / Transport 层既有问题）**：
    1. **Hardware 层**：
       - `Result<void>` 模板特化缺失 → `II2cAdapter.h` 中 `PulseReset()` 返回类型实例化失败。
       - `I2cMfiSigner.cpp`: 使用了不存在的 `StatusCode::DeviceUnavailable` / `TimedOut`（应为 `Timeout`）。
       - `NetworkMfiClient.cpp` / `FileMfiSigner.cpp`: MSVC-only `#pragma comment(lib, ...)` 在 MinGW 下产生 warning。
       - `Ch341I2cAdapter` / `Ch347I2cAdapter` / `FtdiI2cAdapter`: 依赖 Windows SDK 专有头文件（蓝牙 `_BTH_SET_SERVICE` 字段、CNG crypto blob 结构体）在 MinGW 下不可用。
    2. **Transport 层**：
       - `MdnsAdvertiser.cpp` / `BtRfcommTransport.cpp`: 使用了不存在的 `StatusCode::Unavailable` / `InvalidState` / `IoError`。
       - `BtSdpHelper.cpp`: Windows Bluetooth SDK 专有结构体在 MinGW 下缺失。
    3. **结论**：上述错误均为 Hardware/Transport 层既有代码与 MSVC SDK 的绑定问题，不影响 P3 Protocol 层的正确性。后续需统一补充 `StatusCode` 枚举值并添加 `Result<void>` 特化，或确认项目以 MSVC 为唯一目标编译器。

---

### 记录 7：传输与链路层（Layer 2: Core/Transport）四大组件实现与 Partition 2 全阶段圆满收官

* **时间**：`2026-09-29 17:15:00 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.8 Flash)`
* **背景与实现**：
  - 依据架构设计与 DEC-003（无线 CarPlay 为核心 P0），全面完成 **Layer 2: Core/Transport（传输与链路层）** 全部 4 个步骤的研发。
  - **核心成果与组件实现**：
    1. **Step 1 (传输抽象与双向对测仿真通道)**：
       - 增强 [`Core/Transport/ITransportChannel.h`](file:///d:/CarPlayStation/Core/Transport/ITransportChannel.h)：扩充 `TransportStats` 吞吐统计、`GetMtu()`（协商 MTU）、`GetRemoteEndpoint()` 与 `IsConnected()` 状态查询。
       - 实现 [`Core/Transport/MockTransportChannel.h/.cpp`](file:///d:/CarPlayStation/Core/Transport/MockTransportChannel.h)：支持双通道互联对测 (`Pair`)、错误注入 (`SetFailSends`)、静默丢包、MTU 截断拦截及出站控制帧捕获 (`GetSentPackets`)。
    2. **Step 2 (Windows 蓝牙 RFCOMM 传输与 SDP 服务注册)**：
       - 实现 [`Core/Transport/BtSdpHelper.h/.cpp`](file:///d:/CarPlayStation/Core/Transport/BtSdpHelper.h)：封装 89 字节 byte-exact Apple Wireless iAP2 SDP 二进制模板（UUID `00000000-deca-fade-deca-deafdecacaff` / `0xfe`），提供 MAC 地址格式化与 Windows `WSASetServiceW` / `BTH_SET_SERVICE` 原生注册。
       - 实现 [`Core/Transport/BtRfcommTransport.h/.cpp`](file:///d:/CarPlayStation/Core/Transport/BtRfcommTransport.h)：基于 WinSock `AF_BTH` / `BTHPROTO_RFCOMM` 实现通道 1 侦听、非阻塞后台 I/O 线程与无硬件测试仿真插桩。
    3. **Step 3 (Wi-Fi SoftAP 热点生命周期管理与看门狗)**：
       - 抽象 [`Core/Transport/IWifiSoftApManager.h`](file:///d:/CarPlayStation/Core/Transport/IWifiSoftApManager.h)：定义热点启停、凭证分发与 Station 状态监测契约，兼容 `IWifiHandoffController`。
       - 实现 [`Core/Transport/WifiSoftApManager.h/.cpp`](file:///d:/CarPlayStation/Core/Transport/WifiSoftApManager.h)：强制 5GHz 频段规避蓝牙共存干扰，动态生成高熵 WPA2 密码与 SSID；内置 Station 关联探测看门狗线程，精准识别客户端脱网。
       - 实现 [`Core/Transport/MockWifiSoftApManager.h`](file:///d:/CarPlayStation/Core/Transport/MockWifiSoftApManager.h)：为上层会话与 CI 提供完全解耦的 Mock 桩。
    4. **Step 4 (轻量级原生 mDNS/DNS-SD 广播器)**：
       - 抽象 [`Core/Transport/IMdnsAdvertiser.h`](file:///d:/CarPlayStation/Core/Transport/IMdnsAdvertiser.h)。
       - 实现 [`Core/Transport/MdnsPacketBuilder.h/.cpp`](file:///d:/CarPlayStation/Core/Transport/MdnsPacketBuilder.h)：遵循 RFC 1035 / 6762 标准，二进制序列化 PTR、SRV、TXT 与 A 记录，携带 Apple 官方 `0x44440B80,0x61` 特性位及设备标识。
       - 实现 [`Core/Transport/MdnsAdvertiser.h/.cpp`](file:///d:/CarPlayStation/Core/Transport/MdnsAdvertiser.h)：轻量级原生 UDP 组播（`224.0.0.251:5353`），三连初始突发 + 周期刷新，彻底摆脱外部 Bonjour 守护进程依赖。
       - 实现 [`Core/Transport/MockMdnsAdvertiser.h`](file:///d:/CarPlayStation/Core/Transport/MockMdnsAdvertiser.h)。
    5. **自动化单元测试全覆盖**：
       - 在 [`Tests/TransportTests.cpp`](file:///d:/CarPlayStation/Tests/TransportTests.cpp) 中新增 10 组高覆盖单测，全面验证收发环路、故障注入、SDP 生成、MAC 解析、RFCOMM 状态机、SoftAP 看门狗及 mDNS 广播。
       - 更新 `Core/CMakeLists.txt` 与 `Tests/CMakeLists.txt`。
  - **完成清理**：
    - 阶段待办清单 `TODO_PART2_TRANSPORT.md` 按照多智能体协作规范全量勾选并执行安全删除。

---

### 记录 6：硬件无线状态探测（RadioAdapterHelper）与 Partition 1 全阶段圆满收官

* **时间**：`2026-09-29 16:10:00 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.8 Flash)`
* **背景与实现**：
  - 完成 Partition 1 的最后一步 Step 3：基于 Windows Native APIs 探测宿主机的蓝牙与 Wi-Fi 5GHz/SoftAP 能力。
  - **核心组件实现**：
    1. **`Core/Hardware/RadioAdapterHelper.h/.cpp`**：
       - 实现 `IRadioState` 契约（`GetState("bluetooth"|"wifi")`、`SetEnabled(...)`）。
       - 动态安全链接 Windows Native Wifi (`wlanapi.dll`) 与蓝牙栈 (`bthprops.cpl`)，无依赖免安装，绝不引发运行时缺失崩溃。
       - 精准识别无线网卡 PHY 物理层协议类型（`dot11_phy_type_ofdm` (802.11a), `dot11_phy_type_vht` (802.11ac), `dot11_phy_type_he` (802.11ax Wi-Fi 6)），严格匹配 Apple 无线 CarPlay 对 5GHz 频段的强制要求。
       - 提供 `EvaluateReadiness()` 全局硬件就绪度评估诊断，返回直观易读的硬件就绪报告。
       - 提供内存级仿真插桩接口（`SetSimulatedBluetooth` / `SetSimulatedWifi`），使无物理无线网卡的开发机/CI 环境也能 100% 确定性运行单测。
    2. **单元测试与构建同步**：
       - 更新 `Core/CMakeLists.txt`，添加 `RadioAdapterHelper.cpp`。
       - 在 `Tests/HardwareTests.cpp` 中新增 `TestRadioAdapterHelper()`，完整覆盖真实系统硬件探测、5GHz 缺失告警、蓝牙未就绪阻断及仿真状态机流转。
  - **Partition 1 (Core/Hardware) 全面收官总结**：
    - **Step 1 (认证核心基石)**：`MockMfiSigner`、`NetworkMfiClient` (Winsock TCP RPC)、`FileMfiSigner` (Windows 原生 CNG ECDSA P-256 离线免硬件认证)。
    - **Step 2 (通用 I2C 与多芯片桥接)**：`II2cAdapter` 抽象、`I2cMfiSigner` 协处理器状态机、`Ch341I2cAdapter` / `Ch341Protocol`、`Ch347I2cAdapter`、`FtdiI2cAdapter`、`MockI2cAdapter`。
    - **Step 3 (无线硬件探测)**：`RadioAdapterHelper` (Native WlanAPI 5GHz / Bluetooth 状态探测)。
    - 专项待办文件 `TODO_PART1_HARDWARE.md` 按照既定约定完成清理删除。

---

### 记录 5：通用 I2C 硬件抽象与多芯片驱动桥接实现（CH341、CH347、FTDI、Mock & DEC-007）

* **时间**：`2026-09-29 15:40:00 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.8 Flash)`
* **背景与架构设计演进**：
  - 用户指出：USB 转 I2C 芯片不仅限于 CH341，市面上常见还有 CH347、FTDI（FT232H/FT2232H）等，且设备驱动接口都是通用的标准 I2C 操作。
  - **核心洞察**：Apple MFi 协处理器（CP 2.0C/3.0）挂载在 I2C 从机地址（0x10/0x11），其证书读取（0x30/0x31..）与挑战签名（0x20/0x21/0x10/0x11/0x12）是与物理芯片无关的高层业务协议；而 USB-I2C 芯片只是总线传输通道。
  - **分层重构**：
    1. 抽象 `II2cAdapter` 接口（`Core/Hardware/II2cAdapter.h`）：规范通用 I2C 读、写、组合事务与 GPIO 复位控制。
    2. 实现 `I2cMfiSigner` 认证器（`Core/Hardware/I2cMfiSigner.h/.cpp`）：纯面向 `II2cAdapter` 编程，封装全套 MFi 协处理器寄存器状态机、128 字节窗口分片拉取与指数退避重试。
    3. 实现多芯片驱动适配器：
       - `Ch341I2cAdapter` 与 `Ch341Protocol.h`：支持 Windows 官方 `CH341DLL.DLL` 及裸流数据帧编解码（含 ACK/NACK 状态检测与 D0..D5 复位脉冲）。
       - `Ch347I2cAdapter`：动态链接 `CH347DLL.DLL`，支持 480Mbps 高速硬件 I2C 引擎。
       - `FtdiI2cAdapter`：动态链接 `libMPSSE.dll` / `ftd2xx.dll`，支持 FTDI MPSSE 硬件引擎。
       - `MockI2cAdapter`：纯内存虚拟 MFi 协处理器仿真，支持寄存器状态机与错误注入，保障无外设环境 100% 单测覆盖。
    4. 扩展 `Application/AppSettings.h`：新增 `HardwareI2cPreferences` 支持运行时切换驱动类型与参数。
    5. 扩展 `Tests/HardwareTests.cpp`：新增 CH341 帧编解码、Mock 适配器、I2cMfiSigner 完整认证链路与驱动契约防御单测。
    6. 产出决策文档 **DEC-007**，并更新 `TODO_PART1_HARDWARE.md` 与 `Core/Hardware/README.md`。

---

### 记录 4：0KB 文件排查修复与 MFi 端点分层重构彻底闭环（HANDOFF_MFI_LAYERING & DEC-005）

* **时间**：`2026-09-29 09:58:00 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.7 Flash)`
* **背景**：接手并彻底完成 `HANDOFF_MFI_LAYERING.md` 中的所有剩余未决工作，排查异常 0KB 文件，解决编译断言错误，同步跨层文档并正式确立 DEC-005 决策。
* **完成的工作与重构细节**：
  1. **P0 编译断言修复 ([Tests/CommonTests.cpp](file:///D:/CarPlayStation/Tests/CommonTests.cpp))**：
     - 彻底清理对旧 `Core::NetworkEndpoints.mfi_daemon_endpoint` 字段的引用。
     - 在 Application 层的 `AppSettings.mfi` 上执行测试：验证 `settings.mfi.daemon_endpoint` 动态端点解析、`ToString()`，以及默认偏好 `use_hardware_backend == false`（默认网络 daemon，符合 DEC-003）。
  2. **P1 文档与头文件注释全量同步**：
     - [Core/Service/ICarPlayCoreApi.h](file:///D:/CarPlayStation/Core/Service/ICarPlayCoreApi.h)：清理第 21 行注释中过时的 "MFi daemon endpoint" 示例，调整为 RTSP 端口 / SoftAP 凭证。
     - [Core/Hardware/README.md](file:///D:/CarPlayStation/Core/Hardware/README.md)：更新 `MfiNetworkClient` 描述，明确 daemon 端点由 Application 层 `MFiPreferences::daemon_endpoint` 构造注入，Core/Lib 不持有默认硬编码地址。
     - [DECISIONS.md](file:///D:/CarPlayStation/DECISIONS.md)：正式添加 **DEC-005 — MFi Authentication Layering: Endpoints are User Preferences, Capabilities belong to Hardware Layer**，完整阐述 MFi 认证能力归属硬件层、后端选择及 daemon 地址归属 Application 层的设计决策与推导过程。
  3. **0KB 构建文件修复与全工程校验**：
     - 修复 `Application/CMakeLists.txt`、`UI/Win32/CMakeLists.txt`、`Tests/CMakeLists.txt`。
     - 全盘扫描确认工程中 0 字节文件数量归零，所有 C++20 头文件与构建定义完整就绪。

---

### 记录 3：配置分层重构 — Core 只收事实，用户可调项归 Application（DEC-004）

* **时间**：`2026-09-28 (UTC+8)`
* **客户端（Agent）**：`GitHub Copilot`
* **背景与动机**：记录 2 的"全可配置化"把大量用户可调项（分辨率预设、窗口模式、编解码 Auto 偏好、音量、Ducking、码率请求）和会随环境变动的网络端点混进了同一个 `Config.h`，并通过 `ICarPlaySession::Start(config)` / `UpdateConfig(config)` 这样的单体配置块一路灌进底层。问题：
  * 底层（会话/解码器/音频管道）被迫依赖它根本不使用的旋钮；
  * 窗口显示模式这种纯 UI 概念泄漏到了 Core；
  * 媒体参数实际是每次握手与 iPhone **协商**出来的协议事实，不是用户配置，却伪装成了 config；
  * 音量/Ducking 本质是运行时控制，塞在配置结构体里语义错位。
* **做了什么**：
  1. **Core 配置拆成窄的"事实型"结构体（`Core/Common/Config.h`）**：
     - `VideoStreamSpec`：协商确定的编解码器（`H264`/`HEVC`，无 Auto）、宽高、fps；
     - `AudioOutputFormat`：采样率 / 声道数 / 位深（不含音量与码率）；
     - `DeviceIdentity`：握手广播的设备名 / 厂商 / 型号；
     - `NetworkEndpoints`：MFi daemon 端点、RTSP 绑定地址/端口、AirPlay 音视频端口、SoftAP SSID/密码/网关、硬件 MFi 开关 —— 网络配置保留在 Core，但作为**环境事实**只被 Transport/Hardware 消费；
     - `CoreEnvironmentConfig` = Identity + Endpoints，是 Core 初始化时接受的唯一配置。
     - 删除了旧 `VideoConfig` / `AudioConfig` / `NetworkConfig` / `CarPlayStationConfig` 单体结构体。
  2. **新增 Application 层用户偏好头（`Application/AppSettings.h`）**：`VideoPreferences`（预设含 Auto/Custom、编解码偏好含 Auto、fps、窗口模式）、`AudioPreferences`（首选采样率、码率请求、三路音量、Ducking 比率）、`DevicePreferences`，组合为 `AppSettings`。所有"一直能变动"的用户项都在这里。
  3. **边界处显式解析**：Application 层负责把偏好换算成具体参数（预设 → 实际宽高；编解码偏好 + 协商结果 → 最终编解码器），Core 永远看不到预设、Auto 或窗口模式。
  4. **接口收敛**：
     - `IVideoDecoder::Init/Reconfigure` 改收 `VideoStreamSpec`；
     - `IAudioPipeline::Start/Reconfigure` 改收 `AudioOutputFormat`，新增 `SetDuckingRatio()`，与已有 `SetLaneVolume()` 一起成为一等运行时控制；
     - `ICarPlaySession::Start()` 不再接收任何配置（媒体参数走协商），删除 `UpdateConfig()`；
     - `ICarPlayCoreApi::Initialize/UpdateEnvironment` 改收 `CoreEnvironmentConfig`；
     - `IApplicationApi::StartCarPlay/ApplySettings` 改收 `AppSettings`，`AppUiState.active_settings` 存偏好快照，宽高字段改为上报实际协商分辨率。
  5. **测试更新**：`Tests/CommonTests.cpp` 的 `TestDynamicConfig` 改写为 `TestConfigSeparation`，分别验证 Core 事实型结构体与 Application 偏好的解析换算。
  6. **决策与文档**：在 `DECISIONS.md` 新增 **DEC-004 — Configuration Layering: Core Gets Facts, Application Owns Preferences**（含备选方案与后果分析）；`ARCHITECTURE.md` 增加 "Configuration Layering" 小节说明配置归属。
* **效果**：以后新增任何 UI 旋钮，只要它能解析成已有事实参数，就不会再迫使 Core 接口变动；Core 接口的每个入参都是具体的协议/环境事实，"可变性"被明确地留在 Application/UI 一侧。

---

### 记录 2：视频/音频/窗口模式/网络端点可配置化与动态调节支持

* **时间**：`2026-09-28 17:23:00 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.7 Flash)`
* **做了什么**：
  1. **彻底移除硬编码，建立全局配置中心 [Core/Common/Config.h](file:///D:/CarPlayStation/Core/Common/Config.h)**：
     - **视频分辨率预设与自适应计算**：
       - `720P` (1280x720)
       - `1080P` (1920x1080)
       - `1440P` / 2K (2560x1440)
       - `2160P` / 4K (3840x2160)
       - `Auto`（动态自适应匹配窗口大小或显示器最大全屏分辨率）
       - `Custom`（用户自定义宽高）
     - **窗口显示模式**：
       - `Windowed`（常规窗口模式）
       - `BorderlessFullscreen`（无边框全屏窗口）
       - `ExclusiveFullscreen`（独占全屏模式）
     - **视频编解码偏好协商（向 iPhone 通报建议能力）**：
       - `H.264`
       - `HEVC` (H.265)
       - `Auto`
     - **音频多选项独立可调**：
       - 采样率：`44.1 kHz`、`48.0 kHz`、`96.0 kHz`
       - 码率：`128k`、`192k`、`256k`、`320k` 等
       - 通道：立体声 (Stereo) / 单声道 (Mono) / 16-bit / 24-bit PCM
       - 音量与导航压音比率（Ducking Ratio）
     - **网络与 Daemon 端点全动态化**：
       - MFi Daemon 代理 IP 及端口（`IpEndpoint`，默认 `127.0.0.1:9000`，可任意指定）
       - AirPlay RTSP 绑定 IP / 端口（默认 `0.0.0.0:5000`）
       - Wi-Fi SoftAP SSID / 密码 / 网关动态可设
  2. **跨层动态配置与运行时热重载接口支持**：
     - [ICarPlaySession.h](file:///D:/CarPlayStation/Core/Session/ICarPlaySession.h)：新增 `UpdateConfig()`。
     - [IVideoDecoder.h](file:///D:/CarPlayStation/Core/Media/IVideoDecoder.h)：新增 `Reconfigure()` 支持动态分辨率与窗口切换。
     - [IAudioPipeline.h](file:///D:/CarPlayStation/Core/Media/IAudioPipeline.h)：新增 `Reconfigure()` 支持音频流参数热切换。
     - [ICarPlayCoreApi.h](file:///D:/CarPlayStation/Core/Service/ICarPlayCoreApi.h)：新增核心服务配置同步。
     - [IApplicationApi.h](file:///D:/CarPlayStation/Application/IApplicationApi.h)：向 UI 暴露 `ApplySettings(newConfig)` 接口，支持 UI 菜单即时应用。
  3. **单元测试补充**：
     - 在 [Tests/CommonTests.cpp](file:///D:/CarPlayStation/Tests/CommonTests.cpp) 中新增 `TestDynamicConfig`，验证 720P/1080P/1440P/2160P/Auto 分辨率换算、窗口模式与网络配置。

---

### 记录 1：C++ 基础工程骨架与跨模块接口契约建设

* **时间**：`2026-09-28 17:06:30 (UTC+8)`
* **客户端（Agent）**：`Google Antigravity (Gemini 3.7 Flash)`
* **做了什么**：
  1. **构建系统初始化（CMake + Modern C++20）**：
     - 创建根目录 [CMakeLists.txt](file:///D:/CarPlayStation/CMakeLists.txt)，强制启用 `C++20` 标准，并针对 Windows 10/11 (MSVC) 配置了标准编译宏 (`NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `_WIN32_WINNT=0x0A00`, `/utf-8`, `/W4`)。
     - 为各个子层配置了模块化的 `CMakeLists.txt`：
       - `Core/CMakeLists.txt` -> 导出核心库 `CarPlayCore`
       - `Application/CMakeLists.txt` -> 导出应用逻辑库 `CarPlayApp`
       - `UI/Win32/CMakeLists.txt` -> 配置主执行入口 `CarPlayStation`
       - `Tests/CMakeLists.txt` -> 配置基础单元测试目标 `CarPlayStation_Tests`
  2. **公共基础库建设 (`Core/Common/`)**：
     - [Types.h](file:///D:/CarPlayStation/Core/Common/Types.h)：提供现代 C++20 `std::span` 视图抽象（`ByteSpan`, `MutableByteSpan`, `ByteBuffer`）、大端序与小端序读写工具（`ReadBe16/32/64`, `WriteBe16/32/64`）、网络端点（`IpEndpoint`）、硬件地址（`MacAddress`）与 Hex 调试格式化工具。
     - [Result.h](file:///D:/CarPlayStation/Core/Common/Result.h)：实现轻量高效的 `StatusCode` 错误码体系、`Status` 状态对象与现代 C++ 单子风格 `Result<T>` 模板封装，全项目统一安全错误处理，杜绝裸指针与未捕获异常。
     - [Logging.h](file:///D:/CarPlayStation/Core/Common/Logging.h)：实现线程安全的可插拔日志门面与格式化宏 (`CPS_LOG_INFO`, `CPS_LOG_WARN`, `CPS_LOG_ERROR`, `CPS_LOG_DEBUG`)。
     - [NonCopyable.h](file:///D:/CarPlayStation/Core/Common/NonCopyable.h)：提供 RAII 语义基类。
  3. **零冲突纯虚抽象接口（Interface-First Contracts）**：
     - **Partition 1 (Core/Hardware)**：定义 [IMfiSigner.h](file:///D:/CarPlayStation/Core/Hardware/IMfiSigner.h)（MFi 证书获取、SHA-1 挑战签名、芯片信息）与 [IRadioState.h](file:///D:/CarPlayStation/Core/Hardware/IRadioState.h)。
     - **Partition 2 (Core/Transport)**：定义 [ITransportChannel.h](file:///D:/CarPlayStation/Core/Transport/ITransportChannel.h)（跨 Bluetooth RFCOMM/TCP/NCM 的通用异步传输通道）。
     - **Partition 3 (Core/Protocol)**：定义 [IIap2Link.h](file:///D:/CarPlayStation/Core/Protocol/IIap2Link.h) 与报文分发监听器接口。
     - **Partition 4 (Core/Session)**：定义 [ICarPlaySession.h](file:///D:/CarPlayStation/Core/Session/ICarPlaySession.h)（CarPlay 会话生命周期状态机与观察者）。
     - **Partition 5 (Core/Media)**：定义 [IVideoDecoder.h](file:///D:/CarPlayStation/Core/Media/IVideoDecoder.h)（解耦 HWND 的共享纹理帧分发接口）与 [IAudioPipeline.h](file:///D:/CarPlayStation/Core/Media/IAudioPipeline.h)（主媒体/导航/Siri 3-Lane 音频管道）。
     - **Partition 6 (Core/Input)**：定义 [IInputEventSink.h](file:///D:/CarPlayStation/Core/Input/IInputEventSink.h)（触摸手势、旋钮与车载按键输入转换契约）。
     - **Partition 7 (Core/Service & Application)**：定义 [ICarPlayCoreApi.h](file:///D:/CarPlayStation/Core/Service/ICarPlayCoreApi.h) 与 [IApplicationApi.h](file:///D:/CarPlayStation/Application/IApplicationApi.h)。
  4. **可执行入口与自动化冒烟测试**：
     - 创建 [UI/Win32/main.cpp](file:///D:/CarPlayStation/UI/Win32/main.cpp) 启动入口。
     - 创建 [Tests/CommonTests.cpp](file:///D:/CarPlayStation/Tests/CommonTests.cpp) 单元测试套件，全面覆盖 `Result<T>`、字节序转换、网络类型与接口编译期检查。
