#pragma once

#include <optional>
#include <regex>
#include <string>

// Extracts an 11-character YouTube video ID from a bare ID or the usual URL
// shapes: watch?v=, youtu.be/, /shorts/, /embed/, /live/.
inline std::optional<std::string> ParseVideoId(const std::string& input) {
    static const std::regex bare(R"(^\s*([A-Za-z0-9_-]{11})\s*$)");
    static const std::regex url(R"((?:[?&]v=|youtu\.be/|/shorts/|/embed/|/live/)([A-Za-z0-9_-]{11})(?:[^A-Za-z0-9_-]|$))");
    std::smatch m;
    if (std::regex_search(input, m, bare) || std::regex_search(input, m, url))
        return m[1].str();
    return std::nullopt;
}
