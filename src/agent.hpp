#pragma once
#include "memory.hpp"
#include "memory/response_cache.hpp"
#include "provider.hpp"
#include "skill.hpp"
#include "tool.hpp"
#include "config.hpp"
#include <string>
#include <vector>
#include <memory>

namespace ptrclaw {

class Embedder; // forward declaration
class EventBus; // forward declaration
class ToolManager; // forward declaration
struct SkillRequestEvent; // forward declaration

class Agent {
public:
    Agent(std::unique_ptr<Provider> provider,
          const Config& config);
    ~Agent();

    // Process a user message and return the assistant's final text reply
    std::string process(const std::string& user_message);

    // Get current history size
    size_t history_size() const { return history_.size(); }

    // Get estimated token usage
    uint32_t estimated_tokens() const;

    // Clear history
    void clear_history();

    // Switch model
    void set_model(const std::string& model);
    const std::string& model() const { return model_; }

    // Switch provider
    void set_provider(std::unique_ptr<Provider> provider);
    std::string provider_name() const;

    // Optional event bus integration (nullptr = disabled)
    void set_event_bus(EventBus* bus);
    void set_session_id(const std::string& id) { session_id_ = id; }
    void set_channel(const std::string& ch) { channel_ = ch; }
    void set_binary_path(const std::string& path) { binary_path_ = path; }

    // Memory system
    void set_memory(std::unique_ptr<Memory> memory);
    Memory* memory() const { return memory_.get(); }

    // Response cache
    void set_response_cache(std::unique_ptr<ResponseCache> cache);

    // Embedder for vector search (non-owning, caller retains ownership)
    void set_embedder(Embedder* embedder);

    // Skills
    void load_skills(const std::string& dir = "");
    const std::vector<SkillDef>& available_skills() const { return available_skills_; }
    bool activate_skill(const std::string& name);
    void deactivate_skill();
    const std::string& active_skill_name() const { return active_skill_name_; }

    // Soul hatching
    bool is_hatched() const;
    void start_hatch();
    bool hatching() const { return hatching_; }

private:
    bool has_active_memory() const;
    void compact_history();
    void inject_system_prompt();
    void invalidate_system_prompt();
    const SkillDef* find_skill(const std::string& name) const;
    void run_synthesis();
    void maybe_synthesize();
    void on_tools_available(const std::vector<ToolSpec>& specs);
    void on_skill_request(const SkillRequestEvent& req);

    std::unique_ptr<Provider> provider_;
    std::vector<ChatMessage> history_;
    std::vector<ToolSpec> cached_tool_specs_;
    Config config_;
    std::string model_;
    bool system_prompt_injected_ = false;
    EventBus* event_bus_ = nullptr;
    uint64_t tools_sub_id_ = 0;
    uint64_t skill_sub_id_ = 0;
    std::string session_id_;
    std::string channel_;
    std::string binary_path_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<ResponseCache> response_cache_;
    Embedder* embedder_ = nullptr;
    uint32_t turns_since_synthesis_ = 0;
    bool hatching_ = false;
    std::vector<SkillDef> available_skills_;
    std::string active_skill_name_;
    std::optional<uint32_t> last_prompt_tokens_;
};

} // namespace ptrclaw
