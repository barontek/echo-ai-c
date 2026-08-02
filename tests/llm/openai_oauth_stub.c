#include "llm/openai_oauth.h"

int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id)
{
    (void)auth;
    if (access_token) *access_token = NULL;
    if (account_id) *account_id = NULL;
    return -1;
}
