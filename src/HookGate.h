#pragma once
#include <algorithm>
#include <cstdint>
#include <span>

namespace fps {
inline bool NativeHookSite(std::uintptr_t current, std::uintptr_t expected,
                           std::span<const std::uint8_t> actualCode,
                           std::span<const std::uint8_t> expectedCode) noexcept {
    return current == expected && actualCode.size() == expectedCode.size() &&
           std::equal(actualCode.begin(), actualCode.end(), expectedCode.begin());
}
}
