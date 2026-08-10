/*
 * factory.h - provider factory: name catalog, default endpoints, and
 * reasoning-effort support queries implemented in factory.c.
 * Depends on: provider.h (LLMProvider type and construction API).
 */

#ifndef ECHO_FACTORY_H
#define ECHO_FACTORY_H

#include "provider.h"

/**
 * provider_names_available - list the constructible provider names
 * @count: receives the number of entries; may be NULL.
 *
 * Aliases like "lmstudio" and unimplemented names like "anthropic" are
 * not listed.
 *
 * Return: pointer to a static array of static strings; the caller must
 * not free either. Thread-safe; immutable.
 */
const char *const *provider_names_available(int *count);

/**
 * provider_default_base_url - resolve a provider's default endpoint
 * @name: provider name.
 *
 * This is the same fallback each provider's _create uses when given a
 * NULL base_url, centralized so startup (main.c) and mid-session
 * switches (routes_ws.c) resolve the same URL.
 *
 * Return: pointer to a static string; the caller must not free. NULL
 * for unknown names. Thread-safe; immutable.
 */
const char *provider_default_base_url(const char *name);

/**
 * provider_supports_effort - does the named provider accept effort
 * @name: provider name; NULL or unknown names return 0.
 *
 * Return: 1 for openai, openai_compatible, ollama, and opencode_zen;
 * 0 otherwise. Thread-safe; immutable.
 */
int provider_supports_effort(const char *name);

/**
 * provider_effort_options - list a provider's accepted effort values
 * @name: provider name; NULL or unknown names return NULL.
 *
 * The wire validation mirrors these lists in the providers' own
 * *_reasoning_effort_valid functions.
 *
 * Return: pointer to a static NULL-terminated array of static strings
 * in UI display order; the caller must not free. NULL/empty effort
 * ("provider default") is accepted for every provider and is not
 * listed. NULL when the provider has no effort support. Thread-safe;
 * immutable.
 */
const char *const *provider_effort_options(const char *name);

/**
 * provider_effort_valid - validate an effort value for a provider
 * @name: provider name.
 * @effort: value to validate; NULL and empty are accepted for any
 *   provider.
 *
 * Return: 1 when effort is NULL/empty or in the named provider's
 * accepted set; 0 for unknown providers or values outside the set.
 * Thread-safe; immutable.
 */
int provider_effort_valid(const char *name, const char *effort);

#endif
