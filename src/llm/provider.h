/*
 * provider.h - LLM provider interface and factory: provider names,
 * construction, default endpoints, and reasoning-effort support.
 * Depends on: agent/message.h (Message, LLMResponse, ToolCall).
 */

#ifndef ECHO_PROVIDER_H
#define ECHO_PROVIDER_H

#include "../agent/message.h"

typedef struct OpenAIOAuth OpenAIOAuth;

typedef struct LLMProvider LLMProvider;

/* Provider vtable. Every callback is invoked with a live provider and
 * returns a caller-owned LLMResponse (free with llm_response_free());
 * destroy() frees both the provider and its ctx. ctx is provider-owned
 * opaque state and must not be touched by the caller.
 *
 * Concurrency: no method is concurrent-safe — a provider instance may be
 * used from one thread at a time (the agent loop serializes all calls).
 * chat()/chat_streaming()/extract_structured() must not run concurrently
 * on the same provider; each implementation documents any internal
 * _Atomic state it uses to tolerate overlapping calls across DIFFERENT
 * instances. */
struct LLMProvider {
    LLMResponse *(*chat)(LLMProvider *self, Message *messages, int count,
                         const char *model, double temperature, int timeout,
                         const char *tools_json);
    LLMResponse *(*chat_streaming)(LLMProvider *self, Message *messages, int count,
                                   const char *model, double temperature, int timeout,
                                   void (*on_chunk)(const char *chunk, void *userdata),
                                   void *userdata,
                                   const char *tools_json);
    LLMResponse *(*extract_structured)(LLMProvider *self, Message *messages, int count,
                                        const char *model, double temperature, int timeout,
                                        const char *json_schema);
    void (*destroy)(LLMProvider *self);
    void *ctx;
};

/**
 * get_provider - construct a provider by name, without OAuth
 * @name: provider name: "ollama", "openai_compatible", or
 *   "opencode_zen"; the legacy alias "lmstudio" is also accepted.
 *   Unknown names return NULL.
 * @model: accepted and ignored — model selection happens per chat call,
 *   not at construction.
 * @base_url: borrowed endpoint, or NULL for the provider default (the
 *   same one provider_default_base_url() reports).
 * @api_token: borrowed Bearer token, or NULL/empty for none.
 * @num_ctx: context-window size for ollama (ignored by other
 *   providers); <= 0 selects the provider default.
 * @keep_alive_secs: model keep-alive for ollama (ignored by other
 *   providers); <= 0 selects the provider default.
 * @effort: borrowed reasoning-effort hint, or NULL/empty for the
 *   provider default; validated per provider (see provider_effort_valid).
 *
 * "openai" cannot be constructed here — it needs an OAuth manager and
 * returns NULL from this function; use get_provider_with_auth().
 *
 * Return: caller-owned LLMProvider, or NULL for unknown names, "openai"
 * without auth, invalid effort, or allocation failure. The caller must
 * release it via its destroy() callback. Thread-safe: the factory keeps
 * no state; the returned provider's thread-safety depends on the
 * provider implementation.
 */
LLMProvider *get_provider(const char *name, const char *model,
                          const char *base_url, const char *api_token,
                          int num_ctx, int keep_alive_secs,
                          const char *effort);

/**
 * get_provider_with_auth - construct a provider by name, with OAuth
 * @openai_auth: borrowed OAuth manager used only by "openai"; must
 *   outlive the returned provider. May be NULL — then "openai" returns
 *   NULL.
 * @name, @model, @base_url, @api_token, @num_ctx, @keep_alive_secs,
 *   @effort: as in get_provider(). Note that "openai" ignores base_url
 *   and api_token (OAuth-only provider).
 *
 * Return: caller-owned LLMProvider, or NULL for unknown names, "openai"
 * without auth, invalid effort, or allocation failure. The caller must
 * release it via its destroy() callback. Thread-safe: the factory keeps
 * no state; the returned provider's thread-safety depends on the
 * provider implementation.
 */
LLMProvider *get_provider_with_auth(const char *name, const char *model,
                                    const char *base_url, const char *api_token,
                                    int num_ctx, int keep_alive_secs,
                                    const char *effort,
                                    OpenAIOAuth *openai_auth);

#endif
