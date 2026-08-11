/*
 * oauth_vault.h - credential staging, persistence, and secure clearing
 * for the OpenAI OAuth manager. All credential memory is cleansed
 * before free.
 * Depends on: openai_oauth_internal.h, oauth_jwt.h, oauth_codec.h.
 */

#ifndef ECHO_OAUTH_VAULT_H
#define ECHO_OAUTH_VAULT_H

#include <stdint.h>
#include <time.h>

#include "openai_oauth_internal.h"

/**
 * secure_free - cleanse and free a string, then NULL the pointer
 * @value: pointer to the string to free; NULL or *value NULL is a no-op.
 *
 * Return: void.
 */
void secure_free(char **value);

/**
 * next_generation - monotonically advance a generation counter
 * @value: current generation.
 *
 * Return: @value + 1, or 0 on wraparound (never returns the same value).
 */
uint64_t next_generation(uint64_t value);

/**
 * credentials_clear - free every field of staged credentials
 * @credentials: credentials to release; NULL is a no-op.
 *
 * Return: void.
 */
void credentials_clear(OAuthCredentials *credentials);

/**
 * clear_credentials_locked - drop and cleanse the manager's credentials
 * @auth: manager; the caller must hold @auth->lock.
 *
 * Return: void.
 */
void clear_credentials_locked(OpenAIOAuth *auth);

/**
 * clear_pending_sensitive_locked - drop in-flight login secrets
 * @auth: manager; the caller must hold @auth->lock.
 *
 * Cleanses and frees state/verifier/challenge/device fields.
 *
 * Return: void.
 */
void clear_pending_sensitive_locked(OpenAIOAuth *auth);

/**
 * token_set_parse - parse a token endpoint response into credentials
 * @data: JSON token response.
 * @existing_refresh: fallback refresh token when absent; borrowed.
 * @existing_account: fallback account id when absent; borrowed.
 * @existing_plan: fallback plan type when absent; borrowed.
 * @now: current time for expiry computation.
 * @output: receives caller-owned staged credentials.
 *
 * Return: 0 on success; -1 on parse/validation failure with *output
 * untouched and any partial allocations freed.
 */
int token_set_parse(const char *data, const char *existing_refresh,
                    const char *existing_account, const char *existing_plan,
                    time_t now, OAuthCredentials *output);

/**
 * stored_credentials_parse - parse a persisted credentials row
 * @data: JSON row as saved by session_manager_save_provider_oauth.
 * @output: receives caller-owned staged credentials.
 *
 * Return: 0 on success; -1 on malformed input with partial allocations
 * freed.
 */
int stored_credentials_parse(const char *data, OAuthCredentials *output);

/**
 * commit_credentials_locked - install staged credentials into the manager
 * @auth: manager; the caller must hold @auth->lock.
 * @staged: credentials to install; ownership transfers to the manager
 *   and the struct is zeroed.
 *
 * Return: void.
 */
void commit_credentials_locked(OpenAIOAuth *auth, OAuthCredentials *staged);

/**
 * persist_staged_locked - save credentials to the attached session
 * @auth: manager; the caller must hold @auth->lock.
 * @credentials: credentials to serialize (borrowed).
 *
 * Return: 0 on success, -1 when no session is attached, serialization
 * or storage fails.
 */
int persist_staged_locked(OpenAIOAuth *auth, const OAuthCredentials *credentials);

#endif /* ECHO_OAUTH_VAULT_H */
