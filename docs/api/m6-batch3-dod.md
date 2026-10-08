# M6 批3 DoD 统一模板（控件族）

适用：3a（16 构造器）、3b（控件方法）、3c（coverage 尾 8 项）。每一片独立
DoD，独立提交；native 先合、SDK/测试随后（测试需要真实现）。

## 切片模板（每片必含）

1. **范围**：本片的台账行（`objects.json` / `coverage.json` 精确名单），不多不少。
2. **真控件**：每个构造器/方法建出真实 HWND（对应 Win32 类名记录在案）。
3. **OS 读回**：断言 OS 可观察副作用，禁止“调用没抛即通过”——
   构造器：`GetClassName` + `IsWindowVisible`；
   ListView：`LVM_GETITEMCOUNT`；TreeView：`TVM_GETCOUNT`；
   StatusBar：`SB_GETPARTS`；Cue 类：`EM_GETCUEBANNER` 回读；
   Tab：选中态回读。读回失败 = 实现失败，不是环境抖动（fixture 自建自毁）。
4. **L5**：桌面系统级测试（真 Win32 + 自建 fixture + 清理），文件头 `Realism: L5`。
5. **台账翻项**：`objects.json`/`coverage.json` 状态翻转与本片同提交；
   executor/module 指针指向同一实现（3b 同名双入口不写两遍）。
6. **无新通道**：构造器走 `build_ctrl_object` 统一入口（先审计三处定义
   `gui_object.cpp:666/1485/2599` 的分工）；方法走既有 executor；事件走
   既有 Subscription 队列。不满足就拆片，不加通道。
7. **小步提交**：一波一 commit（建议粒度：List/Combo → Date/MonthCal →
   Slider/UpDown → Pic/Link/Hotkey/StatusBar/Tab 族）；提交信息注明台账行。

## 政策 verdict 模板（先文档，后代码；本批 3a 用）

verdict = `objects.json` 状态翻 `unsupported-by-policy` + `error` 栏写清
理由与替代路径。理由必须独立于 AHK（禁“AHK 就是这样”），引用三处之一：
capability 模型装不下、安全边界（重入/生命周期/裸句柄）、OS 不提供该语义。
`compatibilityTest` 保持 `missing`（未实现的方法无拒绝入口可测；背书 =
台账状态 + `checkObjectsLedger`，沿 §4.7 的 16 条先例）。

## 三片实例化
- **3a**：16 构造器（ComboBox、DateTime、DDL、DropDownList、Hotkey、Link、
  ListBox、ListView、MonthCal、Pic、Slider、StatusBar、Tab、Tab2、TreeView、
  UpDown）+ 3 verdict（AddActiveX、AddTab3、AddCustom，见 `gui-menu.md` §4.8）。
  `MenuBar` 留批4（依赖 Menu）。
- **3b**：ListView 11 + TreeView 12 + StatusBar 3 + Tab.UseTab +
  DateTime.SetFormat + Edit.SetCue + ComboBox.SetCue（~30 实现）；
  GuiControl 22 同名入口指针收敛到同一实现（`see ListView.Add`式引证）。
  SDK 门面新增一律 0-based（B 刀之后不许出现 1-based 新 API）。
- **3c**：`GuiFromHwnd`/`GuiCtrlFromHwnd`（先定 §4.7 政策分叉：只反查运行时
  自有 HWND 表，未知值 `target_gone` + 审计）+ `LoadPicture`/`IL_Create`/
  `IL_Add`/`IL_Destroy`（ImageList 不透明 id）；`MenuSelect`→批4，
  `MenuFromHandle`→批4 与 Menu 模型一并判。

## SDK 车道（已落地批2表面）

`sdk/src/gui.ts`：`Gui`（`create/add/addButton…/show/hide/destroy/submit/
onEvent/getPos`）与 `GuiControl`（同步 `name/type/classNN`，其余 Promise），
`tests/sdk/gui.test.ts`（L3 mock 桥）。3a 的 16 个构造器 native 落地后，
门面按同形状追加 `addComboBox…`（SDK 不超前于 native，不做 mock 戏）。
