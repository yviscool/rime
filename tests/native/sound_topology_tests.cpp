// Realism: L3 - the walk under test is the real walk_parts() from
// sound_endpoint.cpp, running against the real devicetopology.h vtables;
// only the device is replaced, by a synthetic IPart tree, because a single
// machine's driver never shows all of AHK's rules at once. Every expectation
// is written out from rime-research/AutoHotkey-alpha/source/lib/sound.cpp
// (:163-251 for the walk, :311-320 for which IID each control type asks for)
// and never derived from what walk_parts() returns. The reference counts
// asserted at the end are the leak half of the contract: the walk must not
// still hold a part, a list or a control reference when it is done.

#include "sound_topology.hpp"

#include <endpointvolume.h>
#include <objbase.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using rime::win32::ComponentSearch;
using rime::win32::ControlKind;
using rime::win32::SoundService;
using rime::win32::walk_parts;

void section(const char* name) {
  std::printf("---- %s ----\n", name);
  std::fflush(stdout);
}

// The control a subunit hands out. Only identity matters here - walk_parts
// stores it as IUnknown and the caller casts later - so this implements the
// three IUnknown methods and nothing else.
class FakeControl final : public IUnknown {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
      *out = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // test-owned

  [[nodiscard]] ULONG refs() const { return refs_; }

 private:
  ULONG refs_{1};
};

// A list as EnumPartsIncoming/Outgoing returns it. Parts are borrowed, the
// list itself is reference counted so the walk's own balancing can be seen.
class FakePartsList final : public IPartsList {
 public:
  explicit FakePartsList(std::vector<IPart*> parts) : parts_(std::move(parts)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IPartsList)) {
      *out = static_cast<IPartsList*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // registry-owned

  [[nodiscard]] ULONG refs() const { return refs_; }

  HRESULT GetCount(UINT* out) override {
    if (out == nullptr) return E_POINTER;
    *out = static_cast<UINT>(parts_.size());
    return S_OK;
  }

  HRESULT GetPart(UINT index, IPart** out) override {
    if (out == nullptr) return E_POINTER;
    if (index >= parts_.size()) return E_INVALIDARG;
    parts_[index]->AddRef();
    *out = parts_[index];
    return S_OK;
  }

 private:
  std::vector<IPart*> parts_;
  ULONG refs_{1};
};

// Every list the fakes hand out lands here so the test can assert the walk
// released them all; ownership ends at process exit.
std::vector<std::unique_ptr<FakePartsList>> g_lists;

IPartsList* make_list(const std::vector<IPart*>& parts) {
  g_lists.push_back(std::make_unique<FakePartsList>(parts));
  return g_lists.back().get();  // refcount 1, released by the caller's ComPtr
}

class FakePart final : public IPart {
 public:
  FakePart(const PartType type, std::wstring name = {})
      : type_(type), name_(std::move(name)) {}

  // What Activate() answers with: a null control means "this subunit has no
  // such control", the IID means "it has this one" - AHK's BIF sets one IID
  // per control type (lib/sound.cpp:311-320) and a mismatch is a refusal.
  void gives_control(FakeControl* control, const IID& iid) {
    control_ = control;
    control_iid_ = iid;
  }

  void outgoing(std::vector<IPart*> parts) { outgoing_ = std::move(parts); }
  void incoming(std::vector<IPart*> parts) { incoming_ = std::move(parts); }

  [[nodiscard]] ULONG refs() const { return refs_; }
  [[nodiscard]] FakeControl* control() const { return control_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IPart)) {
      *out = static_cast<IPart*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // test-owned

  HRESULT GetName(LPWSTR* out) override {
    if (out == nullptr) return E_POINTER;
    if (name_.empty()) return E_INVALIDARG;
    const std::size_t bytes = (name_.size() + 1) * sizeof(wchar_t);
    auto* buffer = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (buffer == nullptr) return E_OUTOFMEMORY;
    std::memcpy(buffer, name_.c_str(), bytes);
    *out = buffer;
    return S_OK;
  }
  HRESULT GetLocalId(UINT*) override { return E_NOTIMPL; }
  HRESULT GetGlobalId(LPWSTR*) override { return E_NOTIMPL; }
  HRESULT GetPartType(PartType* out) override {
    if (out == nullptr) return E_POINTER;
    *out = type_;
    return S_OK;
  }
  HRESULT GetSubType(GUID*) override { return E_NOTIMPL; }
  HRESULT GetControlInterfaceCount(UINT*) override { return E_NOTIMPL; }
  HRESULT GetControlInterface(UINT, IControlInterface**) override { return E_NOTIMPL; }
  HRESULT EnumPartsIncoming(IPartsList** out) override { return make_list_(incoming_, out); }
  HRESULT EnumPartsOutgoing(IPartsList** out) override { return make_list_(outgoing_, out); }
  // The Windows SDK spells this GetTopologyObject (singular); mingw's
  // WIDL-generated devicetopology.h spells it GetTopologyObjects (plural).
  // Only the name this SDK declares can carry `override`, so the fake is
  // written twice rather than pretending one spelling works everywhere.
#ifdef _MSC_VER
  HRESULT GetTopologyObject(IDeviceTopology**) override { return E_NOTIMPL; }
#else
  HRESULT GetTopologyObjects(IDeviceTopology**) override { return E_NOTIMPL; }
#endif
  HRESULT Activate(DWORD, REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (control_ == nullptr || (riid != __uuidof(IUnknown) && riid != control_iid_)) {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    control_->AddRef();
    *out = static_cast<IUnknown*>(control_);
    return S_OK;
  }
  HRESULT RegisterControlChangeCallback(REFGUID, IControlChangeNotify*) override {
    return E_NOTIMPL;
  }
  HRESULT UnregisterControlChangeCallback(IControlChangeNotify*) override { return E_NOTIMPL; }

 private:
  static HRESULT make_list_(const std::vector<IPart*>& children, IPartsList** out) {
    if (out == nullptr) return E_POINTER;
    *out = make_list(children);  // refcount 1, handed to the caller
    return S_OK;
  }

  PartType type_{Connector};
  std::wstring name_;
  std::vector<IPart*> outgoing_;
  std::vector<IPart*> incoming_;
  FakeControl* control_{nullptr};
  IID control_iid_{};
  ULONG refs_{1};
};

ComponentSearch search_for(const std::string& name, const int instance, const ControlKind kind) {
  ComponentSearch search;
  search.spec.master = false;
  search.spec.name = name;
  search.spec.instance = instance;
  search.kind = kind;
  search.flow = Out;
  return search;
}

}  // namespace

int main() {
  section("connector name, single child");
  {
    // root -> [wave]: one subunit whose whole purpose is the connector, so
    // the connector counts (lib/sound.cpp:177-181 `part_count == 1`).
    FakePart wave(Connector, L"Wave Out");
    FakePart root(Subunit);
    root.outgoing({&wave});

    auto found = search_for("Wave Out", 1, ControlKind::Name);
    assert(walk_parts(&root, found));
    assert(found.count == 1);
    assert(found.name == "Wave Out");
    assert(found.control.get() == nullptr);

    auto wrong = search_for("Line In", 1, ControlKind::Name);
    assert(!walk_parts(&root, wrong));
    assert(wrong.count == 0);
    assert(wrong.name.empty());

    // Same tree, volume requested: the connector itself carries no control,
    // so control stays null - the caller turns that into "Component doesn't
    // support this control type" (lib/sound.cpp:434-437).
    auto volume = search_for("", 1, ControlKind::Volume);
    assert(walk_parts(&root, volume));
    assert(volume.count == 1);
    assert(volume.control.get() == nullptr);

    // wave was handed out by GetPart four times above and must be back at the
    // test's own single reference each time.
    assert(wave.refs() == 1);
    assert(root.refs() == 1);
  }

  section("instance counts branches, not parts");
  {
    // root -> [left, right], each owning exactly one connector. Each subunit
    // sees a list of one, so both connectors count - in order.
    FakePart conn_a(Connector, L"Line In");
    FakePart conn_b(Connector, L"CD Audio");
    FakePart left(Subunit);
    left.outgoing({&conn_a});
    FakePart right(Subunit);
    right.outgoing({&conn_b});
    FakePart root(Subunit);
    root.outgoing({&left, &right});

    auto first = search_for("", 1, ControlKind::Volume);
    assert(walk_parts(&root, first));
    assert(first.count == 1);

    auto second = search_for("", 2, ControlKind::Volume);
    assert(walk_parts(&root, second));
    assert(second.count == 2);

    // No third branch: the walk reports every candidate it saw, and the
    // caller turns count != instance into "Component not found"
    // (lib/sound.cpp:254-286).
    auto third = search_for("", 3, ControlKind::Volume);
    assert(!walk_parts(&root, third));
    assert(third.count == 2);

    // A connector listed next to other parts is ignored outright: root's
    // list has two entries, so "Direct" is never a candidate.
    FakePart direct(Connector, L"Direct");
    root.outgoing({&left, &right, &direct});
    auto direct_search = search_for("Direct", 1, ControlKind::Name);
    assert(!walk_parts(&root, direct_search));
    assert(direct_search.count == 0);

    assert(left.refs() == 1);
    assert(right.refs() == 1);
    assert(conn_a.refs() == 1);
    assert(conn_b.refs() == 1);
    assert(direct.refs() == 1);
  }

  section("control comes from the subunit on the way back up");
  {
    // root -> [mid(control)] -> [wave]. The connector matches inside mid's
    // frame, and the control is taken from the subunit that led there, one
    // level below the root - "parts are considered from right to left, as we
    // return from recursion" (lib/sound.cpp:197-200).
    FakeControl volume_control;
    FakePart wave(Connector, L"Master Volume");
    FakePart mid(Subunit);
    mid.outgoing({&wave});
    mid.gives_control(&volume_control, __uuidof(IAudioVolumeLevel));
    FakePart root(Subunit);
    root.outgoing({&mid});

    {
      auto volume = search_for("Master Volume", 1, ControlKind::Volume);
      assert(walk_parts(&root, volume));
      assert(volume.count == 1);
      assert(volume.control.get() == static_cast<IUnknown*>(&volume_control));
      // Exactly one reference beyond the test's own: the walk adopted what
      // Activate handed it instead of adding a second one.
      assert(volume_control.refs() == 2);
    }
    // Destroying the result is what gives the control back.
    assert(volume_control.refs() == 1);

    // The BIF asks for IAudioMute when mute is wanted (lib/sound.cpp:317-318);
    // a subunit that only exposes volume must refuse, so the caller reports
    // the missing control instead of handing back the wrong interface.
    {
      auto mute = search_for("Master Volume", 1, ControlKind::Mute);
      assert(walk_parts(&root, mute));
      assert(mute.control.get() == nullptr);
      assert(mute.count == 1);
      // Refused: the volume control was never AddRef'ed for a mute search.
      assert(volume_control.refs() == 1);
    }

    assert(mid.refs() == 1);
    assert(wave.refs() == 1);
  }

  section("sibling suppression");
  {
    // Both trees: root -> [mid(control), filler], mid -> ... -> "A".
    // They differ only in whether mid itself has a sibling.
    FakeControl mid_control;
    FakePart filler(Connector, L"Filler");

    // No sibling at mid: mid's list has one entry, so ignore_remaining_
    // subunits is never set and root still takes mid's control.
    FakePart wave_a(Connector, L"A");
    FakePart sub_a(Subunit);
    sub_a.outgoing({&wave_a});
    FakePart mid_bare(Subunit);
    mid_bare.outgoing({&sub_a});
    mid_bare.gives_control(&mid_control, __uuidof(IAudioVolumeLevel));
    FakePart root_bare(Subunit);
    root_bare.outgoing({&mid_bare, &filler});
    {
      auto unsuppressed = search_for("A", 1, ControlKind::Volume);
      assert(walk_parts(&root_bare, unsuppressed));
      assert(unsuppressed.control.get() == static_cast<IUnknown*>(&mid_control));
      assert(mid_control.refs() == 2);
    }
    assert(mid_control.refs() == 1);

    // With a sibling at mid: mid's list has two entries, so after the match
    // mid is flagged as shared and root must NOT climb any further
    // (lib/sound.cpp:214-218). Without that flag the assertion above would
    // be the one that ran here too - the two trees are the control group.
    FakePart wave_a2(Connector, L"A");
    FakePart sub_a2(Subunit);
    sub_a2.outgoing({&wave_a2});
    FakePart sub_sib(Subunit);
    sub_sib.outgoing({&filler});
    FakePart mid_shared(Subunit);
    mid_shared.outgoing({&sub_a2, &sub_sib});
    mid_shared.gives_control(&mid_control, __uuidof(IAudioVolumeLevel));
    FakePart root_shared(Subunit);
    root_shared.outgoing({&mid_shared, &filler});
    {
      auto suppressed = search_for("A", 1, ControlKind::Volume);
      assert(walk_parts(&root_shared, suppressed));
      assert(suppressed.control.get() == nullptr);
      assert(suppressed.count == 1);
    }
    // Still 1: the walk refused to take a control the sibling rule protects.
    assert(mid_control.refs() == 1);

    assert(mid_bare.refs() == 1);
    assert(mid_shared.refs() == 1);
    assert(sub_a.refs() == 1);
    assert(sub_a2.refs() == 1);
    assert(mid_control.refs() == 1);
  }

  section("direction and the Name shortcut");
  {
    // flow In enumerates incoming parts (lib/sound.cpp:167-168); the outgoing
    // side of the same root is invisible to it.
    FakePart from_out(Connector, L"Outgoing");
    FakePart from_in(Connector, L"Incoming");
    FakePart root(Subunit);
    root.outgoing({&from_out});
    root.incoming({&from_in});

    auto incoming = search_for("Incoming", 1, ControlKind::Name);
    incoming.flow = In;
    assert(walk_parts(&root, incoming));
    assert(incoming.name == "Incoming");

    auto outgoing_only = search_for("Incoming", 1, ControlKind::Name);
    assert(!walk_parts(&root, outgoing_only));

    // A Name search never asks a subunit to activate anything: the BIF reads
    // search.name before it looks at search.control (lib/sound.cpp:427-430),
    // and the walk has no IID of its own to ask for.
    FakeControl volume_control;
    FakePart wave(Connector, L"Speakers");
    FakePart mid(Subunit);
    mid.outgoing({&wave});
    mid.gives_control(&volume_control, __uuidof(IAudioVolumeLevel));
    FakePart name_root(Subunit);
    name_root.outgoing({&mid});

    auto by_name = search_for("Speakers", 1, ControlKind::Name);
    assert(walk_parts(&name_root, by_name));
    assert(by_name.name == "Speakers");
    assert(by_name.control.get() == nullptr);

    // The Name search asked for no IID, so the control was never AddRef'ed.
    assert(from_in.refs() == 1);
    assert(from_out.refs() == 1);
    assert(wave.refs() == 1);
    assert(name_root.refs() == 1);
    assert(volume_control.refs() == 1);
    assert(mid.refs() == 1);
  }

  section("reference counts");
  {
    // Every list handed to the walk was released again: its refcount is back
    // to zero (the registry in g_lists holds no COM reference, only
    // ownership), so a walk that kept one would show as 1 here.
    for (const auto& list : g_lists) {
      assert(list->refs() == 0);
    }
    // Parts and controls are asserted where they stand, next to the walk
    // that used them: 1 means the walk's temporary AddRef/Release balanced
    // out instead of leaking a reference.
  }

  std::printf("sound topology tests passed\n");
  return 0;
}
