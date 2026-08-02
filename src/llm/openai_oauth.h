#ifndef ECHO_OPENAI_OAUTH_H
#define ECHO_OPENAI_OAUTH_H

#include <stddef.h>
#include <time.h>

#include "../session/session_manager.h"

typedef struct OpenAIOAuth OpenAIOAuth;

typedef enum {
    OPENAI_OAUTH_SIGNED_OUT = 0,
    OPENAI_OAUTH_PENDING = 1,
    OPENAI_OAUTH_SIGNED_IN = 2
} OpenAIOAuthState;

typedef enum {
    OPENAI_OAUTH_TOKEN_OK = 0,
    OPENAI_OAUTH_TOKEN_SIGNED_OUT = -1,
    OPENAI_OAUTH_TOKEN_TRANSIENT = -2,
    OPENAI_OAUTH_TOKEN_PERMANENT = -3,
    OPENAI_OAUTH_TOKEN_CANCELLED = -4
} OpenAIOAuthTokenResult;

typedef enum {
    OPENAI_OAUTH_DEVICE_COMPLETE = 0,
    OPENAI_OAUTH_DEVICE_PENDING = 1,
    OPENAI_OAUTH_DEVICE_TRANSIENT = -1,
    OPENAI_OAUTH_DEVICE_TERMINAL = -2,
    OPENAI_OAUTH_DEVICE_CANCELLED = -3
} OpenAIOAuthDeviceResult;

/* Returns a caller-owned manager, or NULL on allocation/synchronization failure. */
OpenAIOAuth *openai_oauth_create(void);

/* Cancels work and frees auth; its borrowed SessionManager must still be alive. */
void openai_oauth_destroy(OpenAIOAuth *auth);

/* Borrows sm until detach/destroy and atomically loads its stored credentials. */
int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *sm);

/* Returns caller-owned URL/login ID; fails without attached encrypted storage. */
int openai_oauth_start(OpenAIOAuth *auth, char **authorization_url,
                       char **login_id);

/* Starts headless login and returns caller-owned verification data/login ID. */
int openai_oauth_device_start(OpenAIOAuth *auth, char **verification_url,
                              char **user_code, char **login_id,
                              unsigned int *poll_interval_seconds);

/* Performs at most one due device poll and commits credentials on completion. */
OpenAIOAuthDeviceResult openai_oauth_device_poll(OpenAIOAuth *auth,
                                                 const char *login_id);

/* Returns state and caller-owned public fields; NULL output pointers are valid. */
OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error);

/* Returns public status only when login_id identifies the current/latest login. */
int openai_oauth_status_for_login(OpenAIOAuth *auth, const char *login_id,
                                  OpenAIOAuthState *state, char **account_id,
                                  char **plan_type, char **error);

/* Cancels only the pending transaction identified by login_id. */
int openai_oauth_cancel_login(OpenAIOAuth *auth, const char *login_id);

/* Returns caller-owned credentials, or -1 for any signed-out/refresh failure. */
int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id);

/* Returns caller-owned credentials and preserves transient/permanent failures. */
OpenAIOAuthTokenResult openai_oauth_get_access_token_result(
    OpenAIOAuth *auth, char **access_token, char **account_id);

/* Forces one single-flight refresh after a 401 and returns caller-owned fields. */
OpenAIOAuthTokenResult openai_oauth_force_refresh(OpenAIOAuth *auth,
                                                   char **access_token,
                                                   char **account_id);

/* Refreshes only if rejected_access_token is still current after a 401. */
OpenAIOAuthTokenResult openai_oauth_refresh_after_401(
    OpenAIOAuth *auth, const char *rejected_access_token,
    char **access_token, char **account_id);

/* Cancels login/refresh, then removes encrypted and in-memory credentials. */
int openai_oauth_logout(OpenAIOAuth *auth);

#ifdef OPENAI_OAUTH_TEST
char *openai_oauth_test_build_authorize_url(const char *state,
                                             const char *challenge);
char *openai_oauth_test_pkce_challenge(const char *verifier);
int openai_oauth_test_parse_callback(const void *request, size_t request_len,
                                     char **code, char **state, char **denial);
int openai_oauth_test_parse_token(const char *json, int has_refresh,
                                  time_t now, char **access, char **refresh,
                                  char **account, char **plan,
                                  time_t *expires_at);
int openai_oauth_test_jwt_metadata(const char *jwt, char **account, char **plan);
int openai_oauth_test_needs_refresh(time_t expires_at, time_t now);
int openai_oauth_test_parse_device_start(const char *json, char **device_auth_id,
                                          char **user_code,
                                          unsigned int *interval,
                                          unsigned int *expires_in);
int openai_oauth_test_parse_device_authorization(const char *json, char **code,
                                                  char **verifier);
#endif

#endif
