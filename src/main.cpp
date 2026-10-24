#include "ShadowHooks.h"
namespace {
// One version for F4SEPlugin_Query (read by OG F4SE) and F4SEPlugin_Version (read by AE F4SE).
constexpr REL::Version kPluginVersion{1, 0, 0, 0};
void Message(F4SE::MessagingInterface::Message* message) {
    if (!message) return;
    switch (message->type) {
    case F4SE::MessagingInterface::kPostPostLoad:
        if (!fps::InstallShadowHooks(fps::LoadSettings()))
            logger::error("Shadow hook installation failed; remaining callbacks forward native work. See preceding rollback/protection diagnostics.");
        break;
    case F4SE::MessagingInterface::kPreLoadGame:
    case F4SE::MessagingInterface::kNewGame:
        fps::ResetShadows();
        fps::RetryRegisterHooks();
        break;
    case F4SE::MessagingInterface::kInputLoaded:
    case F4SE::MessagingInterface::kGameDataReady:
    case F4SE::MessagingInterface::kGameLoaded:
    case F4SE::MessagingInterface::kPostLoadGame:
        fps::RetryRegisterHooks();
        break;
    default:
        break;
    }
}
}
extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* extender, F4SE::PluginInfo* info) {
    if (!extender || !info) return false;
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = "FasterShadows";
    info->version = kPluginVersion.pack();
    return !extender->IsEditor();
}
extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* extender) {
    if (!extender) return false;
    // An unsupported runtime returns before F4SE::Init. The log comes after Init on purpose:
    // its folder (My Games/<save folder>/F4SE) is only known once Init has run.
    const auto version = REL::Module::get().version();
    if (!fps::SupportedRuntime(version)) return false;
    F4SE::Init(extender, false);
    auto directory = F4SE::log::log_directory();
    if (!directory) return false;
    auto log = std::make_shared<spdlog::logger>("FPS", std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        (*directory / "FasterShadows.log").string(), true));
    spdlog::set_default_logger(log);
    spdlog::set_pattern("[%H:%M:%S] [%l] %v");
    spdlog::flush_on(spdlog::level::info);
    logger::info("Faster Shadows {}.{}; runtime {}", kPluginVersion.major(), kPluginVersion.minor(), version.string("."sv));
    auto* messages = F4SE::GetMessagingInterface();
    return messages && messages->RegisterListener(Message);
}
F4SE_EXPORT auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};
    data.PluginName("FasterShadows"sv);
    data.PluginVersion(kPluginVersion);
    data.AuthorName("Noud; shadow policy by dnai92"sv);
    // CommonLib's F4SE::Init initializes its address database. Our hook sites
    // additionally use exact-version RVA profiles with instruction fingerprints.
    data.UsesAddressLibrary(true);
    // The current AE extender introduced a new address-library bit. Retain
    // both dependency declarations, but require the explicit runtime list:
    // no blanket structure-layout compatibility is claimed.
    data.addressIndependence |= 1 << 2;
    data.UsesSigScanning(false);
    data.IsLayoutDependent(false);
    data.HasNoStructUse(false);
    data.CompatibleVersions({REL::Version{1,10,163,0}, REL::Version{1,10,984,0}, REL::Version{1,11,240,0}});
    return data;
}();
