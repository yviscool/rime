// Realism: L5 - production JS wiring (rime:screen module, real ScreenService)
// reads the machine's own displays, and the numbers the JS layer reports are
// compared against a native read taken before the runtime started rather than
// against the JS layer's own arithmetic. Nothing is mutated: a query dispatches
// no Action, which the empty trace proves. A second capability-less runtime
// proves the gate.

#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_screen.hpp"
#include "rime/win32/screen.hpp"
#include "rime/win32/screen_seam.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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
          "screen.monitorCount().then(() => {}, e => { globalThis.countDenied = String(e); });\n"
          "screen.monitor().then(() => {}, e => { globalThis.monitorDenied = String(e); });\n"
          "screen.pixel(0, 0).then(() => {}, e => { globalThis.pixelDenied = String(e); });\n"
          "screen.pixelSearch({left: 0, top: 0, right: 1, bottom: 1}, 0)\n"
          "  .then(() => {}, e => { globalThis.searchDenied = String(e); });",
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
          "  throw new Error('pixelSearch must name the capability: ' + globalThis.searchDenied);",
          "screen-deny-check.mjs");
    assert(denied_runtime.stop().ok());
  }
  return 0;
}
