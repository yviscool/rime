# Native Interop and Isolation

状态：默认 `unsupported-by-policy`；需要受信任插件或显式宿主能力才能实现。

源码证据：`functions.h` 的 `DllCall`、`ComCall`、`CallbackCreate`、`CallbackFree`、`ObjGetDataPtr`、`ObjGetDataSize`、`ObjSetDataPtr`，以及 `source/lib/DllCall.cpp`、`source/lib/CCallback.cpp`、`source/script_com.cpp`。

标准 TS API 不暴露裸 `HWND`、`HANDLE`、COM 指针、任意函数指针、对象内存地址或任意 DLL 调用。受控替代必须声明 ABI、参数布局、线程/apartment、权限、取消和卸载条件；存在活动回调或外部引用时 unload 必须失败并给出诊断。
