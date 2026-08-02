#include <stdarg.h>
#include <stdlib.h>

#include "llm/openai_oauth.h"
#include "utils/logging.h"

int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id)
{
    (void)auth;
    if (access_token) *access_token = NULL;
    if (account_id) *account_id = NULL;
    return -1;
}

OpenAIOAuthTokenResult openai_oauth_refresh_after_401(
    OpenAIOAuth *auth, const char *rejected_access_token,
    char **access_token, char **account_id)
{
    (void)auth;
    (void)rejected_access_token;
    if (access_token) *access_token = NULL;
    if (account_id) *account_id = NULL;
    return OPENAI_OAUTH_TOKEN_SIGNED_OUT;
}

void log_msg(LogLevel level, const char *file, int line, const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}
