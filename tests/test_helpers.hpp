#pragma once
#include "provider.hpp"
#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace ptrclaw {

// Minimal provider stub for tests that don't need call tracking.
class StubProvider : public Provider {
public:
    ChatResponse chat(const std::vector<ChatMessage>&,
                      const std::vector<ToolSpec>&,
                      const std::string&, double) override {
        return ChatResponse{"reply", {}, {}, {}};
    }
    std::string chat_simple(const std::string&, const std::string&,
                            const std::string&, double) override {
        return "simple";
    }
    bool supports_native_tools() const override { return true; }
    std::string provider_name() const override { return "stub"; }
};

// RAII guard that redirects HOME to a temp directory.
// Automatically creates ~/.ptrclaw/skills/ under the temp dir.
struct HomeGuard {
    std::string old_home;
    std::filesystem::path tmpdir;
    std::filesystem::path skills_dir;

    explicit HomeGuard()
        : tmpdir(std::filesystem::temp_directory_path() /
                 ("ptrclaw_test_home_" + std::to_string(getpid()) + "_" +
                  std::to_string(std::rand())))
    {
        // One getenv call, not two: the second could return null where the first
        // did not, and assigning null to std::string is undefined behaviour.
        const char* home = std::getenv("HOME");
        old_home = home ? home : "";
        skills_dir = tmpdir / ".ptrclaw" / "skills";
        std::filesystem::create_directories(skills_dir);
        setenv("HOME", tmpdir.c_str(), 1);
    }

    void add_skill(const std::string& filename, const std::string& content) {
        std::ofstream f(skills_dir / filename);
        f << content;
    }

    // Path config code resolves to while this guard is active.
    std::filesystem::path config_path() const {
        return tmpdir / ".ptrclaw" / "config.json";
    }

    // Seed a config file. modify_config_json() returns false when the file does
    // not exist, so credential-writing paths need one present to be exercised.
    void write_default_config() const {
        std::ofstream f(config_path());
        f << Config::defaults_json().dump(4) << "\n";
    }

    // Read the config file back — for asserting on what was persisted rather than
    // only on the in-memory Config.
    nlohmann::json read_config() const {
        std::ifstream f(config_path());
        if (!f.is_open()) return nlohmann::json::object();
        nlohmann::json j;
        try { f >> j; } catch (...) { return nlohmann::json::object(); }
        return j;
    }

    ~HomeGuard() noexcept {
        setenv("HOME", old_home.c_str(), 1);
        std::error_code ec;
        std::filesystem::remove_all(tmpdir, ec);
    }

    HomeGuard(const HomeGuard&) = delete;
    HomeGuard& operator=(const HomeGuard&) = delete;
};

} // namespace ptrclaw
