// Realism: L5 - production JS wiring (rime:screen module, real ScreenService)
// reads the machine's own displays, and the numbers the JS layer reports are
// compared against a native read taken before the runtime started rather than
// against the JS layer's own arithmetic. Nothing is mutated: a query dispatches
// no Action, which the empty trace proves. A second capability-less runtime
// proves the gate. ImageSearch runs against the same synthetic screen with a
// BMP needle this test writes itself, so its expected corner is known before
// the file decoder reads it. Caret is the one read that needs the desktop
// arranged first: this test gives the process a foreground window with a
// caret at a known offset and asserts the JS layer reports that point.

#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_screen.hpp"
#include "rime/win32/screen.hpp"
#include "rime/win32/screen_seam.hpp"
#include "rime/win32/window.hpp"
#include "../screen_image_fixture.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::ScreenModuleBinding;
using rime::win32::ScreenService;

// Evaluates an assertion script; failures abort with the engine's message.
void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// Key paths and device names travel into JS as JSON strings so backslashes
// survive both the C++ literal and the JS source.
std::string json_string(const std::string& text) {
  return rime::core::json::stringify(rime::core::json::Value::string(text));
}

std::string number(const int value) { return std::to_string(value); }

// The synthetic screen the pixel contract runs against: a pixel's red channel
// carries its absolute x, its green channel its absolute y and its blue
// channel a constant, so a golden expectation names one coordinate and no
// other. Replacing the capture replaces the environment, never the code under
// test (AGENTS testing rules).
bool synthetic_capture(const RECT& bounds, std::vector<std::uint8_t>& bgra, int& width,
                       int& height) {
  width = bounds.right - bounds.left;
  height = bounds.bottom - bounds.top;
  if (width <= 0 || height <= 0) return false;
  bgra.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int absolute_x = bounds.left + x;
      const int absolute_y = bounds.top + y;
      const std::size_t offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4u;
      bgra[offset + 0] = 0x40u;  // B
      bgra[offset + 1] = static_cast<std::uint8_t>(absolute_y & 0xFF);  // G
      bgra[offset + 2] = static_cast<std::uint8_t>(absolute_x & 0xFF);  // R
      bgra[offset + 3] = 0xFFu;
    }
  }
  return true;
}

}  // namespace

int main() {
  ScreenService screen_service;
  // The pixel contract needs exact colors, which no live desktop can promise;
  // the monitor contract does not depend on it (geometry comes from
  // EnumDisplayMonitors either way).
  rime::win32::screen_seam::set_capture(synthetic_capture);

  // The ImageSearch needle is a 2x1 strip of the two pixels the synthetic
  // screen shows at (2, 3) and (3, 3), written as a BMP by this test so the
  // expected corner is known before any decoder reads the file.
  rime::test::TempImagePath needle_path;
  assert(needle_path.usable());
  assert(rime::test::write_bmp_24(needle_path.path(), 2, 1, {0x00020340, 0x00030340}));

  // Independent observation: the OS answers before the runtime exists, so a
  // JS result is never only self-reported.
  int native_count = 0;
  assert(screen_service.monitor_count(native_count).ok());
  assert(native_count >= 1);
  ScreenService::Monitor native_primary;
  assert(screen_service.monitor_at(0, native_primary).ok());
  assert(native_primary.primary);
  ScreenService::Monitor native_first;
  assert(screen_service.monitor_at(1, native_first).ok());

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
                                  std::unordered_set<std::string>{"screen.capture"}),
                              trace);
  ScreenModuleBinding binding{&screen_service, &kernel};

  rime::js::Runtime runtime;
  assert(rime::win32::register_screen_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  // Caret segment first: it is the only read that depends on the desktop's
  // foreground window, so its fixture is built before anything else runs.
  // Window and caret both live on WindowService's UI thread - a window owned
  // by this thread would never pump its queue, which shows up as an
  // unresponsive black box that the activation ladder blocks on.
  rime::win32::WindowService windows;
  assert(windows.start().ok());
  HWND caret_window = nullptr;
  assert(windows.ui()
             .call([&] {
               caret_window =
                   CreateWindowExW(0, L"STATIC", L"Rime Caret Slice Fixture",
                                   WS_POPUP | WS_VISIBLE, 40, 60, 320, 200, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
             })
             .ok());
  assert(caret_window != nullptr);
  std::vector<rime::win32::WindowInfo> listed;
  assert(windows.list(listed).ok());
  std::optional<std::uint64_t> caret_window_id;
  for (const auto& window : listed) {
    if (window.title.find("Rime Caret Slice Fixture") != std::string::npos) {
      caret_window_id = window.id;
    }
  }
  assert(caret_window_id.has_value());

  // Re-ask for the foreground against a deadline: the desktop can take it
  // back, and a failed assertion has to say so instead of weakening itself.
  rime::core::Error focus_result = rime::core::Error::none();
  bool focused = false;
  const auto focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    focus_result = windows.focus(*caret_window_id);
    focused = focus_result.ok();
    if (!focused) std::this_thread::sleep_for(50ms);
  } while (!focused && std::chrono::steady_clock::now() < focus_deadline);
  if (!focused) {
    std::fprintf(stderr, "caret fixture focus was never granted: %s - %s\n",
                 rime::core::error_code_name(focus_result.code), focus_result.message.c_str());
    std::fflush(stderr);
  }
  assert(focused);

  BOOL created_caret = FALSE;
  BOOL moved_caret = FALSE;
  BOOL shown_caret = FALSE;
  assert(windows.ui()
             .call([&] {
               created_caret = CreateCaret(caret_window, nullptr, 4, 16);
               moved_caret = created_caret ? SetCaretPos(5, 7) : FALSE;
               shown_caret = moved_caret ? ShowCaret(caret_window) : FALSE;
             })
             .ok());
  assert(created_caret && moved_caret && shown_caret);

  // Expected point = window position + the offset this test chose. The window
  // position comes from GetWindowRect, which observes the window rather than
  // the query under test; a popup has no non-client area, so its client
  // origin is its window rect.
  RECT fixture_rect{};
  assert(GetWindowRect(caret_window, &fixture_rect));
  const int expected_caret_x = fixture_rect.left + 5;
  const int expected_caret_y = fixture_rect.top + 7;
  assert(GetForegroundWindow() == caret_window);
  ScreenService::Caret native_caret = screen_service.caret();
  assert(native_caret.found);
  assert(native_caret.x == expected_caret_x && native_caret.y == expected_caret_y);

  check(runtime,
        "import { screen } from 'rime:screen';\n"
        "globalThis.caret = null;\n"
        "screen.caret().then(r => { globalThis.caret = r; }, e => { globalThis.caret = String(e); });",
        "screen-caret.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (typeof globalThis.caret === 'string') throw new Error(globalThis.caret);\n"
        "const c = globalThis.caret;\n"
        "if (c.found !== true)\n"
        "  throw new Error('the fixture caret must be reported: ' + JSON.stringify(c));\n"
        "if (JSON.stringify(Object.keys(c).sort()) !== JSON.stringify(['found', 'x', 'y']))\n"
        "  throw new Error('caret must carry found, x and y: ' + JSON.stringify(c));\n"
        "if (c.x !== " + number(expected_caret_x) + " || c.y !== " + number(expected_caret_y) + ")\n"
        "  throw new Error('caret must be the fixture point: ' + JSON.stringify(c));",
        "screen-caret-check.mjs");

  // Second half of the contract, at the JS layer: once the caret is gone the
  // same call resolves {found:false} with no coordinate at all, and our own
  // window is still the foreground one.
  BOOL hidden_caret = FALSE;
  BOOL destroyed_caret = FALSE;
  assert(windows.ui()
             .call([&] {
               hidden_caret = HideCaret(caret_window);
               destroyed_caret = DestroyCaret();
             })
             .ok());
  assert(hidden_caret && destroyed_caret);
  assert(GetForegroundWindow() == caret_window);
  check(runtime,
        "import { screen } from 'rime:screen';\n"
        "globalThis.noCaret = null;\n"
        "screen.caret().then(r => { globalThis.noCaret = r; }, e => { globalThis.noCaret = String(e); });",
        "screen-no-caret.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (typeof globalThis.noCaret === 'string') throw new Error(globalThis.noCaret);\n"
        "const n = globalThis.noCaret;\n"
        "if (n.found !== false)\n"
        "  throw new Error('no caret must resolve found:false: ' + JSON.stringify(n));\n"
        "if (JSON.stringify(Object.keys(n)) !== JSON.stringify(['found']))\n"
        "  throw new Error('a miss must carry no coordinate: ' + JSON.stringify(n));",
        "screen-no-caret-check.mjs");

  BOOL destroyed_window = FALSE;
  assert(windows.ui().call([&] { destroyed_window = DestroyWindow(caret_window); }).ok());
  assert(destroyed_window);
  assert(windows.stop().ok());

  // ---- SysGet / SysGetIPAddresses ------------------------------------------
  // Both answers are pinned by a native read taken before the JS layer runs,
  // which is what this slice verifies: the JS -> native wiring. The address
  // format is spelled out here instead of being borrowed from the service,
  // so a decoder that accepted anything would not satisfy these checks.
  const int native_metric = screen_service.system_metric(SM_CXSCREEN);
  assert(native_metric > 0);
  std::vector<std::string> native_addresses;
  assert(screen_service.ip_addresses(native_addresses).ok());
  assert(!native_addresses.empty());
  rime::core::json::Value expected_addresses = rime::core::json::Value::array();
  for (const std::string& address : native_addresses) {
    expected_addresses.push(rime::core::json::Value::string(address));
  }

  check(runtime,
        "import { screen } from 'rime:screen';\n"
        "globalThis.sysMetric = null;\n"
        "globalThis.sysBad = null;\n"
        "globalThis.ip = null;\n"
        "globalThis.sysTypeError = null;\n"
        "try { screen.sysGet('not a number'); } catch (e) { globalThis.sysTypeError = e.constructor.name; }\n"
        "screen.sysGet(0)\n"
        "  .then(r => { globalThis.sysMetric = r; }, e => { globalThis.sysMetric = String(e); });\n"
        "screen.sysGet(-1)\n"
        "  .then(r => { globalThis.sysBad = r; }, e => { globalThis.sysBad = String(e); });\n"
        "screen.sysGetIPAddresses()\n"
        "  .then(r => { globalThis.ip = r; }, e => { globalThis.ip = String(e); });",
        "screen-sysget.mjs");
  assert(runtime.settle(5000ms).ok());
  // The module answers with its own record shapes - `{metric}` and
  // `{addresses}` - because this slice exercises `rime:screen` itself; the
  // facade that unwraps them is pinned separately in tests/sdk/screen.test.ts.
  check(runtime,
        "if (globalThis.sysTypeError !== 'TypeError')\n"
        "  throw new Error('a non-numeric index must be a TypeError: ' +\n"
        "                   JSON.stringify(globalThis.sysTypeError));\n"
        "if (typeof globalThis.sysMetric === 'string') throw new Error(globalThis.sysMetric);\n"
        "const m = globalThis.sysMetric;\n"
        "if (JSON.stringify(Object.keys(m)) !== JSON.stringify(['metric']))\n"
        "  throw new Error('sysGet must resolve {metric}: ' + JSON.stringify(m));\n"
        "if (typeof m.metric !== 'number')\n"
        "  throw new Error('the metric must be a number: ' + JSON.stringify(m));\n"
        "if (m.metric !== " + number(native_metric) + ")\n"
        "  throw new Error('sysGet must report the metric the native read saw: ' +\n"
        "                   JSON.stringify(m));\n"
        "if (typeof globalThis.sysBad === 'string') throw new Error(globalThis.sysBad);\n"
        "if (JSON.stringify(globalThis.sysBad) !== JSON.stringify({metric: 0}))\n"
        "  throw new Error('an unknown index must resolve {metric:0}: ' +\n"
        "                   JSON.stringify(globalThis.sysBad));\n"
        "if (typeof globalThis.ip === 'string') throw new Error(globalThis.ip);\n"
        "const ip = globalThis.ip;\n"
        "if (JSON.stringify(Object.keys(ip)) !== JSON.stringify(['addresses']))\n"
        "  throw new Error('sysGetIPAddresses must resolve {addresses}: ' + JSON.stringify(ip));\n"
        "const list = ip.addresses;\n"
        "if (!Array.isArray(list))\n"
        "  throw new Error('addresses must be an array: ' + JSON.stringify(ip));\n"
        "const expected = " +
        rime::core::json::stringify(expected_addresses) + ";\n"
        "if (list.length !== expected.length)\n"
        "  throw new Error('the JS list must match the native read: ' + JSON.stringify(list));\n"
        "if (JSON.stringify([...list].sort()) !== JSON.stringify([...expected].sort()))\n"
        "  throw new Error('the JS list must hold the native addresses: ' + JSON.stringify(list));\n"
        "for (const entry of list) {\n"
        "  const groups = entry.split('.');\n"
        "  if (groups.length !== 4) throw new Error('four groups expected: ' + entry);\n"
        "  for (const group of groups) {\n"
        "    if (group.length === 0 || group.length > 3)\n"
        "      throw new Error('bad group in: ' + entry);\n"
        "    for (const symbol of group)\n"
        "      if (symbol < '0' || symbol > '9') throw new Error('bad digit in: ' + entry);\n"
        "    if (Number(group) > 255) throw new Error('octet out of range in: ' + entry);\n"
        "  }\n"
        "}\n"
        "if (list.indexOf('127.0.0.1') === -1)\n"
        "  throw new Error('the loopback address must be reported: ' + JSON.stringify(list));",
        "screen-sysget-check.mjs");

  // Segment 1: the count and the primary record both match the native read.
  check(runtime,
        "import { screen } from 'rime:screen';\n"
        "globalThis.count = null;\n"
        "globalThis.primary = null;\n"
        "screen.monitorCount()\n"
        "  .then(r => { globalThis.count = r.count; }, e => { globalThis.count = String(e); });\n"
        "screen.monitor()\n"
        "  .then(m => { globalThis.primary = m; }, e => { globalThis.primary = String(e); });",
        "screen-count.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (typeof globalThis.count === 'string') throw new Error(globalThis.count);\n"
        "if (globalThis.count !== " + number(native_count) + ")\n"
        "  throw new Error('monitorCount must equal the native count: ' + globalThis.count);\n"
        "if (typeof globalThis.primary === 'string') throw new Error(globalThis.primary);\n"
        "const p = globalThis.primary;\n"
        "if (p.primary !== true) throw new Error('an omitted index must select the primary');\n"
        "if (p.index !== " + number(native_primary.index) + ")\n"
        "  throw new Error('primary index mismatch: ' + p.index);\n"
        "if (p.bounds.left !== " + number(native_primary.left) +
            " || p.bounds.top !== " + number(native_primary.top) + " || p.bounds.right !== " +
            number(native_primary.right) + " || p.bounds.bottom !== " +
            number(native_primary.bottom) + ")\n"
            "  throw new Error('primary bounds mismatch: ' + JSON.stringify(p.bounds));\n"
            "if (p.work.left !== " + number(native_primary.work_left) +
            " || p.work.right !== " + number(native_primary.work_right) + ")\n"
            "  throw new Error('primary work area mismatch: ' + JSON.stringify(p.work));\n"
            "if (p.name !== " + json_string(native_primary.name) + ")\n"
            "  throw new Error('primary name mismatch: ' + p.name);",
        "screen-count-check.mjs");

  // Segment 2: explicit indexes, the primary asked for by number, and the
  // three argument shapes the module must reject synchronously.
  check(runtime,
        "import { screen } from 'rime:screen';\n"
        "globalThis.first = null;\n"
        "globalThis.last = null;\n"
        "globalThis.rangeCode = null;\n"
        "globalThis.syncErrors = [];\n"
        "screen.monitor(1)\n"
        "  .then(m => { globalThis.first = m; }, e => { globalThis.first = String(e); });\n"
        "screen.monitor(" + number(native_count) + ")\n"
        "  .then(m => { globalThis.last = m; }, e => { globalThis.last = String(e); });\n"
        "screen.monitor(" +
        number(native_count + 1) +
        ")\n"
        "  .then(() => { globalThis.rangeCode = 'resolved'; },\n"
        "        e => { globalThis.rangeCode = e.code; });\n"
        "try { screen.monitor(-1); globalThis.syncErrors.push('negative accepted'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }\n"
        "try { screen.monitor('one'); globalThis.syncErrors.push('string accepted'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }",
        "screen-index.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (typeof globalThis.first === 'string') throw new Error(globalThis.first);\n"
        "if (globalThis.first.index !== 1)\n"
        "  throw new Error('monitor(1) must report index 1: ' + globalThis.first.index);\n"
        "if (typeof globalThis.last === 'string') throw new Error(globalThis.last);\n"
        "if (globalThis.last.index !== " + number(native_count) + ")\n"
        "  throw new Error('monitor(count) must report the last index');\n"
        "if (globalThis.last.bounds.left !== " + number(native_first.left) +
        ") throw new Error('monitor(1) must report the first bounds');\n"
        "if (globalThis.rangeCode !== 'invalid_contract')\n"
        "  throw new Error('an unknown index must be invalid_contract, got ' +\n"
        "                   globalThis.rangeCode);\n"
        "const expected = ['RangeError', 'TypeError'];\n"
        "if (JSON.stringify(globalThis.syncErrors) !== JSON.stringify(expected))\n"
        "  throw new Error('argument rejections must be RangeError then TypeError, got ' +\n"
        "                   JSON.stringify(globalThis.syncErrors));",
        "screen-index-check.mjs");

  // Segment 3: the pixel family against the synthetic screen - exact golden
  // colors, a found coordinate, a miss as a result, and the contract errors
  // that keep a bad coordinate or color out of the worker lane.
  check(runtime,
        "import { screen } from 'rime:screen';\n"
        "globalThis.pixelColor = null;\n"
        "globalThis.pixelError = null;\n"
        "globalThis.hit = null;\n"
        "globalThis.miss = null;\n"
        "globalThis.wideHit = null;\n"
        "globalThis.searchError = null;\n"
        "globalThis.syncErrors = [];\n"
        "screen.pixel(3, 5)\n"
        "  .then(c => { globalThis.pixelColor = c; }, e => { globalThis.pixelError = String(e); });\n"
        "const area = { left: 0, top: 0, right: 7, bottom: 7 };\n"
        "screen.pixelSearch(area, 0x00020340)\n"
        "  .then(r => { globalThis.hit = r; }, e => { globalThis.hit = String(e); });\n"
        "screen.pixelSearch(area, 0x123456)\n"
        "  .then(r => { globalThis.miss = r; }, e => { globalThis.miss = String(e); });\n"
        "screen.pixelSearch({ left: 2, top: 3, right: 2, bottom: 3 }, 0x00020300,\n"
        "                    { variation: 64 })\n"
        "  .then(r => { globalThis.wideHit = r; }, e => { globalThis.wideHit = String(e); });\n"
        "screen.pixelSearch({ left: 10000000, top: 10000000, right: 10000001,\n"
        "                      bottom: 10000001 }, 0x00020340)\n"
        "  .then(() => {}, e => { globalThis.searchError = e.code + ':' + e.message; });\n"
        "try { screen.pixel(1.5, 0); globalThis.syncErrors.push('fraction accepted'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }\n"
        "try { screen.pixel(9999999999, 0); globalThis.syncErrors.push('huge accepted'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }\n"
        "try { screen.pixelSearch(area, 0x1000000); globalThis.syncErrors.push('big color'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }",
        "screen-pixel.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.pixelError) throw new Error(globalThis.pixelError);\n"
        "if (typeof globalThis.pixelColor === 'string')\n"
        "  throw new Error(globalThis.pixelColor);\n"
        "if (globalThis.pixelColor.color !== 0x00030540)\n"
        "  throw new Error('pixel(3, 5) must be the synthetic color: 0x' +\n"
        "                   globalThis.pixelColor.color.toString(16));\n"
        "if (typeof globalThis.hit === 'string') throw new Error(globalThis.hit);\n"
        "if (!globalThis.hit.found || globalThis.hit.x !== 2 || globalThis.hit.y !== 3)\n"
        "  throw new Error('search must find (2, 3): ' + JSON.stringify(globalThis.hit));\n"
        "if (typeof globalThis.miss === 'string') throw new Error(globalThis.miss);\n"
        "if (globalThis.miss.found !== false)\n"
        "  throw new Error('a miss must resolve {found:false}: ' +\n"
        "                   JSON.stringify(globalThis.miss));\n"
        "if ('x' in globalThis.miss)\n"
        "  throw new Error('a miss must not carry coordinates: ' +\n"
        "                   JSON.stringify(globalThis.miss));\n"
        "if (typeof globalThis.wideHit === 'string') throw new Error(globalThis.wideHit);\n"
        "if (!globalThis.wideHit.found || globalThis.wideHit.x !== 2 ||\n"
        "    globalThis.wideHit.y !== 3)\n"
        "  throw new Error('variation 64 must widen the match: ' +\n"
        "                   JSON.stringify(globalThis.wideHit));\n"
        "if (!globalThis.searchError || !globalThis.searchError.includes('intersect'))\n"
        "  throw new Error('an area outside the screen must be invalid_contract: ' +\n"
        "                   globalThis.searchError);\n"
        "const expected = ['TypeError', 'RangeError', 'RangeError'];\n"
        "if (JSON.stringify(globalThis.syncErrors) !== JSON.stringify(expected))\n"
        "  throw new Error('argument rejections must be TypeError, RangeError, RangeError: ' +\n"
        "                   JSON.stringify(globalThis.syncErrors));",
        "screen-pixel-check.mjs");

  // Segment 4: ImageSearch over the same synthetic screen - the fixture needle
  // fits only at (2, 3), two rectangles cannot hold it, a file that does not
  // exist fails as the caller's contract, and the three argument shapes the
  // module rejects synchronously.
  check(runtime,
        "import { screen } from 'rime:screen';\n"
        "globalThis.syncErrors = [];\n"
        "globalThis.imageHit = null;\n"
        "globalThis.imageMiss = null;\n"
        "globalThis.imageTooSmall = null;\n"
        "globalThis.imageFileError = null;\n"
        "globalThis.imageAreaError = null;\n"
        "const needle = " + json_string(needle_path.path()) + ";\n"
        "const area = { left: 0, top: 0, right: 7, bottom: 7 };\n"
        "screen.imageSearch(area, needle)\n"
        "  .then(r => { globalThis.imageHit = r; }, e => { globalThis.imageHit = String(e); });\n"
        "screen.imageSearch({ left: 0, top: 0, right: 1, bottom: 7 }, needle)\n"
        "  .then(r => { globalThis.imageMiss = r; }, e => { globalThis.imageMiss = String(e); });\n"
        "screen.imageSearch({ left: 0, top: 0, right: 0, bottom: 7 }, needle)\n"
        "  .then(r => { globalThis.imageTooSmall = r; },\n"
        "        e => { globalThis.imageTooSmall = String(e); });\n"
        "screen.imageSearch(area, needle + '.missing')\n"
        "  .then(() => {}, e => { globalThis.imageFileError = e.code + ':' + e.message; });\n"
        "screen.imageSearch({ left: 10000000, top: 10000000, right: 10000001,\n"
        "                     bottom: 10000001 }, needle)\n"
        "  .then(() => {}, e => { globalThis.imageAreaError = e.code + ':' + e.message; });\n"
        "try { screen.imageSearch(area, 42); globalThis.syncErrors.push('path accepted'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }\n"
        "try { screen.imageSearch(area, needle, 'x'); globalThis.syncErrors.push('options'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }\n"
        "try { screen.imageSearch(area, needle, { variation: 300 });\n"
        "      globalThis.syncErrors.push('variation'); }\n"
        "catch (e) { globalThis.syncErrors.push(e.constructor.name); }",
        "screen-image.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (typeof globalThis.imageHit === 'string') throw new Error(globalThis.imageHit);\n"
        "if (!globalThis.imageHit.found || globalThis.imageHit.x !== 2 ||\n"
        "    globalThis.imageHit.y !== 3)\n"
        "  throw new Error('the needle must fit at (2, 3): ' +\n"
        "                   JSON.stringify(globalThis.imageHit));\n"
        "if (typeof globalThis.imageMiss === 'string') throw new Error(globalThis.imageMiss);\n"
        "if (globalThis.imageMiss.found !== false)\n"
        "  throw new Error('an area without the pixels must miss: ' +\n"
        "                   JSON.stringify(globalThis.imageMiss));\n"
        "if ('x' in globalThis.imageMiss)\n"
        "  throw new Error('a miss must not carry coordinates: ' +\n"
        "                   JSON.stringify(globalThis.imageMiss));\n"
        "if (typeof globalThis.imageTooSmall === 'string')\n"
        "  throw new Error(globalThis.imageTooSmall);\n"
        "if (globalThis.imageTooSmall.found !== false)\n"
        "  throw new Error('an area narrower than the needle must miss: ' +\n"
        "                   JSON.stringify(globalThis.imageTooSmall));\n"
        "if (!globalThis.imageFileError ||\n"
        "    !globalThis.imageFileError.includes('could not be loaded'))\n"
        "  throw new Error('a missing file must be invalid_contract: ' +\n"
        "                   globalThis.imageFileError);\n"
        "if (!globalThis.imageAreaError || !globalThis.imageAreaError.includes('intersect'))\n"
        "  throw new Error('an area outside the screen must be invalid_contract: ' +\n"
        "                   globalThis.imageAreaError);\n"
        "const expected = ['TypeError', 'TypeError', 'RangeError'];\n"
        "if (JSON.stringify(globalThis.syncErrors) !== JSON.stringify(expected))\n"
        "  throw new Error('argument rejections must be TypeError, TypeError, RangeError: ' +\n"
        "                   JSON.stringify(globalThis.syncErrors));",
        "screen-image-check.mjs");

  // A query dispatches no Action, so the trace stays empty across every call
  // above.
  assert(trace->snapshot().empty());

  rime::win32::screen_seam::set_capture(nullptr);
  assert(runtime.stop().ok());

  // Capability gate: an empty policy rejects the read by name. The JS lane is
  // process-wide, so this runtime only starts after the first one released it.
  {
    rime::action::Kernel denied_kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{}));
    ScreenModuleBinding denied_binding{&screen_service, &denied_kernel};
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_screen_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    check(denied_runtime,
          "import { screen } from 'rime:screen';\n"
          "globalThis.countDenied = null;\n"
          "globalThis.monitorDenied = null;\n"
          "globalThis.pixelDenied = null;\n"
          "globalThis.searchDenied = null;\n"
          "globalThis.imageDenied = null;\n"
          "globalThis.caretDenied = null;\n"
          "globalThis.sysDenied = null;\n"
          "globalThis.ipDenied = null;\n"
          "screen.monitorCount().then(() => {}, e => { globalThis.countDenied = String(e); });\n"
          "screen.monitor().then(() => {}, e => { globalThis.monitorDenied = String(e); });\n"
          "screen.pixel(0, 0).then(() => {}, e => { globalThis.pixelDenied = String(e); });\n"
          "screen.pixelSearch({left: 0, top: 0, right: 1, bottom: 1}, 0)\n"
          "  .then(() => {}, e => { globalThis.searchDenied = String(e); });\n"
          "screen.imageSearch({left: 0, top: 0, right: 1, bottom: 1}, 'x.bmp')\n"
          "  .then(() => {}, e => { globalThis.imageDenied = String(e); });\n"
          "screen.caret().then(() => {}, e => { globalThis.caretDenied = String(e); });\n"
          "screen.sysGet(0).then(() => {}, e => { globalThis.sysDenied = String(e); });\n"
          "screen.sysGetIPAddresses()\n"
          "  .then(() => {}, e => { globalThis.ipDenied = String(e); });",
          "screen-deny.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    check(denied_runtime,
          "if (!globalThis.countDenied ||\n"
          "    !globalThis.countDenied.includes('screen.capture'))\n"
          "  throw new Error('monitorCount must name the capability: ' +\n"
          "                   globalThis.countDenied);\n"
          "if (!globalThis.monitorDenied ||\n"
          "    !globalThis.monitorDenied.includes('screen.capture'))\n"
          "  throw new Error('monitor must name the capability: ' + globalThis.monitorDenied);\n"
          "if (!globalThis.pixelDenied ||\n"
          "    !globalThis.pixelDenied.includes('screen.capture'))\n"
          "  throw new Error('pixel must name the capability: ' + globalThis.pixelDenied);\n"
          "if (!globalThis.searchDenied ||\n"
          "    !globalThis.searchDenied.includes('screen.capture'))\n"
          "  throw new Error('pixelSearch must name the capability: ' + globalThis.searchDenied);\n"
          "if (!globalThis.imageDenied ||\n"
          "    !globalThis.imageDenied.includes('screen.capture'))\n"
          "  throw new Error('imageSearch must name the capability: ' + globalThis.imageDenied);\n"
          "if (!globalThis.caretDenied ||\n"
          "    !globalThis.caretDenied.includes('screen.capture'))\n"
          "  throw new Error('caret must name the capability: ' + globalThis.caretDenied);\n"
          "if (!globalThis.sysDenied ||\n"
          "    !globalThis.sysDenied.includes('screen.capture'))\n"
          "  throw new Error('sysGet must name the capability: ' + globalThis.sysDenied);\n"
          "if (!globalThis.ipDenied ||\n"
          "    !globalThis.ipDenied.includes('screen.capture'))\n"
          "  throw new Error('sysGetIPAddresses must name the capability: ' +\n"
          "                   globalThis.ipDenied);",
          "screen-deny-check.mjs");
    assert(denied_runtime.stop().ok());
  }
  return 0;
}
