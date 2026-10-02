#include "test_support.hpp"

namespace {

// A static initializer, not a main() call: the suppression must be active
// before any test code runs, and linking this object into every test
// executable keeps individual mains free of setup boilerplate.
struct KeepFailuresNonInteractive {
  KeepFailuresNonInteractive() { rime::test::keep_failures_non_interactive(); }
};

KeepFailuresNonInteractive g_keep_failures_non_interactive;

}  // namespace
