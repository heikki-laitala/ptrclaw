/*
 * ptrclaw.h — Embeddable PtrClaw Core API
 *
 * C API for embedding PtrClaw in host applications (Flutter, mobile, desktop,
 * CLI wrappers, or any language with C FFI support).
 *
 * Link against libptrclaw_shared (shared library target) for dynamic linking,
 * or include engine.cpp together with ptrclaw_lib for static embedding.
 *
 * Thread safety
 * -------------
 *   PtrClawEngine handles are immutable after creation and safe to read from
 *   multiple threads.  PtrClawSession handles are NOT thread-safe — callers
 *   must serialise access (one active call at a time per session).
 *
 * Typical usage
 * -------------
 *   PtrClawEngine eng = ptrclaw_engine_create(
 *       "{\"provider\":\"anthropic\","
 *       " \"api_key\":\"sk-ant-...\","
 *       " \"model\":\"claude-sonnet-4-6\"}");
 *
 *   PtrClawSession sess = ptrclaw_session_create(eng, "user-42");
 *
 *   if (ptrclaw_process(sess, "Hello!") == PTRCLAW_OK)
 *       puts(ptrclaw_session_last_response(sess));
 *
 *   ptrclaw_session_destroy(sess);
 *   ptrclaw_engine_destroy(eng);
 */

#ifndef PTRCLAW_H
#define PTRCLAW_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return codes ──────────────────────────────────────────────── */

#define PTRCLAW_OK           0  /* success                         */
#define PTRCLAW_ERR_INVALID  1  /* null argument or bad config     */
#define PTRCLAW_ERR_PROVIDER 2  /* LLM provider call failed        */

/* ── Version ───────────────────────────────────────────────────── */

/* Returns a NUL-terminated string such as "0.0.9". Never NULL. */
const char *ptrclaw_version(void);

/* ── Opaque handle types ────────────────────────────────────────── */

typedef struct PtrClawEngine_  *PtrClawEngine;
typedef struct PtrClawSession_ *PtrClawSession;

/* ── Engine lifecycle ───────────────────────────────────────────── */

/*
 * ptrclaw_engine_create — parse config_json and initialise an engine.
 *
 * config_json is a UTF-8 JSON object.  Recognised keys:
 *
 *   "provider"              string   LLM provider name (default: "anthropic").
 *                                    Registered providers: anthropic, openai,
 *                                    ollama, openrouter, compatible.
 *   "api_key"               string   Provider API key (required for cloud
 *                                    providers; may be empty for local ones).
 *   "model"                 string   Model name (default: "claude-sonnet-4-6").
 *   "temperature"           number   Sampling temperature 0.0–1.0 (default 0.7).
 *   "tools"                 array    Tool names to enable; omit the key to
 *                                    enable all registered tools.  Pass an
 *                                    empty array [] to disable all tools.
 *   "max_tool_iterations"   number   Agent loop cap (default 10).
 *   "max_history_messages"  number   Rolling history window (default 50).
 *
 * Returns a non-null handle on success.  Returns NULL only when config_json
 * is NULL.  On JSON parse errors the handle is still returned so that the
 * caller can inspect ptrclaw_engine_last_error().
 */
PtrClawEngine ptrclaw_engine_create(const char *config_json);

void          ptrclaw_engine_destroy(PtrClawEngine engine);

/* Returns the last error message, or an empty string.  Never NULL. */
const char   *ptrclaw_engine_last_error(PtrClawEngine engine);

/* ── Session lifecycle ──────────────────────────────────────────── */

/*
 * ptrclaw_session_create — create a stateful, single-user conversation.
 *
 * engine     An engine handle returned by ptrclaw_engine_create().
 * session_id Optional identifier used for log correlation (may be NULL or "").
 *
 * Returns NULL on failure; inspect ptrclaw_engine_last_error() for the reason.
 * Each session owns its own HTTP client, provider instance, and history.
 */
PtrClawSession ptrclaw_session_create(PtrClawEngine engine, const char *session_id);

void           ptrclaw_session_destroy(PtrClawSession session);

/* Erase conversation history without destroying the session. */
void           ptrclaw_session_clear_history(PtrClawSession session);

/* ── Message processing ─────────────────────────────────────────── */

/*
 * ptrclaw_process — send a user message and block until the response is ready.
 *
 * The full response (including any tool-use round-trips) is stored internally
 * and accessible via ptrclaw_session_last_response() until the next call to
 * ptrclaw_process() or ptrclaw_process_stream() on the same session.
 *
 * Returns PTRCLAW_OK on success, or a PTRCLAW_ERR_* code on failure.
 */
int         ptrclaw_process(PtrClawSession session, const char *message);

/* Returns the response from the most recent successful ptrclaw_process*() call.
 * The pointer is owned by the session and invalidated by the next process call.
 * Returns an empty string (never NULL) if no response has been produced yet. */
const char *ptrclaw_session_last_response(PtrClawSession session);

/*
 * Streaming chunk callback.
 *
 *   chunk    UTF-8 text delta (may be an empty string on the terminal call).
 *   done     0 for intermediate chunks; 1 for the terminal sentinel.
 *   userdata Opaque pointer passed through from ptrclaw_process_stream().
 *
 * The callback is invoked on the same thread that called ptrclaw_process_stream().
 * Do not call any ptrclaw_* function from within the callback.
 */
typedef void (*PtrClawChunkCallback)(const char *chunk, int done, void *userdata);

/*
 * ptrclaw_process_stream — like ptrclaw_process but delivers text deltas via
 * on_chunk as the provider streams output.  Falls back to a single chunk when
 * the active provider does not support streaming.
 *
 * on_chunk is called one or more times with done=0, then exactly once with
 * done=1 (and an empty chunk) to signal completion.
 *
 * The assembled response is also available via ptrclaw_session_last_response()
 * after this function returns.
 *
 * Returns PTRCLAW_OK on success, or a PTRCLAW_ERR_* code on failure.
 */
int ptrclaw_process_stream(PtrClawSession session,
                           const char *message,
                           PtrClawChunkCallback on_chunk,
                           void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PTRCLAW_H */
