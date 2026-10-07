# 核心内建函数来源

`functions.h` 的 253 项是通过 `md_func*` 元数据声明的库函数集合，但 `script.cpp` 还维护独立的 `g_BIF` 核心内建函数表（`BIF1` 41 + `BIFn` 47 + `BIFi` 13 = 101）。全部 101 项已在 `core-builtins.json` 的 `entries` 中逐项登记（含参数、来源、lane、能力与测试状态），按 domain 分布：

- **runtime-language**（64）：`ACos`、`ASin`、`ATan`、`ATan2`、`Abs`、`Ceil`、`Chr`、`Cos`、`DefineProp`、`Exp`、`Floor`、`Format`、`FormatTime`、`GetMethod`、`HasBase`、`HasMethod`、`HasProp`、`InStr`、`IsAlnum`、`IsAlpha`、`IsDigit`、`IsFloat`、`IsInteger`、`IsLower`、`IsNumber`、`IsObject`、`IsSetRef`、`IsSpace`、`IsTime`、`IsUpper`、`IsXDigit`、`LTrim`、`Ln`、`Log`、`Max`、`Min`、`Mod`、`ObjBindMethod`、`ObjGetBase`、`ObjGetCapacity`、`ObjHasOwnProp`、`ObjOwnPropCount`、`ObjOwnProps`、`ObjSetBase`、`ObjSetCapacity`、`Ord`、`Props`、`RTrim`、`Random`、`Round`、`Sin`、`Sort`、`Sqrt`、`StrCompare`、`StrLen`、`StrLower`、`StrTitle`、`StrUpper`、`SubStr`、`Tan`、`Throw`、`Trim`、`Type`、`VerCompare`
- **native-interop**（22）：`ComCall`、`ComObjActive`、`ComObjConnect`、`ComObjFlags`、`ComObjFromPtr`、`ComObjGet`、`ComObjQuery`、`ComObjType`、`ComObjValue`、`DllCall`、`NumGet`、`NumPut`、`ObjAddRef`、`ObjFromPtr`、`ObjFromPtrAddRef`、`ObjPtr`、`ObjPtrAddRef`、`ObjRelease`、`StrGet`、`StrPtr`、`StrPut`、`VarSetStrCapacity`
- **gui-menu**（6）：`SoundGetInterface`、`SoundGetMute`、`SoundGetName`、`SoundGetVolume`、`SoundSetMute`、`SoundSetVolume`
- **storage**（6）：`FileOpen`、`RegCreateKey`、`RegDelete`、`RegDeleteKey`、`RegRead`、`RegWrite`
- **window**（2）：`WinActive`、`WinExist`
- **input**（1）：`Click`

这些函数不能简单并入 `coverage.json` 而不标记来源，因为它们的参数元数据、实现入口和错误行为来自 `script.cpp`/`script_func_impl.h`，而不是 `functions.h`。特别是：

- `FileOpen` 返回有所有权的 File 对象，必须进入 `objects.json` 和 IO 生命周期测试；
- `ComObj*` 必须进入 COM 隔离矩阵；
- `NumGet`/`NumPut`/`StrPtr`/`VarSetStrCapacity` 涉及内存地址，默认不能直接映射到公共 TS API；
- `Throw`、`Type`、`DefineProp`、`HasBase`、`HasProp`、`Props` 属于语言/对象运行时，不是 Windows 自动化模块；
- `Click` 是输入注入能力，必须与 `MouseClick` 的兼容语义合并审计；
- `Random`、数学、字符串和格式化函数属于 SDK-owned，但仍需对照原版的语义还原测试。

因此当前已知的函数型表面至少是 `253 + 101` 项，尚不包括对象成员、动态属性、内置变量、指令、语法事件和 Host ABI。这个数字只用于审计定位，不代表最终 API 数量，因为同名方法、别名和动态成员必须按来源和语义去重。

## 测试证据

`contractTest` 与 `compatibilityTest` 分工：前者是 Runtime 侧被真实消费者执行的合同测试，后者是 SDK/兼容面的消费者。六项曾是「状态翻 `implemented` 但 `contractTest` 为 `missing`」的证据缺口，回填依据如下：

- `Format`/`FormatTime`/`Round`/`Sort`/`VerCompare` → `tests/sdk/runtime-language.test.ts`。js lane 没有 native 层可以测，这个 bun 测试就是唯一的真实消费者，期望值全部锚到 AHK 源码（`string.cpp:1409-1476`、`math.cpp:44-45`、`string.cpp:777-870`、`util.cpp:3299-3338`）；`coverage.json` 的 `DateAdd`/`DateDiff` 已经用同一文件作 `contractTest`，这里是同例。**局限**：该测试是纯逻辑级（AGENTS L2），按「低于 L4 不得作为行为契约唯一证据」，它只够给这五项的函数语义背书，不够给 Runtime 分层背书；补法是把 `@rime/runtime-language` 暴露给 QuickJS 后加一条 slice 竖切作为第二个消费者，在此之前这两列保持同值而不是编一个不存在的文件。
- `Click` → `tests/native/input_tests.cpp`：它构造与 `Click` 同形的 `send_mouse` down/up 批次，并用真实钩子按 `self_injected` + `button == 1` 观察回来（L5，含空批次与非法按钮的拒绝路径）。参数到 steps 的映射在 `tests/sdk/send.test.ts`（`compatibilityTest`），down/up step 的 JS 边界校验在 `tests/js/input_slice.cpp`（同列）。

## 必须纳入的字段

核心函数清单需要和 `coverage.json` 使用相同字段，并额外加入 `sourceKind: "core-bif"`、`definitionFile`、`minParams`、`maxParams`、`returnsObject`、`unsafeMemory` 和 `compatibilityTest`。CI 必须在 `g_BIF` 变化时提示清单漂移。
