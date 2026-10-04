#include "rime/win32/bootstrap.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: rime_js_bundle <bundle.js> [expected exit code]\n";
    return 2;
  }
  // The harness runs the exact production wiring (the same Bootstrap the
  // desktop host uses) under the demo grant the bundle documents: reads
  // plus the input hook, while mutations still fail the policy with a
  // clear reason.
  const int got = rime::win32::run_bundle_file(argv[1], rime::win32::demo_capabilities());
  if (argc == 3) {
    // Optional expected runtime.exit() code: a mismatch fails the test, so
    // an unexpected 0/1 from a broken exit path cannot pass silently.
    const int expected = std::atoi(argv[2]);
    if (got != expected) {
      std::fprintf(stderr, "rime_js_bundle: expected exit code %d, got %d\n", expected, got);
      return 1;
    }
    return 0;
  }
  return got;
}
