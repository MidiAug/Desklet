# 同桌 / Desklet

Windows 原生虚拟桌面切换工具。此仓库用于保存便携版下载。

打开 [最新发布](https://github.com/MidiAug/Desklet/releases/latest)，下载 `Desklet-0.5.3-windows-x64.zip`，解压后双击 `Desklet.exe`。无需管理员权限和开发环境。

仓库为私有，在其他电脑下载需要登录有访问权限的 GitHub 账号。
关闭主窗口后程序留在托盘；右键托盘图标可退出。更新前先退出旧版。开机自启请在程序放在固定位置后启用。

0.5.3 修复失焦或拖动其他内容后出现白色粗边框。支持桌面切换、重命名、排序、应用窗口行为和可选顶部快捷切换。

发布前重新构建并通过 --check、--render-test、--resident-check、--compat-check。依赖 Windows 内部桌面接口，系统更新可能影响兼容性。真实第三方应用和其他系统版本尚未全面验收。
