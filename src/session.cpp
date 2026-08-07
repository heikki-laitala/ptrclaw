#include "session.hpp"
#include "commands.hpp"
#include "embedder.hpp"
#include "onboard.hpp"
#include "event.hpp"
#include "event_bus.hpp"
#include "tool_manager.hpp"
#include "plugin.hpp"
#include "util.hpp"
#include <iostream>
#ifdef PTRCLAW_HAS_OPENAI_OAUTH
#include "providers/oauth_openai.hpp"
#endif

namespace ptrclaw {

SessionManager::SessionManager(Config& config, HttpClient& http)
    : config_(config), http_(http)
{}

std::shared_ptr<Memory> SessionManager::memory_for(const std::string& session_id) {
    if (config_.memory.isolation != "session") {
        if (!shared_memory_) {
            shared_memory_ = create_memory(config_);
            if (shared_memory_) {
                shared_memory_->apply_config(config_.memory);
                if (embedder_) {
                    shared_memory_->set_embedder(
                        embedder_, config_.memory.embeddings.text_weight,
                        config_.memory.embeddings.vector_weight);
                }
            }
        }
        return shared_memory_;
    }

    auto it = session_memory_.find(session_id);
    if (it != session_memory_.end()) return it->second;

    std::shared_ptr<Memory> mem = create_memory(config_, session_id);
    if (mem) {
        mem->apply_config(config_.memory);
        if (embedder_) {
            mem->set_embedder(embedder_, config_.memory.embeddings.text_weight,
                              config_.memory.embeddings.vector_weight);
        }
    }
    session_memory_[session_id] = mem;
    return mem;
}

std::shared_ptr<ResponseCache> SessionManager::cache_for(
    const std::string& session_id) {
    if (!config_.memory.response_cache) return nullptr;

    std::string base = expand_home("~/.ptrclaw/response_cache.json");
    auto make = [&](const std::string& path) {
        return std::make_shared<ResponseCache>(path, config_.memory.cache_ttl,
                                               config_.memory.cache_max_entries);
    };

    if (config_.memory.isolation != "session" || session_id.empty()) {
        if (!shared_cache_) shared_cache_ = make(base);
        return shared_cache_;
    }

    auto it = session_cache_.find(session_id);
    if (it != session_cache_.end()) return it->second;

    auto cache = make(session_store_path(base, session_id));
    session_cache_[session_id] = cache;
    return cache;
}

Session SessionManager::create_session(const std::string& session_id) {
    auto sr = switch_provider(
        config_.provider, config_.model, config_.model, config_, http_);
    if (!sr.provider) {
        throw std::runtime_error("Cannot create provider: " + sr.error);
    }

    Session session;
    session.id = session_id;
    {
        // Agent holds `Config config_` by value and create_memory() copies it
        // again, so constructing a session deep-copies config_.providers —
        // reading the very strings the OAuth refresh callback rewrites from
        // another worker. Copying them unlocked can read a heap buffer mid
        // reallocation. Taken after switch_provider() above, which takes and
        // releases the same non-recursive mutex itself.
        std::lock_guard<std::mutex> lock(provider_credentials_mutex());
        session.agent = std::make_unique<Agent>(std::move(sr.provider), config_,
                                                memory_for(session_id),
                                                cache_for(session_id));
    }
    session.last_active = epoch_seconds();

    if (!binary_path_.empty()) {
        session.agent->set_binary_path(binary_path_);
    }

    if (event_bus_) {
        // Identity before subscription, and the order matters once turns run on
        // several threads. Agent's event handlers fall back to accepting anything
        // when session_id_ is empty — the CLI and embed cases never set one — so an
        // Agent subscribed first would, for the width of these two calls, take a
        // concurrently-created session's ToolsAvailableEvent, and read session_id_
        // while this thread wrote it. Subscribing after the write also gives the
        // happens-before edge, through the bus mutex.
        session.agent->set_session_id(session_id);
        session.agent->set_event_bus(event_bus_);

        auto tools = create_builtin_tools();
        session.tool_manager = std::make_unique<ToolManager>(
            std::move(tools), config_, *event_bus_, session_id);
        session.tool_manager->wire_memory(session.agent->memory());
    }

    if (embedder_) {
        session.agent->set_embedder(embedder_);
    }

    return session;
}

Agent& SessionManager::get_session(const std::string& session_id) {
    Agent* agent = nullptr;
    ToolManager* fresh_tools = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.last_active = epoch_seconds();
            return *(it->second.agent);
        }

        // Refused, not evicted. Each session holds an Agent, a Config copy and a memory
        // backend, and with generated ids the count is caller-driven — but freeing a
        // session here could pull an Agent out from under a worker mid-dispatch, which is
        // exactly what eviction drains the turn pool to avoid. Throwing ends this one turn
        // with an error the caller sees; handle_message() turns it into a reply.
        if (config_.agent.max_sessions > 0 &&
            sessions_.size() >= config_.agent.max_sessions) {
            throw std::runtime_error(
                "session limit reached (" +
                std::to_string(config_.agent.max_sessions) +
                "): no room for a new session until an existing one goes idle");
        }

        auto [inserted, _] = sessions_.emplace(session_id, create_session(session_id));
        agent = inserted->second.agent.get();
        fresh_tools = inserted->second.tool_manager.get();
    }

    // Announce the new session with the lock released. Turns run on several
    // threads now, so a handler that called back into SessionManager — as the
    // tool-spec handler does, indirectly — would deadlock on a non-recursive
    // mutex. The reference stays valid: unordered_map does not move its nodes,
    // and eviction only runs while every worker is idle.
    // Same order as when these ran inside create_session: specs first, so a
    // SessionCreated subscriber sees a session whose tools are already announced.
    if (fresh_tools) {
        fresh_tools->publish_tool_specs(session_id);
    }
    if (event_bus_) {
        SessionCreatedEvent ev;
        ev.session_id = session_id;
        event_bus_->publish(ev);
    }

    return *agent;
}

void SessionManager::remove_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
    session_memory_.erase(session_id);
    session_cache_.erase(session_id);
}

void SessionManager::evict_idle(uint64_t max_idle_seconds) {
    std::vector<std::string> evicted;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t now = epoch_seconds();

        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if ((now - it->second.last_active) > max_idle_seconds) {
                evicted.push_back(it->first);
                session_memory_.erase(it->first);
                session_cache_.erase(it->first);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Outside the lock, for the reason given in get_session().
    if (event_bus_) {
        for (const auto& id : evicted) {
            SessionEvictedEvent ev;
            ev.session_id = id;
            event_bus_->publish(ev);
        }
    }
}

std::vector<std::string> SessionManager::list_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, _] : sessions_) {
        ids.push_back(id);
    }
    return ids;
}

#ifdef PTRCLAW_HAS_OPENAI_OAUTH
std::optional<PendingOAuth> SessionManager::get_pending_oauth(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_oauth_.find(session_id);
    if (it == pending_oauth_.end()) return std::nullopt;
    if (it->second.created_at > 0 &&
        epoch_seconds() - it->second.created_at > kPendingOAuthExpirySeconds) {
        pending_oauth_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

void SessionManager::set_pending_oauth(const std::string& session_id, PendingOAuth pending) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_oauth_[session_id] = std::move(pending);
}

void SessionManager::clear_pending_oauth(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_oauth_.erase(session_id);
}
#endif

void SessionManager::handle_message(const MessageReceivedEvent& ev) {
    std::string failure;
    try {
        dispatch_message(ev);
        return;
    } catch (const std::exception& e) {
        if (ev.from_cli) throw;
        failure = e.what();
    } catch (...) {
        if (ev.from_cli) throw;
        failure = "unknown error";
    }

    // A turn that throws must still end the turn. get_session() throws when the
    // provider cannot be built, and Agent::process can throw too; inline on the
    // poll loop that reached main()'s handler and exited, dropping the caller's
    // connection with it. On a worker it is caught, so without this the caller
    // waits out turn_timeout_seconds — two minutes of nothing, by default.
    //
    // Rethrown for the local CLI above, deliberately. `ptrclaw -m` and pipe mode
    // have no other failure signal: swallowing this would make a missing API key
    // exit 0 with the apology on stdout, which a cron job or CI step reads as the
    // model's answer.
    std::cerr << "[session] turn failed for " << ev.session_id << ": "
              << failure << "\n";
    if (!event_bus_) return;

    MessageReadyEvent reply;
    reply.session_id = ev.session_id;
    reply.reply_target = ev.message.reply_target.value_or("");
    reply.content = "Sorry — that turn failed: " + failure;
    event_bus_->publish(reply);
}

void SessionManager::dispatch_message(const MessageReceivedEvent& ev) {
    auto& agent = get_session(ev.session_id);
    if (!ev.message.channel.empty()) {
        agent.set_channel(ev.message.channel);
    }
    // A channel whose frontend owns the conversation pushes the window with every
    // message, which makes the session's accumulated history stale by definition —
    // so replace it rather than adding to it.
    //
    // Applied here, on the thread that runs process(), and not by the channel
    // itself: Agent has no internal synchronisation, so a channel calling
    // set_history() from its own accept/serve thread would race an in-flight turn.
    if (ev.message.history) {
        agent.set_history(*ev.message.history);
    }
    std::string chat_id = ev.message.reply_target.value_or("");

    auto send_reply = [&](const std::string& content) {
        MessageReadyEvent reply;
        reply.session_id = ev.session_id;
        reply.reply_target = chat_id;
        reply.content = content;
        event_bus_->publish(reply);
    };

    auto begin_hatch = [&]() {
        agent.start_hatch();
        send_reply(agent.process("Begin the hatching interview."));
    };

    // Slash commands are the operator's surface, not a visitor's, and a channel cannot
    // tell the two apart: it has no notion of who is speaking. /model and /provider
    // change what the buyer is billed for, /memory import writes memory entries —
    // soul:identity among them — and /start clears history and restarts the hatching
    // interview. The CLI is a shell the operator already owns, so commands stay on
    // there unconditionally; anywhere else they are opt-in.
    //
    // Keyed on from_cli, NOT on session_id: for a channel message session_id is the
    // caller's own `session` string, so testing it against "cli" would have let anyone
    // reopen the whole command surface by asking for that session name.
    if (ev.from_cli || config_.allow_channel_commands) {
        if (handle_command(ev, agent, send_reply, begin_hatch)) return;
    }
    // Falling through is deliberate: with commands off, "/model gpt-4" is just something
    // a visitor said, and the agent answers it as text. Rejecting it instead would make
    // an ordinary message starting with a slash fail for no reason the visitor can see.

    // Auto-hatch: if memory exists but no soul, enter hatching
    // so the user's first message kicks off the interview.
    //
    // Never when the caller pushed the conversation: start_hatch() calls
    // history_.clear() and resets caller_system_prompt_, so it would throw the
    // window away and answer the user with an interview question instead. A caller
    // that supplies the history has already said who this agent is, and there is
    // nobody at the far end of an onboarding interview to answer it.
    //
    // Never on a pod either, and for the same reason one step further out: start_hatch()
    // replaces the entire system prompt with the interview, tools included, so the session
    // cannot do the work it was asked for until somebody answers questions about what to
    // call the assistant — and nobody at the far end of a served request is going to.
    //
    // Keyed on a configured workspace rather than on per-session memory, which was the
    // obvious signal and the wrong one. Session isolation is a storage layout, and
    // docs/memory.md documents one interview per chat under it; a Telegram user is a person
    // who can answer, while /hatch sits behind allow_channel_commands, so suppressing it
    // there would leave them no way to create an identity at all. A configured workspace or
    // shared context is a deployment saying it serves callers instead of a person.
    const bool serving_pod = !config_.serving.workspace_root.empty() ||
                             !config_.serving.context_dir.empty();
    if (!ev.message.history && agent.memory() && !serving_pod
        && !agent.is_hatched() && !agent.hatching()) {
        agent.start_hatch();
    }

    send_reply(agent.process(ev.message.content));
}

bool SessionManager::handle_command(
    const MessageReceivedEvent& ev,
    Agent& agent,
    const std::function<void(const std::string&)>& send_reply,
    const std::function<void()>& begin_hatch) {

    // Handle /start command
    if (ev.message.content == "/start") {
        if (agent.memory() && !agent.is_hatched()) {
            begin_hatch();
        } else {
            std::string greeting = "Hello";
            if (ev.message.first_name) greeting += " " + *ev.message.first_name;
            greeting += "! I'm PtrClaw, an AI assistant. How can I help you?";
            send_reply(greeting);
        }
        return true;
    }

    // Handle /new and /clear commands
    if (ev.message.content == "/new" || ev.message.content == "/clear") {
        agent.clear_history();
        send_reply("Conversation history cleared. What would you like to discuss?");
        return true;
    }

    // Handle /soul command — developer-only
    if (ev.message.content == "/soul") {
        send_reply(cmd_soul(agent, config_.dev));
        return true;
    }

    // Handle /hatch command
    if (ev.message.content == "/hatch") {
        send_reply(cmd_hatch(agent));
        return true;
    }

    // Handle /status command
    if (ev.message.content == "/status") {
        send_reply(cmd_status(agent));
        return true;
    }

    // Handle /model command
    if (ev.message.content.rfind("/model ", 0) == 0) {
        send_reply(cmd_model(trim(ev.message.content.substr(7)),
                             agent, config_, http_, ev.from_cli));
        return true;
    }

    // Handle /memory commands
    if (ev.message.content == "/memory") {
        send_reply(cmd_memory(agent));
        return true;
    }
    if (ev.message.content == "/memory export") {
        send_reply(cmd_memory_export(agent));
        return true;
    }
    if (ev.message.content.rfind("/memory import ", 0) == 0) {
        send_reply(cmd_memory_import(agent, trim(ev.message.content.substr(15))));
        return true;
    }

    // Handle /skill command
    if (ev.message.content == "/skill" ||
        ev.message.content.rfind("/skill ", 0) == 0) {
        std::string args = (ev.message.content.size() > 7)
            ? ev.message.content.substr(7) : "";
        send_reply(cmd_skill(args, agent));
        return true;
    }

    // Handle /help command
    if (ev.message.content == "/help") {
        bool is_channel = (ev.session_id != kCliSessionId);
        send_reply(cmd_help(config_.dev, is_channel));
        return true;
    }

    // Handle /models command
    if (ev.message.content == "/models") {
        send_reply(cmd_models(agent, config_));
        return true;
    }

    // Handle /provider command
    if (ev.message.content.rfind("/provider ", 0) == 0) {
        send_reply(cmd_provider(ev.message.content.substr(10),
                                agent, config_, http_, ev.from_cli));
        return true;
    }

    // Handle auth commands + raw OAuth paste
    if (ev.message.content.rfind("/auth", 0) == 0
#ifdef PTRCLAW_HAS_OPENAI_OAUTH
        || get_pending_oauth(ev.session_id).has_value()
#endif
    ) {
        // /auth writes a credential into the shared Config and persists it to
        // ~/.ptrclaw/config.json — process-wide, for every session. That is a local
        // operator's decision, not something a remote caller gets to make on
        // everyone else's behalf, so allow_channel_commands does not extend to it:
        // opting a channel into commands is not opting it into credential writes.
        //
        // from_cli, not session_id: the id is a routing key the caller picks, so
        // testing it against "cli" would hand /auth to anyone who asked for that
        // session name — the bypass fixed in f6d68fc, one command over.
        if (!ev.from_cli) {
            if (ev.message.content.rfind("/auth", 0) == 0) {
                send_reply("/auth is only available on the local CLI — it sets "
                           "credentials for the whole process. Configure them in "
                           "~/.ptrclaw/config.json or via environment variables.");
                return true;
            }
        } else if (handle_auth_command(ev, agent, send_reply)) {
            return true;
        }
    }

    return false;
}

void SessionManager::subscribe_events() {
    if (!event_bus_) return;

    ptrclaw::subscribe<MessageReceivedEvent>(*event_bus_,
        [this](const MessageReceivedEvent& ev) {
            handle_message(ev);
        });
}

bool SessionManager::handle_auth_command(
    const MessageReceivedEvent& ev,
    Agent& agent,
    const std::function<void(const std::string&)>& send_reply) {

#ifdef PTRCLAW_HAS_OPENAI_OAUTH
    auto finish_oauth = [&](const PendingOAuth& pending,
                             const std::string& code) {
        auto r = apply_oauth_result(code, pending, config_, http_);
        if (!r.success) { send_reply(r.error); return; }
        clear_pending_oauth(ev.session_id);

        const auto& openai_entry = config_.providers["openai"];
        std::string model = oauth_model_after_connect(
            agent.provider_name(), agent.model(), openai_entry);

        // Through switch_provider rather than a provider built by the OAuth flow: it is the
        // one place that decides which credential a model may use, and oauth_models can
        // exclude even the default. Building an OAuth provider here regardless would send
        // the next request to the subscription backend with a model config excludes.
        auto sr = switch_provider("openai", model, model, config_, http_);
        if (!sr.provider) {
            // The tokens are stored and stay stored: losing them over the model selection
            // would mean another trip through the browser flow.
            send_reply("OpenAI OAuth connected and tokens saved, but " + model +
                       " cannot be used \xe2\x80\x94 " + sr.error);
            return;
        }

        agent.set_provider(std::move(sr.provider));
        agent.set_model(model);
        // The provider and model are half of what just changed, and apply_oauth_result
        // only persists the token fields. Without this the next start comes back on the
        // old provider — while the reply below claims it was saved.
        config_.provider = "openai";
        config_.model = model;
        bool saved = r.persisted && config_.persist_selection();

        // Connecting OAuth and then not using it needs saying, or the next turn looks like
        // subscription traffic when it is billed to the API key.
        std::string note = openai_oauth_eligible(model, openai_entry)
            ? ""
            : " Note: oauth_models excludes " + model + ", so it uses the API key.";
        send_reply(std::string("OpenAI OAuth connected ✅ Model switched to ") +
                   model + "." +
                   (saved
                    ? " Saved to ~/.ptrclaw/config.json"
                    : " (warning: could not persist to config file)") + note);
    };
#endif

    // Handle /auth commands
    if (ev.message.content.rfind("/auth", 0) == 0) {
        auto parts = split(ev.message.content, ' ');

#ifdef PTRCLAW_HAS_OPENAI_OAUTH
        // /auth openai start — two-step OAuth flow
        if (parts.size() >= 3 && parts[1] == "openai" && parts[2] == "start") {
            auto openai_it = config_.providers.find("openai");
            if (openai_it == config_.providers.end()) {
                send_reply("OpenAI provider config missing.");
                return true;
            }

            auto flow = start_oauth_flow(openai_it->second);
            set_pending_oauth(ev.session_id, std::move(flow.pending));

            send_reply(
                "Open this URL to authorize OpenAI:\n" + flow.authorize_url +
                "\n\nThen paste the full callback URL with:\n"
                "/auth openai finish <callback_url>\n"
                "(or paste just the code)");
            return true;
        }

        // /auth openai finish <url_or_code>
        if (parts.size() >= 4 && parts[1] == "openai" && parts[2] == "finish") {
            auto pending = get_pending_oauth(ev.session_id);
            if (!pending || pending->provider != "openai") {
                send_reply("No pending OpenAI auth flow. Start with: /auth openai start");
                return true;
            }

            std::string input = ev.message.content.substr(
                ev.message.content.find("finish") + 7);
            auto parsed = parse_oauth_input(input);

            if (parsed.code.empty()) {
                send_reply("Missing code. Paste callback URL or auth code.");
                return true;
            }
            if (!parsed.state.empty() && parsed.state != pending->state) {
                send_reply("State mismatch. Please restart with /auth openai start");
                return true;
            }

            finish_oauth(*pending, parsed.code);
            return true;
        }
#else
        // Without the flow the two handlers above are gone, but "start"/"finish" are
        // still documented subcommands — and the generic "/auth <provider> <api_key>"
        // branch below would happily take "start" as the key and persist it to
        // config.json, destroying a working credential. Refuse them explicitly.
        if (parts.size() >= 3 && parts[1] == "openai" &&
            (parts[2] == "start" || parts[2] == "finish")) {
            send_reply("The OpenAI OAuth sign-in flow is not built into this binary. "
                       "Set an API key instead: /auth openai <api_key>");
            return true;
        }
#endif

        // /auth <provider> <key> — set API key for any provider
        if (parts.size() >= 3) {
            const std::string& prov = parts[1];
            bool known = false;
            for (const auto& n : PluginRegistry::instance().provider_names()) {
                if (n == prov) { known = true; break; }
            }
            if (is_hidden_provider(prov)) known = false;
            if (!known) {
                send_reply("Unknown provider: " + prov);
            } else if (prov == "ollama") {
                send_reply("Ollama is local and doesn't need an API key. "
                           "Set base_url in ~/.ptrclaw/config.json");
            } else {
                const std::string& api_key = parts[2];
                // Same map and same config.json the OAuth refresh callback
                // rewrites — see provider_credentials_mutex(). operator[] can
                // rehash, and persist_provider_key shares config.json's temp path.
                std::lock_guard<std::mutex> lock(provider_credentials_mutex());
                config_.providers[prov].api_key = api_key;
                persist_provider_key(prov, api_key);
                send_reply("API key saved for " + std::string(provider_label(prov)) + ".");
            }
            return true;
        }

        // /auth — show status
        std::string status = format_auth_status(config_) +
                            "\nSet credentials: /auth <provider> <api_key>";
#ifdef PTRCLAW_HAS_OPENAI_OAUTH
        status += "\nOAuth: /auth openai start";
#endif
        send_reply(status);
        return true;
    }

#ifdef PTRCLAW_HAS_OPENAI_OAUTH
    // If OpenAI OAuth is pending, accept raw callback URL/code directly.
    auto pending_oauth = get_pending_oauth(ev.session_id);
    if (pending_oauth && pending_oauth->provider == "openai") {
        std::string raw = trim(ev.message.content);
        bool looks_like_oauth_reply =
            (!raw.empty() &&
             (raw.find("code=") != std::string::npos ||
              raw.find("auth/callback") != std::string::npos ||
              raw.find("localhost:1455") != std::string::npos));

        if (looks_like_oauth_reply && raw.rfind("/auth", 0) != 0) {
            auto parsed = parse_oauth_input(raw);

            if (parsed.code.empty()) {
                send_reply("Missing code. Paste callback URL or auth code.");
                return true;
            }
            if (!parsed.state.empty() && parsed.state != pending_oauth->state) {
                send_reply("State mismatch. Please restart with /auth openai start");
                return true;
            }

            finish_oauth(*pending_oauth, parsed.code);
            return true;
        }
    }
#endif

    return false;
}

} // namespace ptrclaw
