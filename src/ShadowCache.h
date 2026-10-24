// Copyright (c) 2026 Noud. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fps {
// Static shadow cache (StaticShadowCache): whether a shadow pass draws a caster that may have moved,
// so it must draw on every frame rather than come from the cached slice. Layout (Ghidra Combined,
// 2026-10-23): BSRenderPass +0x10 shader property, +0x18 BSGeometry; NiAVObject world +0x70 and
// previous world +0xC0 (UpdateWorldData copies world into previous world before it updates);
// BSGeometry skin instance +0x140; shader property flags +0x30. Anything unreadable counts as moving.
inline constexpr std::size_t passProperty = 0x10, passGeometry = 0x18;
inline constexpr std::size_t geometryWorld = 0x70, geometryPreviousWorld = 0xC0, transformBytes = 0x40;
inline constexpr std::size_t geometrySkin = 0x140, propertyFlagsOffset = 0x30;

inline bool MovingCaster(const std::byte* pass, std::uint64_t skinnedFlag) noexcept {
    if (!pass) return true;
    const std::byte* geometry{};
    std::memcpy(&geometry, pass + passGeometry, sizeof(geometry));
    if (!geometry) return true;
    const void* skin{};
    std::memcpy(&skin, geometry + geometrySkin, sizeof(skin));
    if (skin) return true;
    const std::byte* property{};
    std::memcpy(&property, pass + passProperty, sizeof(property));
    if (property) {
        std::uint64_t flags{};
        std::memcpy(&flags, property + propertyFlagsOffset, sizeof(flags));
        if (flags & skinnedFlag) return true;
    }
    return std::memcmp(geometry + geometryWorld, geometry + geometryPreviousWorld, transformBytes) != 0;
}
}  // namespace fps
