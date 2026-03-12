#include <catch2/catch_test_macros.hpp>
#include "../src/skill.hpp"
#include <filesystem>
#include <fstream>

using namespace ptrclaw;

// ── Helper ──────────────────────────────────────────────────────

static std::string make_temp_dir() {
    auto path = std::filesystem::temp_directory_path() /
                ("ptrclaw_skill_" + std::to_string(std::rand()));
    std::filesystem::create_directories(path);
    return path.string();
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// ── parse_skill_file tests ──────────────────────────────────────

TEST_CASE("parse_skill_file: valid frontmatter + body", "[skill]") {
    std::string content = R"(---
name: code_review
description: Review code for bugs and security issues
tools: [file_read, shell]
---

You are reviewing code. Focus on:
1. Security vulnerabilities
2. Logic errors)";

    auto skill = parse_skill_file(content, "/test/code_review.md");
    REQUIRE(skill.has_value());
    REQUIRE(skill->name == "code_review");
    REQUIRE(skill->description == "Review code for bugs and security issues");
    REQUIRE(skill->tools.size() == 2);
    REQUIRE(skill->tools[0] == "file_read");
    REQUIRE(skill->tools[1] == "shell");
    REQUIRE(skill->prompt.find("Security vulnerabilities") != std::string::npos);
    REQUIRE(skill->path == "/test/code_review.md");
}

TEST_CASE("parse_skill_file: name only (minimal)", "[skill]") {
    std::string content = "---\nname: simple\n---\nDo something.";
    auto skill = parse_skill_file(content);
    REQUIRE(skill.has_value());
    REQUIRE(skill->name == "simple");
    REQUIRE(skill->description.empty());
    REQUIRE(skill->tools.empty());
    REQUIRE(skill->prompt == "Do something.");
}

TEST_CASE("parse_skill_file: missing name returns nullopt", "[skill]") {
    std::string content = "---\ndescription: no name here\n---\nbody";
    REQUIRE_FALSE(parse_skill_file(content).has_value());
}

TEST_CASE("parse_skill_file: no frontmatter returns nullopt", "[skill]") {
    REQUIRE_FALSE(parse_skill_file("just some text").has_value());
}

TEST_CASE("parse_skill_file: empty tools list", "[skill]") {
    std::string content = "---\nname: test\ntools: []\n---\nbody";
    auto skill = parse_skill_file(content);
    REQUIRE(skill.has_value());
    REQUIRE(skill->tools.empty());
}

TEST_CASE("parse_skill_file: handles CRLF line endings", "[skill]") {
    std::string content = "---\r\nname: crlf_test\r\ndescription: test\r\n---\r\nBody text";
    auto skill = parse_skill_file(content);
    REQUIRE(skill.has_value());
    REQUIRE(skill->name == "crlf_test");
    REQUIRE(skill->prompt == "Body text");
}

TEST_CASE("parse_skill_file: empty body", "[skill]") {
    std::string content = "---\nname: no_body\n---\n";
    auto skill = parse_skill_file(content);
    REQUIRE(skill.has_value());
    REQUIRE(skill->name == "no_body");
    REQUIRE(skill->prompt.empty());
}

// ── load_skills tests ───────────────────────────────────────────

TEST_CASE("load_skills: loads .md files from directory", "[skill]") {
    auto dir = make_temp_dir();
    write_file(dir + "/review.md", "---\nname: review\ndescription: Code review\n---\nReview code.");
    write_file(dir + "/summarize.md", "---\nname: summarize\ndescription: Summarize text\n---\nSummarize.");
    write_file(dir + "/ignored.txt", "not a skill");
    write_file(dir + "/bad.md", "no frontmatter here");

    auto skills = load_skills(dir);
    REQUIRE(skills.size() == 2);
    // Sorted by name
    REQUIRE(skills[0].name == "review");
    REQUIRE(skills[1].name == "summarize");

    std::filesystem::remove_all(dir);
}

TEST_CASE("load_skills: non-existent directory returns empty", "[skill]") {
    auto skills = load_skills("/nonexistent/path/skills");
    REQUIRE(skills.empty());
}

TEST_CASE("load_skills: empty directory returns empty", "[skill]") {
    auto dir = make_temp_dir();
    auto skills = load_skills(dir);
    REQUIRE(skills.empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("load_skills: finds skills in subdirectories", "[skill]") {
    auto dir = make_temp_dir();
    std::filesystem::create_directories(dir + "/email");
    write_file(dir + "/review.md", "---\nname: review\n---\nReview code.");
    write_file(dir + "/email/protonmail.md", "---\nname: protonmail\ndescription: Email\n---\nEmail skill.");

    auto skills = load_skills(dir);
    REQUIRE(skills.size() == 2);
    // Sorted by name
    REQUIRE(skills[0].name == "protonmail");
    REQUIRE(skills[1].name == "review");

    std::filesystem::remove_all(dir);
}
