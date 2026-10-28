#include "ui/theme.h"

#include <cctype>
#include <fstream>
#include <string>
#include <system_error>

namespace app {
namespace {

ftxui::Color rgb(unsigned red, unsigned green, unsigned blue) {
    return ftxui::Color::RGB(static_cast<std::uint8_t>(red),
                             static_cast<std::uint8_t>(green),
                             static_cast<std::uint8_t>(blue));
}

std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

}  // namespace

const std::array<ThemeName, 4>& available_themes() {
    static const std::array<ThemeName, 4> themes = {
        ThemeName::Selenized,
        ThemeName::Solarized,
        ThemeName::Nord,
        ThemeName::Catppuccin,
    };
    return themes;
}

std::string_view theme_name(ThemeName theme) {
    switch (theme) {
        case ThemeName::Selenized:
            return "Selenized";
        case ThemeName::Solarized:
            return "Solarized";
        case ThemeName::Nord:
            return "Nord";
        case ThemeName::Catppuccin:
            return "Catppuccin";
    }
    return "Selenized";
}

std::optional<ThemeName> parse_theme_name(std::string_view name) {
    const auto normalized = lowercase(name);
    for (const auto theme : available_themes()) {
        if (lowercase(theme_name(theme)) == normalized) {
            return theme;
        }
    }
    return std::nullopt;
}

ThemePalette theme_palette(ThemeName theme) {
    switch (theme) {
        case ThemeName::Selenized:
            return {
                rgb(16, 60, 72), rgb(173, 188, 188), rgb(114, 137, 143), rgb(70, 149, 247),
                rgb(242, 117, 190), rgb(65, 199, 185), rgb(117, 185, 56), rgb(114, 137, 143),
                rgb(219, 179, 45), rgb(65, 199, 185), rgb(219, 179, 45), rgb(70, 149, 247),
                rgb(117, 185, 56), rgb(250, 87, 80), rgb(24, 73, 86), rgb(224, 240, 240),
                rgb(117, 185, 56), rgb(219, 179, 45), rgb(250, 87, 80),
            };
        case ThemeName::Solarized:
            return {
                rgb(0, 43, 54), rgb(131, 148, 150), rgb(88, 110, 117), rgb(38, 139, 210),
                rgb(211, 54, 130), rgb(42, 161, 152), rgb(133, 153, 0), rgb(88, 110, 117),
                rgb(181, 137, 0), rgb(42, 161, 152), rgb(181, 137, 0), rgb(38, 139, 210),
                rgb(133, 153, 0), rgb(220, 50, 47), rgb(7, 54, 66), rgb(253, 246, 227),
                rgb(133, 153, 0), rgb(181, 137, 0), rgb(220, 50, 47),
            };
        case ThemeName::Nord:
            return {
                rgb(46, 52, 64), rgb(216, 222, 233), rgb(97, 110, 136), rgb(136, 192, 208),
                rgb(180, 142, 173), rgb(143, 188, 187), rgb(163, 190, 140), rgb(97, 110, 136),
                rgb(235, 203, 139), rgb(136, 192, 208), rgb(235, 203, 139), rgb(129, 161, 193),
                rgb(163, 190, 140), rgb(191, 97, 106), rgb(59, 66, 82), rgb(236, 239, 244),
                rgb(163, 190, 140), rgb(235, 203, 139), rgb(191, 97, 106),
            };
        case ThemeName::Catppuccin:
            return {
                rgb(30, 30, 46), rgb(205, 214, 244), rgb(108, 112, 134), rgb(137, 180, 250),
                rgb(203, 166, 247), rgb(148, 226, 213), rgb(166, 227, 161), rgb(108, 112, 134),
                rgb(250, 179, 135), rgb(137, 220, 235), rgb(249, 226, 175), rgb(137, 180, 250),
                rgb(166, 227, 161), rgb(243, 139, 168), rgb(49, 50, 68), rgb(205, 214, 244),
                rgb(166, 227, 161), rgb(249, 226, 175), rgb(243, 139, 168),
            };
    }
    return theme_palette(ThemeName::Selenized);
}

std::optional<ThemeName> load_theme_preference(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (!input || !std::getline(input, value)) {
        return std::nullopt;
    }
    return parse_theme_name(value);
}

bool save_theme_preference(const std::filesystem::path& path, ThemeName theme) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output || !(output << theme_name(theme) << '\n')) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

}  // namespace app