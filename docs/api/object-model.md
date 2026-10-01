# Object、GUI、COM 与宿主对象模型

状态：`contract-only`，尚未实现。

## TS 边界

Runtime 不把 AHK 的 C++ 对象、`IUnknown*`、`HMENU`、`HGLOBAL`、缓冲区裸指针或内部对象地址交给 QuickJS。上层得到的是 Runtime-owned opaque ID、不可变快照和显式 `Subscription`。拥有状态的对象只能在所属 lane 操作。

## AHK 对象到 TS 的映射

| AHK 类型 | TS 方向 | 所属 lane | 关键问题 |
|---|---|---|---|
| Object/Array/Map/Func | 原生 TS 对象、数组、Map、函数 | JS | 兼容层需定义 AHK 的索引、ByRef、枚举和异常差异 |
| Buffer | `BufferRef` + `Uint8Array` 拷贝 | Worker/JS | 不暴露地址；跨线程只能传复制或受控共享快照 |
| Gui/GuiCtrl | `GuiRef`/`ControlRef` | UI | HWND 只在 UI lane；事件回调回到 JS scheduler |
| Menu/MenuBar | `MenuRef` | UI | HMENU 不出 native；显示是可取消的 UI action |
| InputHook | `InputSubscription` | Hook/UI | Stop、Wait、EndReason、EndKey 和回调计数必须可观察 |
| File | `FileRef` | IO | HANDLE/FILE* 不出 JS；close 和 runtime shutdown 必须确定性执行 |
| ComObject | 受权限的 typed facade | Automation MTA | COM apartment、VARIANT、SAFEARRAY 和事件连接不可跨线程裸传 |
| Host Script/Funcs/Vars/Labels | 序列化检查描述 | JS Host | 检查不能触发脚本；执行必须显式提交 Action |

## GUI/菜单成员矩阵要求

`GuiType::sMembers`、`GuiControlType::sMembers*`、`UserMenu::sMembers`、`FileObject::sMembers`、`InputObject::sMembers` 和 `ComObject::s*Members` 必须逐个提取成员名、参数、返回值、源码实现和对象生命周期，另建 `objects.json`。仅列出 `Gui*` 函数不足以覆盖这些方法。
