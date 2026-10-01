# 来源闭包与审计方法

当前研究目录的公开行为来源不是单一注册表，而是以下闭包：

```text
functions.h/md_func*       -> 253 个库函数
script.cpp/g_BIF           -> 101 个核心 BIF
script_object*.cpp         -> Object/Array/Map/Func/Buffer/Regex 成员和动态属性
script_gui*.cpp            -> Gui/GuiControl 专用对象
script_menu.cpp             -> Menu/MenuBar 对象
input_object.cpp            -> InputHook 对象
TextIO.cpp                  -> File 对象
script_com.cpp              -> ComObject/VARIANT/SAFEARRAY/事件
ahklib.idl/ahklib.cpp      -> Host ABI 和静态检查对象
globaldata.*/vars.cpp       -> 内置变量和隐式状态
hotkey/hook/application     -> 指令、标签、Hook、消息泵和伪线程
error.cpp                   -> 错误、警告、退出和恢复语义
Debugger.cpp                -> 调试、检查、暂停、单步和宿主诊断
script_registry.cpp         -> 注册表/系统集成边界
```

## 审计顺序

1. 从源码注册表提取名字、参数、返回值和实现符号；
2. 追到实际 Win32/OLE/CRT 调用和资源所有权；
3. 记录线程、Apartment、消息泵、可中断点和重入规则；
4. 判断属于标准 TS、Runtime Native、隔离插件还是明确不暴露；
5. 写入唯一 API ID、状态、权限、错误和测试 ID；
6. 对源码注册表变更执行清单漂移检查。

“有名字”不等于“已覆盖”。只有完成这六步并通过 contract/native/lifecycle 测试，条目才可标记 `implemented`。
