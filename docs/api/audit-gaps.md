# 第二轮审计缺口

这份清单防止把“253 个函数已列出”误认为“AHK API 已完整建模”。

## 已发现但尚未逐项提取

- 对象成员矩阵已按真实源码签名提取进 `objects.json`：Gui、GuiControl、Menu、InputHook、File 以及 Object、Array、Map、Func、Buffer、ClipboardAll、RegExMatch、ComObject 的每个成员都补齐了 `name/kind/parameters/returnType/sourceDefinition/lane/async/ownership/error/compatibilityTest`；剩余缺口只是 `md_member*` 元数据层的参数个数与可选参数边界仍需 CI 做漂移检查。
- `script.cpp` 的 101 个核心内建函数（`BIF1` 41 + `BIFn` 47 + `BIFi` 13）；它们不在 `functions.h`，必须建立独立来源字段和语义还原测试。
- 内置变量：`builtins.json` 目前是按源码命中的分域清单，不是完整定义表；需要从 `globaldata` 的注册/解析逻辑提取所有动态变量、只读属性和更新时机。
- `ahklib.idl` 的全部 ABI 与描述对象；需要生成版本化 Rime Host ABI contract，而不是把它归入普通函数。
- COM/VARIANT/SAFEARRAY 互操作，包括事件 sink、引用计数、Apartment 和异常转换。
- AHK 语法级能力：热键标签、热字符串、自动执行段、类/原型、ByRef、动态属性、异常、伪线程、`Critical` 和嵌套消息泵。
- 指令和配置状态：`#Requires`、`#SingleInstance`、`#Persistent`、`#InstallKeybdHook`、`#HotIf`、`#Hotstring` 等不在 `functions.h` 中。
- 内置错误原型、警告、`OnError` 继续/重抛规则、ErrorLevel/退出码语义。
- `globaldata.*` 中的隐式状态、Timer 层、Hook 句柄、输入状态、GUI 链表、单实例和调度变量。
- `Debugger.cpp` 的调试、暂停、检查和宿主诊断入口，以及 `script_registry.cpp` 的系统集成边界。
- 控件专用对象：ListView、TreeView、StatusBar、Edit、Date、Tab、ComboBox 的成员和消息 fallback。
- 资源和二进制对象：Buffer、ClipboardAll、ImageList、Picture、菜单句柄、File 对象的确定性关闭。

## 必须增加的验证

1. 从源码元数据生成库函数、核心 BIF、对象成员、内置变量和 ABI 的清单，并在 CI 中检查源变更导致的清单漂移。
2. 每个清单项拥有唯一 ID、源码定位和 contract test ID；禁止只写功能域级描述。
3. 对象和订阅执行 GC/显式关闭/Runtime shutdown 三种路径，验证没有 Hook、COM、窗口过程或 JS 回调泄漏。
4. 对 Hotkey、Hotstring、Timer、OnMessage、GUI 回调测试队列满载、嵌套泵、取消竞态和重入策略。
5. 对 Host ABI 测试 load、execute、error、inspect、exit、busy-unload 和失败诊断。
6. 每个还原保留的 API 分别测试 AHK 参考语义与 Rime 原生 TS 行为，不能用一个宽泛的“Windows API 测试”代替。

## 完成标准

只有当 `coverage.json`、`core-builtins.json`、`objects.json`、`builtins.json`、ABI、指令、状态和错误清单中的每一项都具有精确源码证据、TS 契约、实现状态和测试 ID，才能称为“设计完备”。实现完备还要求状态全部达到 `implemented` 或有明确的 `unsupported-by-policy` 替代方案。
