#include "llm/openai_oauth.h"

OpenAIOAuthState openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_OUT;
int openai_oauth_stub_attach_result = 0;

int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *sm)
{
    (void)auth; (void)sm;
    return openai_oauth_stub_attach_result;
}

OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error)
{
    (void)auth;
    if (account_id) *account_id = NULL;
    if (plan_type) *plan_type = NULL;
    if (error) *error = NULL;
    return openai_oauth_stub_state;
}
