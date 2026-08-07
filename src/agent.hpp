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
    // `memory` and `cache` let the caller supply the two file-backed stores
    // instead of the Agent building them from config. SessionManager uses this to
    // hand every session the same instance in shared mode and a per-session
    // instance under memory.isolation = "session".
    //
    // Sharing is required, not an optimisation. Both stores rewrite their file
    // whole — and through atomic_write_file, which derives its temp path from the
    // target — so two instances over one path lose writes and can interleave into
    // the same temp file. One instance is safe: both lock internally.
    //
    // A supplied store arrives already configured; the Agent does not re-apply
    // config or embedder to it, which would write state other sessions are
    // reading.
    Agent(std::unique_ptr<Provider> provider,
          const Config& config,
          std::shared_ptr<Memory> memory = nullptr,
          std::shared_ptr<ResponseCache> cache = nullptr);
    ~Agent();

    // Process a user message and return the assistant's final text reply
    std::string process(const std::string& user_message);

    // Get current history size
    size_t history_size() const { return history_.size(); }

    // Get estimated token usage
    uint32_t estimated_tokens() const;

    // Clear history
    void clear_history();

    // Replace the conversation history. For callers that own conversation state
    // themselves — restoring a persisted conversation, or supplying the context
    // window per request. A leading System message is kept as the system prompt;
    // otherwise the built-in prompt is injected on the next process() call.
    void set_history(std::vector<ChatMessage> messages);

    // Read the conversation history, e.g. to persist it across restarts.
    const std::vector<ChatMessage>& history() const { return history_; }

    // Switch model
    void set_model(const std::string& model);
    const std::string& model() const { return model_; }

    // Switch provider
    void set_provider(std::unique_ptr<Provider> provider);
    std::string provider_name() const;

    // Optional event bus integration (nullptr = disabled)
    void set_event_bus(EventBus* bus);
    void set_session_id(const std::string& id);
    void set_channel(const std::string& ch) { channel_ = ch; }
    void set_binary_path(const std::string& path) { binary_path_ = path; }

    // Memory system
    void set_memory(std::unique_ptr<Memory> memory);
    Memory* memory() const { return memory_.get(); }

    // Response cache
    void set_response_cache(std::shared_ptr<ResponseCache> cache);
    ResponseCache* response_cache() const { return response_cache_.get(); }

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

    // Whether a store exists that can actually keep anything: the "none" backend is a real
    // object whose store() is a no-op. Public because every decision about hatching turns on
    // it — an interview that cannot be persisted would run again on every launch, and
    // announce an identity that was never written.
    bool has_active_memory() const;

private:
    // (Re)build the response cache. An empty session_id, or shared isolation,
    // points it at the process-wide cache file. No-op when the caller supplied
    // one — it is theirs to scope.
    void build_response_cache(const std::string& session_id);
    void compact_history();
    void inject_system_prompt();
    void invalidate_system_prompt();

    // Key material for the response cache: the conversation as the model will see it,
    // minus the current message. See the definition for why the old key was unsafe.
    std::string conversation_cache_context() const;
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
    // True when history_[0] is a system prompt the caller supplied via
    // set_history() rather than one we generated. invalidate_system_prompt()
    // must not erase it — see the comment there.
    bool caller_system_prompt_ = false;
    EventBus* event_bus_ = nullptr;
    uint64_t tools_sub_id_ = 0;
    uint64_t skill_sub_id_ = 0;
    std::string session_id_;
    std::string channel_;
    std::string binary_path_;
    // shared_ptr because in shared-isolation mode every session points at one
    // instance. See the constructor comment.
    std::shared_ptr<Memory> memory_;
    std::shared_ptr<ResponseCache> response_cache_;
    // True when the store came from the caller, who owns its configuration.
    bool external_memory_ = false;
    bool external_cache_ = false;
    Embedder* embedder_ = nullptr;
    uint32_t turns_since_synthesis_ = 0;
    bool hatching_ = false;
    std::vector<SkillDef> available_skills_;
    std::string active_skill_name_;
    std::optional<uint32_t> last_prompt_tokens_;
};

} // namespace ptrclaw
