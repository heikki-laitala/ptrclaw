#pragma once
#include "provider.hpp"
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
        return ChatResponse{"reply", {}};
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
        old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
        skills_dir = tmpdir / ".ptrclaw" / "skills";
        std::filesystem::create_directories(skills_dir);
        setenv("HOME", tmpdir.c_str(), 1);
    }

    void add_skill(const std::string& filename, const std::string& content) {
        std::ofstream f(skills_dir / filename);
        f << content;
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
