// Core Audio endpoint controls behind AHK's SoundGet*/SoundSet* family
// (rime-research/AutoHotkey-alpha/source/lib/sound.cpp:56-160 device and
// component parsing, :163-286 topology search, :292-536 the BIF itself).
//
// Everything here is scoped to one call on the calling worker: the apartment
// guard takes and releases COM, the interfaces live in function locals, and
// only UTF-8 strings or scalar values cross the boundary. No Action is built
// - the module body that calls in reads capability `media.sound` first.

#include "rime/win32/sound.hpp"

#include "com_scope.hpp"
#include "sound_topology.hpp"
#include "utf.hpp"

// The endpoint and topology headers first: they only need declarations, and
// including them after <initguid.h> would define their GUIDs in this TU.
#include <devicetopology.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

// PKEY_Device_FriendlyName below is a DEFINE_PROPERTYKEY that this toolchain
// only declares once INITGUID is on - same pattern as uia_service.cpp.
// Only the _devpkey header: it shares the guard _INC_FUNCTIONDISCOVERYKEYS
// with functiondiscoverykeys.h, so including both defines the PKEY never.
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

// AHK's script.h:216-218 verbatim: docs/api/sound.md and the tests match on
// these strings, and nothing here interpolates localised Win32 text.
constexpr const char* kDeviceNotFound = "Device not found";
constexpr const char* kComponentNotFound = "Component not found";
constexpr const char* kNoControlType = "Component doesn't support this control type";

Error target(const char* text) { return {Code::TargetGone, text}; }
Error wrong_control_type() { return {Code::Unsupported, kNoControlType}; }
Error hresult_error(const char* operation, HRESULT status) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X",
                static_cast<unsigned int>(static_cast<std::uint32_t>(status)));
  return {Code::ExecutionFailed,
          std::string("sound ") + operation + " failed (HRESULT " + text + ")"};
}

// AHK's ParseInteger, which decides whether a device or component string is
// an index or a name (util.cpp:951-981 via util.h:484-494): spaces and tabs
// are skipped, the sign is optional, decimal and 0x-hex are both accepted, the
// whole string must be consumed, and the unsigned accumulator wraps exactly
// like digitsTo<UINT64> (util.cpp:907-921) before the result is narrowed to
// the caller's int. Anything else is not an integer - the caller then treats
// the string as a name.
bool parse_int_exact(const std::string& raw, int& out) {
  std::size_t cursor = 0;
  while (cursor < raw.size() && (raw[cursor] == ' ' || raw[cursor] == '\t')) ++cursor;
  if (cursor >= raw.size()) return false;

  bool negative = raw[cursor] == '-';
  if (negative || raw[cursor] == '+') ++cursor;
  if (cursor >= raw.size()) return false;

  bool hex = false;
  auto is_hex_digit = [](char symbol) {
    const unsigned char byte = static_cast<unsigned char>(symbol);
    return std::isxdigit(byte) != 0;
  };
  if (raw[cursor] == '0' && cursor + 2 < raw.size() &&
      (raw[cursor + 1] == 'x' || raw[cursor + 1] == 'X') && is_hex_digit(raw[cursor + 2])) {
    hex = true;
    cursor += 2;
  }

  std::uint64_t magnitude = 0;
  bool digit_seen = false;
  for (; cursor < raw.size(); ++cursor) {
    const unsigned char symbol = static_cast<unsigned char>(raw[cursor]);
    int value = -1;
    if (symbol >= '0' && symbol <= '9') {
      value = symbol - '0';
    } else if (hex && std::isxdigit(symbol)) {
      value = symbol <= '9' ? symbol - '0' : (symbol | 0x20) - 'a' + 10;
    } else {
      break;
    }
    digit_seen = true;
    magnitude = hex ? magnitude * 16 + static_cast<std::uint64_t>(value)
                    : magnitude * 10 + static_cast<std::uint64_t>(value);
  }
  if (!digit_seen) return false;
  if (cursor != raw.size()) return false;  // a suffix makes it a name, not an index

  // Two's complement in unsigned arithmetic, then the same narrowing AHK
  // ends up with: -INT64_MIN stays INT64_MIN instead of overflowing.
  const std::uint64_t bits = negative ? (0ull - magnitude) : magnitude;
  out = static_cast<int>(static_cast<std::int64_t>(bits));
  return true;
}

// The device's friendly name ("Speakers (Realtek High Definition Audio)"),
// which is what AHK matches a device string against (lib/sound.cpp:41-53).
std::wstring device_friendly_name(IMMDevice* device) {
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) return {};
  PROPVARIANT prop;
  PropVariantInit(&prop);
  std::wstring name;
  if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &prop)) && prop.vt == VT_LPWSTR &&
      prop.pwszVal != nullptr) {
    name = prop.pwszVal;
  }
  PropVariantClear(&prop);
  return name;
}

// lib/sound.cpp:56-126. Returns the device or a "Device not found" target
// error: an explicit string that matches nothing, a bad index and a machine
// with no default render endpoint all land on the same AHK error.
Error open_device(const SoundService::DeviceSpec& spec, IMMDevice** out) {
  *out = nullptr;
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr =
      CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                       reinterpret_cast<void**>(enumerator.put()));
  if (FAILED(hr)) return hresult_error("device enumerator", hr);

  if (spec.use_default) {
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, out))) {
      return target(kDeviceNotFound);
    }
    return Error::none();
  }

  ComPtr<IMMDeviceCollection> devices;
  if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED,
                                            devices.put()))) {
    return target(kDeviceNotFound);
  }
  if (spec.name.empty()) {
    if (FAILED(devices->Item(static_cast<UINT>(spec.index), out))) return target(kDeviceNotFound);
    return Error::none();
  }

  const std::wstring target_name = from_utf8(spec.name);
  UINT count = 0;
  (void)devices->GetCount(&count);
  int countdown = spec.index;
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IMMDevice> candidate;
    if (FAILED(devices->Item(index, candidate.put()))) continue;
    const std::wstring name = device_friendly_name(candidate.get());
    if (name.size() < target_name.size()) continue;
    if (_wcsnicmp(name.c_str(), target_name.c_str(), target_name.size()) != 0) continue;
    if (countdown-- != 0) continue;
    *out = candidate.detach();
    return Error::none();
  }
  return target(kDeviceNotFound);
}

}  // namespace

// The walk itself is defined at namespace scope because sound_topology.hpp
// declares it there for the topology test; everything around it stays in the
// anonymous namespace, since only the SoundService entry points are public.
// lib/sound.cpp:163-251: the connector rule `part_count == 1` ("ignore
// connectors with no subunits of their own") and the sibling suppression
// after a subunit that had siblings are both AHK's, because they decide
// which of several components answers to a bare instance number.
bool walk_parts(IPart* root, ComponentSearch& search) {
  ComPtr<IPartsList> parts;
  const HRESULT listed = (search.flow == In) ? root->EnumPartsIncoming(parts.put())
                                             : root->EnumPartsOutgoing(parts.put());
  if (FAILED(listed)) return false;
  UINT part_count = 0;
  if (FAILED(parts->GetCount(&part_count))) part_count = 0;

  const bool check_name = !search.spec.name.empty();
  const std::wstring wanted = check_name ? from_utf8(search.spec.name) : std::wstring();

  for (UINT index = 0; index < part_count; ++index) {
    ComPtr<IPart> part;
    if (FAILED(parts->GetPart(index, part.put()))) continue;
    PartType type = Connector;
    if (FAILED(part->GetPartType(&type))) continue;

    if (type == Connector) {
      if (part_count != 1) continue;
      if (check_name) {
        LPWSTR raw = nullptr;
        if (FAILED(part->GetName(&raw))) continue;
        const bool matches = raw != nullptr && _wcsicmp(raw, wanted.c_str()) == 0;
        CoTaskMemFree(raw);
        if (!matches) continue;
      }
      if (++search.count != search.spec.instance) continue;
      if (search.kind == ControlKind::Name) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(part->GetName(&raw)) && raw != nullptr) {
          search.name = to_utf8(raw);
          CoTaskMemFree(raw);
        }
      }
      // A connector carries no volume or mute control; the caller reports
      // that as "Component doesn't support this control type" exactly as AHK
      // does when search.control stays null.
      return true;
    }

    if (!walk_parts(part.get(), search)) continue;
    if (!search.control && !search.ignore_remaining_subunits &&
        search.kind != ControlKind::Name) {
      const IID wanted_iid = search.kind == ControlKind::Volume ? __uuidof(IAudioVolumeLevel)
                                                                : __uuidof(IAudioMute);
      void* activated = nullptr;
      if (SUCCEEDED(part->Activate(CLSCTX_ALL, wanted_iid, &activated)) && activated != nullptr) {
        search.control.reset(static_cast<IUnknown*>(activated));
      }
      if (part_count > 1) search.ignore_remaining_subunits = true;
    }
    return true;
  }
  return false;
}

namespace {

// lib/sound.cpp:254-286. "Component not found" is raised here, so a caller

// only has to decide between that error and "the component exists but carries
// no such control" - the two AHK errors are different.
Error resolve_component(IMMDevice* device, ComponentSearch& search) {
  search.count = 0;
  search.control.reset();
  search.name.clear();
  search.ignore_remaining_subunits = false;

  ComPtr<IDeviceTopology> topology;
  if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(topology.put())))) {
    return target(kComponentNotFound);
  }
  ComPtr<IConnector> connector;
  if (FAILED(topology->GetConnector(0, connector.put()))) return target(kComponentNotFound);
  DataFlow flow = Out;
  if (FAILED(connector->GetDataFlow(&flow))) return target(kComponentNotFound);
  search.flow = flow;
  ComPtr<IConnector> connected;
  if (FAILED(connector->GetConnectedTo(connected.put()))) return target(kComponentNotFound);
  ComPtr<IPart> root;
  if (FAILED(connected->QueryInterface(root.put()))) return target(kComponentNotFound);

  (void)walk_parts(root.get(), search);
  if (search.count != search.spec.instance) return target(kComponentNotFound);
  return Error::none();
}

// The scalar conversion AHK applies to a topology volume control: dB to a
// 0..1 scalar against the control's own range, per channel
// (lib/sound.cpp:457-470). `max_level` is what Windows reports as the
// overall volume and what a read returns.
struct ChannelLevels {
  std::vector<double> scalar;
  std::vector<double> level_min;
  std::vector<double> level_range;
  double max_level{0};
};

Error read_channels(IAudioVolumeLevel* level, ChannelLevels& out) {
  UINT channels = 0;
  if (FAILED(level->GetChannelCount(&channels)) || channels == 0) {
    return hresult_error("volume channel count", E_INVALIDARG);
  }
  out.scalar.assign(channels, 0.0);
  out.level_min.assign(channels, 0.0);
  out.level_range.assign(channels, 0.0);
  for (UINT index = 0; index < channels; ++index) {
    float db = 0;
    float min_db = 0;
    float max_db = 0;
    float unused = 0;
    if (FAILED(level->GetLevel(index, &db)) ||
        FAILED(level->GetLevelRange(index, &min_db, &max_db, &unused))) {
      return hresult_error("volume level", E_FAIL);
    }
    const double minimum = std::pow(10.0, min_db / 20.0);
    const double range = std::pow(10.0, max_db / 20.0) - minimum;
    out.level_min[index] = minimum;
    out.level_range[index] = range;
    out.scalar[index] =
        range == 0 ? 0.0 : (std::pow(10.0, db / 20.0) - minimum) / range;
    out.max_level = (std::max)(out.max_level, out.scalar[index]);
  }
  return Error::none();
}

}  // namespace

bool SoundService::parse_volume_setting(const std::string& raw, VolumeSetting& out) {
  // AHK's gate is IsNumeric(aSetting, TRUE, FALSE, TRUE) followed by ATOF
  // (util.cpp:326-436, called from lib/sound.cpp:300-302): spaces and tabs
  // are trimmed at both ends, a sign, a decimal point, an exponent and a 0x
  // prefix are all legal, a suffix that is not part of the number rejects the
  // string, and a blank string is rejected because aAllowAllWhitespace is
  // FALSE. The character whitelist only has to be a superset of what strtod
  // accepts - strtod's full-string consumption is the actual check, and it is
  // what rejects "0x", "1e", "1.2.3" and "50abc" the way AHK does.
  std::size_t begin = 0;
  while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t')) ++begin;
  std::size_t finish = raw.size();
  while (finish > begin && (raw[finish - 1] == ' ' || raw[finish - 1] == '\t')) --finish;
  const std::string text = raw.substr(begin, finish - begin);
  if (text.empty()) return false;

  for (const char symbol : text) {
    const unsigned char byte = static_cast<unsigned char>(symbol);
    const bool hex_letter = (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
    if (!(std::isdigit(byte) || hex_letter || symbol == '+' || symbol == '-' || symbol == '.' ||
          symbol == 'e' || symbol == 'E' || symbol == 'x' || symbol == 'X')) {
      return false;
    }
  }

  const char* const first = text.c_str();
  char* parsed = nullptr;
  const double value = std::strtod(first, &parsed);
  if (parsed != first + text.size()) return false;
  if (std::isnan(value)) return false;

  // ATOF keeps its result in a float (lib/sound.cpp:331) and clamps it to
  // [-1, 1], so an overflow like "1e999" becomes 1 (or -1) rather than an
  // error - clamp first, exactly on the /100 scalar.
  double scalar = value / 100.0;
  if (scalar < -1) {
    scalar = -1;
  } else if (scalar > 1) {
    scalar = 1;
  }
  out.scalar = scalar;
  // Decided on the raw first character, not the trimmed one: the BIF reads
  // *aSetting (lib/sound.cpp:335-336), so " +5" is an absolute 5 in AHK too.
  out.adjust = raw[0] == '+' || raw[0] == '-';
  return true;
}

SoundService::ComponentSpec SoundService::parse_component(const std::string& raw) {
  ComponentSpec out;
  if (raw.empty()) return out;  // master
  out.master = false;
  int instance = 0;
  if (parse_int_exact(raw, instance)) {
    out.name.clear();
    out.instance = instance;
    return out;
  }
  const std::size_t colon = raw.rfind(':');
  if (colon == std::string::npos) {
    out.name = raw;
    out.instance = 1;
    return out;
  }
  out.name = raw.substr(0, colon);
  // AHK runs ATOI on the tail, so a non-numeric tail means instance 0, which
  // matches nothing - kept rather than "helpfully" defaulting to 1.
  const std::string tail = raw.substr(colon + 1);
  out.instance = std::atoi(tail.c_str());
  return out;
}

SoundService::DeviceSpec SoundService::parse_device(const std::string& raw) {
  DeviceSpec out;
  if (raw.empty()) return out;  // default render endpoint
  out.use_default = false;
  const std::size_t colon = raw.rfind(':');
  if (colon != std::string::npos) {
    out.name = raw.substr(0, colon);
    out.index = std::atoi(raw.c_str() + colon + 1) - 1;
    return out;
  }
  int index = 0;
  if (parse_int_exact(raw, index)) {
    out.name.clear();
    out.index = index - 1;  // AHK's 1-based device index
    return out;
  }
  out.name = raw;
  out.index = 0;
  return out;
}

Error SoundService::get_volume(const ComponentSpec& component, const DeviceSpec& device,
                               double& out_percent) {
  out_percent = 0;
  const ComApartment apartment(COINIT_MULTITHREADED);
  if (!apartment.ok()) return hresult_error("COM init", apartment.hr());

  ComPtr<IMMDevice> endpoint;
  if (const Error error = open_device(device, endpoint.put()); !error.ok()) return error;

  if (component.master) {
    ComPtr<IAudioEndpointVolume> volume;
    HRESULT hr = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(volume.put()));
    if (FAILED(hr)) return hresult_error("activate endpoint volume", hr);
    float scalar = 0;
    if (FAILED(volume->GetMasterVolumeLevelScalar(&scalar))) {
      return hresult_error("read master volume", E_FAIL);
    }
    out_percent = static_cast<double>(scalar) * 100.0;
    return Error::none();
  }

  ComponentSearch search;
  search.spec = component;
  search.kind = ControlKind::Volume;
  if (const Error error = resolve_component(endpoint.get(), search); !error.ok()) return error;
  if (!search.control) return wrong_control_type();

  auto* level = reinterpret_cast<IAudioVolumeLevel*>(search.control.get());
  ChannelLevels channels;
  if (const Error error = read_channels(level, channels); !error.ok()) return error;
  out_percent = channels.max_level * 100.0;
  return Error::none();
}

Error SoundService::set_volume(const VolumeSetting& setting, const ComponentSpec& component,
                               const DeviceSpec& device) {
  const ComApartment apartment(COINIT_MULTITHREADED);
  if (!apartment.ok()) return hresult_error("COM init", apartment.hr());

  ComPtr<IMMDevice> endpoint;
  if (const Error error = open_device(device, endpoint.put()); !error.ok()) return error;

  if (component.master) {
    ComPtr<IAudioEndpointVolume> volume;
    HRESULT hr = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(volume.put()));
    if (FAILED(hr)) return hresult_error("activate endpoint volume", hr);
    float scalar = static_cast<float>(setting.scalar);
    if (setting.adjust) {
      float current = 0;
      if (FAILED(volume->GetMasterVolumeLevelScalar(&current))) {
        return hresult_error("read master volume", E_FAIL);
      }
      scalar = static_cast<float>(setting.scalar + current);
      if (scalar > 1) scalar = 1;
      if (scalar < 0) scalar = 0;
    }
    if (FAILED(volume->SetMasterVolumeLevelScalar(scalar, nullptr))) {
      return hresult_error("write master volume", E_FAIL);
    }
    return Error::none();
  }

  ComponentSearch search;
  search.spec = component;
  search.kind = ControlKind::Volume;
  if (const Error error = resolve_component(endpoint.get(), search); !error.ok()) return error;
  if (!search.control) return wrong_control_type();

  auto* level = reinterpret_cast<IAudioVolumeLevel*>(search.control.get());
  ChannelLevels channels;
  if (const Error error = read_channels(level, channels); !error.ok()) return error;

  double target_scalar = setting.scalar;
  if (setting.adjust) {
    target_scalar += channels.max_level;
    if (target_scalar > 1) target_scalar = 1;
    if (target_scalar < 0) target_scalar = 0;
  }

  const UINT count = static_cast<UINT>(channels.scalar.size());
  std::vector<float> db(count, 0.0f);
  for (UINT index = 0; index < count; ++index) {
    double fraction = target_scalar;
    if (channels.max_level != 0) {
      fraction *= channels.scalar[index] / channels.max_level;  // preserve balance
    }
    double amplitude = channels.level_min[index] + fraction * channels.level_range[index];
    if (amplitude <= 0) amplitude = channels.level_min[index];
    db[index] = static_cast<float>(20.0 * std::log10(amplitude));
  }
  if (FAILED(level->SetLevelAllChannels(db.data(), count, nullptr))) {
    return hresult_error("write volume levels", E_FAIL);
  }
  return Error::none();
}

Error SoundService::get_mute(const ComponentSpec& component, const DeviceSpec& device,
                             bool& out_muted) {
  out_muted = false;
  const ComApartment apartment(COINIT_MULTITHREADED);
  if (!apartment.ok()) return hresult_error("COM init", apartment.hr());

  ComPtr<IMMDevice> endpoint;
  if (const Error error = open_device(device, endpoint.put()); !error.ok()) return error;

  if (component.master) {
    ComPtr<IAudioEndpointVolume> volume;
    HRESULT hr = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(volume.put()));
    if (FAILED(hr)) return hresult_error("activate endpoint volume", hr);
    BOOL muted = FALSE;
    if (FAILED(volume->GetMute(&muted))) return hresult_error("read master mute", E_FAIL);
    out_muted = muted != FALSE;
    return Error::none();
  }

  ComponentSearch search;
  search.spec = component;
  search.kind = ControlKind::Mute;
  if (const Error error = resolve_component(endpoint.get(), search); !error.ok()) return error;
  if (!search.control) return wrong_control_type();

  auto* mute = reinterpret_cast<IAudioMute*>(search.control.get());
  BOOL muted = FALSE;
  if (FAILED(mute->GetMute(&muted))) return hresult_error("read mute", E_FAIL);
  out_muted = muted != FALSE;
  return Error::none();
}

Error SoundService::set_mute(bool muted, const ComponentSpec& component, const DeviceSpec& device) {
  const ComApartment apartment(COINIT_MULTITHREADED);
  if (!apartment.ok()) return hresult_error("COM init", apartment.hr());

  ComPtr<IMMDevice> endpoint;
  if (const Error error = open_device(device, endpoint.put()); !error.ok()) return error;

  if (component.master) {
    ComPtr<IAudioEndpointVolume> volume;
    HRESULT hr = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(volume.put()));
    if (FAILED(hr)) return hresult_error("activate endpoint volume", hr);
    if (FAILED(volume->SetMute(muted ? TRUE : FALSE, nullptr))) {
      return hresult_error("write master mute", E_FAIL);
    }
    return Error::none();
  }

  ComponentSearch search;
  search.spec = component;
  search.kind = ControlKind::Mute;
  if (const Error error = resolve_component(endpoint.get(), search); !error.ok()) return error;
  if (!search.control) return wrong_control_type();

  auto* mute = reinterpret_cast<IAudioMute*>(search.control.get());
  if (FAILED(mute->SetMute(muted ? TRUE : FALSE, nullptr))) {
    return hresult_error("write mute", E_FAIL);
  }
  return Error::none();
}

Error SoundService::get_name(const ComponentSpec& component, const DeviceSpec& device,
                             std::string& out_name) {
  out_name.clear();
  const ComApartment apartment(COINIT_MULTITHREADED);
  if (!apartment.ok()) return hresult_error("COM init", apartment.hr());

  ComPtr<IMMDevice> endpoint;
  if (const Error error = open_device(device, endpoint.put()); !error.ok()) return error;

  if (component.master) {
    out_name = to_utf8(device_friendly_name(endpoint.get()));
    return Error::none();
  }

  ComponentSearch search;
  search.spec = component;
  search.kind = ControlKind::Name;
  if (const Error error = resolve_component(endpoint.get(), search); !error.ok()) return error;
  out_name = search.name;
  return Error::none();
}

}  // namespace rime::win32
