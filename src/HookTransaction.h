// Copyright (c) 2026 Noud. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

namespace fps {
struct HookOutcome {
    bool active{}, rollbackComplete{true}, protectionsRestored{true};
};
// Page restoration failure rolls back our pointer writes as well. Callbacks
// remain inert unless this entire transaction succeeds.
template<class Backend>
HookOutcome InstallTransaction(std::size_t count, Backend& backend) {
    if (!backend.Prepare()) return {false, true, backend.Restore()};
    std::size_t written = 0;
    while (written < count && backend.Replace(written)) ++written;
    const bool restored = backend.Restore();
    if (written == count && restored) return {true, true, true};
    while (written) backend.Rollback(--written);
    const bool finalRestore = backend.Restore();
    bool clean = true;
    for (std::size_t i = 0; i < count; ++i) clean = !backend.Owned(i) && clean;
    return {false, clean, finalRestore};
}
} // namespace fps
