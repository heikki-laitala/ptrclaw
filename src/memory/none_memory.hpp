#pragma once
#include "../memory.hpp"

namespace ptrclaw {

class NoneMemory : public Memory {
public:
    std::string backend_name() const override { return "none"; }

    std::string store(const std::string&, const std::string&,
                      MemoryCategory, const std::string&) override { return {}; }

    std::vector<MemoryEntry> recall(const std::string&, uint32_t,
                                    std::optional<MemoryCategory>) override { return {}; }

    std::optional<MemoryEntry> get(const std::string&) override { return std::nullopt; }

    std::vector<MemoryEntry> list(std::optional<MemoryCategory>, uint32_t) override { return {}; }

    bool forget(const std::string&) override { return false; }

    uint32_t count(std::optional<MemoryCategory>) override { return 0; }

    std::string snapshot_export() override { return "[]"; }

    uint32_t snapshot_import(const std::string&) override { return 0; }

    uint32_t hygiene_purge(uint32_t) override { return 0; }
};

} // namespace ptrclaw
