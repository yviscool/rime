#pragma once

#include "rime/core/cancellation.hpp"
#include "rime/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rime::win32 {

class UiThread;

// Attribute letters follow AHK's FileAttribToStr (source/util.cpp): the
// returned string concatenates R A S H N D O C T L in that fixed order with
// no separators (a plain file reads back "A", a directory "D").
struct FileStatInfo {
  std::uint64_t size{0};
  std::int64_t mtime_unix_ms{0};
  std::uint32_t attrib_bits{0};
  bool is_dir{false};
  std::string attrib_string;
};

struct DirEntryInfo {
  std::string name;  // UTF-8 base name, no directory prefix
  bool is_dir{false};
};

// Decoded .lnk fields (AHK FileGetShortcut rule): empty strings when the
// link leaves them unset, never an error for a well-formed link.
struct ShortcutInfo {
  std::string target;      // UTF-8 resolved path
  std::string working_dir;  // UTF-8
  std::string args;        // UTF-8 argument string
  std::string icon;        // UTF-8 icon path, "" when the link sets none
};

// One field of DriveInfo is populated per drive_get() field; the rest keep
// their defaults (empty / 0 / -1). `list` fills `list` with drive letters
// spelled "A".."Z".
struct DriveInfo {
  std::string letter;       // "C:" for letter-based fields, "" for `list`
  std::string filesystem;
  std::string label;
  std::string type;         // Unknown|Removable|Fixed|Network|CDROM|RAMDisk
  std::string status;       // Ready|Invalid|NotReady|ReadOnly|Unknown
  std::uint64_t total_bytes{0};
  std::uint64_t free_bytes{0};
  std::uint32_t serial{0};
  std::vector<std::string> list;
  int capacity_percent{-1};  // -1 until a space query filled it
};

// Storage domain service. Every method is synchronous and safe to call on
// the worker lane; download() additionally yields to its cancellation token
// and deadline between chunks so one transfer cannot monopolize the lane.
//
// Ownership: the service owns the open-file handle table (opaque uint64 ids;
// a raw HANDLE never leaves this class), the session FileEncoding and a
// borrowed pointer to the runtime's single UI pump for the two selectors.
// stop() closes every residual handle, returns how many it closed and may be
// called repeatedly (bootstrap and the module teardown both use it).
class StorageService final {
 public:
  StorageService() = default;
  ~StorageService();
  StorageService(const StorageService&) = delete;
  StorageService& operator=(const StorageService&) = delete;

  // ---- reads (worker lane; the module gates capability filesystem.read) ----
  // Decodes the whole file with the session encoding into UTF-8.
  rime::core::Error read_text(const std::string& path, std::string& out) const;
  // Reads a .lnk shortcut (target, working dir, args, icon). COM apartment
  // is entered per call; a corrupt link fails instead of guessing.
  rime::core::Error read_shortcut(const std::string& path, ShortcutInfo& out) const;
  // File version resource as "M.m.b.r" (VS_FIXEDFILEINFO). A file without
  // version info reads back as an empty string, not an error.
  rime::core::Error read_version(const std::string& path, std::string& out) const;
  // Whole file as raw bytes, capped at 1 GiB per call.
  rime::core::Error read_bytes(const std::string& path, std::vector<std::uint8_t>& out) const;
  rime::core::Error stat(const std::string& path, FileStatInfo& out) const;
  // Immediate children of one directory; "." and ".." are never reported.
  rime::core::Error list(const std::string& path, std::vector<DirEntryInfo>& out) const;
  // A variable that is not set reads back as an empty string (AHK EnvGet).
  rime::core::Error env_get(const std::string& name, std::string& out) const;
  // Missing file or missing key is an ExecutionFailed, never a silent "".
  rime::core::Error ini_read(const std::string& path, const std::string& section,
                             const std::string& key, std::string& out) const;
  // field: type|list|serial|spacefree|status|statuscd|filesystem|label|capacity.
  // `letter` carries the drive for every field except `list`, where it is the
  // AHK DriveGetList type filter ("" | CDROM|Removable|Fixed|Network|RAMDisk|
  // Unknown) and an unknown filter is an InvalidContract.
  rime::core::Error drive_get(const std::string& field, const std::string& letter,
                              DriveInfo& out) const;

  // ---- file handles (ids owned by this service; HANDLE stays internal) ----
  // mode "r" (file must exist), "a" (create/append - the handle starts at end
  // of file), "w" (truncate/create). handle_out is a monotonic id, never a
  // Win32 handle. The caller gates capability: "r" -> filesystem.read,
  // "a"/"w" -> filesystem.write.
  rime::core::Error file_open(const std::string& path, const std::string& mode,
                              std::uint64_t& handle_out, std::uint64_t& length_out);
  // Reads up to `count` bytes at the current position; eof reports a short
  // read. A stale or unknown id is InvalidState, not a crash.
  rime::core::Error file_read(std::uint64_t handle, std::uint32_t count,
                              std::vector<std::uint8_t>& out, bool& eof) const;
  // whence 0 = begin, 1 = current, 2 = end; `pos` is the new position.
  rime::core::Error file_seek(std::uint64_t handle, std::int64_t offset, int whence,
                              std::uint64_t& pos);
  rime::core::Error file_stat(std::uint64_t handle, std::uint64_t& pos,
                              std::uint64_t& length) const;
  rime::core::Error file_close(std::uint64_t handle);
  [[nodiscard]] std::size_t open_handle_count() const;
  // Closes every residual handle, returns the number closed; repeatable.
  std::size_t stop();

  // ---- writes (executor on the worker lane; capability filesystem.write) ----
  // Encodes with the session encoding; append_text creates the file when the
  // path does not exist yet.
  rime::core::Error append_text(const std::string& path, const std::string& utf8) const;
  rime::core::Error write_text(const std::string& path, const std::string& utf8) const;
  rime::core::Error file_copy(const std::string& src, const std::string& dst, bool overwrite);
  // Falls back to copy+delete when src and dst sit on different volumes.
  rime::core::Error file_move(const std::string& src, const std::string& dst, bool overwrite);
  rime::core::Error file_delete(const std::string& path);
  // Creates missing intermediate directories (AHK DirCreate).
  rime::core::Error dir_create(const std::string& path);
  rime::core::Error dir_delete(const std::string& path, bool recursive);
  rime::core::Error dir_copy(const std::string& src, const std::string& dst, bool overwrite);
  rime::core::Error dir_move(const std::string& src, const std::string& dst, bool overwrite);
  // add/remove are AHK attribute letters (A R H S T N O); an unknown letter is
  // an InvalidContract. Both empty is an InvalidContract (nothing to change).
  rime::core::Error set_attrib(const std::string& path, const std::string& add,
                               const std::string& remove);
  // which: "mtime" | "atime" | "ctime"; unix_ms is UTC milliseconds.
  rime::core::Error set_time(const std::string& path, const std::string& which,
                             std::int64_t unix_ms);
  rime::core::Error recycle(const std::string& path);
  // root "" empties the recycle bin of every drive (destructive; never
  // exercised by tests).
  rime::core::Error recycle_empty(const std::string& root);
  rime::core::Error make_shortcut(const std::string& link_path, const std::string& target,
                                  const std::string& args, const std::string& workdir,
                                  const std::string& icon, const std::string& description);
  // Semantics equal file_copy: AHK embeds the source into the script binary
  // at compile time, which this runtime does not do (documented deviation).
  rime::core::Error file_install(const std::string& src, const std::string& dst, bool overwrite);
  rime::core::Error env_set(const std::string& name, const std::string& value);
  rime::core::Error ini_write(const std::string& path, const std::string& section,
                              const std::string& key, const std::string& value) const;
  // An empty `key` deletes the whole section; a missing file is an error.
  rime::core::Error ini_delete(const std::string& path, const std::string& section,
                               const std::string& key) const;
  rime::core::Error drive_set_label(const std::string& letter, const std::string& label);
  rime::core::Error drive_lock(const std::string& letter);
  rime::core::Error drive_unlock(const std::string& letter);
  rime::core::Error drive_eject(const std::string& letter);
  rime::core::Error drive_retract(const std::string& letter);
  // File.Write family: writes `bytes` at the handle's current position.
  rime::core::Error handle_write(std::uint64_t handle, const std::vector<std::uint8_t>& bytes);

  // ---- session settings (synchronous, mutex-guarded) ----
  // encoding: "utf-8" (default) | "utf-8-bom" | "utf-16" | "utf-16-be" |
  // "cp0" | "cp1252" | "latin1". set_encoding returns InvalidContract for
  // anything else instead of silently accepting it.
  [[nodiscard]] std::string encoding() const;
  rime::core::Error set_encoding(std::string enc);

  // ---- download (worker; WinHTTP) ----
  // Only http/https URLs are accepted (InvalidContract otherwise). Reads in
  // chunks and checks `cancel` plus `deadline_unix_ms` between them; a failed
  // or cancelled transfer removes the partial destination file.
  rime::core::Error download(const std::string& url, const std::string& path,
                             rime::core::CancellationToken cancel,
                             std::int64_t deadline_unix_ms, std::uint64_t& bytes_out);

  // ---- selectors (modal IFileDialog on the runtime's UI pump) ----
  // Browses set_ui_thread()'s pump through UiThread::call: the queued phase is
  // bounded by the call timeout, but once the task is claimed the modal dialog
  // runs to completion and cannot be interrupted - a cancelled token only
  // skips work that has not started. Returns InvalidState when no pump is
  // attached, so a headless embed can never open a dialog by accident.
  rime::core::Error select_file(const std::string& filter, const std::string& default_name,
                                bool multi, std::vector<std::string>& out,
                                rime::core::CancellationToken cancel);
  rime::core::Error select_dir(const std::string& caption, std::string& out,
                               rime::core::CancellationToken cancel);

  // Borrows the runtime's single UI pump (never a second one). Passing nullptr
  // detaches it again; select_file/select_dir then report InvalidState.
  void set_ui_thread(UiThread* ui);

 private:
  [[nodiscard]] std::string current_encoding() const;

  // The Win32 HANDLE values stay in this table; `void*` keeps windows.h out
  // of this header (HANDLE is void* on every Windows target).
  mutable std::mutex handles_mutex_;
  std::unordered_map<std::uint64_t, void*> handles_;
  std::uint64_t next_handle_id_{1};

  mutable std::mutex encoding_mutex_;
  std::string encoding_{"utf-8"};

  UiThread* ui_thread_{nullptr};
};

}  // namespace rime::win32
