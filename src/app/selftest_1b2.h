// selftest_1b2.h — Hidden `--selftest` console entry point (stage 1B-2).
#pragma once

namespace pulse::app {

// Runs the 1B-2 console self-test (menu model, drag & drop data object,
// drop-effect semantics, breadcrumb split, ops-layer create/move through
// pulse_shell). Prints PASS/FAIL lines; returns 0 iff all pass.
int RunSelfTest1B2();

} // namespace pulse::app
