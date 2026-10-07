#pragma once

// The component search AHK runs against a device's topology, split out of
// sound_endpoint.cpp so a test can drive walk_parts() with a synthetic IPart
// tree. This walk is what decides which of several components answers to a
// bare instance number and which subunit's control reaches the caller - a
// machine's real topology can only ever show one arrangement of those rules,
// so the rules themselves need a tree of their own to be pinned against.

#include "com_scope.hpp"
#include "rime/win32/sound.hpp"

#include <devicetopology.h>

#include <string>

namespace rime::win32 {

// What the caller wants back - AHK's SoundControlType minus the raw-IID form
// (lib/sound.cpp:96-102).
enum class ControlKind { Volume, Mute, Name };

// AHK's SoundComponentSearch (lib/sound.cpp:129-143) with its results typed
// instead of an IUnknown the caller casts blindly.
struct ComponentSearch {
  SoundService::ComponentSpec spec;
  ControlKind kind{ControlKind::Volume};
  DataFlow flow{Out};
  int count{0};
  bool ignore_remaining_subunits{false};
  ComPtr<IUnknown> control;  // IAudioVolumeLevel or IAudioMute
  std::string name;          // ControlKind::Name only
};

// lib/sound.cpp:163-251, transcribed. Returns true when the search reached a
// connector whose number matches spec.instance; search.count then says how
// many candidates were seen, search.name holds that connector's name for
// ControlKind::Name, and search.control holds the subunit control found on
// the way back up - null for a connector, or when sibling suppression kept
// the walk from taking a control that other components share.
bool walk_parts(IPart* root, ComponentSearch& search);

}  // namespace rime::win32
