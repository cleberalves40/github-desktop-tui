#pragma once

#include <ftxui/screen/color.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

namespace app {

enum class ThemeName {
    Selenized,
    Solarized,
    Nord,
    Catppuccin,
};

struct ThemePalette {
    ftxui::Color background;
    ftxui::Color foreground;
    ftxui::Color muted;
    ftxui::Color accent;
    ftxui::Color keyword;
    ftxui::Color type;
    ftxui::Color string;
    ftxui::Color comment;
    ftxui::Color number;
    ftxui::Color heading;
    ftxui::Color emphasis;
    ftxui::Color link;
    ftxui::Color added;
    ftxui::Color removed;
    ftxui::Color selection_background;
    ftxui::Color selection_foreground;
    ftxui::Color success;
    ftxui::Color warning;
    ftxui::Color error;
};

const std::array<ThemeName, 4>& available_themes();
std::string_view theme_name(ThemeName theme);
std::optional<ThemeName> parse_theme_name(std::string_view name);
ThemePalette theme_palette(ThemeName theme);
std::optional<ThemeName> load_theme_preference(const std::filesystem::path& path);
bool save_theme_preference(const std::filesystem::path& path, ThemeName theme);

}  // namespace app