#include "MenuSettings.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
namespace fps::menu {
namespace {
constexpr wchar_t path[] = L".\\Data\\MCM\\Settings\\FasterShadows.ini";
std::atomic<bool> changed{false};
std::filesystem::file_time_type WriteTime() noexcept {
    std::error_code ec;
    const auto when = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : when;
}
}
std::optional<bool> Saved(const wchar_t* section, const wchar_t* key) noexcept {
    const auto value = static_cast<int>(GetPrivateProfileIntW(section, key, -1, path));
    if (value < 0) return std::nullopt;
    return value != 0;
}
bool Watch() noexcept {
    try {
        std::thread([] {
            auto last = WriteTime();
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const auto now = WriteTime();
                std::error_code ec;
                // A save in progress can leave the file empty for a moment: look again a second later.
                if (now == last || std::filesystem::file_size(path, ec) == 0 || ec) continue;
                last = now;
                changed.store(true, std::memory_order_release);
            }
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}
bool Changed() noexcept { return changed.exchange(false, std::memory_order_acq_rel); }
}
