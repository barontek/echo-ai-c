/*
 * session_manager.c - refcounted, mutex-protected session store facade:
 * lifecycle, provider-oauth storage, message append/truncate, event
 * logging, and the fault-injection test hooks. Schema/serialization/
 * listing/purge live in session_db/serialize/list/purge units; branch
 * fork/switch/tag/branch_info live in session_branch.c.
 * Depends on: sqlite3, pthreads, encryption, migration, memory, session,
 * utils (logging, string_utils).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include <cjson/cJSON.h>
#include <sqlite3.h>

#include "session_manager.h"
#include "session_manager_internal.h"
#include "session_db.h"
#include "session_serialize.h"
#include "memory.h"
#include "migration.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#ifdef SESSION_MANAGER_TEST
static int sm_alloc_counter = 0;
static int sm_alloc_fail_at = -1;
static int sm_realloc_counter = 0;
static int sm_realloc_fail_at = -1;
static int sm_asprintf_counter = 0;
static int sm_asprintf_fail_at = -1;



void session_manager_test_set_alloc_fail(int nth_allocation)
{
    sm_alloc_counter = 0;
    sm_alloc_fail_at = nth_allocation;
}

void session_manager_test_set_realloc_fail(int nth_allocation)
{
    sm_realloc_counter = 0;
    sm_realloc_fail_at = nth_allocation;
}

void *sm_test_realloc(void *ptr, size_t size)
{
    sm_realloc_counter++;
    if (sm_realloc_counter == sm_realloc_fail_at) return NULL;
    return realloc(ptr, size);
}

char *sm_test_strdup(const char *s)
{
    sm_alloc_counter++;
    if (sm_alloc_counter == sm_alloc_fail_at) return NULL;
    return str_dup(s);
}

int sm_test_asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vasprintf(strp, fmt, ap);
    va_end(ap);
    sm_asprintf_counter++;
    if (sm_asprintf_counter == sm_asprintf_fail_at)
    {
        free(*strp);
        *strp = NULL;
        return -1;
    }
    return rc;
}

void session_manager_test_set_asprintf_fail(int nth)
{
    sm_asprintf_counter = 0;
    sm_asprintf_fail_at = nth;
}

#define str_dup sm_test_strdup
#define realloc sm_test_realloc
#define asprintf sm_test_asprintf

#define str_dup sm_test_strdup
#define realloc sm_test_realloc
#define asprintf sm_test_asprintf

/* Fault-injection knob for B1: lets a test force the Nth
 * sqlite3_bind_* call (across bind_text/bind_int/bind_blob/bind_null in any
 * session_manager function) to return SQLITE_NOMEM so we can prove the
 * save path actually checks binds instead of trusting them. Production
 * builds never see this; only translation units compiled with
 * -DSESSION_MANAGER_TEST=1 do. */
static int sm_bind_counter = 0;
static int sm_bind_fail_at = -1;

/* Fault-injection knobs for C13: force the Nth encryption_encrypt call
 * (all session_manager functions) and the Nth oauth allocation to fail,
 * so the save/load paths can be proven to check them. */
static int sm_enc_counter = 0;
static int sm_enc_fail_at = -1;
static int sm_oauth_alloc_counter = 0;
static int sm_oauth_alloc_fail_at = -1;


void session_manager_test_set_bind_fail(int nth_bind)
{
    sm_bind_counter = 0;
    sm_bind_fail_at = nth_bind;
}

int sm_test_bind_text(sqlite3_stmt *s, int idx, const char *t, int n,
                             void (*del)(void *))
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_text(s, idx, t, n, del);
}

int sm_test_bind_int(sqlite3_stmt *s, int idx, int v)
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_int(s, idx, v);
}

int sm_test_bind_blob(sqlite3_stmt *s, int idx, const void *p, int n,
                             void (*del)(void *))
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_blob(s, idx, p, n, del);
}

int sm_test_bind_null(sqlite3_stmt *s, int idx)
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_null(s, idx);
}

void session_manager_test_set_encrypt_fail(int nth_encrypt)
{
    sm_enc_counter = 0;
    sm_enc_fail_at = nth_encrypt;
}

void session_manager_test_set_oauth_alloc_fail(int nth_allocation)
{
    sm_oauth_alloc_counter = 0;
    sm_oauth_alloc_fail_at = nth_allocation;
}

static void *sm_oauth_malloc(size_t size)
{
    sm_oauth_alloc_counter++;
    if (sm_oauth_alloc_counter == sm_oauth_alloc_fail_at) return NULL;
    return malloc(size);
}

unsigned char *sm_test_encrypt(const EncryptionKey *key,
                                      const unsigned char *pt, int pt_len,
                                      int *out_len)
{
    sm_enc_counter++;
    if (sm_enc_counter == sm_enc_fail_at) return NULL;
    return encryption_encrypt(key, pt, pt_len, out_len);
}

#define sqlite3_bind_text  sm_test_bind_text
#define sqlite3_bind_int   sm_test_bind_int
#define sqlite3_bind_blob  sm_test_bind_blob
#define sqlite3_bind_null  sm_test_bind_null
#define encryption_encrypt sm_test_encrypt
#define oauth_malloc       sm_oauth_malloc
#else
#define oauth_malloc       malloc
#endif

#define SALT_FILE "salt"
#define DB_FILE "echo-ai.db"



int session_manager_save_provider_oauth(SessionManager *sm,
                                         const char *provider_name,
                                         const char *data)
{
    if (!sm || !sm->db || !sm->key_initialized || !provider_name ||
        !provider_name[0] || !data || !data[0])
        return -1;
    size_t provider_len = strlen(provider_name);
    size_t plain_len = strlen(data);
    if (provider_len > (size_t)INT_MAX ||
        plain_len > (size_t)(INT_MAX - 128))
        return -1;

    unsigned char *encrypted = NULL;
    int encrypted_len = 0;
    int result = -1;
    session_manager_lock(sm);
    encrypted = encryption_encrypt(&sm->enc_key, (const unsigned char *)data,
                                   (int)plain_len, &encrypted_len);
    if (!encrypted || encrypted_len <= 0)
    {
        log_error("encrypt provider oauth", "provider", provider_name, NULL);
        goto cleanup;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO provider_oauth(provider, data_encrypted) "
                      "VALUES(?, ?) ON CONFLICT(provider) DO UPDATE SET "
                      "data_encrypted=excluded.data_encrypted";
    int rc = sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 1, provider_name, (int)provider_len,
                               SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_blob(stmt, 2, encrypted, encrypted_len,
                               SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE)
    {
        result = 0;
    }
    else
    {
        log_error("sqlite save provider oauth", "provider", provider_name, NULL);
    }
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK)
    {
        log_error("sqlite finalize provider oauth save", "provider",
                  provider_name, NULL);
        result = -1;
    }

cleanup:
    if (encrypted)
    {
        if (encrypted_len > 0)
            memset(encrypted, 0, (size_t)encrypted_len);
        free(encrypted);
    }
    session_manager_unlock(sm);
    return result;
}

ProviderOAuthLoadResult session_manager_load_provider_oauth_ex(
    SessionManager *sm, const char *provider_name, char **data_out)
{
    if (data_out) *data_out = NULL;
    if (!sm || !sm->db || !sm->key_initialized || !provider_name ||
        !provider_name[0] || !data_out)
        return PROVIDER_OAUTH_LOAD_INVALID_ARGUMENT;
    size_t provider_len = strlen(provider_name);
    if (provider_len > (size_t)INT_MAX)
        return PROVIDER_OAUTH_LOAD_INVALID_ARGUMENT;

    ProviderOAuthLoadResult result = PROVIDER_OAUTH_LOAD_SQL_ERROR;
    char *data = NULL;
    unsigned char *plain = NULL;
    int plain_len = 0;
    session_manager_lock(sm);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT data_encrypted FROM provider_oauth WHERE provider = ?";
    int rc = sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_bind_text(stmt, 1, provider_name, (int)provider_len,
                           SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE)
    {
        result = PROVIDER_OAUTH_LOAD_NOT_FOUND;
        goto cleanup;
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt, 0) != SQLITE_BLOB)
        goto cleanup;

    const void *blob = sqlite3_column_blob(stmt, 0);
    int blob_len = sqlite3_column_bytes(stmt, 0);
    if (!blob || blob_len <= 0)
    {
        result = PROVIDER_OAUTH_LOAD_DECRYPT_ERROR;
        goto cleanup;
    }
    plain = encryption_decrypt(&sm->enc_key, blob, blob_len, &plain_len);
    if (!plain || plain_len <= 0 || memchr(plain, '\0', (size_t)plain_len))
    {
        result = PROVIDER_OAUTH_LOAD_DECRYPT_ERROR;
        goto cleanup;
    }
    data = oauth_malloc((size_t)plain_len + 1U);
    if (!data)
    {
        result = PROVIDER_OAUTH_LOAD_OOM;
        goto cleanup;
    }
    memcpy(data, plain, (size_t)plain_len);
    data[plain_len] = '\0';
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) goto cleanup;
    result = PROVIDER_OAUTH_LOAD_OK;

cleanup:
    if (plain)
    {
        if (plain_len > 0) memset(plain, 0, (size_t)plain_len);
        free(plain);
    }
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK)
        result = PROVIDER_OAUTH_LOAD_SQL_ERROR;
    if (result == PROVIDER_OAUTH_LOAD_SQL_ERROR)
        log_error("sqlite load provider oauth", "provider", provider_name,
                  "err", sqlite3_errmsg(sm->db), NULL);
    else if (result == PROVIDER_OAUTH_LOAD_DECRYPT_ERROR)
        log_error("decrypt provider oauth", "provider", provider_name, NULL);
    else if (result == PROVIDER_OAUTH_LOAD_OOM)
        log_error("allocate provider oauth result", "provider", provider_name,
                  NULL);
    if (result == PROVIDER_OAUTH_LOAD_OK)
    {
        *data_out = data;
        data = NULL;
    }
    free(data);
    session_manager_unlock(sm);
    return result;
}

char *session_manager_load_provider_oauth_alloc(SessionManager *sm,
                                           const char *provider_name)
{
    char *result = NULL;
    if (session_manager_load_provider_oauth_ex(sm, provider_name, &result) !=
        PROVIDER_OAUTH_LOAD_OK)
        return NULL;
    return result;
}

int session_manager_delete_provider_oauth(SessionManager *sm,
                                          const char *provider_name)
{
    if (!sm || !sm->db || !provider_name || !provider_name[0]) return -1;
    size_t provider_len = strlen(provider_name);
    if (provider_len > (size_t)INT_MAX) return -1;

    int result = -1;
    session_manager_lock(sm);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM provider_oauth WHERE provider = ?";
    int rc = sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 1, provider_name, (int)provider_len,
                               SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE)
        result = 0;
    else
        log_error("sqlite delete provider oauth", "provider", provider_name, NULL);
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK)
        result = -1;
    session_manager_unlock(sm);
    return result;
}

static int init_encryption(SessionManager *sm, const char *password,
                           SessionManagerCreateResult *result)
{
    if (migration_check_and_recover(sm, password) != 0)
    {
        log_error("password migration recovery failed", NULL);
        return -1;
    }

    char *salt_path = NULL;
    if (asprintf(&salt_path, "%s/%s", sm->data_dir, SALT_FILE) < 0) return -1;

    int is_first_run = encryption_first_run_detect(sm->data_dir);

    if (is_first_run)
    {
        log_info("first run detected, creating salt", NULL);
        if (encryption_salt_create(salt_path) != 0)
        {
            log_error("failed to create salt", NULL);
            free(salt_path);
            return -1;
        }
    }

    unsigned char salt[64] = {0};
    int salt_len = 0;
    if (encryption_salt_load(salt_path, salt, &salt_len) != 0)
    {
        log_error("failed to load salt", NULL);
        if (is_first_run) (void)unlink(salt_path);
        memset(salt, 0, sizeof(salt));
        free(salt_path);
        return -1;
    }

    if (encryption_key_derive(password, salt, salt_len, &sm->enc_key) != 0)
    {
        log_error("key derivation failed", NULL);
        if (is_first_run) (void)unlink(salt_path);
        memset(salt, 0, sizeof(salt));
        free(salt_path);
        return -1;
    }

    sm->key_initialized = 1;

    char *verifier_path = NULL;
    if (asprintf(&verifier_path, "%s/.verifier", sm->data_dir) < 0)
    {
        if (is_first_run) (void)unlink(salt_path);
        memset(salt, 0, sizeof(salt));
        free(salt_path);
        return -1;
    }
    struct stat st;
    int verifier_exists = stat(verifier_path, &st) == 0;
    int verifier_result = verifier_exists ?
        encryption_check_verifier(&sm->enc_key, verifier_path) :
        (is_first_run ? encryption_create_verifier(&sm->enc_key, verifier_path) : -1);
    if (verifier_result != 0)
    {
        if (verifier_exists) *result = SESSION_MANAGER_CREATE_AUTH_FAILED;
        log_error(verifier_exists ? "password verifier mismatch" :
                    (is_first_run ? "failed to create verifier" :
                                    "password verifier missing"), NULL);
        if (is_first_run)
        {
            (void)unlink(verifier_path);
            (void)unlink(salt_path);
        }
        free(verifier_path);
        memset(salt, 0, sizeof(salt));
        free(salt_path);
        return -1;
    }
    free(verifier_path);
    memset(salt, 0, sizeof(salt));
    free(salt_path);

    log_info("encryption initialized", NULL);
    return 0;
}

SessionManager *session_manager_create(const char *data_dir, const char *password)
{
    return session_manager_create_ex(data_dir, password, NULL);
}

SessionManager *session_manager_create_ex(const char *data_dir,
                                          const char *password,
                                          SessionManagerCreateResult *result)
{
    SessionManagerCreateResult local_result = SESSION_MANAGER_CREATE_STORAGE_FAILED;
    if (!result) result = &local_result;
    *result = SESSION_MANAGER_CREATE_STORAGE_FAILED;
    if (!data_dir || !password) return NULL;

    SessionManager *sm = calloc(1, sizeof(SessionManager));
    if (!sm) return NULL;

    sm->data_dir = str_dup(data_dir);
    if (!sm->data_dir) {
        free(sm);
        return NULL;
    }

    atomic_init(&sm->ref_count, 1U);
    pthread_mutex_init(&sm->lock, NULL);

    /* mkdir_p must run before init_encryption: the salt file is written
     * into data_dir, which doesn't exist yet on a fresh HOME. */
    if (mkdir_p(data_dir) != 0 && errno != EEXIST)
    {
        log_error("failed to create data dir", "path", data_dir, NULL);
        session_manager_free(sm);
        return NULL;
    }

    if (init_encryption(sm, password, result) != 0)
    {
        log_error("failed to initialize encryption", NULL);
        session_manager_free(sm);
        return NULL;
    }

    char *db_path = NULL;
    if (asprintf(&db_path, "%s/%s", sm->data_dir, DB_FILE) < 0)
    {
        session_manager_free(sm);
        return NULL;
    }

    if (sqlite3_open(db_path, &sm->db) != SQLITE_OK)
    {
        log_error("failed to open session database", "path", db_path, NULL);
        free(db_path);
        session_manager_free(sm);
        return NULL;
    }
    free(db_path);

    if (init_db(sm->db) != 0)
    {
        session_manager_free(sm);
        return NULL;
    }

    /* user_memory is a sibling table in the same DB; create it here so every
     * consumer of a SessionManager (build_system_prompt -> memory_list_all,
     * the memory tool, REST/WS init paths) has the same existence guarantee as
     * agent_sessions, regardless of which entrypoint constructed the SM. */
    if (memory_table_init(sm->db) != 0)
    {
        log_error("failed to initialize user_memory table", NULL);
        session_manager_free(sm);
        return NULL;
    }

    log_info("session manager initialized", "data_dir", sm->data_dir, NULL);
    *result = SESSION_MANAGER_CREATE_OK;
    return sm;
}

SessionManager *session_manager_retain(SessionManager *sm)
{
    if (!sm) return NULL;
    unsigned int old_count = atomic_fetch_add_explicit(
        &sm->ref_count, 1U, memory_order_relaxed);
    if (old_count == 0U || old_count == UINT_MAX)
    {
        (void)atomic_fetch_sub_explicit(&sm->ref_count, 1U,
                                        memory_order_relaxed);
        return NULL;
    }
    return sm;
}

void session_manager_free(SessionManager *sm)
{
    if (!sm) return;
    if (atomic_fetch_sub_explicit(&sm->ref_count, 1U,
                                  memory_order_acq_rel) != 1U)
        return;
    if (sm->db) sqlite3_close(sm->db);
    pthread_mutex_destroy(&sm->lock);
    free(sm->data_dir);
    memset(sm, 0, sizeof(SessionManager));
    free(sm);
}

Session *session_manager_create_session(SessionManager *sm, const char *title)
{
    if (!sm || !sm->key_initialized) return NULL;

    Session *s = session_create(title);
    if (!s) return NULL;

    if (session_manager_save_session(sm, s) != 0)
    {
        session_free(s);
        return NULL;
    }

    return s;
}

Session *session_manager_load_session_alloc(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->key_initialized || !sm->db) return NULL;
    if (!id[0]) return NULL;

    session_manager_lock(sm);
    Session *s = load_session_locked(sm, id);
    session_manager_unlock(sm);
    return s;
}

void session_manager_lock(SessionManager *sm)
{
    if (!sm) return;
    if (pthread_mutex_lock(&sm->lock) != 0)
        log_error("session_manager: mutex lock failed", NULL);
}

void session_manager_unlock(SessionManager *sm)
{
    if (!sm) return;
    if (pthread_mutex_unlock(&sm->lock) != 0)
        log_error("session_manager: mutex unlock failed", NULL);
}

void sm_lock(SessionManager *sm)
{
    if (pthread_mutex_lock(&sm->lock) != 0)
        log_error("session_manager: internal mutex lock failed", NULL);
}

void sm_unlock(SessionManager *sm)
{
    if (pthread_mutex_unlock(&sm->lock) != 0)
        log_error("session_manager: internal mutex unlock failed", NULL);
}

Session *session_manager_load_session_nolock_alloc(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->key_initialized || !sm->db) return NULL;
    if (!id[0]) return NULL;
    return load_session_locked(sm, id);
}

int session_manager_save_session(SessionManager *sm, Session *session)
{
    if (!sm || !session || !sm->key_initialized || !sm->db) return -1;
    if (!session->id || !session->id[0]) return -1;
    session_manager_lock(sm);
    int rc = save_session_core_locked(sm, session, SM_UPSERT);
    session_manager_unlock(sm);
    return rc;
}

int session_manager_save_session_nolock(SessionManager *sm, Session *session)
{
    if (!sm || !session || !sm->key_initialized || !sm->db) return -1;
    if (!session->id || !session->id[0]) return -1;
    return save_session_core_locked(sm, session, SM_UPSERT);
}

int session_manager_add_message(SessionManager *sm, const char *session_id,
                                 const char *role, const char *content,
                                 const char *tool_call_id, const char *tool_name)
{
    /* C10: hold sm->lock across load→mutate→save so no other thread
     * can interleave a save on the same row between our load and save,
     * eliminating the last-writer-wins race. */
    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) {
        session_manager_unlock(sm);
        return -1;
    }

    int idx = s->messages_count;
    if (idx < 0 || (size_t)idx >= SIZE_MAX / sizeof(Message) - 1U)
    {
        session_free(s);
        session_manager_unlock(sm);
        return -1;
    }
    int new_count = idx + 1;
    Message *new_msgs = realloc(s->messages, sizeof(Message) * (size_t)new_count);
    if (!new_msgs) {
        session_free(s);
        session_manager_unlock(sm);
        return -1;
    }

    s->messages = new_msgs;
    s->messages_count = new_count;
    memset(&s->messages[idx], 0, sizeof(Message));
    s->messages[idx].role = str_dup(role ? role : "");
    s->messages[idx].content = str_dup(content ? content : "");
    if (tool_call_id) s->messages[idx].tool_call_id = str_dup(tool_call_id);
    if (tool_name) s->messages[idx].tool_name = str_dup(tool_name);
    if (!s->messages[idx].role || !s->messages[idx].content ||
        (tool_call_id && !s->messages[idx].tool_call_id) ||
        (tool_name && !s->messages[idx].tool_name))
    {
        message_clear(&s->messages[idx]);
        s->messages_count = idx;
        session_free(s);
        session_manager_unlock(sm);
        return -1;
    }

    int rc = save_session_core_locked(sm, s, SM_UPSERT);
    session_manager_unlock(sm);
    session_free(s);
    return rc;
}

int session_manager_truncate_history(SessionManager *sm, const char *session_id, int index)
{
    /* C10: hold sm->lock across the load→mutate→save triad. */
    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) {
        session_manager_unlock(sm);
        return -1;
    }

    if (index < 0 || index >= s->messages_count)
    {
        session_free(s);
        session_manager_unlock(sm);
        return -1;
    }

    for (int i = index; i < s->messages_count; i++)
        message_clear(&s->messages[i]);
    s->messages_count = index;

    int rc = save_session_core_locked(sm, s, SM_UPSERT);
    session_manager_unlock(sm);
    session_free(s);
    return rc;
}

int session_manager_log_event(SessionManager *sm, const char *session_id,
                               const char *event_type, const char *data)
{
    /* C10: hold sm->lock across load→mutate→save. Also C11: an
     * agent_save_session that holds the lock across its own triad blocks
     * here, so its metadata/events written back ARE the freshest. */
    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) {
        session_manager_unlock(sm);
        return -1;
    }

    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "event_type", event_type ? event_type : "");
    cJSON_AddStringToObject(ev, "data", data ? data : "");

    time_t now = time(NULL);
    char ts[64];
    struct tm tm_storage;
    struct tm *tm_ptr = localtime_r(&now, &tm_storage);
    if (tm_ptr) strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_ptr);
    else snprintf(ts, sizeof(ts), "%ld", (long)now);
    cJSON_AddStringToObject(ev, "timestamp", ts);

    cJSON_AddItemToArray(s->events, ev);

    int rc = save_session_core_locked(sm, s, SM_UPSERT);
    session_manager_unlock(sm);
    session_free(s);
    return rc;
}
