/*
 * oauth_callback.h - localhost callback server for the interactive login
 * flow: request validation, token exchange, and the callback thread
 * lifecycle.
 * Depends on: openai_oauth_internal.h, oauth_vault.h, oauth_http.h.
 */

#ifndef ECHO_OAUTH_CALLBACK_H
#define ECHO_OAUTH_CALLBACK_H

#include <pthread.h>

#include "openai_oauth_internal.h"

/**
 * parse_callback_request - validate and parse a raw callback HTTP request
 * @request: raw bytes of the HTTP GET request.
 * @request_len: byte length, bounded by OAUTH_REQUEST_MAX.
 * @callback: receives the parsed code/state/denial fields; strings are
 *   caller-owned and freed with free()/secure_free().
 *
 * Enforces GET /auth/callback with HTTP/1.1, single Host header on
 * loopback:1455, no content-length/transfer-encoding, and no control
 * bytes.
 *
 * Return: 0 on success, -1 on any validation failure (callback zeroed).
 */
int parse_callback_request(const void *request, size_t request_len,
                           OAuthCallback *callback);

/**
 * state_matches - constant-time compare of two strings
 * @expected: expected value; NULL fails.
 * @actual: candidate value; NULL fails.
 *
 * Return: 1 when both are non-NULL, equal length, and equal content.
 */
int state_matches(const char *expected, const char *actual);

/**
 * complete_callback_tokens - commit tokens from a callback exchange
 * @auth: manager (lock is taken internally).
 * @generation: the login generation that must still be current.
 * @json: token endpoint response body.
 *
 * Parses, persists, and commits the tokens when the generation is still
 * current; records an error otherwise.
 *
 * Return: 0 on success, -1 on parse or persistence failure.
 */
int complete_callback_tokens(OpenAIOAuth *auth, uint64_t generation,
                             const char *json);

/**
 * cancel_callback_locked - cancel an in-flight callback
 * @auth: manager; the caller must hold @auth->lock.
 *
 * Bumps the generation, requests stop, shuts down tracked sockets, and
 * clears sensitive state.
 *
 * Return: void.
 */
void cancel_callback_locked(OpenAIOAuth *auth);

/**
 * take_callback_thread_locked - claim the joinable callback thread
 * @auth: manager; the caller must hold @auth->lock.
 * @thread: receives the thread handle when one is joinable.
 *
 * Return: 1 when a joinable thread was claimed (caller must join it via
 * join_callback_thread), 0 otherwise.
 */
int take_callback_thread_locked(OpenAIOAuth *auth, pthread_t *thread);

/**
 * join_callback_thread - join a thread handle
 * @thread: handle to join.
 * @joinable: 1 when @thread was actually started.
 *
 * Return: 0 on success or when not joinable, -1 on join failure.
 */
int join_callback_thread(pthread_t thread, int joinable);

/**
 * lifecycle_finish - clear the lifecycle-busy flag and wake waiters
 * @auth: manager (lock is taken internally).
 *
 * Return: void.
 */
void lifecycle_finish(OpenAIOAuth *auth);

/**
 * active_operation_finish - decrement the active-op count and wake waiters
 * @auth: manager (lock is taken internally).
 *
 * Return: void.
 */
void active_operation_finish(OpenAIOAuth *auth);

/**
 * reap_previous_callback - join a finished callback thread, if any
 * @auth: manager (lock is taken internally).
 *
 * Return: 0 on success or when nothing to join, -1 when a callback is
 * still active, the manager is busy/destroying, or join failed.
 */
int reap_previous_callback(OpenAIOAuth *auth);

/**
 * callback_thread_main - callback server thread entry point
 * @userdata: OAuthThreadArgs*; ownership transfers and is freed by the
 *   thread. Created by openai_oauth_start().
 *
 * Listens on loopback:1455, validates and exchanges one callback, then
 * publishes the outcome and finishes the generation.
 *
 * Return: NULL (thread exit).
 */
void *callback_thread_main(void *userdata);

/**
 * shutdown_fd - shutdown a socket with ENOTCONN tolerated
 * @fd: socket to shut down; negative is a no-op.
 *
 * Return: void.
 */
void shutdown_fd(int fd);

#endif /* ECHO_OAUTH_CALLBACK_H */
