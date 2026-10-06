#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/storage.hpp"

namespace rime::win32 {

// Executes `storage.write` (capability `filesystem.write`).
// Payload contract: {"op": string, ...} - one op per family (append, write,
// copy, move, delete, mkdir, rmdir, dircopy, dirmove, setAttrib, setTime,
// recycle, recycleEmpty, shortcut, install, env, iniWrite, iniDelete,
// driveLabel, driveLock, driveUnlock, driveEject, driveRetract, handleWrite);
// every field is validated per op and an unknown op is InvalidContract.
// Target contract: {"kind": "storage", "id": "fs"}.
class StorageExecutor final : public rime::action::Executor {
 public:
  explicit StorageExecutor(StorageService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  StorageService& service_;
};

}  // namespace rime::win32
