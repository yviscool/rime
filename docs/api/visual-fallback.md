# Visual Fallback（视觉兜底）

状态：`design`（本文件是规格，不声称实现）。已实现部分以 `docs/api/screen.md`
为准：`pixel / pixelSearch / imageSearch / caret / sysGet`；本文件只回答
“语义找不到、Win32 够不着、发输入也不行时，视觉层怎么接住”，不重复 `screen.md`。

## 1. 四层选择顺序（与 `future-runtime.md §5` 同源）

```text
UIA semantic → Win32 direct → input → visual
```

执行层按此顺序降级，并在 Trace 中记录实际命中的层级。用户调用的是语义能力，
不是坐标和 HWND。当前已落地的只有后两层的一半：

| 层 | 状态 | 证据 |
|---|---|---|
| UIA semantic | `implemented`（find/invoke/read/write） | `engine/automation`、`sdk/src/automation.ts` |
| Win32 direct | `implemented`（Control*/Win*） | `engine/win32`、`sdk/src/control.ts` |
| input（SendInput/点击） | `implemented` | `engine/win32` input、tests L5 |
| visual/search（pixel/imageSearch） | `implemented` | `docs/api/screen.md`、三层 contract |
| visual/capture（返回截图给调用方） | 缺口：无 `screen.capture` 公开 API | 本文件 §3 |
| visual/OCR（读字） | 缺口：无引擎、无依赖、无测试 | 本文件 §3 |
| visual/template（透明色/多图/缩放） | 缺口：只支持等比 RGB（见 `screen.md` 偏差 1-4） | 本文件 §3 |

## 2. 层选择规则（纯策略，可先按此写 L2 测试）

1. 有 UIA 元素且支持所需 Pattern → `uia`，不碰坐标。
2. 否则有 Win32 控件（classNN/控件句柄可解析）→ `win32`。
3. 否则目标可聚焦、可发键鼠 → `input`（需 `windows.input.inject`）。
4. 否则只能看屏 → `visual`（需 `screen.capture`）：`pixelSearch` 定点、
   `imageSearch` 定图；都 miss 即 `{found:false}`——miss 是结果，不是错误。
5. 需要读字（验证码、画布文字、无 UIA 名的自绘控件）→ `ocr`：当前无能力，
   必须显式失败并指引（`unsupported: screen.ocr`），不得用 `imageSearch`
   假装“读到了字”。

`visual` 层永远返回屏幕坐标 + `{found}` 形状，不抛“没找到”错误；
文件坏、区域越界、variation 越界才拒绝（`invalid_contract`），与 `screen.md`
的错误表一致。

## 3. 缺口与验收（不画饼）

* `screen.capture(area?) → ImageSnapshot`：未设计像素所有权（谁拥有 HBITMAP、
  跨 lane 怎么传只读拷贝）、未定 capability（沿用 `screen.capture` 还是新名）。
  在此之前，调用方只能用 `pixel/imageSearch` 做“屏上找”，拿不到图。
* `screen.ocr(area) → string`：无引擎选型（Windows OCR / ONNX / 第三方皆未定），
  无测试。AHK 本体也没有内建 OCR（靠库），所以这不计入 AHK 兼容分母，
  只计入本 Runtime 的 visual 完整性。
* 模板增强（透明色、多尺度、图标资源）：`screen.md` 偏差 1-4 逐条列出，
  每条增强都必须先有 golden fixture（针+场+variation 边界），再谈实现。

验收：每层降级在 Trace 留痕；visual miss 不带坐标；ocr 缺席显式失败；
截图 API 落地前任何“返回图片”的说法都以本文件为准判为未实现。
