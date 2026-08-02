#include "llm/openai_oauth.h"

int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *sm)
{
    (void)auth; (void)sm;
    return 0;
}

OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error)
{
    (void)auth;
    if (account_id) *account_id = NULL;
    if (plan_type) *plan_type = NULL;
    if (error) *error = NULL;
    return OPENAI_OAUTH_SIGNED_OUT;
}
