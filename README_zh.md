# CarPlayStation

[English](README.md) | [简体中文](README_zh.md)

**原生 Windows 无线 CarPlay 接收器**

> [!NOTE]  
> **界面语言说明：** 当前软件界面（设置抽屉、向导及提示）**暂时仅支持简体中文**。多语言支持（包含英文界面）将在后续版本逐步完善。

CarPlayStation 能够将您的 Windows 电脑变成一台车载中控主机（Head Unit）。通过模拟标准 CarPlay 车机配件，与 iPhone 进行蓝牙（RFCOMM）握手并交接到 Wi-Fi（AirPlay）通道，运行完整的 CarPlay 协议栈（iAP2 识别认证、MFi 鉴权、配对加密、RTSP 会话协商、Stream Keys 交换），在本地解码并渲染 H.264 视频流与 AAC 音频流，同时支持多点触控、键盘手势控制、车载通话接听与 Siri 交互。

**技术选型：** 全模块基于 **原生 C++（C++17/C++20）与 Win32 / Direct3D 11 API** 编写，**零 Electron、零 .NET、无额外庞大运行库**，核心程序体积仅约 3.8 MB。

> **关于性能与低配设备（如赛扬 J4005 / 工控机 / 廉价小主机）：**  
> 虽然采用了原生 C++ 架构，但 CarPlay 视频流需要逐帧进行 **ChaCha20-Poly1305 实时解密** 以及 H.264 视频解码与渲染。对于 J4005 这种双核双线程、无 AVX 指令集的老弱低压 CPU 来说，高分辨率/高帧率下 CPU 依然容易被吃满（软硬解都可能顶不住）。遇到掉帧或卡顿，**强烈建议在设置中将分辨率调至 800×480 或 1280×720，并将帧率限制在 30fps**。大家都是摸着石头过河，谁也别笑话谁，实用够用就好 😄。

---

## 🌟 核心特性

- 🖥 **原生轻量实现**：C++17 + Win32 + Direct3D 11 / Direct2D 渲染 + libmpv 解码，没有笨重的浏览器外壳。
- 🛠 **三种可插拔 MFi 认证后端**：
  - **本地证书模式**：导入软件证书及私钥（PKCS#8 + P7B），纯软算力签名，开箱即用。
  - **真实硬件 MFi 芯片直连（业内领先）**：支持外接 Apple 官方 MFi 认证协处理器（CP 2.0C / 3.0），通过 WCH CH341 / CH347 或 FTDI D2XX USB-to-I2C 桥接芯片硬件鉴权，完美还原真实车规级体验。
  - **远程 MFi HTTP 签名**：支持局域网或远程部署签名中继服务，多台电脑共享一套鉴权服务。
- 🎵 **专业级 WASAPI 3通道音频管线**：
  - 媒体音乐、导航播报、电话语音三条独立低延迟音频通道（基于 fdk-aac 原生解码）；
  - 麦克风高清音频上行（WASAPI 音频采集 + AAC-ELD 编码 + RTP PT100 封装回传）。
- 📞 **车载通话与车载控制**：
  - 支持 iAP2 `0x4155 CallStateUpdate` 通话状态跟踪，车载来电原生接听（`F3`）与拒接/挂断（`F4`）；
  - 快捷唤醒 Siri（`F1` 触发 `requestSiri` 事件）；
  - 动态 Now Playing 歌曲信息 HUD 胶囊显示与动态窗口标题；
  - 随 Windows 系统自动同步深色/浅色主题（向手机下发 `setNightMode`）。
- 👆 **硬件级多点触控与全能键鼠映射**：
  - 自动探测 Windows 触摸屏硬件接触点能力，原生映射多点触控 HID 报告；
  - 普通笔记本与键鼠支持滚轮滑动、WASD / 方向键滑动手势、回车点击、返回键映射。

---

## 📋 功能支持状态

- [x] **无线 CarPlay**（蓝牙 RFCOMM 引导 + Wi-Fi 移动热点 / 局域网 mDNS/DNS-SD 传输）
- [x] **MFi 鉴权系统**（本地证书 / USB-I2C 硬件芯片 / 远程 HTTP）
- [x] **身份配对与持久化**（`pair-setup` SRP / `pair-verify` / 自动记录配对设备）
- [x] **AirPlay RTSP 协商与密钥交换**（Phase 1 / Info / Record / Phase 2，ChaCha20-Poly1305 实时解密）
- [x] **D3D11 视频硬解与渲染**（libmpv 硬解管道）
- [x] **WASAPI 三通道音频下行与麦克风上行**
- [x] **Siri 唤醒与交互**（`F1`）
- [x] **电话状态感知与快捷接挂**（`F3` / `F4`）
- [x] **Now Playing 音乐元数据与 HUD 实时显示**
- [x] **昼夜深浅主题自动同步**（Windows 主题与 CarPlay NightMode 双向联动）
- [x] **多点触控 / 鼠标滚轮手势 / 键盘控制**
- [x] **首次运行向导与偏好设置记忆**
- [ ] **有线 USB 直连 (USB/NCM)**：受限于 Windows x86 平台 PC OEM BIOS 固件锁定 Intel xDCI 控制器仅能处于 Host 模式（缺乏外设从机堆栈，详见 [DEC-003](DECISIONS.md)），后续规划支持 ARM64 原生从机或外置 RP2350/Linux 桥接模块。

---

## 🚀 快速上手指南

### 1. 系统要求

- 操作系统：Windows 10 / Windows 11 (x64)
- 硬件要求：具备 Bluetooth 4.0+ 蓝牙适配器与无线网卡（建议支持 5GHz 频段以确保低延迟音视频传输）
- 移动设备：iPhone（需开启 Wi-Fi、蓝牙，并在设置中开启 CarPlay）

### 2. 运行准备（防火墙放行）

由于 Windows 移动热点或局域网会将无线适配器归入“公用网络”配置文件，Windows 防火墙可能会拦截 mDNS 服务广播与 RTSP 音视频端口。首次使用前，建议以管理员身份运行防火墙配置脚本：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\Set-UpFirewall.ps1
```

### 3. Wi-Fi 移动热点关键配置（⚠️ 核心避坑点）

在 Windows“设置 → 网络和 Internet → 移动热点”中，**必须严格按以下要求配置热点属性**，否则会导致 iPhone 无法加入网络或 CarPlay 握手超时断开：

1. **网络频带（频段）必须选【5 GHz】：**  
   CarPlay 传输高码率音视频流对延迟极度敏感。2.4 GHz 频段与系统蓝牙处于同频段，存在极其严重的射频互扰，极易导致握手超时或音画卡顿撕裂。**务必在编辑热点时将“网络频带”手动指定为 5 GHz**（不要选 2.4 GHz 或“任何可用频率”）。
2. **加密方式（安全类型）要求：必须为 WPA2 或 WPA2/WPA3 混合：**  
   - **Windows 11：** 必须设置为 **WPA2/WPA3-个人版** 或 **WPA2-个人版**（⚠️ **纯 WPA3 不行**，会导致 iPhone 无法完成配网握手！）。  
   - **Windows 10：** 必须设置为 **WPA2-个人版 (AES)**。  
   - ⚠️ **切勿设为开放（无密码）网络或纯 WPA3**，密码长度建议为 8~63 位纯数字或英文。

---

### 4. 连接与使用步骤

1. **开启并配置移动热点**：按照上述要求开启 Windows 5 GHz 移动热点。
2. **蓝牙配对**：在 Windows 设置中开启蓝牙，并将 iPhone 与电脑进行**蓝牙配对**。
3. **启动应用**：运行 `CarPlayStation.exe`：
   - 首次启动会弹出配置向导，选择您的 MFi 认证后端（软件证书文件、USB-I2C 硬件芯片、或远程 HTTP）；
   - 设置首选视频分辨率与音频设备。
4. **车机连接**：打开 iPhone 的 **设置 → 通用 → CarPlay 车载**，选择发现的 **CarPlayStation** 配件。
5. iPhone 弹出确认框后轻点“使用 CarPlay 车载”，握手交接至 5 GHz 热点后，车机画面窗口将自动呈现！

---

## 🎮 控制与快捷键列表

CarPlayStation 拥有完整的输入转译模块：

| 操作按键 / 手势 | 手机端映射动作 |
| --- | --- |
| **鼠标左键拖动 / 触屏拖拽** | 屏幕滑动 / 滚动 |
| **鼠标滚轮** | 上下快速滑动 |
| **鼠标右键 / Backspace / Esc** | 返回上一级（Back） |
| **↑ ↓ ← → / W A S D** | 向上 / 向下 / 向左 / 向右滑动手势 |
| **Enter (回车键)** | 点击屏幕中央 |
| **空格键 (Space) / 媒体按键** | 播放 / 暂停 / 上一首 / 下一首 |
| **F1** | 激活 Siri 语音助手 |
| **F2** | 打开/收起本地设置抽屉 |
| **F3** | 接听来电 |
| **F4** | 拒接 / 挂断来电 |
| **Ctrl + F4** | 呼出实时运行日志面板 |
| **F11** | 切换全屏显示 / 窗口化 |

---

## 🏛 架构设计

项目采用纯原生分层设计，划分为五大板块：

```text
UI            Win32 窗口管理、Direct3D 11/Direct2D 渲染、输入路由、向导抽屉
Application   业务逻辑、设置存储、命令分发、MFi 签名工厂 (DEC-002)
Core          7层严格单向依赖 CarPlay 核心协议栈 (硬件、传输、iAP2、会话、媒体、输入、API外观)
Lib           外部原生库：WASAPI (系统 API)、libmpv、fdk-aac、WCH/FTDI 驱动库
Reference     协议逆向与开发参考项目 (子模块，仅供参考，不参与实际编译)
```

详细架构说明请参阅 [ARCHITECTURE.md](ARCHITECTURE.md) 与 [Core/README.md](Core/README.md)。

---

## 🛠 从源码编译

如果您希望自行从源码进行构建：

### 环境要求
- Windows 10/11 (x64)
- Visual Studio 2022 (或 Build Tools 2022/2026，需安装 C++ 桌面开发工作负载)
- CMake 3.20 或更高版本
- Git

### 编译步骤

1. 克隆仓库及子模块：
   ```bat
   git clone --recursive https://github.com/YourUsername/CarPlayStation.git
   cd CarPlayStation
   ```

2. 配置与生成 Release 版本：
   ```bat
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ```

3. 运行测试套件验证协议正确性：
   ```bat
   cd build
   ctest --output-on-failure
   ```

生成的可执行文件位于 `build\UI\Win32\Release\CarPlayStation.exe`。

---

## 📄 许可证

本项目依据 [LICENSE](LICENSE) 开源发布。
仅供个人学习、技术研究与车载互联协议测试交流使用，请勿用于非法商业用途。

