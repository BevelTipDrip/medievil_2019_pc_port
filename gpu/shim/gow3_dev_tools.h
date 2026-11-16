// gow3: GOW3_DEV_TOOLS (gpu/CMakeLists.txt, study-tooling-off-cost.md step 2): 1 dev build (diagnostics
// compiled in, switched on by their variables), 0 release build (their switches compile-time false).
// CMake defines it for every target; a compile outside CMake counts as dev.
#pragma once
#ifndef GOW3_DEV_TOOLS
#define GOW3_DEV_TOOLS 1
#endif
