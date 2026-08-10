/*
 * openai_oauth.h - OAuth manager for the ChatGPT Codex provider: device
 * and localhost-callback login flows, single-flight token refresh, and
 * encrypted credential persistence via SessionManager.
 * Depends on: session_manager.h.
 */

#ifndef ECHO_OPENAI_OAUTH_H
#define ECHO_OPENAI_OAUTH_H

#include <stddef.h>
#include <time.h>

#include "../session/session_manager.h"

typedef struct OpenAIOAuth OpenAIOAuth;

/* Login state reported by openai_oauth_status(): SIGNED_OUT when no
 * valid credentials are loaded, PENDING while a login transaction is in
 * flight, SIGNED_IN once valid credentials are loaded. */
typedef enum {
    OPENAI_OAUTH_SIGNED_OUT = 0,
    OPENAI_OAUTH_PENDING = 1,
    OPENAI_OAUTH_SIGNED_IN = 2
} OpenAIOAuthState;

/* Access-token outcome codes for the openai_oauth_get_access_token*
 * and refresh families: OK when a fresh token is returned; SIGNED_OUT
 * when no session/credentials are attached; TRANSIENT for retryable
 * transport/parse failures; PERMANENT when the refresh was rejected and
 * the stored credentials were deleted; CANCELLED when the manager was
 * destroyed or superseded mid-flight. */
typedef enum {
    OPENAI_OAUTH_TOKEN_OK = 0,
    OPENAI_OAUTH_TOKEN_SIGNED_OUT = -1,
    OPENAI_OAUTH_TOKEN_TRANSIENT = -2,
    OPENAI_OAUTH_TOKEN_PERMANENT = -3,
    OPENAI_OAUTH_TOKEN_CANCELLED = -4
} OpenAIOAuthTokenResult;

/* Device-flow poll outcome codes from openai_oauth_device_poll():
 * COMPLETE when credentials were obtained and committed; PENDING when
 * not due yet or another poll is in flight; TRANSIENT for retryable
 * failures; TERMINAL when the arguments or transaction are invalid or
 * expired; CANCELLED when the manager was destroyed or superseded
 * mid-poll. */
typedef enum {
    OPENAI_OAUTH_DEVICE_COMPLETE = 0,
    OPENAI_OAUTH_DEVICE_PENDING = 1,
    OPENAI_OAUTH_DEVICE_TRANSIENT = -1,
    OPENAI_OAUTH_DEVICE_TERMINAL = -2,
    OPENAI_OAUTH_DEVICE_CANCELLED = -3
} OpenAIOAuthDeviceResult;

/**
 * openai_oauth_create - allocate an OAuth manager
 *
 * Return: caller-owned OpenAIOAuth, or NULL on allocation or pthread
 * init failure. Free with openai_oauth_destroy(). Thread-safe: all
 * state is mutex-protected, so the manager may be shared across
 * threads.
 */
OpenAIOAuth *openai_oauth_create(void);

/**
 * openai_oauth_destroy - cancel all OAuth work and free the manager
 * @auth: manager to destroy; NULL is accepted as a no-op.
 *
 * The borrowed SessionManager (from openai_oauth_attach_session) must
 * still be alive — it is not freed, only dropped. Blocks until any
 * in-flight callback, refresh, or poll finishes.
 *
 * Return: void.
 */
void openai_oauth_destroy(OpenAIOAuth *auth);

/**
 * openai_oauth_attach_session - borrow a session and load stored credentials
 * @auth: OAuth manager.
 * @sm: SessionManager borrowed until the next attach or destroy; NULL
 *   detaches (in-memory credentials are cleared).
 *
 * Cancels any in-flight callback/refresh first, then loads the
 * encrypted credentials stored under the "openai" provider key.
 *
 * Return: 0 on success — no stored credentials is success (the manager
 * stays signed out). -1 when attach fails (lifecycle busy, thread join
 * or lock failure) or the stored credentials are invalid; in the
 * invalid-credentials case the session is still attached and the
 * reason is recorded in the manager's error field.
 */
int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *sm);

/**
 * openai_oauth_start - begin an interactive localhost-callback login
 * @auth: OAuth manager; must have an attached session.
 * @authorization_url: receives a caller-owned URL to open in a browser;
 *   free with free().
 * @login_id: receives a caller-owned opaque login identifier; free with
 *   free(). Pass it to openai_oauth_status_for_login() and
 *   openai_oauth_cancel_login().
 *
 * Listens on loopback port 1455 and runs the callback exchange on a
 * background thread; credentials are committed and persisted when the
 * callback completes.
 *
 * Return: 0 on success; -1 when no session is attached, a previous
 * login is still active, or thread/socket/allocation setup fails. The
 * setup failure is not recorded in the error field — poll the login
 * via openai_oauth_status_for_login() to observe the outcome.
 */
int openai_oauth_start(OpenAIOAuth *auth, char **authorization_url,
                       char **login_id);

/**
 * openai_oauth_device_start - begin a headless device-flow login
 * @auth: OAuth manager; must have an attached session.
 * @verification_url: receives a caller-owned URL to show the user; free
 *   with free().
 * @user_code: receives a caller-owned code the user enters on that URL;
 *   free with free().
 * @login_id: receives a caller-owned opaque login identifier; free with
 *   free(). Pass it to openai_oauth_device_poll().
 * @poll_interval_seconds: receives the server's minimum interval between
 *   polls.
 *
 * Registers the device with the issuer and stores the pending
 * transaction; advance it by polling openai_oauth_device_poll().
 *
 * Return: 0 on success; -1 on any failure (no session, previous login
 * active, transport/parse error, allocation failure). The reason is
 * recorded in the manager's error field.
 */
int openai_oauth_device_start(OpenAIOAuth *auth, char **verification_url,
                              char **user_code, char **login_id,
                              unsigned int *poll_interval_seconds);

/**
 * openai_oauth_device_poll - perform at most one due poll of a device login
 * @auth: OAuth manager.
 * @login_id: login identifier from openai_oauth_device_start().
 *
 * Polls only when the server interval has elapsed and no other poll is
 * in flight; otherwise it returns pending without touching the network.
 * On completion the credentials are committed and persisted and the
 * transaction is finished.
 *
 * Return: OPENAI_OAUTH_DEVICE_COMPLETE when credentials were obtained;
 * OPENAI_OAUTH_DEVICE_PENDING when not due yet or another poll is in
 * flight; OPENAI_OAUTH_DEVICE_TRANSIENT for retryable failures (call
 * again later); OPENAI_OAUTH_DEVICE_TERMINAL for invalid arguments, a
 * login_id that does not match the current login, or expiry — the
 * transaction is over; OPENAI_OAUTH_DEVICE_CANCELLED when the manager
 * was destroyed or superseded mid-poll.
 */
OpenAIOAuthDeviceResult openai_oauth_device_poll(OpenAIOAuth *auth,
                                                 const char *login_id);

/**
 * openai_oauth_status - query login state and public metadata
 * @auth: OAuth manager.
 * @account_id: receives a caller-owned account id string when signed in,
 *   else NULL; free with free(). May be NULL.
 * @plan_type: receives a caller-owned plan-type string when signed in,
 *   else NULL; free with free(). May be NULL.
 * @error: receives a caller-owned error message when the last operation
 *   failed, else NULL; free with free(). May be NULL.
 *
 * NULL output pointers are accepted and skipped.
 *
 * Return: OPENAI_OAUTH_SIGNED_OUT, OPENAI_OAUTH_PENDING (a login
 * transaction is active), or OPENAI_OAUTH_SIGNED_IN. Outputs are left
 * NULL if a copy fails (allocation); the state is still returned.
 * Thread-safe; internally mutex-protected.
 */
OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error);

/**
 * openai_oauth_status_for_login - status restricted to one login
 * @auth: OAuth manager.
 * @login_id: login identifier from openai_oauth_start() or
 *   openai_oauth_device_start().
 * @state: receives the status (same values as openai_oauth_status()).
 * @account_id, @plan_type, @error: caller-owned outputs as in
 *   openai_oauth_status(); all may be NULL.
 *
 * Public status is only reported when login_id still identifies the
 * current/latest login transaction.
 *
 * Return: 0 on success; -1 when login_id does not match the current
 * login or arguments are invalid — the outputs are left NULL.
 * Thread-safe; internally mutex-protected.
 */
int openai_oauth_status_for_login(OpenAIOAuth *auth, const char *login_id,
                                  OpenAIOAuthState *state, char **account_id,
                                  char **plan_type, char **error);

/**
 * openai_oauth_cancel_login - cancel a pending login transaction
 * @auth: OAuth manager.
 * @login_id: login identifier from openai_oauth_start() or
 *   openai_oauth_device_start().
 *
 * Cancels the callback thread or device poll only when login_id matches
 * the current login; other transactions are never disturbed.
 *
 * Return: 0 when the transaction was cancelled; -1 when nothing was
 * cancelled (login_id mismatch, no active login, or the manager is busy
 * or destroying). Thread-safe; internally mutex-protected.
 */
int openai_oauth_cancel_login(OpenAIOAuth *auth, const char *login_id);

/**
 * openai_oauth_get_access_token - get a valid access token, refreshing if due
 * @auth: OAuth manager.
 * @access_token: receives a caller-owned access token (secret); free
 *   with free().
 * @account_id: receives a caller-owned account id string, or NULL when
 *   unknown; free with free().
 *
 * Refreshes (single-flight) when the token is within the skew window of
 * expiry; see openai_oauth_test_needs_refresh for the window.
 *
 * Return: 0 on success; -1 for any signed-out or refresh failure. Use
 * openai_oauth_get_access_token_result() to distinguish transient from
 * permanent failures. Thread-safe; internally mutex-protected.
 */
int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id);

/**
 * openai_oauth_get_access_token_result - get a token with failure detail
 * @auth: OAuth manager.
 * @access_token: receives a caller-owned access token (secret); free
 *   with free().
 * @account_id: receives a caller-owned account id string, or NULL when
 *   unknown; free with free().
 *
 * Same behavior as openai_oauth_get_access_token(), but preserves the
 * failure class instead of collapsing it to -1.
 *
 * Return: OPENAI_OAUTH_TOKEN_OK (0) with caller-owned outputs on
 * success; OPENAI_OAUTH_TOKEN_SIGNED_OUT (-1) when no session or
 * credentials are attached; OPENAI_OAUTH_TOKEN_TRANSIENT (-2) for
 * transport/parse/other retryable failures; OPENAI_OAUTH_TOKEN_PERMANENT
 * (-3) when the refresh was rejected and the stored credentials were
 * deleted; OPENAI_OAUTH_TOKEN_CANCELLED (-4) when the manager was
 * destroyed or superseded mid-refresh. Outputs are NULL on non-OK.
 * Thread-safe; internally mutex-protected.
 */
OpenAIOAuthTokenResult openai_oauth_get_access_token_result(
    OpenAIOAuth *auth, char **access_token, char **account_id);

/**
 * openai_oauth_force_refresh - force one single-flight token refresh
 * @auth: OAuth manager.
 * @access_token: receives a caller-owned access token (secret); free
 *   with free().
 * @account_id: receives a caller-owned account id string, or NULL when
 *   unknown; free with free().
 *
 * Ignores the expiry skew window and always refreshes. If another
 * refresh is already in flight this call waits for it and returns its
 * outcome (or the current token when the other caller refreshed first).
 *
 * Return: same OpenAIOAuthTokenResult codes as
 * openai_oauth_get_access_token_result(). Thread-safe; internally
 * mutex-protected.
 */
OpenAIOAuthTokenResult openai_oauth_force_refresh(OpenAIOAuth *auth,
                                                   char **access_token,
                                                   char **account_id);

/**
 * openai_oauth_refresh_after_401 - refresh only if the rejected token is current
 * @auth: OAuth manager.
 * @rejected_access_token: the access token that produced the 401
 *   (secret); compared against the stored token.
 * @access_token: receives a caller-owned access token (secret); free
 *   with free().
 * @account_id: receives a caller-owned account id string, or NULL when
 *   unknown; free with free().
 *
 * When the stored token no longer matches the rejected one, another
 * caller already refreshed: the current token is returned without a
 * new refresh. When it still matches, one forced single-flight refresh
 * runs.
 *
 * Return: same OpenAIOAuthTokenResult codes as
 * openai_oauth_get_access_token_result(). Thread-safe; internally
 * mutex-protected.
 */
OpenAIOAuthTokenResult openai_oauth_refresh_after_401(
    OpenAIOAuth *auth, const char *rejected_access_token,
    char **access_token, char **account_id);

/**
 * openai_oauth_logout - cancel login/refresh and remove all credentials
 * @auth: OAuth manager.
 *
 * Cancels any callback or poll, waits for in-flight work, deletes the
 * encrypted credentials from the session, and clears all in-memory
 * token state.
 *
 * Return: 0 on success (credentials deleted and in-memory state
 * cleared); -1 when no session is attached, the stored credentials
 * could not be deleted (in-memory credentials are kept), or the
 * manager is busy or destroying. Thread-safe; internally
 * mutex-protected.
 */
int openai_oauth_logout(OpenAIOAuth *auth);

#ifdef OPENAI_OAUTH_TEST
/**
 * openai_oauth_test_build_authorize_url_alloc - build the issuer authorize URL
 * @state: OAuth state value embedded in the URL (borrowed).
 * @challenge: PKCE challenge value embedded in the URL (borrowed).
 *
 * Test-only hook wrapping the login URL builder; performs no network
 * I/O.
 *
 * Return: caller-owned URL string, or NULL on allocation failure. Free
 * with free(). Thread-safe; no shared state.
 */
char *openai_oauth_test_build_authorize_url_alloc(const char *state,
                                             const char *challenge);

/**
 * openai_oauth_test_pkce_challenge_alloc - derive the S256 PKCE challenge
 * @verifier: code verifier string (borrowed).
 *
 * Test-only hook wrapping the challenge derivation; performs no network
 * I/O.
 *
 * Return: caller-owned base64url challenge, or NULL on allocation
 * failure. Free with free(). Thread-safe; no shared state.
 */
char *openai_oauth_test_pkce_challenge_alloc(const char *verifier);

/**
 * openai_oauth_test_parse_callback - parse a raw callback HTTP request
 * @request: complete raw HTTP request bytes (GET line and headers, no
 *   body).
 * @request_len: byte length of request.
 * @code: receives a caller-owned authorization code, or NULL when the
 *   request carried a denial; free with free().
 * @state: receives a caller-owned OAuth state string; free with free().
 * @denial: receives a caller-owned error string when the user denied,
 *   else NULL; free with free().
 *
 * Test-only hook wrapping the strict callback parser (GET only, Host
 * validation, header and query bounds, no duplicate keys); performs no
 * network I/O.
 *
 * Return: 0 on success; -1 when the request is malformed or lacks the
 * required fields — outputs are left NULL.
 */
int openai_oauth_test_parse_callback(const void *request, size_t request_len,
                                     char **code, char **state, char **denial);

/**
 * openai_oauth_test_parse_token - parse a token-endpoint response
 * @json: token response JSON body.
 * @has_refresh: 1 to simulate an existing refresh token that is kept
 *   when the JSON omits refresh_token; 0 otherwise.
 * @now: current time used to compute expires_at.
 * @access: receives a caller-owned access token (secret); free with
 *   free().
 * @refresh: receives a caller-owned refresh token (secret); free with
 *   free().
 * @account: receives a caller-owned account id from the id_token, or
 *   NULL when absent; free with free().
 * @plan: receives a caller-owned plan type from the id_token, or NULL
 *   when absent; free with free().
 * @expires_at: receives the absolute expiry time.
 *
 * Test-only hook wrapping the token-set parser used by the callback and
 * refresh paths; performs no network I/O.
 *
 * Return: 0 on success; -1 on invalid JSON or missing/invalid fields —
 * outputs are left NULL/0.
 */
int openai_oauth_test_parse_token(const char *json, int has_refresh,
                                  time_t now, char **access, char **refresh,
                                  char **account, char **plan,
                                  time_t *expires_at);

/**
 * openai_oauth_test_jwt_metadata - extract account and plan from an id_token
 * @jwt: JWT string; the payload segment is decoded and parsed.
 * @account: receives a caller-owned chatgpt_account_id, or NULL when the
 *   JWT lacks it; free with free().
 * @plan: receives a caller-owned chatgpt_plan_type, or NULL when the
 *   JWT lacks it; free with free().
 *
 * Test-only hook wrapping the id_token claim extraction; performs no
 * network I/O.
 *
 * Return: 0 on success; -1 on a malformed JWT or invalid fields —
 * outputs are NULL.
 */
int openai_oauth_test_jwt_metadata(const char *jwt, char **account, char **plan);

/**
 * openai_oauth_test_needs_refresh - decide whether a refresh is due
 * @expires_at: absolute token expiry (seconds since epoch).
 * @now: current time (seconds since epoch).
 *
 * Test-only hook wrapping the expiry check used before every token
 * access; performs no network I/O.
 *
 * Return: 1 when the token is expired or within the 300-second skew
 * window of expiry, 0 otherwise.
 */
int openai_oauth_test_needs_refresh(time_t expires_at, time_t now);

/**
 * openai_oauth_test_parse_device_start - parse a device-start response
 * @json: device registration JSON body.
 * @device_auth_id: receives a caller-owned device id; free with free().
 * @user_code: receives a caller-owned user code; free with free().
 * @interval: receives the server's poll interval in seconds (1-300).
 * @expires_in: receives the registration lifetime in seconds (1-3600).
 *
 * Test-only hook wrapping the device-start parser used by
 * openai_oauth_device_start(); performs no network I/O.
 *
 * Return: 0 on success; -1 on invalid JSON or missing/invalid fields —
 * outputs are left NULL/0.
 */
int openai_oauth_test_parse_device_start(const char *json, char **device_auth_id,
                                          char **user_code,
                                          unsigned int *interval,
                                          unsigned int *expires_in);

/**
 * openai_oauth_test_parse_device_authorization - parse a device token response
 * @json: device authorization JSON body.
 * @code: receives a caller-owned authorization code; free with free().
 * @verifier: receives a caller-owned PKCE verifier (secret); free with
 *   free().
 *
 * Test-only hook wrapping the authorization parser used by
 * openai_oauth_device_poll(); performs no network I/O.
 *
 * Return: 0 on success; -1 on invalid JSON or missing/invalid fields —
 * outputs are left NULL.
 */
int openai_oauth_test_parse_device_authorization(const char *json, char **code,
                                                  char **verifier);
#endif

#endif
