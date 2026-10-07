# File, Directory, Drive and Environment API

状态：`implemented`（完整垂直）：`StorageService`（`engine/win32/src/storage.cpp`）、`storage.write` Action executor（`engine/win32/src/storage_executor.cpp`）、`rime:storage` 模块（`engine/win32/js/src/storage_module.cpp`）、SDK 门面与 `File` 类（`sdk/src/storage.ts`）、`Bootstrap` 接线与 `contracts/registry/actions.json` 行均已落地；三层 contract 测试：`tests/native/storage_tests.cpp`（`add_test NAME rime_win32_fs_service`）、`tests/js/storage_slice.cpp`（`quickjs_storage_slice`）、`tests/sdk/storage.test.ts`。

## API 清单

### Native service（同步；经模块或 executor 调用，JS 不能直接触达）

| 方法 | 作用 | capability（调用方负责） | AHK 对应 |
| --- | --- | --- | --- |
| `read_text(path, out)` | 整文件按会话编码解码为 UTF-8 | `filesystem.read` | `FileRead`（`functions.h:117`） |
| `read_bytes(path, out)` | 整文件原始字节，1 GiB 上限 | `filesystem.read` | `FileRead` 的 Raw 模式 |
| `stat(path, out)` | 大小/修改时间/属性位/是否目录 | `filesystem.read` | `FileGetSize`/`FileGetTime`/`FileGetAttrib`/`FileExist` 合并 |
| `list(path, out)` | 直接子项（`.`/`..` 永不出现） | `filesystem.read` | 无（AHK 无目录列举内建；差异条目） |
| `env_get(env name)` | 未设置读回 `""` | `filesystem.read` | `EnvGet`（`functions.h:94`） |
| `ini_read(path, section, key, out)` | 缺文件/缺节/缺键均为错误 | `filesystem.read` | `IniRead`（`functions.h:152`） |
| `drive_get(field, letter, out)` | 9 字段查询 | `filesystem.read` | `DriveGet*`（`functions.h:71-79`） |
| `read_shortcut(path, out)` | 解码 `.lnk`：target/工作目录/参数/图标，未设置字段读回 `""` | `filesystem.read` | `FileGetShortcut`（`functions.h:109`，实现在 `script_autoit.cpp:1184`） |
| `read_version(path, out)` | 文件版本资源 `M.m.b.r`；无版本资源读回 `""`（缺文件仍是错误） | `filesystem.read` | `FileGetVersion`（`functions.h:114`，实现在 `script_autoit.cpp:1377`） |
| `file_open`/`file_read`/`file_seek`/`file_stat`/`file_close` | 句柄族（服务持有 `HANDLE`，对外是单调 `uint64` id） | `r`→`filesystem.read`；`a`/`w`→`filesystem.write` | `FileOpen`（`script.cpp:61`）与 `File.*` |
| `append_text` / `write_text` | 追加 / 覆盖写 | `filesystem.write` | `FileAppend`（`functions.h:100`） |
| `file_copy`/`file_move`/`file_delete` | 文件复制/移动/删除 | `filesystem.write` | `FileCopy`/`FileMove`/`FileDelete` |
| `dir_create`/`dir_delete`/`dir_copy`/`dir_move` | 目录四操作 | `filesystem.write` | `Dir*`（`functions.h:61-65`） |
| `set_attrib` / `set_time` | 属性字母 `A R H S T N O` 增删 / 三时间戳 | `filesystem.write` | `FileSetAttrib`/`FileSetTime`（`functions.h:121-122`） |
| `recycle` / `recycle_empty` | 移入回收站 / 清空回收站 | `filesystem.write` | `FileRecycle`/`FileRecycleEmpty` |
| `make_shortcut` | `.lnk` 创建 | `filesystem.write` | `FileCreateShortcut`（`functions.h:102`） |
| `file_install` | 等价 `file_copy`（见"设计取舍"） | `filesystem.write` | `FileInstall`（`functions.h:115`） |
| `env_set` / `ini_write` / `ini_delete` | 环境变量 / INI 写删 | `filesystem.write` | `EnvSet`/`IniWrite`/`IniDelete` |
| `drive_set_label`/`drive_lock`/`drive_unlock`/`drive_eject`/`drive_retract` | 卷标与光驱状态 | `filesystem.write` | `DriveSetLabel`/`DriveLock`/`DriveUnlock`/`DriveEject`/`DriveRetract` |
| `handle_write(handle, bytes)` | 句柄当前位置写 | `filesystem.write` | `File.Write` |
| `encoding()` / `set_encoding(enc)` | 会话编码 7 种 | 无（服务状态） | `FileEncoding`（`functions.h:106`） |
| `download(url, path, cancel, deadline, out)` | WinHTTP 分块下载 | `filesystem.write` | `Download`（`functions.h:68`） |
| `select_file` / `select_dir` | 模态 `IFileDialog` | 无 capability；需 `set_ui_thread` | `FileSelect`（`functions.h:120`）/ `DirSelect`（`:66`） |
| `stop()` | 关闭全部残余句柄，返回数量，可重复 | — | 无（生命周期 API） |

### `storage.write` Action（executor，24 op）

Target 恒为 `{"kind": "storage", "id": "fs"}`；成功 `detail` 为 `storage write applied`，`value` 为 `{"op": <op>}`。每个 op 有显式字段白名单，多余字段一律拒绝。

| op | 必填 | 可选 | 服务调用 |
| --- | --- | --- | --- |
| `append` / `write` | `path`,`text` | — | `append_text` / `write_text` |
| `copy` / `move` / `install` / `dircopy` / `dirmove` | `src`,`dst` | `overwrite` | `file_copy` / `file_move` / `file_install` / `dir_copy` / `dir_move` |
| `delete` / `mkdir` / `recycle` | `path` | — | `file_delete` / `dir_create` / `recycle` |
| `rmdir` | `path` | `recursive` | `dir_delete` |
| `setAttrib` | `path` | `add`,`remove`（两者全空 = 拒绝） | `set_attrib` |
| `setTime` | `path`,`which`,`unixMs` | — | `set_time` |
| `recycleEmpty` | — | `root`（省略 = 所有盘符） | `recycle_empty` |
| `shortcut` | `path`,`target` | `args`,`workdir`,`icon`,`description` | `make_shortcut` |
| `env` | `name`,`value` | — | `env_set` |
| `iniWrite` | `path`,`section`,`key`,`value` | — | `ini_write` |
| `iniDelete` | `path`,`section` | `key`（省略 = 删整节） | `ini_delete` |
| `driveLabel` | `letter`,`label` | — | `drive_set_label` |
| `driveLock` / `driveUnlock` / `driveEject` / `driveRetract` | `letter` | — | 同名方法 |
| `handleWrite` | `handle`,`data` | — | `handle_write`（`data` 为 UTF-8 字符串或 `0..255` 数组） |

`read`/`stat`/`list`/`drive_get`/`env_get`/`ini_read`/`read_shortcut`/`read_version`/`download`/两个选择器**没有 op**：它们由 `rime:storage` 模块直接落到 service——读族在 worker 体内校验 `filesystem.read`，`download` 校验 `filesystem.write`，选择器还需 `set_ui_thread`；只有写家族才经 `storage.write` 分派。`handle` 必须是 `[1, 9223372036854775807]` 整数，`unixMs` 必须在 `[0, 253402300799999]`。

## 源码证据

- `functions.h:61-68`（`Dir*`、`Download`）、`:70-83`（`Drive*`）、`:94-95`（`Env*`）、`:100-122`（`File*`、`FileEncoding`、`FileSelect`、`FileSet*`）、`:151-153`（`Ini*`）、`:280`（`SplitPath`）；实现分散在 `source/lib/file.cpp`、`source/lib/drive.cpp`、`source/lib/env.cpp`；`FileOpen` 在 `source/script.cpp:61`；`Download` 实现（`InternetReadFileExA` 循环）在 `source/script_autoit.cpp:938`、`:1009`；`FileGetShortcut` 在 `source/script_autoit.cpp:1184`、`FileGetVersion` 在 `:1377`；`FileSelect` 实现在 `source/script2.cpp:1511`，`DirSelect` 在 `source/script_autoit.cpp:1065`（注册行 `functions.h:120`、`:66`）；属性字母格式化 `FileAttribToStr` 在 `source/util.cpp:1615`。
- Rime 实现：`engine/win32/include/rime/win32/storage.hpp`、`storage_executor.hpp`；`engine/win32/src/storage.cpp`（service 与全部 Win32/COM 细节）、`engine/win32/src/storage_executor.cpp`（`storage.write`）。
- Win32 表面：`CreateFileW`/`ReadFile`/`WriteFile`/`GetFileSizeEx`/`CopyFileW`/`MoveFileExW`/`DeleteFileW`/`RemoveDirectoryW`/`SetFileAttributesW`/`SetFileTime`、`GetDriveTypeW`/`GetDiskFreeSpaceExW`/`GetVolumeInformationW`、`GetEnvironmentVariableW`/`SetEnvironmentVariableW`/`GetPrivateProfileStringW`/`WritePrivateProfileStringW`/`GetPrivateProfileSectionNamesW`、`SHFileOperationW`（回收站）、`IShellLinkW`+`IPersistFile`（`.lnk` 创建与读取共用）、`GetFileVersionInfoSizeW`/`GetFileVersionInfoW`/`VerQueryValueW`（版本资源，读 `VS_FIXEDFILEINFO` 拼 `M.m.b.r`）、`IFileOpenDialog`/`IFileDialog`（选择器）、`WinHttp*`（下载）。链接在 `engine/win32/CMakeLists.txt`：`winhttp shell32 ole32 uuid version`（`version` 为版本资源新增，其余加在既有 `advapi32` 之后）。
- 测试：`tests/native/storage_tests.cpp`（`rime_fs_tests`，链接 `rime_win32 rime_core rime_action rime_test_support` 与 `ws2_32`——回环下载夹具需要 winsock；`ole32` 让夹具自己 `IShellLinkW` 造 `.lnk`）、`tests/js/storage_slice.cpp`（`quickjs_storage_slice`，同样链接 `ole32`）。

## TS 类型

已落地（`engine/win32/js/src/storage_module.cpp` 导出 `rime:storage`，门面与 `File` 类在 `sdk/src/storage.ts`，模块声明在 `sdk/src/modules.d.ts`）。导出形状：

```ts
declare module "rime:storage" {
  const storage: {
    read(path: string, options?): Promise<string>;
    stat(path: string): Promise<FileInfo>;
    shortcut(path: string, options?): Promise<{ target: string; workingDir: string; args: string; icon: string }>;
    version(path: string, options?): Promise<string>;
    list(path: string): Promise<DirEntry[]>;
    write(payload: StorageWritePayload, options?): Promise<{ op: string }>;
    envGet(name: string): Promise<string>;
    iniRead(path, section, key): Promise<string>;
    driveGet(field, letter?): Promise<DriveInfo>;
    download(url, path, options?): Promise<{ bytes: number }>;
    selectFile(options?): Promise<string[]>;
    selectDir(options?): Promise<string>;
    setEncoding(enc: string): Promise<string>;
  };
  export { storage };
}
```

arity 与 `NativeActionOptions` 形态已按 `clipboard`/`registry` 门面的既有形态定稿（`setEncoding`/`encoding` 原生同步，SDK 门面因模块动态加载而 async）。单字段 wire 结果在门面层解包（`read`←`{text}`、`envGet`←`{value}`、`download`←`{bytes}`、`selectFile`←`{paths}`、`version`←`{version}`），多字段保持对象（`stat`、`write`、`shortcut` 四字段）；模块侧原名只有 `readText`/`shortcut`/`version` 与门面不同，其余同名。

## 底层实现

- **句柄表**：`std::unordered_map<uint64, void*>` + `mutex`，id 从 1 单调递增；`HANDLE` 不离开 service（头文件用 `void*` 避免引入 `windows.h`）。`file_read`/`handle_write` **在持锁状态下**完成 `ReadFile`/`WriteFile`（并发读写同一句柄顺序确定）。陈旧 id → `InvalidState`，不崩溃。
- **模式**：`"r"` = `OPEN_EXISTING`；`"a"` = `OPEN_ALWAYS` + 定位到尾；`"w"` = `CREATE_ALWAYS`。打开目录在 Win32 层即 `ERROR_ACCESS_DENIED`（5），因此 `cannot open a directory: ` 分支实践中不可达——保留它只为语义完整。
- **编码**：会话编码 `utf-8`(默认)/`utf-8-bom`/`utf-16`/`utf-16-be`/`cp0`/`cp1252`/`latin1`，`set_encoding` 大小写不敏感、非法值 `invalid_contract`。读时 BOM 自动识别（EF BB BF / FF FE / FE FF 覆盖端序）；写时 BOM 只在文件起点写出（`append_text` 仅当 `size == 0`，`write_text` 每次），追加永不把 BOM 插进已有内容。代码页编码的不可表示字符用 `?` 替换，不截断。
- **上限**：整文件读 1 GiB（`read_bytes`），句柄单次读 64 MiB，INI 值 1 MiB，环境变量 32 KiB（`GetEnvironmentVariableW` 探测），下载块 64 KiB。
- **目录递归**：`dir_delete(recursive)` 用 `delete_tree`（忽略已消失条目，首错继续收集），非递归且非空 → `directory is not empty`；`dir_copy`/`dir_move` 先验源（存在且为目录）再建目标——源校验失败不会留下半成品目录。
- **drive 字段**：`list` 用 `GetDriveTypeW` 扫 `A:`–`Z:`（无 filter 时排除 `DRIVE_NO_ROOT_DIR`；有 filter 精确匹配）；`status`/`statuscd` 都由 `GetDiskFreeSpaceW` 成败推出 `Ready|Invalid|NotReady|ReadOnly|Unknown`；`capacity_percent = (total - free) * 100 / total`（整数截断，`total == 0` 记 0），`spacefree` 只填 `free_bytes`（`total_bytes = 0`、`capacity_percent = -1`）。
- **INI**：路径先 `GetFullPathNameW` 规范化（profile API 依赖绝对路径）；缺键用哨兵 `\x01RIME-INI-MISSING` 前缀比较区分"缺失"与"空值"（真实值以该 20 字节前缀开头会被误判为缺失——已记录的边界）。
- **下载**：`WinHttpOpen` 起，回环 host 走 `WINHTTP_ACCESS_TYPE_NO_PROXY`（本地服务不受机器代理配置影响），其余 `AUTOMATIC_PROXY`；只接受 `http://`/`https://` 且必须有 host；非 2xx → `download failed with HTTP status <n>: <url>`；**任何**目的地已创建之后的失败（含取消/超时/断流）都删除半成品文件。
- **选择器**：经 `UiThread::call(fn, 30s, cancel)` 排队；取消在被泵认领前生效，**认领后模态对话框跑完不可打断**；无 `set_ui_thread` → `InvalidState`（headless 永不弹窗）。filter 接受 `Name (*.patterns)` 或纯模式列表，空 filter 拒绝。

## 线程和资源所有权

- service 全部方法同步、可在 worker lane 调用；不碰 HWND/COM 之外的 UI 资源（选择器与快捷方式的 COM 在调用线程 `CoInitializeEx`/`CoUninitialize` 成对自管，不跨线程传 COM 指针）。
- `handles_mutex_` 保护句柄表并跨 I/O 持有；`encoding_mutex_` 只在取/换编码时短暂持有，**不跨文件 I/O**。
- `ui_thread_` 是裸指针借用，由 `Bootstrap` 的服务生命周期注入/置空；service 自身不拥有 pump。
- 选择器的 `HWND` owner 来自 `ui->message_window()`，只在 UI Thread 使用；结果以 `std::string`（UTF-8）回传，不传 `IShellItem`。
- `stop()` 关闭全部残余句柄、返回数量、可重复调用；`Bootstrap::stop()` 与模块卸载都用它。

## 异步/取消

- 本域没有独立队列：`storage.write` 经 Dispatcher 有界队列 → Kernel（lane→type→target→取消预检→payload→执行→取消复检）→ 结果；executor 自身不设 timeout（service 调用同步有界），超时由 Kernel 的入口/提交 deadline 检查承担。
- **取消点**：executor 执行前（`action was cancelled before execution`）与执行后（`... after execution`）；`download()` 在块间检查 cancel 与 `deadline_unix_ms`（deadline → `Timeout`）；选择器在排队前与泵认领时检查（认领后不可取消）。
- **重入**：选择器无重入保护——同一 service 的第二次调用会再次排队（依赖 UI 泵串行执行），这是记录在案的限制，不是并发保证。
- 队列满载/事件合并等调度策略由集中调度层承担，storage 不自设节流。

## 权限

- `filesystem.write`：由 Kernel 在 executor 前校验（executor 不再自查），拒绝 → `capability_denied` 且不进入 service（测试断言目标文件未被创建）。executor 也登记在 `Lane::Worker` 上，错 lane 拒绝。
- `filesystem.read`：为读路径声明的 capability，**当前无 checked 点**——`rime:storage` 模块落地时必须在模块工作体内 `kernel->allows("filesystem.read")`（对照 `registry_module.cpp:125`）。
- 选择器与 `encoding` 不设 capability（选择器由 headless `InvalidState` 兜底；编码只是服务实例状态）。

## 错误

`rime::core::Error::Code` → JS `ActionError.code`：`invalid_contract`、`execution_failed`、`target_gone`、`invalid_state`、`cancelled`、`timeout`。稳定文案（`tests/native/storage_tests.cpp` 逐字断言，改动即契约变更）：

| Code | 文案 |
| --- | --- |
| `invalid_contract` | `storage <what> must not be empty`、`storage <what> is not valid UTF-8: <text>`、`storage <what> must not contain a NUL character`（`what` = `path`/`source`/`destination`/`name`/…）、`file mode must be "r", "a" or "w"`、`file is larger than the 1 GiB read limit: <path>`、`file read count exceeds the 64 MiB limit`、`file seek whence must be 0, 1 or 2`、`encoding must be one of utf-8, utf-8-bom, utf-16, utf-16-be, cp0, cp1252, latin1`、`setAttrib requires at least one attribute letter to add or remove`、`unknown attribute letter: <c>`、`setTime which must be one of mtime, atime, ctime`、`setTime unix_ms must be between 0 and 253402300799999`、`drive must not be empty`、`drive must be a letter, "X:" or "X:\": <text>`、`drive field must be one of type, list, serial, spacefree, status, statuscd, filesystem, label, capacity`、`drive list type must be CDROM, Removable, Fixed, Network, RAMDisk or Unknown`、`environment variable name must not be empty`、`environment variable name must not contain '=': <name>`、`environment variable value is not valid UTF-8`、`drive label is not valid UTF-8`、`ini section must not be empty`、`ini key must not be empty`、`shortcut path must not be empty`、`shortcut target must not be empty`、`download URL must start with http:// or https://: <url>`、`download URL is not valid UTF-8: <url>`、`download URL could not be parsed: <url>`、`download URL must be http or https: <url>`、`download URL must include a host: <url>`、`file selector filter must not be empty`、`file selector filter must be "Name (*.patterns)" or a pattern list`、`file selector filter is not valid UTF-8: <filter>`、`file selector default name must not contain a path separator` |
| `execution_failed` | `file not found: <path>`、`path not found: <path>`、`<op> failed for <path> (Win32 error <n>)`、`storage <op> failed (Win32 error <n>)`、`storage <op> failed (HRESULT 0x%08X)`、`path is a directory: <path>`、`path is not a directory: <path>`、`path already exists as a file: <path>`、`destination already exists: <dst>`、`destination already exists as a file: <dst>`、`directory is not empty: <path>`、`file seek moved before the start of the file`、`cannot open a directory: <path>`、`ini section not found: <section>`、`ini key not found: <section>/<key>`、`ini value is too large`、`environment variable is too large: <name>`、`recycle failed for <path> (shell error <n>)`、`selector returned an unreadable path`、`download failed with HTTP status <n>: <url>` |
| `target_gone` | `drive not found: <X>`（裸盘符不带冒号） |
| `invalid_state` | `file handle is not open`、`storage selector has no UI thread` |
| `cancelled` | `selection was cancelled`（用户在对话框内取消）、`selection was cancelled before it started`、`download was cancelled before it started`、`download was cancelled` |
| `timeout` | `download exceeded its deadline` |

executor 自身的 payload 错误统一加前缀 `storage.write payload `：`must be a JSON object`、`requires a non-empty string op`、`does not support op: <op>`、`op <op> does not accept field: <f>`、`requires a string <field>`、`requires a non-empty string <field>`、`<field> must be a boolean`、`requires a string path`、`requires a data field`、`data must be a string or an array of 0..255`、`data elements must be integers 0..255`、`requires an integer handle between 1 and 9223372036854775807`、`which must be one of mtime, atime, ctime`、`unixMs must be an integer between 0 and 253402300799999`、`setAttrib requires at least one of add or remove`。非前缀的 executor 文案：`unsupported action type: <t>`、`storage.write requires target kind 'storage', got: <k>`、`storage.write target id must be 'fs'`；取消为 `action was cancelled before execution` / `after execution`。

## Trace

只有 `storage.write` 进 Trace（Kernel 的 `ActionStarted`/`ActionFinished`，含被 executor/payload 拒绝的 Action——它们以 `invalid_contract` 结束仍计两次）。service 直读方法与选择器是观察/状态路径，**不产生 Trace 条目**（文档化豁免）。`tests/js/storage_slice.cpp` 以 `InMemoryTrace` 断言本域计数（12 Accepted/Started/Finished、被 capability 拒绝的运行时 `Started == 0`）。

## 设计取舍

- **`FileInstall` = 复制**：AHK 在编译期把源文件嵌入脚本二进制；本 runtime 无编译步骤，op 语义改为 `file_copy` 并在此入档，而不是返回"不支持"。
- **`statuscd` 镜像 `status`**：AHK 经 MCI/winmm 查询光驱就绪状态，本 runtime 刻意不链接 `winmm`；`statuscd` 返回与 `status` 相同的推导值，差异在注释与本文件双重声明，覆盖矩阵需要标注。
- **属性字母 `A R H S T N O`**：按 AHK `FileAttribToStr` 的字母表（`source/util.cpp:1615`），`set_attrib` 只接受这 7 个字母的增删，其余 → `unknown attribute letter`。
- **INI 哨兵前缀**：`GetPrivateProfileStringW` 无法区分缺键与空值，用 `\x01RIME-INI-MISSING` 默认值比较；真实值以该前缀开头会误判为缺失——接受此边界以换取"缺失是错误、空值是值"的区分。
- **`capacity_percent` 整数截断**：`used * 100 / total`，不四舍五入、不用浮点——结果必须可跨平台复算。
- **下载回环免代理**：`127.0.0.1` 不查系统代理，本地夹具与机器代理配置解耦；非回环仍走系统自动代理。
- **选择器不设重入保护**：模态对话框在 UI 泵上天然串行，再加一层锁会掩盖"谁在弹窗"的诊断；代价是 headless 嵌入必须走 `InvalidState`（已在测试覆盖）。
- **`list` 无 AHK 对应**：AHK v2 没有目录列举内建，`list` 是本域新增能力；它保持与其他读路径相同的 `filesystem.read` 门禁。
- **`read_shortcut` 只回四个字段（已知缺口）**：AHK `FileGetShortcut` 还输出 `Description`/`IconNum`/`RunState`（分别来自 `IShellLinkW::GetDescription`、`GetIconLocation` 的序号、`GetShowCmd`），本实现暂未暴露，`icon` 只有路径没有序号；补齐要同步 `ShortcutInfo`、`rime:storage` 模块 JSON、门面类型与三层测试。
- **`read_shortcut` 用 `SLGP_RAWPATH`**：AHK 取 target 用 `SLGP_UNCPRIORITY`，那条路会走 shell 文件夹解析、可能触发网络或杀软回调（在 `windows.storage.dll` 里观察到过）；本实现读链接中存的原样路径，理由写在 `engine/win32/src/storage.cpp` 的注释里。
- **`read_version` 路径必填**：AHK `FileGetVersion` 的 `Path` 可选、缺省指"当前脚本"；Rime 没有脚本句柄，因此路径必填（与 `process.edit` 的 `Edit` 同一理由，见 `docs/api/process-shell.md`）。
- **句柄 I/O 持锁**：读写同一句柄的顺序由 service 保证确定，换取"两个 worker 并发写同一文件"时的可预期结果；锁粒度是整个表，接受它作为 v1 的简单性换确定性。

## contract / native / stress 测试

- `tests/native/storage_tests.cpp`（`rime_fs_tests` → `add_test NAME rime_win32_fs_service`，L5）：fixture `%TEMP%\rime_storage_test_<pid>`，**捕获 → 清理 → 断言** 三段（`assert` 中止会跳过析构，故先清理后断言）。覆盖：
  - 读：`read_text` UTF-8/CJK/编码切换回读、BOM 读入、坏编码拒绝、目录当文本读（Win32 error 5）、`read_bytes` 上限前正常路径、`stat`/`list`/`env_get`（未设置读回 `""`）/`ini_read` 缺文件缺节缺键三错误、`drive_get` 9 字段逐项 **raw Win32 复核**（`GetDriveTypeW`/`GetVolumeInformationW`/`GetDiskFreeSpaceExW` 独立读回）与不存在盘符的 `target_gone`、`read_shortcut`（夹具自造 `.lnk` 解码 target/工作目录/参数 + 损坏链接与不存在文件两路拒绝）、`read_version`（系统二进制读到带点的 `M.m.b.r`、纯文本文件读回 `""`、缺文件报错）。
  - `.lnk` 与版本资源在 QuickJS 竖切里再走一遍：`tests/js/storage_slice.cpp` 自建 `IShellLinkW` 夹具 → `storage.shortcut()`/`storage.version()` 断言字段与 `M.m.b.r` 形状，`storage.shortcut(42)`/`storage.version()` 断言同步 `TypeError`；SDK 门面层由 `tests/sdk/storage.test.ts` 断言解包（`version`→`string`、`shortcut`→四字段对象）与 `ActionError` 升级。
  - 句柄：`r`/`a`/`w` 三模式、追加落尾、`file_seek` 三 whence、`file_stat` 位置与长度、`file_close` 幂等、陈旧 id 的 `invalid_state`。
  - 写：append/write/copy/move/delete/mkdir/rmdir（递归与非递归、非空拒绝）/dircopy/dirmove、目标已存在家族文案、`set_attrib` 字母增删 + raw `GetFileAttributesW` 复核、`set_time` 三个 which + raw `GetFileTime` 复核（`ManualClock` 不适用于文件时间戳，这里用 OS 观察值）、`env_set`/`ini_write`/`ini_delete`（键与节两级）+ raw profile API 复核、`file_install` 内容逐字节、`make_shortcut` `.lnk` 魔数复核、`recycle`（`SHFileOperationW` 可观察副作用，夹具内清理）。
  - 下载：进程内 winsock 回环服务器三连接（200 / 404 / deadline），断言成功落盘、HTTP 非 2xx 文案、deadline → `timeout`（半成品被删）、坏 URL 协议拒绝、`download()` 取消预检。
  - 选择器：无 UI 泵 → `invalid_state`、预取消 → `cancelled`（**不弹任何对话框**——真实对话框路径只在人工/集成环境执行）。
  - 生命周期：`stop()` 关闭残余句柄并可重复调用。
  - executor：经真实 Kernel 的成功往返（`detail == "storage write applied"` + `value.op` + 逐 op OS 观察）、空 capability 拒绝且目标文件未创建、12 项 payload 契约违规逐字文案、错 kind/错 id/错 type、预取消。
- **刻意不测**（不是缺口就是环境约束，逐条入档）：`recycle_empty`（破坏性，永不自动执行）、真实选择器弹窗（headless 断言替代）、真实盘符的 lock/unlock/eject/retract/卷标写（只测不存在盘符的拒绝与契约文案）、1 GiB 整读上限与 64 MiB 句柄读上限的正例（需要 >1 GiB 夹具）、`cannot open a directory` 分支（被 access denied 掩盖）、代理行为（回环免代理之外不注入系统代理）。
- 三层测试齐备：`tests/native/storage_tests.cpp`（native contract）、`tests/js/storage_slice.cpp`（生产装配 QuickJS 竖切，含 Trace 计数与第二运行时 capability 门禁）、`tests/sdk/storage.test.ts`（`File` 31 成员 + mock 字节后端）；压力路径由通用调度测试承载。

## 实现状态

`implemented`（全链已落地，2026-10-05 集成收口）：

1. `contracts/registry/actions.json`：action `storage.write` 与 capability `filesystem.read`/`filesystem.write` 行（`matrix:check` 扫描 `storage_executor.cpp` 的 dotted 字面量，`register_executors` 已有同名 literal）。
2. `engine/win32/js/src/bootstrap.cpp`：`storage_service_`/`storage_binding_`、`register_storage_module`、`kernel_.register_executor("storage.write", ...)`、`production_capabilities()` 含 `filesystem.read`/`filesystem.write`、`start()` 在 window 服务起来后 `set_ui_thread(&window_service_.ui())`，`stop()` 先摘泵再 `storage_service_.stop()` 清扫残余句柄。
3. `engine/win32/js/src/storage_module.cpp` + `sdk/src/storage.ts` + `sdk/src/modules.d.ts` 的 `declare module "rime:storage"`：读路径在 worker 体内校验 `filesystem.read`，`storage.write` 经 Action（capability 由 kernel 查）。
4. `docs/api/compatibility-matrix.md`/`coverage.json` 逐函数回填：43 项翻 `implemented`；`FileGetShortcut`/`FileGetVersion` 在 `read_shortcut`/`read_version` 落地后同样翻 `implemented`（`contractTest` = `tests/native/storage_tests.cpp,tests/js/storage_slice.cpp`）。
5. 构建登记：`engine/win32/CMakeLists.txt`（`src/storage.cpp`、`src/storage_executor.cpp`、`PRIVATE winhttp shell32 ole32 uuid version`——`version` 是版本资源的 `GetFileVersionInfo*`/`VerQueryValueW`）、`tests/native/CMakeLists.txt`（`rime_fs_tests` → `add_test NAME rime_win32_fs_service`，链接 `ws2_32 ole32`）、`engine/win32/js/CMakeLists.txt`（`src/storage_module.cpp`）、`tests/js/CMakeLists.txt`（`rime_storage_slice` → `quickjs_storage_slice`，链接 `ole32`）。
