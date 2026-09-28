# CarPlayStation — 任务成果与阶段记录 (result.md)

## 任务执行记录

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
