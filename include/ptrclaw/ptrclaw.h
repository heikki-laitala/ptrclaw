/*
 * ptrclaw.h — Embeddable PtrClaw API
 *
 * C API for embedding PtrClaw in host applications (Flutter, mobile, desktop,
 * or any language with C FFI support).
 *
 * Internally runs the full channel pipeline (EventBus, SessionManager,
 * StreamRelay) so all features work: commands (/status, /model, /skill, ...),
 * memory, skills, streaming, and multi-session management.
 *
 * Thread safety
 * -------------
 *   ptrclaw_send() and ptrclaw_send_stream() may be called from any thread.
 *   The internal event loop runs on a dedicated background thread created by
 *   ptrclaw_create().  Do not call ptrclaw_send() concurrently for the same
 *   session_id — serialise per session.
 *
 * Typical usage
 * -------------
 *   PtrClawHandle h = ptrclaw_create(NULL);  // uses ~/.ptrclaw/config.json
 *
 *   if (ptrclaw_send(h, "user-1", "Hello!") == PTRCLAW_OK)
 *       puts(ptrclaw_last_response(h, "user-1"));
 *
 *   ptrclaw_destroy(h);
 */

#ifndef PTRCLAW_H
#define PTRCLAW_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return codes ──────────────────────────────────────────────── */

#define PTRCLAW_OK           0  /* success                         */
#define PTRCLAW_ERR_INVALID  1  /* null argument or bad state       */
#define PTRCLAW_ERR_PROVIDER 2  /* provider or processing error     */

/* ── Version ───────────────────────────────────────────────────── */

/* Returns a NUL-terminated version string (e.g. "0.0.9"). Never NULL. */
const char *ptrclaw_version(void);

/* ── Opaque handle ─────────────────────────────────────────────── */

typedef struct PtrClawHandle_ *PtrClawHandle;

/* ── Lifecycle ─────────────────────────────────────────────────── */

/*
 * ptrclaw_create — initialise ptrclaw and start the background event loop.
 *
 * config_json  Optional JSON string to override config values.
 *              Pass NULL to use ~/.ptrclaw/config.json as-is.
 *              Recognised keys: "provider", "model", "temperature".
 *
 * Returns a non-null handle on success.
 */
PtrClawHandle ptrclaw_create(const char *config_json);

/* Stop the background loop and free all resources. */
void ptrclaw_destroy(PtrClawHandle handle);

/* Returns the last error message, or an empty string. Never NULL. */
const char *ptrclaw_last_error(PtrClawHandle handle);

/* ── Messaging ─────────────────────────────────────────────────── */

/*
 * ptrclaw_send — send a user message and block until the response is ready.
 *
 * session_id  Identifies the conversation (creates a new session on first use).
 * message     UTF-8 user message text.  Slash commands (/status, /model, etc.)
 *             are handled automatically.
 *
 * Returns PTRCLAW_OK on success.  The response is accessible via
 * ptrclaw_last_response() until the next send on the same session.
 */
int ptrclaw_send(PtrClawHandle handle, const char *session_id,
                 const char *message);

/* Returns the response from the last ptrclaw_send*() for this session.
 * Never NULL (returns empty string if no response yet). */
const char *ptrclaw_last_response(PtrClawHandle handle,
                                   const char *session_id);

/*
 * Streaming chunk callback.
 *
 *   chunk    UTF-8 text delta (empty on the terminal call).
 *   done     0 for intermediate chunks; 1 for the terminal sentinel.
 *   userdata Opaque pointer passed through from ptrclaw_send_stream().
 */
typedef void (*PtrClawChunkCallback)(const char *chunk, int done,
                                      void *userdata);

/*
 * ptrclaw_send_stream — like ptrclaw_send but delivers text deltas via
 * on_chunk as the provider streams output.  Falls back to a single chunk
 * for non-streaming providers.
 *
 * The assembled response is also available via ptrclaw_last_response().
 */
int ptrclaw_send_stream(PtrClawHandle handle, const char *session_id,
                        const char *message,
                        PtrClawChunkCallback on_chunk, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PTRCLAW_H */
