#pragma once
#include "Settings.h"
namespace fps {
bool SupportedRuntime(const REL::Version& version);
bool InstallShadowHooks(const Settings& settings);
// Shadow-map registration entries exist only once the renderer has started.
void RetryRegisterHooks();
void ResetShadows();
}
