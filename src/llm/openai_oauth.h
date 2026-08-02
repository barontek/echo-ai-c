#ifndef ECHO_OPENAI_OAUTH_H
#define ECHO_OPENAI_OAUTH_H

#include "../session/session_manager.h"

typedef struct OpenAIOAuth OpenAIOAuth;

typedef enum {
    OPENAI_OAUTH_SIGNED_OUT = 0,
    OPENAI_OAUTH_PENDING = 1,
    OPENAI_OAUTH_SIGNED_IN = 2
} OpenAIOAuthState;

/* Creates an OAuth manager. The returned manager is owned by the caller and
 * must be destroyed with openai_oauth_destroy. */
OpenAIOAuth *openai_oauth_create(void);
void openai_oauth_destroy(OpenAIOAuth *auth);

/* Attaches encrypted credential storage. The session manager is borrowed and
 * must outlive auth; existing credentials are loaded and decrypted. */
int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *sm);

/* Starts a browser OAuth flow. The returned URL and login id are caller-owned
 * strings. Returns -1 if another flow is active or the loopback listener fails. */
int openai_oauth_start(OpenAIOAuth *auth, char **authorization_url,
                       char **login_id);

/* Returns current state and caller-owned copies of public status fields. */
OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error);

/* Resolves a valid access token, refreshing it synchronously when needed. The
 * returned token and account id are caller-owned. */
int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id);

/* Removes the in-memory and encrypted OAuth credentials. */
int openai_oauth_logout(OpenAIOAuth *auth);

#ifdef OPENAI_OAUTH_TEST
char *openai_oauth_test_build_authorize_url(const char *state,
                                            const char *challenge);
int openai_oauth_test_parse_callback(const char *request, char **code,
                                     char **state);
#endif

#endif
