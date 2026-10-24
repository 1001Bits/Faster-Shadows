#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>

namespace fps {
// DrawWorld's sun-cascade accumulate query, identical in OG (AccumulateSunShadowLight+0x31) and AE
// (MainRenderSetup+0x195): call CullingJob::QBatchEnabled; test al,al; je; cmp byte [rip+bShadows],0;
// je; mov rcx,[rip+St]; lea rdx,[rip+kCullingGroup]; mov rcx,[rcx+0x208] (the sun light).
struct SunAccumulateOperands { std::uintptr_t shadows, sceneNode, cullingGroup; };
inline constexpr std::size_t kSunAccumulateSiteBytes = 0x27;
inline std::optional<SunAccumulateOperands> DecodeSunAccumulateSite(const std::uint8_t* code, std::uintptr_t site) noexcept {
    const auto at = [code](std::size_t i, std::initializer_list<std::uint8_t> bytes) {
        std::size_t k = i;
        for (const auto byte : bytes) if (code[k++] != byte) return false;
        return true;
    };
    const auto rip = [code, site](std::size_t displacement, std::size_t next) {
        std::int32_t relative{};
        std::memcpy(&relative, code+displacement, sizeof(relative));
        return site+next+static_cast<std::uintptr_t>(static_cast<std::intptr_t>(relative));
    };
    if (!at(0x00, {0xE8}) || !at(0x05, {0x84, 0xC0, 0x74}) || !at(0x09, {0x80, 0x3D}) || code[0x0F] != 0x00 ||
        !at(0x10, {0x74}) || !at(0x12, {0x48, 0x8B, 0x0D}) || !at(0x19, {0x48, 0x8D, 0x15}) ||
        !at(0x20, {0x48, 0x8B, 0x89, 0x08, 0x02, 0x00, 0x00}))
        return std::nullopt;
    return SunAccumulateOperands{rip(0x0B, 0x10), rip(0x15, 0x19), rip(0x1C, 0x20)};
}
} // namespace fps
