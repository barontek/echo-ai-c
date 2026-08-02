#ifndef ECHO_REGISTRY_H
#define ECHO_REGISTRY_H

#include "tool.h"
#include "search_provider.h"
#include "../safety/safety.h"
#include "../change_tracker/change_tracker.h"
#include "../session/session_manager.h"
#include "../llm/openai_oauth.h"

void registry_init(SafetyConfig *safety);
void registry_register(Tool *tool);
void registry_set_enabled(const char *names);
Tool *registry_get(const char *name);
char *registry_schemas_json(void);
int registry_count(void);
void registry_set_change_tracker(ChangeTracker *ct);
void registry_set_search_provider(SearchProvider *sp);
SearchProvider *registry_get_search_provider(void);
void registry_set_session_manager(SessionManager *sm);
SessionManager *registry_get_session_manager(void);
/* Stores a borrowed OAuth manager that outlives registered delegate tools. */
void registry_set_openai_oauth(OpenAIOAuth *auth);
OpenAIOAuth *registry_get_openai_oauth(void);
void registry_set_delegate_config(const char *provider_name, const char *base_url,
                                   const char *api_token, const char *model,
                                   int num_ctx, int keep_alive_secs,
                                   double temperature, int timeout, int max_iterations);
int registry_get_delegate_config(const char **provider_name, const char **base_url,
                                  const char **api_token, const char **model,
                                  int *num_ctx, int *keep_alive_secs,
                                  double *temperature, int *timeout, int *max_iterations);
void registry_set_ask_user_callback(char *(*cb)(const char *, void *), void *userdata);
char *registry_invoke_ask_user(const char *question);
int registry_has_ask_user_callback(void);
void registry_destroy(void);

#ifdef REGISTRY_TEST
void registry_test_set_alloc_fail(int nth_allocation);
#endif

#endif
