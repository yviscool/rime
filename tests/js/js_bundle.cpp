#include "rime/win32/bootstrap.hpp"

#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: rime_js_bundle <bundle.js>\n";
    return 2;
  }
  // The harness runs the exact production wiring (the same Bootstrap the
  // desktop host uses) under the demo grant the bundle documents: reads
  // plus the input hook, while mutations still fail the policy with a
  // clear reason.
  return rime::win32::run_bundle_file(argv[1], rime::win32::demo_capabilities());
}
