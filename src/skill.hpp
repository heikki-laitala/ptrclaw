#pragma once
#include <string>
#include <vector>
#include <optional>

namespace ptrclaw {

struct SkillDef {
    std::string name;
    std::string description;
    std::string prompt;              // markdown body after frontmatter
    std::string path;                // source file path
};

// Parse a single .md skill file (frontmatter + body).
// Returns nullopt if parsing fails or name is missing.
std::optional<SkillDef> parse_skill_file(const std::string& content,
                                          const std::string& path = "");

// Load all .md skill files from a directory.
// Returns empty vector if directory doesn't exist.
std::vector<SkillDef> load_skills(const std::string& dir);

// Default skills directory: ~/.ptrclaw/skills/
std::string default_skills_dir();

} // namespace ptrclaw
