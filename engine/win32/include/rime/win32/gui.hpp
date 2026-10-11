#pragma once

#include "rime/core/cancellation.hpp"
#include "rime/core/json.hpp"
#include "rime/core/types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rime::win32 {

class UiThread;

// Win32 GUI surface of M6 batch 1: AHK MsgBox / InputBox (modal dialogs),
// ToolTip, TraySetIcon and TrayTip (script-owned tray state). Every UI-thread
// object - dialog, tooltip window, NOTIFYICONDATA - is owned and touched on
// set_ui_thread()'s pump only (AGENTS: HWND work never leaves the UI Thread);
// public methods marshal through UiThread::call, so they are callable from a
// worker body. The dialogs run as a Win32 modal dialog on that pump - the
// same bounded shape StorageService::select_file uses: the queued phase is
// bounded by the deadline, and once the pump claims the task the modal runs
// to completion (an already-open dialog cannot be interrupted; the caller's
// own timeout closes it, see msg_box()). Nothing here builds an Action.
//
// Semantics mirror AutoHotkey v2 (source citations inline): button names,
// the "Timeout" word, the X/ESC rules and the tray rules are AHK's, not
// inventions; deviations are called out where they exist.
class GuiService {
 public:
  GuiService();
  ~GuiService();
  GuiService(const GuiService&) = delete;
  GuiService& operator=(const GuiService&) = delete;

  // ---- MsgBox (script2.cpp:1022-1058, window.cpp:932-1091, docs MsgBox) ----

  struct MsgBoxSpec {
    std::string text;
    std::string title;
    // AHK Group #1 button sets: 0 OK, 1 OKCancel, 2 AbortRetryIgnore,
    // 3 YesNoCancel, 4 YesNo, 5 RetryCancel, 6 CancelTryAgainContinue.
    int buttons{0};
    // MB_ICON* value or 0 (no icon): 0x10 error, 0x20 question, 0x30 warning,
    // 0x40 information (script2.cpp:968-978 Iconx/Icon?/Icon!/Iconi).
    unsigned int icon{0};
    // 1-based default button (script2.cpp:979-986 Default1..Default4).
    int default_index{1};
    // AHK T option in seconds; 0 = no timeout. Clamped the way window.cpp
    // clamps it (>2147483s saturates; a negative value becomes 0.1s).
    double timeout_seconds{0};
  };

  // Runs the dialog on the UI pump and returns the pressed button's AHK name
  // ("OK", "Yes", "No", "Cancel", "Abort", "Retry", "Ignore", "TryAgain",
  // "Continue") or "Timeout". The X/ESC rules are the OS's own (docs
  // MsgBox): OK-only -> X is OK; a Cancel button present -> X/ESC is Cancel;
  // otherwise X is disabled and ESC does nothing. InvalidContract when the
  // spec's button set is out of 0..6. InvalidState when no pump is attached.
  rime::core::Error msg_box(const MsgBoxSpec& spec, std::string& result_out,
                            std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);

  // ---- InputBox (lib/InputBox.cpp:26-208, docs InputBox) ----

  struct InputBoxSpec {
    std::string prompt;
    std::string title;
    std::string default_value;
    // Password masking (InputBox.cpp:37-48): '*' when set.
    bool password{false};
    // 0 = default dialog size; x/y unset = centered on the cursor's monitor.
    int width{0};
    int height{0};
    std::optional<int> x;
    std::optional<int> y;
    // AHK T option; 0 = none (same clamp as msg_box).
    double timeout_seconds{0};
  };

  struct InputBoxResult {
    std::string value;  // the edit box contents at close, even on Cancel/Timeout
    std::string result; // "OK", "Cancel" or "Timeout" (InputBox.cpp:166-173)
  };

  rime::core::Error input_box(const InputBoxSpec& spec, InputBoxResult& out,
                              std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);

  // ---- ToolTip (script2.cpp:1084-1260, docs ToolTip) ----

  // Shows/updates tooltip `which` (1..20; InvalidContract outside), or
  // destroys it when `text` is blank (script2.cpp:1096-1105). Omitted x/y
  // (nullopt) default to cursor + 16,16 (script2.cpp:1115-1121); given
  // coordinates are screen pixels, clamped inside the monitor work area the
  // way AHK clamps them. Returns no value: AHK's HWND return is a raw handle
  // and this runtime never exposes one (gui-menu.md §0.4).
  rime::core::Error tool_tip(const std::string& text, std::optional<int> x, std::optional<int> y,
                             int which, std::int64_t deadline_unix_ms,
                             rime::core::CancellationToken cancel);

  // ---- TraySetIcon (script.cpp:872-960, docs TraySetIcon) ----

  // Sets the process tray icon, adding the icon first if none exists. An
  // empty file or "*" restores the standard icon (script.cpp:890-903);
  // icon_number 0 means 1 (script.cpp:956-957). freeze records AHK's frozen
  // flag; batch 1 has no automatic icon updates, so it changes nothing else.
  rime::core::Error tray_set_icon(const std::string& file, int icon_number, bool freeze,
                                  bool freeze_given, std::int64_t deadline_unix_ms,
                                  rime::core::CancellationToken cancel);

  // ---- TrayTip (script2.cpp:85-135, docs TrayTip) ----

  // Shows a balloon on the tray icon (NIF_INFO). info_flags is AHK's
  // dwInfoFlags: 0x10 error, 0x20 warning, 0x40 info; mute adds
  // NIIF_NOSOUND. Empty text with a title shows a title-only balloon
  // (script2.cpp:124-126); both empty removes the notification. Like AHK
  // this never fails once the tray icon exists - only a pump/cancel problem
  // can reject.
  rime::core::Error tray_tip(const std::string& text, const std::string& title,
                             unsigned int info_flags, bool mute, std::int64_t deadline_unix_ms,
                             rime::core::CancellationToken cancel);

  // Borrows the runtime's single UI pump (never a second one). Attach after
  // the pump is running; detach on shutdown after stop().
  void set_ui_thread(UiThread* ui);

  // FromHwnd reverse lookup (batch 3e): maps an automation window/control id
  // to a live handle WITHOUT leaving native code. Injected by the host
  // (WindowService::peek_hwnd); both services share the UI pump, so the
  // resolver runs on the calling pump thread. Empty = no reverse lookup
  // (every id misses with TargetGone).
  using HwndResolver = std::function<void*(std::uint64_t)>;
  void set_hwnd_resolver(HwndResolver resolver);

  // Batch-2 event pipe: called on the JS thread with the host's queue push
  // (channel id, serialized payload). Must be set before the first Gui is
  // materialized - records take a copy at creation (gui-menu.md §4.3);
  // re-assignment on a later __New re-points later records at the new queue.
  using EventSink = std::function<void(std::uint64_t, std::string)>;
  void set_event_sink(EventSink sink);

  // Removes the tray icon and destroys the tooltip windows on the pump.
  // Repeatable; a detached or dead pump makes it a no-op (the pump destroys
  // its windows when the UI thread exits).
  rime::core::Error stop();

  // ---- Gui object family (M6 batch 2, gui-menu.md §4) ----
  //
  // The service owns every script-created Gui window on the pump. JS-thread
  // state (callbacks, control object refs) lives in the module binding; this
  // side only ever sees stable ids plus plain-value specs, so nothing with
  // ownership crosses threads. Parsers are pure string->struct and run on the
  // JS thread (synchronous ValueError/TypeError before any pump work); every
  // operation below is worker-lane and marshals through UiThread::call.

  enum class GuiControlKind : int {
    Button,
    CheckBox,
    Edit,
    GroupBox,
    Picture,
    Progress,
    Radio,
    Text,
    // M6 batch 3a: list family. DropDownList is its own kind (CBS_DROPDOWNLIST
    // vs ComboBox CBS_DROPDOWN); AddDDL is a ledger alias for it, not a kind.
    ListBox,
    ComboBox,
    DropDownList,
    // M6 batch 3a wave 2: date and slider families. No aliases here.
    DateTime,
    MonthCal,
    Slider,
    UpDown,
    // M6 batch 3a wave 3: rich family. AddTab2 aliases Tab and AddPic
    // aliases Picture (AHK resolves both early); no extra kinds for them.
    Tab,
    Link,
    Hotkey,
    ListView,
    TreeView,
    StatusBar
  };

  // Event-interest bits shared with the JS-side registration table
  // (engine/win32/js/src/ui_module.cpp mirrors them into GuiEventInterest).
  enum GuiEventBit : std::uint32_t {
    kGuiEventBitClose = 1u << 0,
    kGuiEventBitResize = 1u << 1,
    // Batch 3e: Escape (VK_ESCAPE keydown), ContextMenu (WM_CONTEXTMENU),
    // DropFiles (WM_DROPFILES, DragAcceptFiles while interested).
    kGuiEventBitEscape = 1u << 2,
    kGuiEventBitContextMenu = 1u << 3,
    kGuiEventBitDropFiles = 1u << 4,
  };
  enum CtrlEventBit : std::uint32_t {
    kCtrlEventBitClick = 1u << 0,
    kCtrlEventBitChange = 1u << 1,
    // Batch 3e: ItemSelect (ListView LVN_ITEMCHANGED selection flip,
    // TreeView TVN_SELCHANGED). ListBox/ComboBox selection stays Change.
    kCtrlEventBitItemSelect = 1u << 2,
  };

  // Parsed subset of AHK Gui options (script_gui.cpp:4974+, gui-menu.md §4.4).
  struct GuiStyleOptions {
    // Win32 style/ex-style values computed against the AHK default
    // (script_gui.h:519); what `__New` passes to CreateWindowEx.
    std::uint32_t style{0};
    std::uint32_t ex_style{0};
    bool own_dialogs{false};
    // Per-option mention masks for Gui Opt: Opt applies exactly the bits the
    // caller named (add/remove), computed by the parser from +/-. An
    // absolute-from-default value alone cannot express "-ToolWindow" against
    // a zero ex-style baseline, so the masks carry the delta.
    std::uint32_t style_add{0};
    std::uint32_t style_remove{0};
    std::uint32_t ex_add{0};
    std::uint32_t ex_remove{0};
  };

  // Which events the pump should bother pushing to the JS channel. Mirrored
  // wholesale from JS-side state on every OnEvent/OnMessage/OnCommand/
  // OnNotify registration (gui-menu.md §4.3); keys in ctrl_* are our stable
  // control ids.
  struct GuiEventInterest {
    // Gui-level bits: 1 = Close, 2 = Resize, 4 = Escape, 8 = ContextMenu,
    // 16 = DropFiles.
    std::uint32_t gui_bits{0};
    // Control-level bits: 1 = Click, 2 = Change, 4 = ItemSelect.
    std::map<std::uint64_t, std::uint32_t> ctrl_bits;
    // Monotonic stamp (JS-side, per state): concurrent OnEvent registrations
    // each mirror a snapshot, and pump arrival order differs from JS order,
    // so the pump applies only the newest stamp and drops stale ones. 0 =
    // unstamped (every non-mirror op carries the current table for
    // creation-time adopt; those always apply).
    std::uint64_t seq{0};
    // OnMessage: raw message -> the targets that registered it (0 = the Gui
    // window itself). A set, because the Gui and one of its controls may both
    // subscribe to the same message.
    std::map<std::uint32_t, std::set<std::uint64_t>> messages;
    // OnCommand / OnNotify: notify code -> set of ctrl ids interested.
    std::map<int, std::set<std::uint64_t>> commands;
    std::map<int, std::set<std::uint64_t>> notifies;
  };

  // Everything the pump needs to materialize/locate one Gui. Carried on every
  // call so record creation is idempotent and cross-worker ordering cannot
  // race (gui-menu.md §4.1).
  struct GuiSpec {
    std::uint64_t id{0};
    std::string title;
    std::uint32_t style{0};
    std::uint32_t ex_style{0};
    // Host callback id of this Gui's event channel (0 = no channel yet;
    // events are never delivered without one).
    std::uint64_t channel{0};
    // Event-interest snapshot the JS side held when this call was built.
    // Applied when the pump materializes the record, so a handler registered
    // before the window exists is never lost (gui-menu.md §4.3).
    GuiEventInterest interest;
  };

  struct GuiRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
  };

  struct GuiMoveSpec {
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> width;
    std::optional<int> height;
  };

  struct GuiShowSpec {
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> width;
    std::optional<int> height;
    bool center{false};
    bool auto_size{false};
    bool hide{false};
    bool minimize{false};
    bool maximize{false};
    bool no_activate{false};
  };

  struct GuiFontSpec {
    // Size in points (AHK "s<n>"); 0 = keep current size.
    int size_pt{0};
    // Bold/italic/strike/underline as AHK's options letters (norm/ bold...).
    bool bold{false};
    bool italic{false};
    bool strike{false};
    bool underline{false};
    std::string name;  // empty = current face
  };

  // Parsed subset of AHK control options (script_gui.cpp:5354+). optional
  // tri-state because GuiControl Opt must distinguish "not mentioned" from
  // an explicit "-Hidden" / "-Disabled".
  struct GuiControlOptions {
    GuiMoveSpec pos;  // absolute x/y/w/h only in batch 2
    std::string vname;
    std::optional<bool> hidden;
    std::optional<bool> disabled;
  };

  // One control creation request. `ctrl_id` is allocated JS-side before the
  // pump call so the record and the control object share one id space; the
  // control object itself only exists after the returned promise resolves.
  struct GuiControlSpec {
    GuiSpec gui;
    std::uint64_t ctrl_id{0};
    GuiControlKind kind{GuiControlKind::Text};
    GuiControlOptions options;
    // Content: string for text-bearing kinds, path for Picture, 0..100 for
    // Progress (has_number flags which was given; empty + !has_number = no
    // content). List kinds (ListBox/ComboBox/DropDownList) take string
    // arrays instead (has_items); a single string is one row. Column
    // headers (ListView), root items (TreeView), parts (StatusBar) and tab
    // titles (Tab) likewise ride items; Link takes markup text; Hotkey
    // takes no content at construction (initial hotkey is a 3b method).
    std::string text;
    bool has_number{false};
    double number{0};
    std::vector<std::string> items;
    bool has_items{false};
  };

  // Pure parsers (JS thread; synchronous ValueError/TypeError at the binding,
  // InvalidContract here for out-of-subset letters). Never touch a pump.
  static rime::core::Error parse_gui_options(const std::string& options, GuiStyleOptions& out);
  static rime::core::Error parse_show_options(const std::string& options, GuiShowSpec& out);
  static rime::core::Error parse_control_options(const std::string& options,
                                                 GuiControlOptions& out);
  static rime::core::Error parse_control_type(const std::string& type, GuiControlKind& out);
  static rime::core::Error parse_font_options(const std::string& options, GuiFontSpec& out);

  // Gui lifecycle (worker lane; each marshals to the pump). Operations on a
  // destroyed Gui return InvalidState; Destroy on an id that never
  // materialized is a no-op (AHK: destroying twice is a no-op).
  rime::core::Error gui_show(const GuiSpec& gui, const GuiShowSpec& show,
                             std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);
  rime::core::Error gui_hide(const GuiSpec& gui, std::int64_t deadline_unix_ms,
                             rime::core::CancellationToken cancel);
  rime::core::Error gui_destroy(std::uint64_t gui_id, std::int64_t deadline_unix_ms,
                                rime::core::CancellationToken cancel);
  rime::core::Error gui_move(const GuiSpec& gui, const GuiMoveSpec& move,
                             std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);
  rime::core::Error gui_get_pos(const GuiSpec& gui, bool client, GuiRect& out,
                                std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);
  // show-mode commands: SW_MAXIMIZE / SW_MINIMIZE / SW_RESTORE.
  rime::core::Error gui_window_cmd(const GuiSpec& gui, int show_cmd,
                                   std::int64_t deadline_unix_ms,
                                   rime::core::CancellationToken cancel);
  rime::core::Error gui_flash(const GuiSpec& gui, bool blink, std::int64_t deadline_unix_ms,
                              rime::core::CancellationToken cancel);
  rime::core::Error gui_opt(const GuiSpec& gui, const GuiStyleOptions& style,
                            std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);
  rime::core::Error gui_set_font(const GuiSpec& gui, const GuiFontSpec& font,
                                 std::int64_t deadline_unix_ms,
                                 rime::core::CancellationToken cancel);
  rime::core::Error gui_set_title(const GuiSpec& gui, const std::string& title,
                                  std::int64_t deadline_unix_ms,
                                  rime::core::CancellationToken cancel);
  rime::core::Error gui_get_title(const GuiSpec& gui, std::string& out,
                                  std::int64_t deadline_unix_ms,
                                  rime::core::CancellationToken cancel);
  // BackColor applies a window/control background brush on the pump; the
  // getter mirror lives JS-side (gui-menu.md §4.2).
  rime::core::Error gui_set_back_color(const GuiSpec& gui, std::uint32_t color_ref,
                                       std::int64_t deadline_unix_ms,
                                       rime::core::CancellationToken cancel);
  // Writes one or both margins. A negative axis means "keep the current
  // value" so a script that only ever set MarginX does not reset MarginY
  // back to its default; the JS mirror uses -1 for the axis it has not set.
  rime::core::Error gui_set_margins(const GuiSpec& gui, int margin_x, int margin_y,
                                    std::int64_t deadline_unix_ms,
                                    rime::core::CancellationToken cancel);
  rime::core::Error gui_submit(const GuiSpec& gui, bool hide, rime::core::json::Value& out,
                               std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);
  rime::core::Error gui_focused_control(const GuiSpec& gui, std::uint64_t& ctrl_id_out,
                                        std::int64_t deadline_unix_ms,
                                        rime::core::CancellationToken cancel);
  // Interest mirror: replaces the record's event-interest snapshot. No-op
  // when the record does not exist yet - a later ensure applies the interest
  // the caller carried in GuiSpec, so registering a handler before the first
  // Show still lands (see GuiSpec::interest).
  rime::core::Error gui_set_event_interest(const GuiSpec& gui, const GuiEventInterest& interest,
                                           std::int64_t deadline_unix_ms,
                                           rime::core::CancellationToken cancel);

  // Controls (worker lane).
  rime::core::Error gui_add(const GuiControlSpec& spec, std::int64_t deadline_unix_ms,
                            rime::core::CancellationToken cancel);
  rime::core::Error ctrl_move(const GuiSpec& gui, std::uint64_t ctrl_id, const GuiMoveSpec& move,
                              std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);
  rime::core::Error ctrl_get_pos(const GuiSpec& gui, std::uint64_t ctrl_id, GuiRect& out,
                                 std::int64_t deadline_unix_ms,
                                 rime::core::CancellationToken cancel);
  rime::core::Error ctrl_focus(const GuiSpec& gui, std::uint64_t ctrl_id,
                               std::int64_t deadline_unix_ms,
                               rime::core::CancellationToken cancel);
  rime::core::Error ctrl_focused(const GuiSpec& gui, std::uint64_t ctrl_id, bool& out,
                                 std::int64_t deadline_unix_ms,
                                 rime::core::CancellationToken cancel);
  rime::core::Error ctrl_redraw(const GuiSpec& gui, std::uint64_t ctrl_id,
                                std::int64_t deadline_unix_ms,
                                rime::core::CancellationToken cancel);
  rime::core::Error ctrl_set_font(const GuiSpec& gui, std::uint64_t ctrl_id,
                                  const GuiFontSpec& font, std::int64_t deadline_unix_ms,
                                  rime::core::CancellationToken cancel);
  // Text = window text of any kind; Value = kind-specific (Edit string,
  // CheckBox/Radio 0|1, Progress 0..100; ValueError elsewhere - gui-menu.md
  // §4.2).
  rime::core::Error ctrl_get_text(const GuiSpec& gui, std::uint64_t ctrl_id, std::string& out,
                                  std::int64_t deadline_unix_ms,
                                  rime::core::CancellationToken cancel);
  rime::core::Error ctrl_set_text(const GuiSpec& gui, std::uint64_t ctrl_id,
                                  const std::string& text, std::int64_t deadline_unix_ms,
                                  rime::core::CancellationToken cancel);
  rime::core::Error ctrl_get_value(const GuiSpec& gui, std::uint64_t ctrl_id,
                                   rime::core::json::Value& out, std::int64_t deadline_unix_ms,
                                   rime::core::CancellationToken cancel);
  rime::core::Error ctrl_set_value(const GuiSpec& gui, std::uint64_t ctrl_id,
                                   const rime::core::json::Value& value,
                                   std::int64_t deadline_unix_ms,
                                   rime::core::CancellationToken cancel);
  rime::core::Error ctrl_set_enabled(const GuiSpec& gui, std::uint64_t ctrl_id, bool enabled,
                                     std::int64_t deadline_unix_ms,
                                     rime::core::CancellationToken cancel);
  rime::core::Error ctrl_get_enabled(const GuiSpec& gui, std::uint64_t ctrl_id, bool& out,
                                     std::int64_t deadline_unix_ms,
                                     rime::core::CancellationToken cancel);
  rime::core::Error ctrl_set_visible(const GuiSpec& gui, std::uint64_t ctrl_id, bool visible,
                                     std::int64_t deadline_unix_ms,
                                     rime::core::CancellationToken cancel);
  rime::core::Error ctrl_get_visible(const GuiSpec& gui, std::uint64_t ctrl_id, bool& out,
                                     std::int64_t deadline_unix_ms,
                                     rime::core::CancellationToken cancel);
  // Apply the parsed control-option subset (move/hide/disable).
  rime::core::Error ctrl_opt(const GuiSpec& gui, std::uint64_t ctrl_id,
                             const GuiControlOptions& options, std::int64_t deadline_unix_ms,
                             rime::core::CancellationToken cancel);
  // EM_SETCUEBANNER; activate sets the cue when the edit is empty. There is
  // no Win32 getter (EM_GETCUEBANNER does not exist), so no GetCue - same
  // as AHK (no GuiCtrl.GetCue).
  rime::core::Error ctrl_set_cue(const GuiSpec& gui, std::uint64_t ctrl_id,
                                 const std::string& cue, bool activate,
                                 std::int64_t deadline_unix_ms,
                                 rime::core::CancellationToken cancel);

  // ---- batch 3b: per-kind control methods --------------------------------
  // One tagged call crosses the pump (ctrl_invoke); the JS binding parses
  // arguments per kind and every method below is kind-checked on the pump
  // (wrong kind = InvalidContract, same as ctrl_set_cue's Edit-only rule).
  // Add/Choose/Delete/Insert/Modify/GetCount/GetText/GetNext are shared
  // names dispatched by kind: list kinds (ListBox/ComboBox/DropDownList),
  // ListView rows, TreeView nodes. Positions are 0-based (AHK's 1-based
  // rows/columns are NOT kept); empty results are null/0, never AHK's 0-id.
  // TreeView nodes are addressed by stable ids issued at Add (per control,
  // from 1); HTREEITEM handles never leave the pump.
  enum class GuiControlCallOp {
    ListAdd,
    ListChoose,
    ListDelete,
    LvAddRow,
    LvInsertRow,
    LvModifyRow,
    LvDeleteRows,
    LvGetCount,
    LvGetText,
    LvGetNext,
    LvInsertCol,
    LvModifyCol,
    LvDeleteCol,
    TvAdd,
    TvDelete,
    TvModify,
    TvGet,
    TvGetChild,
    TvGetCount,
    TvGetNext,
    TvGetParent,
    TvGetPrev,
    TvGetSelection,
    TvGetText,
    SbSetParts,
    SbSetText,
    SbSetIcon,
    TabUseTab,
    DtSetFormat,
    HotkeySet,
    SetImageList,
  };
  struct GuiControlCall {
    GuiControlCallOp op{GuiControlCallOp::ListAdd};
    // 0-based row/column/part/index (-1 = omitted). has_index distinguishes
    // an explicit 0 from "no argument" (Delete() clears, Delete(0) removes).
    std::int64_t index{-1};
    bool has_index{false};
    // Second axis: GetText column (ListView), unused elsewhere.
    std::int64_t sub{0};
    bool has_sub{false};
    // TreeView node id (0 = omitted: TvAdd at root, TvDelete clears all).
    std::uint64_t item{0};
    bool has_item{false};
    // Name (TvAdd), format picture (DtSetFormat), value (ListChoose text),
    // file (SbSetIcon), label (TabUseTab by name).
    std::string text;
    // Normalized lowercase selector: count mode / next filter / Get
    // attribute ("expanded"|"checked"|"bold") / traversal / column align.
    std::string mode;
    // Row cells (LV), option values (ListAdd), part widths are numbers.
    std::vector<std::string> columns;
    std::vector<int> widths;
    // Row/node modifier boxes; nullopt = leave unchanged.
    std::optional<bool> selected;
    std::optional<bool> focused;
    std::optional<bool> checked;
    std::optional<bool> expanded;
    std::optional<bool> bold;
    // StatusBar SetText style box. Win32 only offers SBT_NOBORDERS here
    // (SBT_OWNERDRAW needs DRAWITEM handling: out of scope, documented).
    std::optional<bool> borders;
    // ListView column box.
    std::optional<int> width;
    std::optional<std::string> title;
    std::optional<std::string> align;
    // SbSetIcon resource index inside file/dll.
    int icon_index{0};
    // Batch 3d: SetImageList target list id + row/node image index.
    std::uint64_t list{0};
    std::optional<int> image_index;
    // HotkeySet chord, parsed JS-side (same letters as Hotkey content).
    std::uint8_t hot_modifiers{0};
    std::uint8_t hot_vk{0};
  };
  struct GuiControlResult {
    std::int64_t number{0};
    std::uint64_t item{0};
    std::string text;
    bool boolean{false};
  };
  rime::core::Error ctrl_invoke(const GuiSpec& gui, std::uint64_t ctrl_id,
                                const GuiControlCall& call, GuiControlResult& out,
                                std::int64_t deadline_unix_ms,
                                rime::core::CancellationToken cancel);

  // ---- batch 3c: Menu objects ----------------------------------------------
  // Menus are pump-owned HMENUs addressed by stable ids (per service, from
  // 1); like every other handle they never leave the pump. Items are
  // addressed by exact name or 0-based position (has_index); names need not
  // be unique (first match wins, same as AHK FindItem). Item callbacks live
  // JS-side keyed by command id; the service only mints and retires ids.
  // Menu command ids come from a service-global 0x8000+ counter so a
  // WM_COMMAND on any Gui window routes unambiguously.
  enum class GuiMenuCallOp {
    Add,
    Insert,
    Delete,
    Rename,
    Check,
    Uncheck,
    Enable,
    Disable,
    ToggleCheck,
    ToggleEnable,
    SetDefault,
    GetDefault,
    SetClickCount,
    GetClickCount,
    SetColor,
    SetIcon,
    GetCount,
  };
  struct GuiMenuCall {
    GuiMenuCallOp op{GuiMenuCallOp::GetCount};
    // Event channel adoption (GuiSpec::channel shape): every call carries
    // the menu's host channel and the pump adopts it when unset, so no
    // call depends on another landing first.
    std::uint64_t channel{0};
    // Item selector: exact name in text, or 0-based position in index.
    std::string text;
    std::string text2;  // Rename new name (empty = to separator).
    std::int64_t index{-1};
    bool has_index{false};
    // Insert position: name in before (has_before) or 0-based index.
    std::string before;
    bool has_before{false};
    // Insert position: name in text or 0-based index (omitted = append).
    // Add/Insert submenu attachment (0 = plain item).
    std::uint64_t submenu{0};
    bool has_submenu{false};
    // SetColor rgb (COLORREF, same spelling as BackColor), SetIcon file/index/width.
    bool has_color{false};
    std::uint32_t color{0};
    bool apply_to_submenus{true};
    std::string file;
    int icon_index{0};
    int icon_width{0};
    bool has_icon_width{false};
    int click_count{1};
  };
  struct GuiMenuResult {
    std::int64_t number{0};
    std::uint64_t command{0};
    std::string text;
    bool boolean{false};
  };
  // Menu ids are minted JS-side (ui.createMenu) like Gui ids; the pump
  // materializes the record on first touch. Destroy on an id that never
  // materialized is a no-op (same rule as Gui).
  rime::core::Error menu_destroy(std::uint64_t menu_id, std::int64_t deadline_unix_ms,
                                 rime::core::CancellationToken cancel);
  rime::core::Error menu_invoke(std::uint64_t menu_id, const GuiMenuCall& call,
                                GuiMenuResult& out, std::int64_t deadline_unix_ms,
                                rime::core::CancellationToken cancel);
  // Modal popup (TrackPopupMenuEx TPM_RETURNCMD): resolves the selected
  // item's command id (0 = dismissed) and name (""). Same bounded shape as
  // MsgBox: the queued phase honors the deadline, the modal loop runs to
  // completion.
  // Position defaults to the cursor; pass has_position for x/y.
  rime::core::Error menu_show(std::uint64_t menu_id, std::uint64_t channel, bool has_position,
                              int x, int y, std::uint64_t& command_out, std::string& name_out,
                              std::int64_t deadline_unix_ms,
                              rime::core::CancellationToken cancel);
  // Attach (menu_id) or detach (0) a menu bar. Read with menu_bar_of.
  rime::core::Error gui_menu_bar(const GuiSpec& gui, std::uint64_t menu_id,
                                 std::int64_t deadline_unix_ms,
                                 rime::core::CancellationToken cancel);
  rime::core::Error gui_menu_bar_of(const GuiSpec& gui, std::uint64_t& menu_id_out,
                                    std::int64_t deadline_unix_ms,
                                    rime::core::CancellationToken cancel);

  // ---- batch 3d: ImageList objects -----------------------------------------
  // AHK's IL_Create/IL_Add integer handles become a runtime object (RAII
  // like every other handle here): ids minted JS-side, HIMAGELISTs die with
  // the record. Icons only (.ico files and dll/exe indices, the SB/Menu
  // SetIcon shape); bitmap strips are out of scope, documented.
  rime::core::Error image_list_add(std::uint64_t list_id, const std::string& file, int icon_index,
                                   std::int64_t& index_out, std::int64_t deadline_unix_ms,
                                   rime::core::CancellationToken cancel);
  rime::core::Error image_list_create(std::uint64_t list_id, int width, int height, int initial,
                                      int grow, std::int64_t deadline_unix_ms,
                                      rime::core::CancellationToken cancel);
  rime::core::Error image_list_destroy(std::uint64_t list_id, std::int64_t deadline_unix_ms,
                                       rime::core::CancellationToken cancel);

  // ---- batch 3e: FromHwnd reverse lookup -----------------------------------
  // Only runtime-owned windows/controls resolve (the DoD's §4.7 fork):
  // anything else - foreign windows, dead handles, unresolvable ids -
  // misses with TargetGone (JS: null, never a throw). Gui scans top-level
  // records, GuiCtrl scans every child; both skip dead records.
  rime::core::Error gui_from_window(std::uint64_t window_id, std::uint64_t& gui_id_out,
                                    std::int64_t deadline_unix_ms,
                                    rime::core::CancellationToken cancel);
  rime::core::Error gui_ctrl_from_window(std::uint64_t control_id, std::uint64_t& gui_id_out,
                                         std::uint64_t& ctrl_id_out,
                                         std::int64_t deadline_unix_ms,
                                         rime::core::CancellationToken cancel);

  // How many Gui windows the pump currently owns (tests and diagnostics;
  // never a handle).
  std::size_t gui_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Standard worker->pump envelope for every batch-2 operation: no-pump
  // InvalidState, pre-start Cancelled/Timeout from the absolute deadline,
  // then UiThread::call with an in-task cancel check (msg_box's shape).
  using PumpBody = std::function<rime::core::Error()>;
  rime::core::Error run_pump(std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel,
                             const PumpBody& pump_body);

  // Pump-side batch-2 sweep used by stop(): destroys every live Gui window,
  // frees records' fonts/brushes/picture handles. Called inside stop()'s
  // UiThread::call from gui.cpp; defined in gui_window.cpp.
  void stop_pump_sweep();
};

}  // namespace rime::win32
