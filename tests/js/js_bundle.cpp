// Realism: L6 - the production rime_js_bundle executable is spawned end to
// end and its full module export surface is asserted against the registry,
// so two independent consumers must agree on the declared API.

#include "rime/win32/bootstrap.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <unordered_set>
#include <utility>

int main(int argc, char** argv) {
  // Optional leading grant switch: `--production` swaps the demo grant for
  // production_capabilities() so a bundle can drive the write actions through
  // the real Kernel policy. Everything after it is unchanged, so the existing
  // `[bundle.js] [expected exit code]` calls keep their exact behaviour.
  int index = 1;
  std::unordered_set<std::string> capabilities = rime::win32::demo_capabilities();
  if (index < argc && std::string_view(argv[index]) == "--production") {
    capabilities = rime::win32::production_capabilities();
    ++index;
  }
  const int positional = argc - index;
  if (positional < 1 || positional > 2) {
    std::cerr << "usage: rime_js_bundle [--production] <bundle.js> [expected exit code]\n";
    return 2;
  }
  // The harness runs the exact production wiring (the same Bootstrap the
  // desktop host uses) under the demo grant the bundle documents: reads plus
  // the input hook, while mutations still fail the policy with a clear
  // reason. `--production` is the other grant the wiring already ships: every
  // implemented action capability, so executor-layer rejections are reachable
  // instead of being stopped by the policy.
  const int got = rime::win32::run_bundle_file(argv[index], std::move(capabilities));
  if (positional == 2) {
    // Optional expected runtime.exit() code: a mismatch fails the test, so
    // an unexpected 0/1 from a broken exit path cannot pass silently.
    const int expected = std::atoi(argv[index + 1]);
    if (got != expected) {
      std::fprintf(stderr, "rime_js_bundle: expected exit code %d, got %d\n", expected, got);
      return 1;
    }
    return 0;
  }
  return got;
}
