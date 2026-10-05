# AHK API 来源与完整性审计

`functions.h` 只登记通过 `md_func*` 宏导出的 253 个内建函数。它不是完整 API 清单。AutoHotkey-alpha 的公开行为至少由以下来源组成：

| 来源 | 代表文件 | 暴露内容 | 当前覆盖 |
|---|---|---|---|
| 库内建函数 | `source/lib/functions.h`、`source/lib/*.cpp` | 253 个函数的参数和返回值元数据及实现 | 已进入 `coverage.json`；大多数仅 contract-only |
| 核心内建函数 | `source/script.cpp`、`source/script_func_impl.h` | `g_BIF` 注册的 101 个数学、字符串、对象、COM、声音、注册表和输入函数 | 已逐项进入 [`core-builtins.json`](./core-builtins.json)（见 [`core-builtins.md`](./core-builtins.md)） |
| 对象原型 | `script_object.cpp/.h` | Object、Array、Map、Func、Buffer、ClipboardAll、RegExMatch 对象的方法/属性 | 已进入 [`objects.json`](./objects.json)（见 [`object-model.md`](./object-model.md)） |
| GUI 对象 | `script_gui.cpp/.h`、`Gui.*.cpp` | Gui、GuiCtrl、ListView、TreeView、StatusBar、Edit、Date、Tab 等方法/属性 | 成员已进 `objects.json`；控件消息 fallback 仍缺（见 [`audit-gaps.md`](./audit-gaps.md)） |
| Menu 对象 | `script_menu.cpp/.h` | Menu/MenuBar 的 Add、Insert、Delete、Show、Check、Icon、Default、Handle 等 | 成员已进 `objects.json`；API 面见 [`gui-menu.md`](./gui-menu.md) |
| InputHook | `input_object.cpp/.h`、`input.cpp` | InputHook 对象属性、Start/Stop/Wait/OnEnd、匹配和 EndKey 语义 | 23 成员已进 `objects.json`，行为契约见 [`hotkey-events.md`](./hotkey-events.md) |
| File 对象 | `TextIO.cpp/.h` | File 对象构造、读写、Seek、Tell、Encoding、AtEOF、Length 等 | 31 成员已进 `objects.json`；读写面仍 contract-only（M4 storage） |
| COM 对象 | `script_com.cpp/.h`、`script_autoit.cpp` | ComObject、ComValue、ComValueRef、事件连接、SAFEARRAY、VARIANT 转换 | 仅隔离策略 |
| 宿主 ABI | `ahklib.idl`、`abi.h/.cpp`、`README-LIB.md` | Main、LoadFile、Execute、OnProblem、Script、Funcs、Vars、Labels、Files 和描述接口 | 未纳入 Runtime Host contract |
| 内置变量 | `globaldata.h/.cpp`、`script.h` | `A_*`/`A_...` 环境、脚本、输入、窗口、时间、路径和命令行状态 | 未建立变量清单 |
| 语法级事件 | `script.cpp`、`hotkey.cpp`、`hook.cpp` | 热键标签、热字符串、自动执行段、退出/错误回调、伪线程和消息泵语义 | 仅抽象为事件 API |
| 指令/配置 | `globaldata.cpp`、`hotkey.cpp`、`hook.cpp`、`AutoHotkey.cpp` | `#HotIf`、`#Hotstring`、`#InputLevel`、`#Install*Hook`、`#MaxThreads*`、`#SingleInstance`、`#SuspendExempt` 等 | 见 [`directives-and-syntax.md`](./directives-and-syntax.md) |
| 动态属性/元对象 | `script_object.cpp`、`script_expression.cpp` | `DefineProp`、`__Get`、`__Set`、`__Call`、`__Enum`、原型链、ByRef 和函数对象 | 未定义 TS 兼容边界 |

## 对象原型清单

源码已明确注册以下原型，必须与函数矩阵分开建模：

- `Object`: `__Ref`、`Clone`、`DefineProp`、`DeleteProp`、`GetOwnPropDesc`、`HasOwnProp`、`OwnProps`。
- `Array`: `Capacity`、`Length`、`__Enum`、`Clone`、`Delete`、`Get`、`Has`、`InsertAt`、`Pop`、`Push`、`RemoveAt`。
- `Map`: `__Enum`、`Clear`、`Clone`、`Delete`、`Has`、`Set`。
- `Func`: `Bind`、`Call`、`IsByRef`、`IsOptional`、`IsBuiltIn`、`IsVariadic`、`MaxParams`、`MinParams`、`Name`。
- `Buffer`: `Ptr`、`Size`、构造函数。
- `ClipboardAll`: 构造函数和二进制剪贴板所有权。
- `RegExMatchObject`: `__Enum`、`__Get`、`Len`、`Name`、`Pos`。
- `ComObject`: `__Item`、`__Value`、`Ptr` 以及 VARIANT/SAFEARRAY 语义。

这些能力不能简单映射为普通 JS 对象：需要明确可变性、迭代协议、原型、异常、生命周期和是否允许跨线程。Rime 标准库优先使用原生 TS 类型；不为兼容目的模拟 AHK 的名称和调用形状。

## 宿主 ABI 清单

`ahklib.idl` 定义的接口不是 253 个内建函数的附属信息，而是脚本加载、执行和静态检查协议：

- `Main(cmdLine)`、`LoadFile(fileName)`、`Execute()`；
- `OnProblem(callback)`；
- `Script`、`Funcs`、`Vars`、`Labels`、`Files`；
- `IDispCollection.Item/Count/_NewEnum`；
- `IDescribeVar`、`IDescribeLabel`、`IDescribeParam`、`IDescribeFunc`。

Rime 应设计独立的版本化 Host ABI，不直接复用 IDispatch 指针。对应的 TS 检查 API 只返回序列化描述，不能执行未知脚本；卸载前必须检查活动回调、Hook、窗口过程、COM 引用和 JS 引用。

## 完整性规则

宣布“API 对齐”必须同时满足：

1. `coverage.json` 的 253 个函数逐项有精确实现证据；
2. 对象原型、GUI/菜单/输入/文件/COM 对象各有独立成员矩阵；
3. 内置变量和语法级事件有来源清单和 TS 等价语义；
4. Host ABI 有版本化 contract；
5. 每项都标记 `implemented`、`contract-only`、`sdk-owned` 或 `unsupported-by-policy`，并有测试 ID；
6. 任何不暴露的 AHK 能力都记录原因、替代方案和权限边界。
