/*
 * callbacks.h - synchronous dispatch registry for run/LLM/tool lifecycle
 * hooks (start/end/error). Depends on: none (self-contained).
 */

#ifndef ECHO_CALLBACKS_H
#define ECHO_CALLBACKS_H

#define MAX_CALLBACKS 16

typedef struct CallbackManager CallbackManager;

/* Callback signatures. Every callback fires synchronously from the dispatch
 * functions, in registration order; all string arguments are borrowed and
 * only valid for the duration of the call — never retain the pointers.
 * userdata is the per-hook opaque pointer from the matching CallbackHooks. */
typedef void (*on_run_start_fn)(const char *run_id, const char *prompt, void *userdata);
typedef void (*on_run_end_fn)(const char *run_id, const char *response, void *userdata);
typedef void (*on_run_error_fn)(const char *run_id, const char *error, void *userdata);
typedef void (*on_llm_start_fn)(const char *run_id, int message_count, void *userdata);
typedef void (*on_llm_end_fn)(const char *run_id, const char *response, void *userdata);
typedef void (*on_tool_start_fn)(const char *run_id, const char *tool_name, const char *args, void *userdata);
typedef void (*on_tool_end_fn)(const char *run_id, const char *tool_name, const char *result, void *userdata);
typedef void (*on_tool_error_fn)(const char *run_id, const char *tool_name, const char *error, void *userdata);

/* One registered hook set; any field may be left NULL to not receive that
 * event. Copied by value at registration, so the caller may reuse or free
 * its own struct after cb_manager_register. */
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

/**
 * cb_manager_create - allocate an empty callback manager
 *
 * Return: caller-owned CallbackManager with no registered hook sets, or
 * NULL on allocation failure. Release with cb_manager_destroy(). No shared
 * state; the returned manager is a standalone object.
 */
CallbackManager *cb_manager_create(void);

/**
 * cb_manager_destroy - free a callback manager
 * @mgr: manager to free; NULL is a safe no-op.
 *
 * Does not invoke any registered callbacks; hook sets hold no resources of
 * their own (string arguments are borrowed), so nothing else is freed.
 *
 * Return: void; never fails. Thread-safety: safe only while no other
 * thread is still dispatching through the same manager.
 */
void cb_manager_destroy(CallbackManager *mgr);

/**
 * cb_manager_register - add a hook set to a manager
 * @mgr: target manager.
 * @hooks: hook set copied by value; any field may be NULL to opt out of
 *   that event. The caller's struct is not referenced afterwards and may
 *   be reused or freed.
 *
 * Registered hooks fire in registration order. Registering the same
 * function twice fires it twice.
 *
 * Return: 0 on success, -1 when mgr is NULL or the manager already holds
 * MAX_CALLBACKS hook sets. Never fails for any other reason; no error log.
 * Thread-safety: safe only when no other thread dispatches or registers
 * through the same manager concurrently.
 */
int cb_manager_register(CallbackManager *mgr, CallbackHooks hooks);

/**
 * cb_manager_run_start - dispatch the run-start event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @prompt: initial prompt text, or NULL; borrowed like run_id.
 *
 * Calls every registered on_run_start hook synchronously, in
 * registration order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_run_start(CallbackManager *mgr, const char *run_id, const char *prompt);

/**
 * cb_manager_run_end - dispatch the run-end event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @response: final response text, or NULL; borrowed like run_id.
 *
 * Calls every registered on_run_end hook synchronously, in registration
 * order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_run_end(CallbackManager *mgr, const char *run_id, const char *response);

/**
 * cb_manager_run_error - dispatch the run-error event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @error: error description, or NULL; borrowed like run_id.
 *
 * Calls every registered on_run_error hook synchronously, in
 * registration order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_run_error(CallbackManager *mgr, const char *run_id, const char *error);

/**
 * cb_manager_llm_start - dispatch the LLM-start event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @message_count: number of messages sent to the LLM.
 *
 * Calls every registered on_llm_start hook synchronously, in
 * registration order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_llm_start(CallbackManager *mgr, const char *run_id, int message_count);

/**
 * cb_manager_llm_end - dispatch the LLM-end event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @response: LLM reply text, or NULL; borrowed like run_id.
 *
 * Calls every registered on_llm_end hook synchronously, in registration
 * order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_llm_end(CallbackManager *mgr, const char *run_id, const char *response);

/**
 * cb_manager_tool_start - dispatch the tool-start event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @tool_name: name of the tool being invoked, or NULL; borrowed like
 *   run_id.
 * @args: serialized tool arguments, or NULL; borrowed like run_id.
 *
 * Calls every registered on_tool_start hook synchronously, in
 * registration order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_tool_start(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *args);

/**
 * cb_manager_tool_end - dispatch the tool-end event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @tool_name: name of the tool that ran, or NULL; borrowed like run_id.
 * @result: tool result text, or NULL; borrowed like run_id.
 *
 * Calls every registered on_tool_end hook synchronously, in registration
 * order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_tool_end(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *result);

/**
 * cb_manager_tool_error - dispatch the tool-error event
 * @mgr: manager to dispatch through; NULL is a no-op.
 * @run_id: run identifier forwarded to the callbacks; borrowed, valid
 *   only for the duration of the call.
 * @tool_name: name of the tool that failed, or NULL; borrowed like run_id.
 * @error: error description, or NULL; borrowed like run_id.
 *
 * Calls every registered on_tool_error hook synchronously, in
 * registration order, passing the manager-stored userdata of each hook.
 *
 * Return: void; never fails. Thread-safety: safe only when no other
 * thread dispatches or registers through the same manager concurrently.
 */
void cb_manager_tool_error(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *error);

#endif
