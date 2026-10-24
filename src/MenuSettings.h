#pragma once
#include <optional>
namespace fps::menu {
// The Mod Configuration Menu saves the player's choices to Data/MCM/Settings/FasterShadows.ini
// (its defaults stay in Data/MCM/Config/FasterShadows/settings.ini, never read here). A saved
// choice wins over the same option in FasterShadows.ini. Keys are MCM's: bName in the section
// the option has in FasterShadows.ini.
std::optional<bool> Saved(const wchar_t* section, const wchar_t* key) noexcept;
// Watches the settings file from a thread of its own: a file check under MO2's virtual file
// system costs milliseconds the frame must not pay. Changed() reports each new save once.
bool Watch() noexcept;
bool Changed() noexcept;
}
