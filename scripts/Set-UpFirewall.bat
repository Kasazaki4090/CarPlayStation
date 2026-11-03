@echo off
chcp 65001 >nul
title CarPlayStation 防火墙入站规则一键配置脚本

:: 1. 检查管理员权限，若无则自动弹出 UAC 提权窗口
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [INFO] 正在申请管理员权限...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
    exit /b
)

echo =======================================================
echo          CarPlayStation 防火墙入站规则配置
echo =======================================================
echo.

:: 2. 自动定位 CarPlayStation.exe 路径
set "EXE_PATH="

:: 优先检查命令行传入参数
if not "%~1"=="" (
    if exist "%~1" set "EXE_PATH=%~f1"
)

:: 依次探测常见目录
if not defined EXE_PATH if exist "%~dp0CarPlayStation.exe" set "EXE_PATH=%~dp0CarPlayStation.exe"
if not defined EXE_PATH if exist "%~dp0..\CarPlayStation.exe" set "EXE_PATH=%~dp0..\CarPlayStation.exe"
if not defined EXE_PATH if exist "%~dp0..\build\UI\Win32\Release\CarPlayStation.exe" set "EXE_PATH=%~dp0..\build\UI\Win32\Release\CarPlayStation.exe"
if not defined EXE_PATH if exist "%~dp0..\build\UI\Win32\CarPlayStation.exe" set "EXE_PATH=%~dp0..\build\UI\Win32\CarPlayStation.exe"
if not defined EXE_PATH if exist "%~dp0..\build-release\UI\Win32\CarPlayStation.exe" set "EXE_PATH=%~dp0..\build-release\UI\Win32\CarPlayStation.exe"
if not defined EXE_PATH if exist "%~dp0dist\CarPlayStation.exe" set "EXE_PATH=%~dp0dist\CarPlayStation.exe"

:: 规范化为绝对路径
if defined EXE_PATH (
    for %%I in ("%EXE_PATH%") do set "EXE_PATH=%%~fI"
)

if not defined EXE_PATH (
    echo [提示] 未能在默认路径自动找到 CarPlayStation.exe。
    echo 请直接将 CarPlayStation.exe 拖拽到本窗口并按回车：
    set /p "EXE_PATH="
    if not defined EXE_PATH (
        echo [错误] 未指定 CarPlayStation.exe 路径，脚本退出。
        pause
        exit /b 1
    )
    for %%I in ("%EXE_PATH%") do set "EXE_PATH=%%~fI"
)

if not exist "%EXE_PATH%" (
    echo [错误] 找不到文件: %EXE_PATH%
    pause
    exit /b 1
)

echo [1/4] 目标程序: %EXE_PATH%
echo.

:: 3. 配置程序完整放行规则（所有网络配置：公用、专用、域）
echo [2/4] 配置程序放行规则: CarPlayStation (all profiles)...
netsh advfirewall firewall delete rule name="CarPlayStation (all profiles)" >nul 2>&1
netsh advfirewall firewall add rule name="CarPlayStation (all profiles)" dir=in action=allow profile=any program="%EXE_PATH%" enable=yes
if %errorlevel% neq 0 (
    echo [警告] 添加程序规则时返回异常。
) else (
    echo [成功] 程序规则已应用 (Profile: Any)。
)

:: 4. 配置 mDNS UDP 5353 独立放行规则（用于无线热点下设备广播发现）
echo [3/4] 配置 mDNS 发现端口: CarPlayStation mDNS (UDP 5353)...
netsh advfirewall firewall delete rule name="CarPlayStation mDNS (UDP 5353)" >nul 2>&1
netsh advfirewall firewall add rule name="CarPlayStation mDNS (UDP 5353)" dir=in action=allow profile=any protocol=UDP localport=5353 enable=yes
if %errorlevel% neq 0 (
    echo [警告] 添加 mDNS 端口规则时返回异常。
) else (
    echo [成功] mDNS UDP 5353 端口规则已应用 (Profile: Any)。
)

:: 5. 配置 RTSP 控制信令 (TCP 5000) 与 RTP 音视频传输 (UDP 7000-7100) 端口
echo [4/4] 配置 RTSP 控制与 RTP 媒体传输端口...
netsh advfirewall firewall delete rule name="CarPlayStation RTSP (TCP 5000)" >nul 2>&1
netsh advfirewall firewall add rule name="CarPlayStation RTSP (TCP 5000)" dir=in action=allow profile=any protocol=TCP localport=5000 enable=yes

netsh advfirewall firewall delete rule name="CarPlayStation RTP Media (UDP 7000-7100)" >nul 2>&1
netsh advfirewall firewall add rule name="CarPlayStation RTP Media (UDP 7000-7100)" dir=in action=allow profile=any protocol=UDP localport=7000-7100 enable=yes

echo.
echo =======================================================
echo [完成] CarPlayStation 防火墙入站规则已全部成功配置！
echo 无论 Windows 移动热点被判定为公用 (Public) 还是专用 (Private)，
echo iPhone 均可正常连接与投屏。
echo =======================================================
echo.
pause

