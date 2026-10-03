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
