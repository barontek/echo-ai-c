/*
 * registry.h - Process-wide registry of built-in tools and the shared
 * singletons they consume (search provider, session manager, OAuth,
 * delegate config, ask-user callback). Depends on: tool.h,
 * search_provider.h, safety, change_tracker, session_manager, openai_oauth.
 */

#ifndef ECHO_REGISTRY_H
#define ECHO_REGISTRY_H

#include "tool.h"
#include "search_provider.h"
#include "../safety/safety.h"
#include "../change_tracker/change_tracker.h"
#include "../session/session_manager.h"
#include "../llm/openai_oauth.h"

/**
 * registry_init - create and register every built-in tool
 * @safety: borrowed SafetyConfig consulted by safety-checking tools; the
 *   registry never frees it.
 *
 * Resets the tool table, then registers each built-in tool factory.
 * Registry owns all created Tool objects and releases them via their
 * destroy() callbacks in registry_destroy().
 *
 * Return: nothing; tools whose factory returns NULL are silently skipped
 * — the registry emits no log entry for a failed factory. Not
 * thread-safe — call once on startup, before any agent runs.
 */
void registry_init(SafetyConfig *safety);

/**
 * registry_register - hand a Tool over to the registry
 * @tool: caller-allocated Tool; ownership transfers to the registry.
 *
 * The tool is set to disabled and appended to the table; it is
 * released via its destroy() callback by registry_destroy(). Silently
 * ignored when the table is full (MAX_TOOLS) or tool is NULL.
 *
 * Return: nothing. Not thread-safe; call during setup, not while tools
 * are running.
 */
void registry_register(Tool *tool);

/**
 * registry_set_enabled - enable tools named in a comma/space-separated list
 * @names: list of tool names, e.g. "bash, glob"; borrowed for the
 *   duration of the call; NULL or empty is a no-op.
 *
 * Each listed name is matched against registered tools and enabled;
 * unknown or unregistered names are silently ignored.
 *
 * Return: nothing. Not thread-safe.
 */
void registry_set_enabled(const char *names);

/**
 * registry_get - look up an enabled tool by name
 * @name: tool name, e.g. "bash"; NULL returns NULL.
 *
 * Disabled tools are hidden: a registered-but-disabled tool is not
 * returned, so a NULL result means "not registered or not enabled".
 *
 * Return: borrowed Tool owned by the registry, valid until
 * registry_destroy(); NULL as described above. Reads static state; not
 * thread-safe against concurrent registry mutation.
 */
Tool *registry_get(const char *name);

/**
 * registry_schemas_json - serialize the enabled tools' schemas as JSON
 *
 * Emits an array of {"type":"function","function":{name, description,
 * parameters}} objects, one per enabled tool. parameters comes from each
 * tool's parameters_schema, parsed and re-serialized when possible.
 *
 * Return: caller-owned JSON string, freed with free(); NULL on
 * allocation failure. Not thread-safe.
 */
char *registry_schemas_json(void);

/**
 * registry_count - number of registered tools
 *
 * Return: count including disabled tools. Reads static state; not
 * thread-safe.
 */
int registry_count(void);

/**
 * registry_set_change_tracker - attach a change tracker to the write_file tool
 * @ct: borrowed ChangeTracker; not owned by the registry or the tool, and
 *   may be NULL to disable change tracking.
 *
 * Wires the tracker into the "write_file" tool only. Not compiled under
 * REGISTRY_TEST.
 *
 * Return: nothing. Not thread-safe.
 */
void registry_set_change_tracker(ChangeTracker *ct);

/**
 * registry_set_search_provider - install the search provider singleton
 * @sp: provider to install; ownership transfers to the registry, which
 *   releases it via its destroy() callback in registry_destroy().
 *
 * Replaces any previously installed provider; the previous one is
 * dropped without being destroyed.
 *
 * Return: nothing. Not thread-safe.
 */
void registry_set_search_provider(SearchProvider *sp);

/**
 * registry_get_search_provider - fetch the installed search provider
 *
 * Return: borrowed SearchProvider owned by the registry, valid until
 * registry_destroy(); NULL when none is installed. Reads static state;
 * not thread-safe against concurrent set/destroy.
 */
SearchProvider *registry_get_search_provider(void);

/**
 * registry_set_session_manager - install the session manager singleton
 * @sm: borrowed SessionManager used by the memory and sqlite tools; the
 *   registry never frees it. NULL clears the reference.
 *
 * Return: nothing. Not thread-safe.
 */
void registry_set_session_manager(SessionManager *sm);

/**
 * registry_get_session_manager - fetch the installed session manager
 *
 * Return: borrowed SessionManager owned by the caller that installed it;
 * NULL when none is installed. Reads static state; not thread-safe
 * against concurrent set/clear.
 */
SessionManager *registry_get_session_manager(void);

/**
 * registry_set_openai_oauth - install the OAuth manager for delegate tools
 * @auth: borrowed OpenAIOAuth that outlives registered delegate tools;
 *   the registry never frees it. NULL clears the reference.
 *
 * Return: nothing. Not thread-safe.
 */
void registry_set_openai_oauth(OpenAIOAuth *auth);

/**
 * registry_get_openai_oauth - fetch the installed OAuth manager
 *
 * Return: borrowed OpenAIOAuth, valid while the installing caller keeps
 * it alive; NULL when none is installed. Reads static state; not
 * thread-safe against concurrent set/clear.
 */
OpenAIOAuth *registry_get_openai_oauth(void);

/**
 * registry_set_delegate_config - configure the delegate tool's sub-agent
 * @provider_name: LLM provider name, e.g. "ollama"; copied, or NULL.
 * @base_url: sub-agent endpoint; copied, or NULL.
 * @api_token: auth token; copied, or NULL.
 * @model: model name; copied, or NULL.
 * @num_ctx: context window size.
 * @keep_alive_secs: model keep-alive seconds.
 * @temperature: sampling temperature.
 * @timeout: request timeout seconds.
 * @max_iterations: cap on sub-agent tool-call loops.
 *
 * All strings are copied into registry-owned storage. On any copy
 * failure the previous config is left untouched; otherwise it is
 * replaced atomically.
 *
 * Return: nothing; allocation failures are silent. Not thread-safe.
 */
void registry_set_delegate_config(const char *provider_name, const char *base_url,
                                   const char *api_token, const char *model,
                                   int num_ctx, int keep_alive_secs,
                                   double temperature, int timeout, int max_iterations);

/**
 * registry_get_delegate_config - read the delegate sub-agent config
 * @provider_name: out param, filled with the stored provider name, or NULL to skip.
 * @base_url: out param, filled with the stored base URL, or NULL to skip.
 * @api_token: out param, filled with the stored token, or NULL to skip.
 * @model: out param, filled with the stored model, or NULL to skip.
 * @num_ctx: out param, filled with the stored context size, or NULL to skip.
 * @keep_alive_secs: out param, filled with the stored keep-alive, or NULL to skip.
 * @temperature: out param, filled with the stored temperature, or NULL to skip.
 * @timeout: out param, filled with the stored timeout, or NULL to skip.
 * @max_iterations: out param, filled with the stored iteration cap, or NULL to skip.
 *
 * Return: 0 with every non-NULL out param filled, or -1 when no
 * provider_name was ever set (out params left untouched). String out
 * params are borrowed from registry-owned storage and become invalid on
 * the next registry_set_delegate_config() or registry_destroy(). Not
 * thread-safe.
 */
int registry_get_delegate_config(const char **provider_name, const char **base_url,
                                  const char **api_token, const char **model,
                                  int *num_ctx, int *keep_alive_secs,
                                  double *temperature, int *timeout, int *max_iterations);

/**
 * registry_set_ask_user_callback - install the ask_user UI callback
 * @cb: callback invoked by the ask_user tool, or NULL to clear. Borrowed
 *   by the registry; must stay valid until cleared.
 * @userdata: opaque pointer forwarded to cb unchanged.
 *
 * The callback must return a heap-allocated string (or NULL to cancel);
 * the ask_user tool frees it.
 *
 * Return: nothing. Not thread-safe.
 */
void registry_set_ask_user_callback(char *(*cb)(const char *, void *), void *userdata);

/**
 * registry_invoke_ask_user - route a question to the installed UI callback
 * @question: prompt text; borrowed for the duration of the call.
 *
 * Return: the callback's return value — a caller-owned heap string that
 * the ask_user tool frees — or NULL when no callback is installed or the
 * callback cancelled. Not thread-safe.
 */
char *registry_invoke_ask_user(const char *question);

/**
 * registry_has_ask_user_callback - whether a UI callback is installed
 *
 * Return: 1 if installed, 0 otherwise. Reads static state; not
 * thread-safe.
 */
int registry_has_ask_user_callback(void);

/**
 * registry_destroy - release all registered tools and installed singletons
 *
 * Calls destroy() on every registered Tool, destroys the registry-owned
 * search provider, and clears the OAuth reference without freeing it.
 * The borrowed session-manager reference and the ask-user callback are
 * left untouched; the tool table is reset to empty.
 *
 * Return: nothing. Not thread-safe; do not run tools concurrently with
 * this.
 */
void registry_destroy(void);

#ifdef REGISTRY_TEST
/**
 * registry_test_set_alloc_fail - make the Nth str_dup fail inside the registry
 * @nth_allocation: 1-based index of the str_dup call to fail; -1 disables
 *   fault injection.
 *
 * Test-only hook. Resets the call counter, fails the Nth str_dup (only
 * that call), and leaves every other allocation to behave normally.
 *
 * Return: nothing. Single-threaded tests only.
 */
void registry_test_set_alloc_fail(int nth_allocation);
#endif

#endif
