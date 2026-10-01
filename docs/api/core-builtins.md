# 核心内建函数来源

`functions.h` 的 253 项是通过 `md_func*` 元数据声明的库函数集合，但 `script.cpp` 还维护独立的 `g_BIF`/`BIF1` 核心内建函数表。对 `script.cpp` 当前源码提取到 41 个核心函数：

`Abs`、`ATan`、`ATan2`、`Chr`、`Click`、`ComObjActive`、`ComObjConnect`、`ComObjFlags`、`ComObjGet`、`ComObjQuery`、`ComObjType`、`ComObjValue`、`Cos`、`DefineProp`、`Exp`、`FileOpen`、`Format`、`FormatTime`、`HasBase`、`HasProp`、`InStr`、`IsObject`、`Mod`、`NumGet`、`NumPut`、`ObjBindMethod`、`Ord`、`Props`、`Random`、`Round`、`Sin`、`Sort`、`StrCompare`、`StrLen`、`StrPtr`、`SubStr`、`Tan`、`Throw`、`Type`、`VarSetStrCapacity`、`VerCompare`。

这些函数不能简单并入 `coverage.json` 而不标记来源，因为它们的参数元数据、实现入口和错误行为来自 `script.cpp`/`script_func_impl.h`，而不是 `functions.h`。特别是：

- `FileOpen` 返回有所有权的 File 对象，必须进入 `objects.json` 和 IO 生命周期测试；
- `ComObj*` 必须进入 COM 隔离矩阵；
- `NumGet`/`NumPut`/`StrPtr`/`VarSetStrCapacity` 涉及内存地址，默认不能直接映射到公共 TS API；
- `Throw`、`Type`、`DefineProp`、`HasBase`、`HasProp`、`Props` 属于语言/对象运行时，不是 Windows 自动化模块；
- `Click` 是输入注入能力，必须与 `MouseClick` 的兼容语义合并审计；
- `Random`、数学、字符串和格式化函数属于 SDK-owned，但仍需 AHK 兼容测试。

因此当前已知的函数型表面至少是 `253 + 41` 项，尚不包括对象成员、动态属性、内置变量、指令、语法事件和 Host ABI。这个数字只用于审计定位，不代表最终 API 数量，因为同名方法、别名和动态成员必须按来源和语义去重。

## 必须纳入的字段

核心函数清单需要和 `coverage.json` 使用相同字段，并额外加入 `sourceKind: "core-bif"`、`definitionFile`、`minParams`、`maxParams`、`returnsObject`、`unsafeMemory` 和 `compatibilityTest`。CI 必须在 `g_BIF` 变化时提示清单漂移。
