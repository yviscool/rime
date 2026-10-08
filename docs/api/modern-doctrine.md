# Modern API Doctrine（TS-first，AHK 只做养料）

本文件是后来者的第一站：Rime 公共 API 全部按此立约。与 `AGENTS.md`
“AHK 永远是养料，不是目标”同源；AHK 只允许以需求雷达、跑分对手、
行为参照三种身份出现，引用必须注明出处。凡与下述冲突的存量，一律按
“冻结遗产”处理（第 8 节），新增 API 不得引用它们做依据。

## 1. 编号与空值

- 索引一律 0-based。native 线规若保持 1-based（如控件/显示器枚举），
  翻译只发生在 SDK 边界函数内，应用代码永不见 wire 编号。
- “无”一律 `null`，不用 `0`/`-1`/`""` 做哨兵（`listFind` 未命中、
  `listIndex` 未选择、`focusedControl` 无焦点、`monitor()` 写成
  `monitor(null)` 即 primary 亦可省略）。
- 越界一律同步 `RangeError`；缺失一律 `undefined`，不抛
  `ValueError`/`IndexError`/`UnsetItemError`（AHK 错误名永不进入新 API）。

## 2. 具名优于编码

- 按钮集、图标、模式、位操作全部用名：`"yes-no"` 而非 `4`，
  `"warning"` 而非 `0x30`，`{ mode: "add", bits }` 而非 `"+N"`，
  `boolean | "toggle"` 而非 `-1/0/1`，`null` 关透明而非 `-1`。
- 数字编码只允许出现在两个地方：native 线规注释，以及 SDK 内以
  `require*`/`MSG_*` 命名的翻译函数。非法名同步 `TypeError`。

## 3. 结构化优于字符串

- 输入首选 `keyboard.press({ key: "c", ctrl: true })`：和弦是数据。
  Send 字符串（`^c`、`{Enter}`）保留可运行，但文档与注释统一称其为
  “遗留迁移前端”，新代码、新测试、新文档一律用 `press()`。
- 同理：风格用 `{ mode, bits }` 对象，不用 `"+N"` 串；文件用
  `fs/promises` 形状（下节），不用 AHK 动词。

## 4. Node 对齐（文件面）

- 文件生命周期走 `fs/promises` 的名字与形状：`readFile`/`writeFile`/
  `appendFile`/`readdir`/`mkdir`/`rm`/`unlink`/`copyFile`/`rename`/
  `exists`/`stat`/`openFile`；`File` 方法全小驼峰、定宽读写按 DataView
  位宽（`readInt32`/`writeFloat64`）。
- 三处永久偏离（桥与线程模型的物理限制，不是待办）：无 `Buffer`
  （字节过桥是 `number[]`，SDK 侧转 `Uint8Array`）；纯异步（QuickJS
  无 libuv，同步会卡线程）；错误是 `ActionError.code`（codec 版本化
  锁定，不跟 `ENOENT`）。
- `ini`/`env`/`drive`/`shortcut`/`version`/`download`/`selectFile`/
  `selectDir`/`encoding` 是 Rime 扩展（QuickJS 无 `process.env`、无对话
  框、无版本资源），不在 Node 对齐内，但同样遵守第 1、2 节。

## 5. 对象优于函数集合

- `Window`/`Gui`/`Control` 类是唯一的组合面：`resolve` 拿句柄，动词挂
  在句柄上，id 不用手传第二次。函数命名空间不再新增。
- 句柄只持有稳定 id + 上次不可变快照；每个动词经 service 重解析；
  死亡一律标准 `target_gone`，不发明客户端错误种类。

## 6. 同步性诚实

- 同步只读 JS 侧状态（`Name`/`Type`/`ClassNN`、settings）。凡是触碰
  HWND、泵、磁盘、网络的一律 `Promise`；`deadlineMs` 只约束排队与出现
  前阶段，已发生的副作用不可撤销（结果改写 + 声明，见 window.md）。
- 等待一律带超时的条件轮询或事件通知，禁裸 `sleep`（AGENTS 反作弊第 5 条）。

## 7. 错误观

- 类型错误同步 `TypeError`（含 wire 翻译前校验）；运行时失败
  `ActionError` 带版本化 `code`；取消与超时按 boundary 改写并声明副作
  用可能已发生。错误信息写给调用方看，不写 AHK 源码行号。

## 8. 冻结遗产清单（不动，不扩展，不引用）

| 遗产 | 位置 | 重开条件 |
|---|---|---|
| Send DSL 解析器（`^+!#`、`{}` 文法、修饰符时序） | `sdk/src/send/` | 迁移工具需要时才动；`press()` 已覆盖新需求 |
| `runtime-language` 方言垫片（`Format` 方言、ATOF/ATOI、`Sort` 旗、PCRE 旗翻译） | `sdk/src/runtime-language/` | 整文件重写（TEXT/Binary 量级），不接受添补 |
| `setTransColor`/`setRegion` 选项串文法 | `sdk/src/window.ts` | Region/Color 对象模型落地 |
| sound 设备编号（native 1-based；另有两处与实现矛盾的 L2 期望待查） | `sound_endpoint.cpp` + `sound_tests.cpp` | 连测试疑案一起修，需 owner 排期 |
| `ClassNN` 查询键、各桥 1-based 线规 | `control.ts` 桥接口、`screen`/`ui` 桥 | 桥只做线规记录，不向应用层泄漏即视为合规 |
| `object-model.md` Round5 对照表、AHK 覆盖矩阵数字 | docs | 冻结的迁移参照/雷达；测试不再断言怪癖（已执行） |

## 9. 新增 API 检查表（AGENTS 八问的 SDK 侧浓缩）

1. 它属于哪一层，线程与所有权是什么？
2. 有独立于 AHK 的现代 TS 理由吗（写下来）？
3. 索引 0-based 了吗？“无”是 `null` 吗？越界/缺失语义对吗？
4. 名字是词不是码吗？字符串 DSL 出现了吗？
5. Node/平台标准里有现成形状吗（fs、DataView、Error）？有就对齐。
6. 同步/异步诚实吗？取消、deadline、Trace、权限、诊断齐了吗？
7. 测试断言的是 OS 可观察副作用吗（禁恒真/禁自测/禁吞错）？
8. 文档写的是现代用法吗？AHK 引用注明了身份（雷达/对手/参照）吗？

任一答案为“否”不得合入。
