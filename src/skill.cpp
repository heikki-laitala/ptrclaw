#include "skill.hpp"
#include "util.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ptrclaw {

// Trim whitespace from both ends
static std::string trim_ws(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Parse a simple [a, b, c] list from a string value
static std::vector<std::string> parse_list(const std::string& value) {
    std::vector<std::string> result;
    std::string v = trim_ws(value);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') return result;
    v = v.substr(1, v.size() - 2);
    std::istringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto trimmed = trim_ws(item);
        if (!trimmed.empty()) result.push_back(trimmed);
    }
    return result;
}

std::optional<SkillDef> parse_skill_file(const std::string& content,
                                          const std::string& path) {
    // Must start with "---"
    if (content.size() < 4 || content.substr(0, 3) != "---") return std::nullopt;

    // Find closing "---"
    auto close = content.find("\n---", 3);
    if (close == std::string::npos) return std::nullopt;

    // Parse frontmatter lines
    std::string frontmatter = content.substr(4, close - 4); // skip "---\n"
    // Handle "\r\n" in frontmatter
    std::istringstream fm_stream(frontmatter);
    std::string line;

    SkillDef skill;
    skill.path = path;

    while (std::getline(fm_stream, line)) {
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim_ws(line.substr(0, colon));
        std::string val = trim_ws(line.substr(colon + 1));

        if (key == "name") {
            skill.name = val;
        } else if (key == "description") {
            skill.description = val;
        } else if (key == "tools") {
            skill.tools = parse_list(val);
        }
    }

    // Name is required
    if (skill.name.empty()) return std::nullopt;

    // Body is everything after the closing "---" line
    size_t body_start = close + 4; // skip "\n---"
    // Skip trailing \r\n or \n after closing ---
    if (body_start < content.size() && content[body_start] == '\r') body_start++;
    if (body_start < content.size() && content[body_start] == '\n') body_start++;
    if (body_start < content.size()) {
        skill.prompt = content.substr(body_start);
        // Trim trailing whitespace
        auto end = skill.prompt.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            skill.prompt = skill.prompt.substr(0, end + 1);
        }
    }

    return skill;
}

std::vector<SkillDef> load_skills(const std::string& dir) {
    std::vector<SkillDef> skills;
    namespace fs = std::filesystem;

    if (dir.empty() || !fs::exists(dir) || !fs::is_directory(dir)) {
        return skills;
    }

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        std::ifstream file(entry.path());
        if (!file.is_open()) continue;

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        auto skill = parse_skill_file(content, entry.path().string());
        if (skill) {
            skills.push_back(std::move(*skill));
        }
    }

    // Sort by name for consistent ordering
    std::sort(skills.begin(), skills.end(),
              [](const SkillDef& a, const SkillDef& b) { return a.name < b.name; });

    return skills;
}

std::string default_skills_dir() {
    return expand_home("~/.ptrclaw/skills");
}

} // namespace ptrclaw
