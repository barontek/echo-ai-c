#include <stdarg.h>
#include <stdlib.h>

#include "llm/openai_oauth.h"
#include "utils/logging.h"

/* openai_oauth_stubs - unit tests for openai oauth stubs. Depends on: check, the module under test. */
int session_manager_save_provider_oauth(SessionManager *session,
                                        const char *provider, const char *data)
{
    (void)session;
    (void)provider;
    (void)data;
    return -1;
}

ProviderOAuthLoadResult session_manager_load_provider_oauth_ex(
    SessionManager *session, const char *provider, char **data_out)
{
    (void)session;
    (void)provider;
    if (data_out) *data_out = NULL;
    return PROVIDER_OAUTH_LOAD_NOT_FOUND;
}

int session_manager_delete_provider_oauth(SessionManager *session,
                                          const char *provider)
{
    (void)session;
    (void)provider;
    return -1;
}

void log_msg(LogLevel level, const char *file, int line, const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}
