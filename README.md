# 同桌 / Desklet

轻量的 Windows 虚拟桌面管理工具，使用 C++ / Win32 构建，无需管理员权限或额外运行时。

## 功能

- 预览、切换、创建、重命名和排序虚拟桌面。
- 按应用设置窗口行为：独立窗口、各桌面独立展开/收起，或跟随 Windows。
- 可选的屏幕顶部快捷切换、托盘操作和开机自启。
- 支持多显示器与不同 DPI 缩放。

## 下载与使用

从 [Releases](https://github.com/MidiAug/Desklet/releases/latest) 下载 Windows x64 便携包，解压后双击 `Desklet.exe`。

关闭主窗口后，程序仍在托盘运行；右键托盘图标可退出。更新前请先退出旧版。需要开机自启时，请先将程序放在固定目录，再在设置中开启。

点击桌面卡片切换，拖动卡片调整顺序，点击重命名按钮或按 F2 修改名称。顶部快捷切换默认关闭，可在设置中开启。

## 从源码构建

需要 Windows x64、PowerShell、Visual Studio 2022 或更新版本的“使用 C++ 的桌面开发”工具和 Windows SDK。

```powershell
./build.ps1
./build/Desklet.0.5.3.exe
```

使用静态 C++ 运行库，不依赖 WebView 或 .NET。重新生成图标时，需要 Python 和 Pillow，执行 `python scripts/make_icons.py`；普通构建无需 Python。

## 验证

```powershell
./scripts/verify.ps1
```

验证包含状态检查、离屏绘制、只读资源采样及隔离测试窗口的兼容回归，不自动切换系统桌面。输出位于 `artifacts/`，不进入版本控制。

## 兼容性

程序依赖 Windows 内部虚拟桌面接口，Windows 版本差异或系统更新可能影响兼容性。当前发布版经过本机自动验证，尚未全面验证所有 Windows 版本和第三方应用。

“独立窗口”需要应用自身支持多个窗口，不会复制应用进程或登录会话。退出时恢复本次管理的窗口状态，不删除桌面。

## 许可证

[MIT](LICENSE)。Windows 接口参考来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
