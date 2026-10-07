# GUI, Menu, Tray and Dialog API

状态：`SoundGetVolume` / `SoundSetVolume` / `SoundGetMute` / `SoundSetMute` / `SoundGetName` 已实现（实现归属 `@rime/sound`，见 `docs/api/sound.md`），`SoundGetInterface` 为 `unsupported-by-policy`；本域其余项（`Gui*` / `Menu*` / `Tray*` / `ToolTip` / `MsgBox` / `InputBox` / `LoadPicture` / `IL_*`）仍为 `contract-only`。

源码证据：`functions.h` 的 `Gui*`、`Menu*`、`Tray*`、`ToolTip`、`MsgBox`、`InputBox`、`Sound*`、`LoadPicture`、`IL_*`；`source/script_gui.cpp`、`source/script_menu.cpp`、`source/lib/sound.cpp`。

GUI、菜单、托盘和模态对话框对象必须由 UI Thread 所有，以稳定 ID 和事件订阅暴露。模态对话框不能启动第二个脚本消息泵；结果通过同一个 JS 调度器返回。图片和 ImageList 句柄不得直接暴露。
