# MagnifyTray（放大镜托盘程序）

MagnifyTray 是一个面向 Windows 10/11 x64（视窗系统 64 位）的轻量原生托盘程序。

- 按 `Alt+W（Alt 加 W）`：系统放大镜未运行时启动，已运行时关闭。
- 托盘图标右键菜单：仅提供“退出”。
- 不写注册表、不创建配置文件、不联网、不安装服务，也不设置开机启动。
- 同一用户会话中只允许运行一个实例。

关闭时，程序先尝试向系统放大镜窗口发送正常关闭请求。消息送达后最多等待 1.5 秒；仍然存活的实例会在再次核验身份后收到强制终止请求。程序同步等待退出最多 1 秒。若系统仍在清理，托盘恢复响应，约 4 秒后再检查原进程句柄。期间再按热键也会先检查退出状态；只有确认退出后才允许重新启动。最终仍未退出才提示错误码 258。位于其他目录的同名 `Magnify.exe（同名程序）` 不会被关闭。

## 构建

要求：Visual Studio Build Tools 2022（微软构建工具），包含 MSVC（微软 C/C++ 编译器）与 Windows SDK（视窗系统开发工具包），以及 CMake 3.24 或更高版本。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
```

最终发布文件为 `dist/MagnifyTray.exe（放大镜托盘程序）`。

真实热键验收会多次显示并关闭系统放大镜，因此默认不加入普通 CTest（测试驱动器）运行。需要显式验收时使用独立构建目录：

```powershell
cmake -S . -B build-live -G "Visual Studio 17 2022" -A x64 -DMAGNIFYTRAY_ENABLE_LIVE_TESTS=ON
cmake --build build-live --config Release --target LiveAcceptanceTests
ctest --test-dir build-live -C Release --output-on-failure -R LiveAcceptanceTests
```

该测试执行 3 次完整开关、外部启动后关闭和 `MOD_NOREPEAT（禁止连发）` 长按验证，并在成功或失败后恢复为关闭状态。

## 源码结构

- `src/magnifier_controller.*`：与 Windows（视窗系统）接口隔离的切换决策。
- `src/win32_process_backend.*`：进程枚举、文件身份核验、启动与关闭。
- `src/tray_application.*`：单实例、隐藏窗口、全局热键和托盘生命周期。
- `resources/`：原创图标、应用清单与版本资源。
- `tests/`：控制器、文件身份和应用生命周期自动测试。

## 错误处理

热键冲突、系统放大镜启动失败、进程身份无法核验或关闭失败时，程序会显示中文错误说明和 Win32 Error Code（Windows 错误码）。正常切换不会显示通知。
