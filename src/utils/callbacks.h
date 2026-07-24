#ifndef ECHO_CALLBACKS_H
#define ECHO_CALLBACKS_H

#define MAX_CALLBACKS 16

typedef struct CallbackManager CallbackManager;

typedef void (*on_run_start_fn)(const char *run_id, const char *prompt, void *userdata);
typedef void (*on_run_end_fn)(const char *run_id, const char *response, void *userdata);
typedef void (*on_run_error_fn)(const char *run_id, const char *error, void *userdata);
typedef void (*on_llm_start_fn)(const char *run_id, int message_count, void *userdata);
typedef void (*on_llm_end_fn)(const char *run_id, const char *response, void *userdata);
typedef void (*on_tool_start_fn)(const char *run_id, const char *tool_name, const char *args, void *userdata);
typedef void (*on_tool_end_fn)(const char *run_id, const char *tool_name, const char *result, void *userdata);
typedef void (*on_tool_error_fn)(const char *run_id, const char *tool_name, const char *error, void *userdata);

typedef struct {
    on_run_start_fn on_run_start;
    on_run_end_fn on_run_end;
    on_run_error_fn on_run_error;
    on_llm_start_fn on_llm_start;
    on_llm_end_fn on_llm_end;
    on_tool_start_fn on_tool_start;
    on_tool_end_fn on_tool_end;
    on_tool_error_fn on_tool_error;
    void *userdata;
} CallbackHooks;

struct CallbackManager {
    CallbackHooks hooks[MAX_CALLBACKS];
    int count;
};

CallbackManager *cb_manager_create(void);
void cb_manager_destroy(CallbackManager *mgr);
int cb_manager_register(CallbackManager *mgr, CallbackHooks hooks);

void cb_manager_run_start(CallbackManager *mgr, const char *run_id, const char *prompt);
void cb_manager_run_end(CallbackManager *mgr, const char *run_id, const char *response);
void cb_manager_run_error(CallbackManager *mgr, const char *run_id, const char *error);
void cb_manager_llm_start(CallbackManager *mgr, const char *run_id, int message_count);
void cb_manager_llm_end(CallbackManager *mgr, const char *run_id, const char *response);
void cb_manager_tool_start(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *args);
void cb_manager_tool_end(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *result);
void cb_manager_tool_error(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *error);

#endif
