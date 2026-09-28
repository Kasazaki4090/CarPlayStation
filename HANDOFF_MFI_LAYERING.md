# HANDOFF — MFi 端点归属重构：当前进度与剩余工作

* **日期**：2026-09-28 (UTC+8)
* **状态**：**进行中（代码主体已完成，测试有一处编译错误 + 文档待同步）**
* **用途**：本地 AI 处理卡死，本文档供在线 AI 接手完善。所有改动目前都在工作区（未提交）。

---

## 1. 最终决策（用户裁定）

> **MFi daemon 的网址端点（URL endpoint）移到 Application 层；其他认证功能保留在 Core。**
> MFi 认证属于**用户不可直接操作的硬件层面**，和 RTSP/AirPlay 这类普通网络端点性质不同。

具体归属：

| 项目 | 归属层 | 位置 |
|---|---|---|
| MFi 后端选择（本地 CH341 I2C 芯片 vs 网络签名 daemon） | **Application**（用户偏好，可运行时切换） | `Application/AppSettings.h :: MFiPreferences::use_hardware_backend` |
| MFi daemon TCP 端点（host:port，默认 `127.0.0.1:9000`） | **Application**（用户偏好） | `Application/AppSettings.h :: MFiPreferences::daemon_endpoint` |
| 认证能力本身：取证书 / 挑战签名 / 芯片诊断 / 资源释放 | **Core/Hardware**（硬件抽象，用户不可操作层面） | `Core/Hardware/IMfiSigner.h :: IMfiSigner` |
| RTSP 绑定、AirPlay 音视频端口、SoftAP SSID/密码等 | Core（环境事实，Transport 消费） | `Core/Common/Config.h :: NetworkEndpoints` |

原则：**Core 永不硬编码 daemon 地址**；Application 层根据 `MFiPreferences` 构造对应的 `IMfiSigner` 实现（实现在 Lib 层），注入给 Core。

---

## 2. 已完成的工作（工作区，未提交）

1. **`Core/Common/Config.h` — MFi 端点已从 Core 移除**
   - `NetworkEndpoints` 中已删除 `mfi_daemon_endpoint` 和 `enable_hardware_mfi`。
   - 文件头注释与 `NetworkEndpoints` 上方 NOTE（约第 63-65 行）已说明：MFi 后端选择与端点是 Application 层偏好，见 `Application::MFiPreferences`（DEC-003、DEC-004）。

2. **新增 `Application/AppSettings.h`**（未跟踪文件，本重构核心产物之一）
   - `VideoPreferences` / `AudioPreferences` / `DevicePreferences`：用户可调偏好（见 result.md 记录 3、DEC-004）。
   - **`MFiPreferences`**（第 79-88 行）：
     ```cpp
     struct MFiPreferences {
         // true  = local USB-to-I2C bridge chip (CH341 or equivalent) driving an MFi coprocessor directly;
         // false = network signing daemon over TCP at `daemon_endpoint`.
         bool use_hardware_backend{false};
         Core::Common::IpEndpoint daemon_endpoint{"127.0.0.1", 9000}; // Network backend endpoint (default per DEC-003)
     };
     ```
   - `AppSettings` 容器含 `MFiPreferences mfi;`。

3. **新增 `Core/Hardware/IMfiSigner.h`** — 抽象认证接口留在 Core::Hardware：
   - `FetchCertificate()` / `SignChallenge(ByteSpan)` / `GetChipInfo()`（返回 `MFiChipInfo{chip_id, firmware_version, present}`）/ `Shutdown()`。
   - 头注释已写明两种后端（USB-I2C 硬件芯片、TCP 网络 daemon），且"endpoint is Application-layer configuration (see Application/AppSettings.h MFiPreferences); Core never hardcodes it."

4. **接口签名同步**（见 result.md 记录 3）：
   - `Application/IApplicationApi.h`：`StartCarPlay/ApplySettings` 改收 `AppSettings`；`AppUiState.active_settings` 为 `AppSettings`。
   - `Core/Service/ICarPlayCoreApi.h`：`Initialize/UpdateEnvironment` 改收 `CoreEnvironmentConfig`（= DeviceIdentity + NetworkEndpoints，不含 MFi）。

---

## 3. 剩余工作（按优先级）

### P0 — 编译错误（必须修）

**`Tests/CommonTests.cpp:103-104`** 仍在引用已删除的 `env.network.mfi_daemon_endpoint`：

```cpp
// 现状（无法编译，字段已从 Core::NetworkEndpoints 移除）：
env.network.mfi_daemon_endpoint = IpEndpoint{"192.168.2.50", 9999};
assert(env.network.mfi_daemon_endpoint.ToString() == "192.168.2.50:9999");
```

**修法**：MFi 端点断言改到 Application 层偏好上（与第 37-42 行 `using namespace CarPlayStation::Application;` / `AppSettings settings;` 合并）：

```cpp
// MFi daemon endpoint now lives in the Application layer (user preference), not Core:
settings.mfi.daemon_endpoint = IpEndpoint{"192.168.2.50", 9999};
assert(settings.mfi.daemon_endpoint.ToString() == "192.168.2.50:9999");
assert(settings.mfi.use_hardware_backend == false); // 默认走网络 daemon（DEC-003）
```

### P1 — 文档同步（旧描述仍说 MFi 端点在 Core）

1. **`Core/Service/ICarPlayCoreApi.h:21`** 注释 `// Hot-update runtime environment configuration (e.g. MFi daemon endpoint, SoftAP identity)` —— "MFi daemon endpoint" 已不属于 CoreEnvironmentConfig，改为只举 SoftAP / RTSP 端口等例子。
2. **`DECISIONS.md` DEC-004（约第 459 行）**：`NetworkEndpoints — MFi daemon endpoint, ... hardware-MFi flag` 的描述已过时；应更新为 NetworkEndpoints 不含 MFi，并补一句"MFi backend selection + daemon endpoint are Application-layer preferences (`Application::MFiPreferences`)；IMfiSigner 能力本身留在 Core/Hardware（硬件层）"。也可以不改 DEC-004 正文、另加一条新决策记录本次细化（推荐后者，保留决策历史）。
3. **`Core/Hardware/README.md:11`**：`MfiNetworkClient ... configurable via MfiDaemonEndpoint with default 127.0.0.1:9000` —— 补充说明端点由 Application 层 `MFiPreferences::daemon_endpoint` 构造注入，Core/Lib 不持有默认地址配置字段。
4. **`result.md`**：新增"记录 4"，描述本次 MFi 端点从 Core → Application 的归属调整（含用户裁定理由：认证属硬件层、用户不可直接操作；只有 daemon 地址是用户可配的偏好）。

### P2 — 构建验证

```powershell
cmake -S D:\CarPlayStation -B D:\CarPlayStation\build -G "Visual Studio 17 2022"   # 或任意可用生成器
cmake --build D:\CarPlayStation\build --config Release
ctest --test-dir D:\CarPlayStation\build -C Release --output-on-failure          # 跑 CarPlayStation_Tests (CommonTests)
```

修完 P0 后 `Tests/CommonTests.cpp`（`TestConfigSeparation`）应全部通过。注意根 CMakeLists 已含 `Core / Application / UI/Win32 / Tests` 四个子目录，各层 CMakeLists 均已存在。

### P3 — 后续实现方向（不属于本次重构，供参考）

- Lib 层实现两个 `IMfiSigner` 后端：CH341 USB-I2C 硬件桥、TCP daemon 客户端（协议可参考 `Reference/carplayd/ccpa/probes/mfi_oracle.c` 的 op 0x01 cert / 0x02 sign 线格式，仅参考不照抄）。
- Application 层实现"偏好 → IMfiSigner 实例"的构造/注入逻辑（目前只有接口与配置结构体，尚无工厂代码）。

---

## 4. 关键文件索引

| 文件 | 角色 |
|---|---|
| `Application/AppSettings.h` | **新增**。用户偏好容器，含 `MFiPreferences`（端点归属 Application 的落点） |
| `Core/Hardware/IMfiSigner.h` | **新增**。认证能力接口（留在 Core 硬件层） |
| `Core/Common/Config.h` | MFi 字段已移除；保留 RTSP/AirPlay/SoftAP 环境事实 |
| `Application/IApplicationApi.h` / `Core/Service/ICarPlayCoreApi.h` | 接口签名已按 AppSettings / CoreEnvironmentConfig 收敛 |
| `Tests/CommonTests.cpp` | **第 103-104 行待修（P0）** |
| `DECISIONS.md` DEC-003 / DEC-004 | MFi 后端策略 / 配置分层决策；DEC-004 有一处过时描述（P1） |
| `result.md` 记录 3 | 上一轮配置分层重构的完整说明 |
