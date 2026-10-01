# Control and UI Automation API

状态：`contract-only`，尚未实现。

源码证据：`functions.h` 的 `Control*`、`Edit*`、`ListViewGetContent`、`StatusBar*`；`source/script_gui.cpp`、`source/script_gui.h` 及 UIA/COM 相关实现。

Control API 首选 UI Automation pattern，其次 Win32 message，最后才允许显式 visual fallback。UIA/COM 对象只在 Automation MTA 持有；JS 只收到稳定 `ControlId` 和快照。所有读写均为异步，可取消，失败必须说明所选执行层。

必须逐项覆盖 ControlAddItem、ControlChooseIndex、ControlChooseString、ControlClick、ControlDeleteItem、ControlFindItem、ControlFocus、ControlGet*、ControlHide、ControlHideDropDown、ControlMove、ControlSend、ControlSendText、ControlSet*、ControlShow、ControlShowDropDown、Edit*、ListViewGetContent、StatusBarGetText、StatusBarWait。
