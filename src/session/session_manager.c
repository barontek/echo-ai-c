/*
 * session_manager.c - refcounted, mutex-protected session store: sqlite
 * persistence of encrypted session blobs, oauth credential storage,
 * message append/truncate, branch fork/switch, import/export, purge, and
 * event logging.
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

#include <sqlite3.h>

#include "session_manager.h"
#include "memory.h"
#include "migration.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#ifdef SESSION_MANAGER_TEST
static int sm_alloc_counter = 0;
static int sm_alloc_fail_at = -1;
static int sm_realloc_counter = 0;
static int sm_realloc_fail_at = -1;

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

static void *sm_test_realloc(void *ptr, size_t size)
{
    sm_realloc_counter++;
    if (sm_realloc_counter == sm_realloc_fail_at) return NULL;
    return realloc(ptr, size);
}

static char *sm_test_strdup(const char *s)
{
    sm_alloc_counter++;
    if (sm_alloc_counter == sm_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup sm_test_strdup
#define realloc sm_test_realloc

/* Fault-injection knob for B1: lets a test force the Nth
 * sqlite3_bind_* call (across bind_text/bind_int/bind_blob/bind_null in any
 * session_manager function) to return SQLITE_NOMEM so we can prove the
 * save path actually checks binds instead of trusting them. Production
 * builds never see this; only translation units compiled with
 * -DSESSION_MANAGER_TEST=1 do. */
static int sm_bind_counter = 0;
static int sm_bind_fail_at = -1;

void session_manager_test_set_bind_fail(int nth_bind)
{
    sm_bind_counter = 0;
    sm_bind_fail_at = nth_bind;
}

static int sm_test_bind_text(sqlite3_stmt *s, int idx, const char *t, int n,
                             void (*del)(void *))
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_text(s, idx, t, n, del);
}
static int sm_test_bind_int(sqlite3_stmt *s, int idx, int v)
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_int(s, idx, v);
}
static int sm_test_bind_blob(sqlite3_stmt *s, int idx, const void *p, int n,
                             void (*del)(void *))
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_blob(s, idx, p, n, del);
}
static int sm_test_bind_null(sqlite3_stmt *s, int idx)
{
    sm_bind_counter++;
    if (sm_bind_counter == sm_bind_fail_at) return SQLITE_NOMEM;
    return sqlite3_bind_null(s, idx);
}

/* Fault-injection knob for C13: lets a test force the Nth
 * encryption_encrypt call (across all session_manager functions) to return
 * NULL, so we can prove the save path actually checks encrypt failures
 * instead of letting `bind_null` silently overwrite the existing blob. */
static int sm_enc_counter = 0;
static int sm_enc_fail_at = -1;
static int sm_oauth_alloc_counter = 0;
static int sm_oauth_alloc_fail_at = -1;

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

static unsigned char *sm_test_encrypt(const EncryptionKey *key,
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
/* Production builds route oauth result allocation through plain malloc so
 * the TEST-only name stays callable from both branches. */
#define oauth_malloc       malloc
#endif

#define SALT_FILE "salt"
#define DB_FILE "echo-ai.db"

static int mkdir_p(const char *path)
{
    char tmp[1024];
    int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len <= 0 || len >= (int)sizeof(tmp)) return -1;

    for (int i = 0; i < len; i++)
    {
        if (tmp[i] == '/')
        {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(path, 0755);
}

static int init_db(sqlite3 *db)
{
    const char *sql = "CREATE TABLE IF NOT EXISTS agent_sessions ("
                      "id TEXT PRIMARY KEY,"
                      "title_encrypted BLOB,"
                      "title_generation_attempted INTEGER DEFAULT 0,"
                      "created_at TEXT,"
                      "messages_encrypted BLOB,"
                      "metadata_encrypted BLOB,"
                      "events_encrypted BLOB"
                      ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("sqlite create agent_sessions table", "err", err, NULL);
        sqlite3_free(err);
        return -1;
    }

    const char *oauth_sql = "CREATE TABLE IF NOT EXISTS provider_oauth ("
                            "provider TEXT PRIMARY KEY,"
                            "data_encrypted BLOB NOT NULL"
                            ");";
    if (sqlite3_exec(db, oauth_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("sqlite create provider_oauth table", "err", err, NULL);
        sqlite3_free(err);
        return -1;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, NULL);

    return 0;
}

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

char *session_manager_load_provider_oauth(SessionManager *sm,
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
    if (!sm->data_dir) { free(sm); return NULL; }

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

/* C10: the shared load-with-decrypt body. Caller MUST hold sm->lock. */
static Session *load_session_locked(SessionManager *sm, const char *id)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, title_encrypted, title_generation_attempted, created_at, "
                      "messages_encrypted, metadata_encrypted, events_encrypted "
                      "FROM agent_sessions WHERE id = ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare", "err", sqlite3_errmsg(sm->db), NULL);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        if (rc != SQLITE_DONE)
            log_error("sqlite step load", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        return NULL;
    }

    Session *s = calloc(1, sizeof(Session));
    if (!s) { log_error("calloc Session in load", NULL); sqlite3_finalize(stmt); return NULL; }

    const char *db_id = (const char *)sqlite3_column_text(stmt, 0);
    const void *title_blob = sqlite3_column_blob(stmt, 1);
    int title_len = sqlite3_column_bytes(stmt, 1);
    int db_tga = sqlite3_column_int(stmt, 2);
    const char *db_created = (const char *)sqlite3_column_text(stmt, 3);
    const void *msgs_blob = sqlite3_column_blob(stmt, 4);
    int msgs_len = sqlite3_column_bytes(stmt, 4);
    const void *meta_blob = sqlite3_column_blob(stmt, 5);
    int meta_len = sqlite3_column_bytes(stmt, 5);
    const void *events_blob = sqlite3_column_blob(stmt, 6);
    int events_len = sqlite3_column_bytes(stmt, 6);

    s->id = str_dup(db_id ? db_id : "");
    s->title = str_dup("");
    s->title_generation_attempted = db_tga;
    s->created_at = str_dup(db_created ? db_created : "");
    s->events = cJSON_CreateArray();
    s->metadata = cJSON_CreateObject();

    int partial_fail = 0;

    if (title_blob && title_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, title_blob, title_len, &dec_len);
        if (dec)
        {
            free(s->title);
            s->title = str_dup((const char *)dec);
            free(dec);
        }
        else
        {
            log_error("load_session: title could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    if (msgs_blob && msgs_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, msgs_blob, msgs_len, &dec_len);
        if (dec)
        {
            int rc_d = session_deserialize_messages(s, (const char *)dec);
            if (rc_d != 0)
            {
                log_error("load_session: messages decrypted but failed to parse",
                          "id", id, NULL);
                partial_fail = 1;
            }
            free(dec);
        }
        else
        {
            log_error("load_session: messages could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    if (meta_blob && meta_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, meta_blob, meta_len, &dec_len);
        if (dec)
        {
            int rc_d = session_deserialize_metadata(s, (const char *)dec);
            if (rc_d != 0)
            {
                log_error("load_session: metadata decrypted but failed to parse",
                          "id", id, NULL);
                partial_fail = 1;
            }
            free(dec);
        }
        else
        {
            log_error("load_session: metadata could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    if (events_blob && events_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, events_blob, events_len, &dec_len);
        if (dec)
        {
            int rc_d = session_deserialize_events(s, (const char *)dec);
            if (rc_d != 0)
            {
                log_error("load_session: events decrypted but failed to parse",
                          "id", id, NULL);
                partial_fail = 1;
            }
            free(dec);
        }
        else
        {
            log_error("load_session: events could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    sqlite3_finalize(stmt);

    if (partial_fail)
    {
        session_free(s);
        return NULL;
    }

    return s;
}

Session *session_manager_load_session(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->key_initialized || !sm->db) return NULL;
    if (!id[0]) return NULL;

    session_manager_lock(sm);
    Session *s = load_session_locked(sm, id);
    session_manager_unlock(sm);
    return s;
}

/* C10: lock/unlock wrappers so callers in other TUs can hold sm->lock
 * across a load-modify-save triad. PTHREAD_MUTEX_NORMAL — re-entry deadlocks
 * immediately, by design. */
void session_manager_lock(SessionManager *sm)
{
    if (sm) pthread_mutex_lock(&sm->lock);
}

void session_manager_unlock(SessionManager *sm)
{
    if (sm) pthread_mutex_unlock(&sm->lock);
}

/* _nolock variants forward to the locked-statics. Caller MUST already
 * hold sm->lock; do not call any non-_nolock session_manager_* API from
 * within — the non-recursive PTHREAD_MUTEX_NORMAL will deadlock on
 * re-entry. */
Session *session_manager_load_session_nolock(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->key_initialized || !sm->db) return NULL;
    if (!id[0]) return NULL;
    return load_session_locked(sm, id);
}

/* Mode for save_session_core. SM_UPSERT is the historical "save-or-overwrite"
 * behavior used by every caller that has already loaded-or-created the row.
 * SM_INSERT_IF_ABSENT is used by import_session, where the "reject duplicates"
 * promise must be enforced atomically. With SM_INSERT_IF_ABSENT, the SQL is
 * `INSERT INTO ... ON CONFLICT(id) DO NOTHING` so the PRIMARY KEY uniqueness
 * check is the test-and-insert; a 0-changes result means the id already
 * existed and the caller treats it as a duplicate. This eliminates the
 * pre-import `load_session` precheck that was a TOCTOU race (it acquired and
 * released the mutex for the check, then again for the save, with another
 * writer free to insert the same id in between). */
enum save_core_mode { SM_UPSERT, SM_INSERT_IF_ABSENT };

/* C10: the shared serialize+encrypt+SQL body; caller MUST hold sm->lock. */
static int save_session_core_locked(SessionManager *sm, Session *session,
                                     enum save_core_mode mode)
{
    char *messages_json = session_serialize_messages(session);
    char *metadata_json = session_serialize_metadata(session);
    char *events_json = session_serialize_events(session);

    int abort_save = 0;
    if (!messages_json)
    {
        log_error("save_session: serialize messages failed (OOM)", NULL);
        abort_save = 1;
    }
    if (session->metadata && !metadata_json)
    {
        log_error("save_session: serialize metadata failed (OOM)", NULL);
        abort_save = 1;
    }
    if (session->events && !events_json)
    {
        log_error("save_session: serialize events failed (OOM)", NULL);
        abort_save = 1;
    }
    if (!abort_save && messages_json && strlen(messages_json) > (size_t)INT_MAX)
    {
        log_error("save_session: messages JSON exceeds INT_MAX bytes", NULL);
        abort_save = 1;
    }
    if (!abort_save && metadata_json && strlen(metadata_json) > (size_t)INT_MAX)
    {
        log_error("save_session: metadata JSON exceeds INT_MAX bytes", NULL);
        abort_save = 1;
    }
    if (!abort_save && events_json && strlen(events_json) > (size_t)INT_MAX)
    {
        log_error("save_session: events JSON exceeds INT_MAX bytes", NULL);
        abort_save = 1;
    }

    unsigned char *msgs_enc = NULL;
    int msgs_enc_len = 0;
    unsigned char *meta_enc = NULL;
    int meta_enc_len = 0;
    unsigned char *events_enc = NULL;
    int events_enc_len = 0;
    unsigned char *title_enc = NULL;
    int title_enc_len = 0;

    if (!abort_save && session->title && session->title[0])
    {
        title_enc = encryption_encrypt(&sm->enc_key,
                                       (const unsigned char *)session->title,
                                       (int)strlen(session->title), &title_enc_len);
        if (!title_enc)
        {
            log_error("save_session: encryption_encrypt failed for title", NULL);
            abort_save = 1;
        }
    }

    if (!abort_save && messages_json)
    {
        msgs_enc = encryption_encrypt(&sm->enc_key,
                                       (const unsigned char *)messages_json,
                                       strlen(messages_json), &msgs_enc_len);
        if (!msgs_enc)
        {
            log_error("save_session: encryption_encrypt failed for messages", NULL);
            abort_save = 1;
        }
    }

    if (!abort_save && metadata_json)
    {
        meta_enc = encryption_encrypt(&sm->enc_key,
                                       (const unsigned char *)metadata_json,
                                       strlen(metadata_json), &meta_enc_len);
        if (!meta_enc)
        {
            log_error("save_session: encryption_encrypt failed for metadata", NULL);
            abort_save = 1;
        }
    }

    if (!abort_save && events_json)
    {
        events_enc = encryption_encrypt(&sm->enc_key,
                                         (const unsigned char *)events_json,
                                         strlen(events_json), &events_enc_len);
        if (!events_enc)
        {
            log_error("save_session: encryption_encrypt failed for events", NULL);
            abort_save = 1;
        }
    }

    if (abort_save)
    {
        free(messages_json); free(metadata_json); free(events_json);
        free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);
        return -1;
    }

    const char *sql_upsert =
        "INSERT INTO agent_sessions "
        "(id, title_encrypted, title_generation_attempted, created_at, "
        "messages_encrypted, metadata_encrypted, events_encrypted) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "title_encrypted=excluded.title_encrypted, "
        "title_generation_attempted=excluded.title_generation_attempted, "
        "created_at=excluded.created_at, "
        "messages_encrypted=excluded.messages_encrypted, "
        "metadata_encrypted=excluded.metadata_encrypted, "
        "events_encrypted=excluded.events_encrypted";
    const char *sql_insert_if_absent =
        "INSERT INTO agent_sessions "
        "(id, title_encrypted, title_generation_attempted, created_at, "
        "messages_encrypted, metadata_encrypted, events_encrypted) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO NOTHING";

    const char *sql = (mode == SM_INSERT_IF_ABSENT) ? sql_insert_if_absent
                                                    : sql_upsert;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare save", "err", sqlite3_errmsg(sm->db), NULL);
        free(messages_json); free(metadata_json); free(events_json);
        free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);
        return -1;
    }

    int bind_rc = sqlite3_bind_text(stmt, 1, session->id, -1, SQLITE_TRANSIENT);
    if (bind_rc == SQLITE_OK)
    {
        if (title_enc && title_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 2, title_enc, title_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 2);
    }
    if (bind_rc == SQLITE_OK)
        bind_rc = sqlite3_bind_int(stmt, 3, session->title_generation_attempted);
    if (bind_rc == SQLITE_OK)
        bind_rc = sqlite3_bind_text(stmt, 4, session->created_at, -1, SQLITE_TRANSIENT);
    if (bind_rc == SQLITE_OK)
    {
        if (msgs_enc && msgs_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 5, msgs_enc, msgs_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 5);
    }
    if (bind_rc == SQLITE_OK)
    {
        if (meta_enc && meta_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 6, meta_enc, meta_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 6);
    }
    if (bind_rc == SQLITE_OK)
    {
        if (events_enc && events_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 7, events_enc, events_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 7);
    }
    if (bind_rc != SQLITE_OK)
    {
        log_error("sqlite bind save", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        free(messages_json); free(metadata_json); free(events_json);
        free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);
        return -1;
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        log_error("sqlite step save", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        free(messages_json); free(metadata_json); free(events_json);
        free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);
        return -1;
    }

    int changes = sqlite3_changes(sm->db);
    sqlite3_finalize(stmt);

    free(messages_json); free(metadata_json); free(events_json);
    free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);

    if (mode == SM_INSERT_IF_ABSENT)
        return changes > 0 ? 1 : 0;
    return 0;
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

/* Delete the session identified by `id`.
 * Returns:  1 if a row was deleted
 *           0 if no row matched (caller should treat as 404)
 *          -1 on SQLite error (prepare/bind/step failure)
 *
 * Note: does NOT require sm->key_initialized, because delete does not need
 * to decrypt. This is the deliberate asymmetry vs load/save/add_message, all
 * of which need the key. Caller passes a non-empty NUL-terminated id; sm and
 * sm->db must be non-NULL. */
int session_manager_delete_session(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->db) return -1;

    pthread_mutex_lock(&sm->lock);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM agent_sessions WHERE id = ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare delete", "err", sqlite3_errmsg(sm->db), NULL);
        pthread_mutex_unlock(&sm->lock);
        return -1;
    }

    int bind_rc = sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    if (bind_rc != SQLITE_OK)
    {
        log_error("sqlite bind delete", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&sm->lock);
        return -1;
    }

    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(sm->db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);

    if (rc != SQLITE_DONE)
    {
        log_error("sqlite step delete", "err", sqlite3_errmsg(sm->db), NULL);
        return -1;
    }
    return (changed > 0) ? 1 : 0;
}

SessionList *session_manager_list_sessions(SessionManager *sm)
{
    if (!sm || !sm->data_dir || !sm->db || !sm->key_initialized) return NULL;

    pthread_mutex_lock(&sm->lock);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, title_encrypted, title_generation_attempted, created_at "
                      "FROM agent_sessions ORDER BY created_at DESC";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare list", "err", sqlite3_errmsg(sm->db), NULL);
        pthread_mutex_unlock(&sm->lock);
        return NULL;
    }

    SessionList *list = calloc(1, sizeof(SessionList));
    if (!list) { sqlite3_finalize(stmt); pthread_mutex_unlock(&sm->lock); return NULL; }

    int capacity = 16;
    list->ids = malloc(sizeof(char *) * capacity);
    list->titles = malloc(sizeof(char *) * capacity);
    list->created_ats = malloc(sizeof(char *) * capacity);
    list->title_generation_attempteds = malloc(sizeof(int) * capacity);
    if (!list->ids || !list->titles || !list->created_ats || !list->title_generation_attempteds)
    {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&sm->lock);
        session_list_free(list);
        return NULL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (list->count >= capacity)
        {
            capacity *= 2;
            char **new_ids = realloc(list->ids, sizeof(char *) * capacity);
            if (!new_ids)
            {
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&sm->lock);
                session_list_free(list);
                return NULL;
            }
            list->ids = new_ids;

            char **new_titles = realloc(list->titles, sizeof(char *) * capacity);
            if (!new_titles)
            {
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&sm->lock);
                session_list_free(list);
                return NULL;
            }
            list->titles = new_titles;

            char **new_dates = realloc(list->created_ats, sizeof(char *) * capacity);
            if (!new_dates)
            {
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&sm->lock);
                session_list_free(list);
                return NULL;
            }
            list->created_ats = new_dates;

            int *new_tgas = realloc(list->title_generation_attempteds,
                                    sizeof(int) * capacity);
            if (!new_tgas)
            {
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&sm->lock);
                session_list_free(list);
                return NULL;
            }
            list->title_generation_attempteds = new_tgas;
        }

        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const void *title_blob = sqlite3_column_blob(stmt, 1);
        int title_len = sqlite3_column_bytes(stmt, 1);
        int tga = sqlite3_column_int(stmt, 2);
        const char *created = (const char *)sqlite3_column_text(stmt, 3);

        char *title_dup = NULL;
        unsigned char *dec = NULL;
        int dec_len = 0;
        if (title_blob && title_len > 0)
            dec = encryption_decrypt(&sm->enc_key, title_blob, title_len, &dec_len);
        if (dec)
        {
            title_dup = str_dup((const char *)dec);
            free(dec);
        }
        else
        {
            title_dup = str_dup("");
        }

        char *id_dup = str_dup(id ? id : "");
        char *created_dup = str_dup(created ? created : "");
        if (!id_dup || !title_dup || !created_dup)
        {
            /* B6: str_dup failure must not silently truncate; free locals and
             * return NULL via the cleanup path so the caller knows it failed. */
            free(id_dup);
            free(title_dup);
            free(created_dup);
            sqlite3_finalize(stmt);
            pthread_mutex_unlock(&sm->lock);
            session_list_free(list);
            return NULL;
        }
        list->ids[list->count] = id_dup;
        list->titles[list->count] = title_dup;
        list->created_ats[list->count] = created_dup;
        list->title_generation_attempteds[list->count] = tga;
        list->count++;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);
    return list;
}

void session_list_free(SessionList *list)
{
    if (!list) return;
    for (int i = 0; i < list->count; i++)
    {
        free(list->ids[i]);
        free(list->titles[i]);
        free(list->created_ats[i]);
    }
    free(list->ids);
    free(list->titles);
    free(list->created_ats);
    free(list->title_generation_attempteds);
    free(list);
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
    if (!s) { session_manager_unlock(sm); return -1; }

    int idx = s->messages_count;
    if (idx < 0 || (size_t)idx >= SIZE_MAX / sizeof(Message) - 1U)
    {
        session_free(s);
        session_manager_unlock(sm);
        return -1;
    }
    int new_count = idx + 1;
    Message *new_msgs = realloc(s->messages, sizeof(Message) * (size_t)new_count);
    if (!new_msgs) { session_free(s); session_manager_unlock(sm); return -1; }

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
    if (!s) { session_manager_unlock(sm); return -1; }

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

/* ---------------------------------------------------------------------------
 * Branch support (fork / switch / branch_info)
 * ---------------------------------------------------------------------------
 *
 * Data model (see BRANCHING_IMPLEMENTATION_PLAN.md §2): session->messages
 * holds the active chain; every other chain lives as a snapshot record in
 * session->metadata.branches.list:
 *
 *   "branches": {
 *     "active_created_at": "<chain creation ts of the live chain>",
 *     "list": [ { "id": "br_<ts>-<rand>", "created_at": "...",
 *                 "anchor_message_id": "<id of last fork message>",
 *                 "messages": [ ... ] } ]
 *   }
 *
 * At fork time ONE fork_group_id is minted and shared by both the pre-fork
 * message (which keeps its id/content and gets the group only when it had
 * none) and the fresh fork message (new id, new group, new content), so the
 * branch pill renders on both chains and "chains containing a message with
 * that fork_group_id" counts correctly. Re-forking at a message that
 * already carries a fork_group_id keeps the old group on the pre-fork
 * message (per plan: "the pre-fork message keeps its old fork_group_id")
 * and mints a fresh group for the new message — a new fork node.
 *
 * Records are popped on switch-to (only non-live chains are stored), so a
 * chain never has a record while it is live; fork/switch-away therefore
 * usually append a new record. The anchor match is kept as a defensive
 * replace path for invariants that change in future.
 */

static char *branch_mint_id(const char *prefix)
{
    time_t now = time(NULL);
    char *id = NULL;
    if (asprintf(&id, "%s%ld-%d", prefix, (long)now, rand() % 1000000) < 0)
        return NULL;
    return id;
}

static char *branch_now_iso(void)
{
    char ts[64];
    struct timespec now;
    struct tm tm_storage;
    struct tm *tm_ptr;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return NULL;
    tm_ptr = localtime_r(&now.tv_sec, &tm_storage);
    if (!tm_ptr) return NULL;
    /* Millisecond resolution: with second-granularity timestamps a fork's
     * snapshot record and the new live chain can share a created_at, and
     * branch_info's strcmp ordering then reports the wrong active
     * position. Same-ms collisions are still possible but need real
     * timing luck rather than being the normal outcome of a fast edit. */
    if (strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_ptr) == 0)
        return NULL;

    char *iso = NULL;
    if (asprintf(&iso, "%s.%03ld", ts, now.tv_nsec / 1000000L) < 0)
        return NULL;
    return iso;
}

/* Returns the cJSON "list" array of branch records, creating it (plus the
 * enclosing "branches" object) when absent, or NULL on OOM. */
static cJSON *branch_list_ensure(Session *s)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    if (!branches)
    {
        branches = cJSON_CreateObject();
        if (!branches) return NULL;
        if (!cJSON_AddItemToObject(s->metadata, "branches", branches))
        {
            cJSON_Delete(branches);
            return NULL;
        }
    }
    cJSON *list = cJSON_GetObjectItem(branches, "list");
    if (!list)
    {
        list = cJSON_CreateArray();
        if (!list) return NULL;
        if (!cJSON_AddItemToObject(branches, "list", list))
        {
            cJSON_Delete(list);
            return NULL;
        }
    }
    return list;
}

/* The live chain's creation timestamp: metadata.branches.active_created_at,
 * falling back to the session creation time for never-forked sessions.
 * Returns NULL only on OOM. */
static char *branch_active_created_at(Session *s)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    cJSON *active = branches ? cJSON_GetObjectItem(branches, "active_created_at") : NULL;
    if (active && active->valuestring)
        return str_dup(active->valuestring);
    return str_dup(s->created_at ? s->created_at : "");
}

/* Sets branches.active_created_at to `value`, replacing an existing value.
 * A chain is re-born on every fork/switch, so the key must be updated in
 * place — cJSON_AddStringToObject on an existing key APPENDS a duplicate,
 * and GetObjectItem then reads the stale first one (branch_info ordering
 * breaks). */
static int branch_set_active_created_at(Session *s, const char *value)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    if (!branches) return -1;
    cJSON *act = cJSON_GetObjectItem(branches, "active_created_at");
    if (act)
    {
        cJSON *stale = cJSON_DetachItemFromObject(branches,
                                                  "active_created_at");
        if (stale) cJSON_Delete(stale);
    }
    return cJSON_AddStringToObject(branches, "active_created_at", value) ? 0 : -1;
}

/* Deep copy of the live chain as a cJSON array for a snapshot record. */
static cJSON *branch_snapshot_messages(Message *msgs, int count)
{
    return messages_to_json_array(msgs, count);
}

/* Creates a snapshot record object for the live chain. old_chain_created_at
 * is the chain's true creation time (NOT the snapshot time — ordering of
 * branch_info depends on it). anchor_message_id may be NULL. */
static cJSON *branch_record_create(const char *created_at,
                                   const char *anchor_message_id,
                                   cJSON *snapshot)
{
    cJSON *rec = cJSON_CreateObject();
    if (!rec) return NULL;
    char *id = branch_mint_id("br_");
    if (!id) { cJSON_Delete(rec); return NULL; }
    if (!cJSON_AddStringToObject(rec, "id", id)) { free(id); cJSON_Delete(rec); return NULL; }
    free(id);
    if (!cJSON_AddStringToObject(rec, "created_at", created_at ? created_at : ""))
    { cJSON_Delete(rec); return NULL; }
    if (anchor_message_id)
    {
        if (!cJSON_AddStringToObject(rec, "anchor_message_id", anchor_message_id))
        { cJSON_Delete(rec); return NULL; }
    }
    if (!cJSON_AddItemToObject(rec, "messages", snapshot))
    { cJSON_Delete(rec); return NULL; }
    return rec;
}

/* Appends a snapshot record for the live chain to branches.list. The
 * record's created_at is the chain's own creation time (NOT the snapshot
 * time — branch_info orders chains by creation), so a chain that left
 * live keeps its original position in the ordering. A chain is recorded
 * fresh on every leave-live transition; switch_branch pops the live
 * chain's own record first, so appending can never duplicate a chain
 * that is still in the list. IMPORTANT: records are NEVER replaced by
 * anchor match — sibling chains of the same fork share the fork point's
 * id, and replacing by anchor destroyed sibling data (switch-back bug). */
static int branch_record_snapshot_live(Session *s, cJSON *list)
{
    char *created_at = branch_active_created_at(s);
    if (!created_at) return -1;

    cJSON *snapshot = branch_snapshot_messages(s->messages, s->messages_count);
    if (!snapshot) { free(created_at); return -1; }

    cJSON *rec = branch_record_create(created_at, NULL, snapshot);
    free(created_at);
    if (!rec) { cJSON_Delete(snapshot); return -1; }
    if (!cJSON_AddItemToArray(list, rec))
    {
        cJSON_Delete(rec);
        return -1;
    }
    return 0;
}

/* Finds the record with `branch_id` in `list`, or NULL. */
static cJSON *branch_record_find(cJSON *list, const char *branch_id)
{
    if (!list || !branch_id) return NULL;
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i++)
    {
        cJSON *rec = cJSON_GetArrayItem(list, i);
        cJSON *rid = rec ? cJSON_GetObjectItem(rec, "id") : NULL;
        if (rid && rid->valuestring && strcmp(rid->valuestring, branch_id) == 0)
            return rec;
    }
    return NULL;
}

/* Frees every message in [index, count) and shrinks the array. */
static void session_truncate_messages(Session *s, int index)
{
    for (int i = index; i < s->messages_count; i++)
        message_clear(&s->messages[i]);
    s->messages_count = index;
}

int session_manager_fork_branch(SessionManager *sm, const char *session_id,
                                const char *message_id, int index,
                                const char *new_content,
                                SessionManagerForkResult *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!sm || !session_id || !sm->key_initialized || !sm->db || !out)
        return -1;

    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) { session_manager_unlock(sm); return -1; }

    int fi = -1;
    if (message_id)
    {
        for (int i = 0; i < s->messages_count; i++)
            if (s->messages[i].id && strcmp(s->messages[i].id, message_id) == 0)
            { fi = i; break; }
    }
    if (fi < 0)
        fi = index;
    if (fi < 0 || fi >= s->messages_count)
    {
        session_free(s);
        session_manager_unlock(sm);
        return -1;
    }

    int rc = -1;
    cJSON *list = NULL;
    char *created_at = NULL;
    char *fork_message_id = NULL;
    char *fork_group_id = NULL;
    Message fork_copy;

    list = branch_list_ensure(s);
    if (!list) goto cleanup;

    /* Mint an id for the fork message. The group is shared with the
     * pre-fork message — and when the fork point already carries a group
     * (re-forking a previous fork point, e.g. re-editing the same user
     * message), the new chain JOINS that group instead of minting a new
     * one: a fresh group would orphan the pre-fork chain from the pill
     * (its chains carry the old group) and the count would not span the
     * whole family. */
    fork_message_id = branch_mint_id("m_");
    if (!fork_message_id) goto cleanup;
    if (s->messages[fi].fork_group_id)
        fork_group_id = str_dup(s->messages[fi].fork_group_id);
    else
        fork_group_id = branch_mint_id("fg_");
    if (!fork_group_id) goto cleanup;

    /* Copy the pre-fork message; the copy gets the fresh identity and
     * content, the original keeps its content in the snapshot. */
    if (message_copy(&fork_copy, &s->messages[fi]) != 0) goto cleanup;
    free(fork_copy.id);
    fork_copy.id = str_dup(fork_message_id);
    free(fork_copy.fork_group_id);
    fork_copy.fork_group_id = str_dup(fork_group_id);
    if (new_content)
    {
        char *content_dup = str_dup(new_content);
        if (!content_dup) goto cleanup;
        free(fork_copy.content);
        fork_copy.content = content_dup;
    }
    if (!fork_copy.id || !fork_copy.fork_group_id) goto cleanup;

    /* Pre-fork message keeps its identity; give it the minted group (and an
     * id when legacy) so the pill renders on the old chain too. This MUST
     * happen before the snapshot below — branch_info counts chains by the
     * fork_group_id present in each chain, so the old chain's snapshot has
     * to carry the shared group or it would not count (pill would show
     * count=1 and switch-back would lose the pill entirely). */
    if (!s->messages[fi].fork_group_id)
    {
        char *group_dup = str_dup(fork_group_id);
        if (!group_dup) goto cleanup;
        s->messages[fi].fork_group_id = group_dup;
    }
    if (!s->messages[fi].id)
    {
        char *id_dup = str_dup(fork_message_id);
        if (!id_dup) goto cleanup;
        s->messages[fi].id = id_dup;
    }

    /* Snapshot the full pre-fork chain — nothing may be truncated or
     * committed on disk until this exists (all-or-nothing per plan §3.2;
     * on any failure below the session is untouched on disk). */
    cJSON *snapshot = branch_snapshot_messages(s->messages, s->messages_count);
    if (!snapshot) goto cleanup;
    created_at = branch_active_created_at(s);
    if (!created_at) { cJSON_Delete(snapshot); goto cleanup; }
    cJSON *rec = branch_record_create(created_at, NULL, snapshot);
    if (!rec) { cJSON_Delete(snapshot); goto cleanup; }
    if (!cJSON_AddItemToArray(list, rec))
    {
        cJSON_Delete(rec);
        goto cleanup;
    }

    /* Truncate the live array at the fork point and swap in the fork copy.
     * The pre-fork message is only needed for the snapshot above — clear it
     * or its fields (including the group/id assigned pre-snapshot) leak. */
    session_truncate_messages(s, fi + 1);
    message_clear(&s->messages[fi]);
    s->messages[fi] = fork_copy;
    memset(&fork_copy, 0, sizeof(fork_copy));

    /* The new live chain is born now. */
    char *now = branch_now_iso();
    if (!now) goto cleanup;
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    if (!branches) { free(now); goto cleanup; }
    if (branch_set_active_created_at(s, now) != 0)
    { free(now); goto cleanup; }
    free(now);

    if (save_session_core_locked(sm, s, SM_UPSERT) != 0) goto cleanup;

    out->branch_id = str_dup(cJSON_GetObjectItem(rec, "id")->valuestring);
    out->fork_message_id = str_dup(fork_message_id);
    out->fork_group_id = str_dup(fork_group_id);
    if (!out->branch_id || !out->fork_message_id || !out->fork_group_id)
    {
        /* Nothing was persisted atomically — roll the caller's view back
         * to "no fork happened". */
        memset(out, 0, sizeof(*out));
        goto cleanup;
    }
    /* fork_copy was moved into the live array and zeroed; hand the caller
     * its own deep copy of the new tail. Failure here mirrors the str_dup
     * rollback above: the fork IS committed on disk but the caller sees
     * -1 with *out zeroed (reload renders the fork). */
    if (message_copy(&out->fork_message, &s->messages[fi]) != 0)
    {
        memset(out, 0, sizeof(*out));
        goto cleanup;
    }
    rc = 0;

cleanup:
    message_clear(&fork_copy);
    free(created_at);
    free(fork_message_id);
    free(fork_group_id);
    session_free(s);
    session_manager_unlock(sm);
    return rc;
}

int session_manager_switch_branch(SessionManager *sm, const char *session_id,
                                  const char *branch_id)
{
    if (!sm || !session_id || !branch_id || !sm->key_initialized || !sm->db)
        return -1;

    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) { session_manager_unlock(sm); return -1; }

    int rc = -1;
    cJSON *list = branch_list_ensure(s);
    if (!list) goto cleanup;
    cJSON *rec = branch_record_find(list, branch_id);
    if (!rec) goto cleanup;

    /* Serialize the target snapshot once — used both for the no-op
     * comparison below and for loading. */
    cJSON *snap = cJSON_GetObjectItem(rec, "messages");
    if (!snap || !cJSON_IsArray(snap)) goto cleanup;
    char *snap_str = cJSON_PrintUnformatted(snap);
    if (!snap_str) goto cleanup;

    /* A switch to the chain that is already live is a no-op — recording
     * it would append a phantom duplicate record and inflate the pill
     * count. Compare serialized message arrays. */
    cJSON *live_json = messages_to_json_array(s->messages, s->messages_count);
    if (!live_json) { free(snap_str); goto cleanup; }
    char *live_str = cJSON_PrintUnformatted(live_json);
    cJSON_Delete(live_json);
    if (!live_str) { free(snap_str); goto cleanup; }
    int same = strcmp(live_str, snap_str) == 0;
    free(live_str);
    if (same)
    {
        rc = 0;
        goto cleanup;
    }

    /* Preserve the current live chain so no chain is lost. */
    if (branch_record_snapshot_live(s, list) != 0)
    { free(snap_str); goto cleanup; }

    /* Load the target snapshot into session->messages. */
    session_truncate_messages(s, 0);
    int drc = session_deserialize_messages(s, snap_str);
    free(snap_str);
    if (drc != 0) goto cleanup;

    /* The newly live chain's creation time is its record's. */
    cJSON *rec_created = cJSON_GetObjectItem(rec, "created_at");
    char *now = branch_now_iso();
    if (!now) goto cleanup;
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    if (!branches) { free(now); goto cleanup; }
    if (branch_set_active_created_at(s,
            rec_created && rec_created->valuestring
                ? rec_created->valuestring : now) != 0)
    { free(now); goto cleanup; }
    free(now);

    /* Pop the target record — only non-live chains are stored. */
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i++)
    {
        if (cJSON_GetArrayItem(list, i) == rec)
        {
            cJSON_DetachItemFromArray(list, i);
            cJSON_Delete(rec);
            break;
        }
    }

    if (save_session_core_locked(sm, s, SM_UPSERT) != 0) goto cleanup;
    rc = 0;

cleanup:
    session_free(s);
    session_manager_unlock(sm);
    return rc;
}

/* Tags the message at `index` of the live chain with `fork_group_id` plus
 * a freshly minted id — marks the regenerated assistant response as the
 * new chain's fork point. The fork copy minted by fork_branch is dropped
 * when the agent context is saved over the live chain after the run, so
 * the marker is re-applied here. All-or-nothing: on any failure the
 * session is unchanged on disk and NULL is returned. Caller frees. */
char *session_manager_tag_message(SessionManager *sm, const char *session_id,
                                  int index, const char *fork_group_id)
{
    if (!sm || !session_id || !fork_group_id || !sm->key_initialized || !sm->db)
        return NULL;

    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) { session_manager_unlock(sm); return NULL; }

    int rc = -1;
    char *id_out = NULL;
    if (index < 0 || index >= s->messages_count) goto cleanup;

    char *id = branch_mint_id("m_");
    if (!id) goto cleanup;

    Message *m = &s->messages[index];
    if (!m->fork_group_id)
    {
        char *group = str_dup(fork_group_id);
        if (!group) { free(id); goto cleanup; }
        m->fork_group_id = group;
    }
    free(m->id);
    m->id = id;

    id_out = str_dup(m->id);
    if (!id_out) goto cleanup;
    if (save_session_core_locked(sm, s, SM_UPSERT) != 0) goto cleanup;
    rc = 0;

cleanup:
    if (rc != 0)
    {
        free(id_out);
        id_out = NULL;
    }
    session_free(s);
    session_manager_unlock(sm);
    return id_out;
}

/* Count of chains (records + live) containing a message with fork_group_id
 * `group`, plus each such chain's created_at and record id, for
 * active-position ordering and branch_switch target resolution. Chain i's
 * record id (or NULL for the live chain) lands in ids[i] when ids is set. */
static int branch_group_chain_count(cJSON *list, const char *group,
                                    char **created_ats, int max_created,
                                    char **ids, int max_ids)
{
    int count = 1; /* the live chain */
    int created_count = 0;
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n && created_count < max_created; i++)
    {
        cJSON *rec = cJSON_GetArrayItem(list, i);
        cJSON *rec_msgs = rec ? cJSON_GetObjectItem(rec, "messages") : NULL;
        if (!rec_msgs || !cJSON_IsArray(rec_msgs)) continue;
        int mcount = cJSON_GetArraySize(rec_msgs);
        int hit = 0;
        for (int j = 0; j < mcount; j++)
        {
            cJSON *m = cJSON_GetArrayItem(rec_msgs, j);
            cJSON *g = m ? cJSON_GetObjectItem(m, "fork_group_id") : NULL;
            if (g && g->valuestring && strcmp(g->valuestring, group) == 0)
            { hit = 1; break; }
        }
        if (!hit) continue;
        count++;
        cJSON *created = cJSON_GetObjectItem(rec, "created_at");
        if (created && created->valuestring && created_ats)
            created_ats[created_count] = str_dup(created->valuestring);
        if (ids && max_ids > created_count)
        {
            cJSON *rid = cJSON_GetObjectItem(rec, "id");
            ids[created_count] =
                (rid && rid->valuestring) ? str_dup(rid->valuestring) : NULL;
        }
        created_count++;
    }
    return count;
}

char *session_manager_branch_info_alloc(SessionManager *sm, const char *session_id)
{
    if (!sm || !session_id || !sm->key_initialized || !sm->db) return NULL;

    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) { session_manager_unlock(sm); return NULL; }

    char *result = NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) goto cleanup;

    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    cJSON *list = branches ? cJSON_GetObjectItem(branches, "list") : NULL;

    for (int i = 0; i < s->messages_count; i++)
    {
        if (!s->messages[i].fork_group_id) continue;

        char *created_ats[64];
        char *branch_ids[64];
        for (int k = 0; k < 64; k++)
        {
            created_ats[k] = NULL;
            branch_ids[k] = NULL;
        }
        int count = branch_group_chain_count(list, s->messages[i].fork_group_id,
                                             created_ats, 64,
                                             branch_ids, 64);
        if (count <= 0) { count = 1; }

        char *live_created = branch_active_created_at(s);
        int active = 1;
        if (live_created)
        {
            for (int k = 0; k < count - 1; k++)
            {
                if (created_ats[k] &&
                    strcmp(created_ats[k], live_created) < 0)
                    active++;
            }
        }

        cJSON *entry = cJSON_CreateObject();
        if (!entry)
        {
            free(live_created);
            for (int k = 0; k < 64; k++)
            {
                free(created_ats[k]);
                free(branch_ids[k]);
            }
            goto cleanup;
        }
        cJSON_AddStringToObject(entry, "message_id",
                                s->messages[i].id ? s->messages[i].id : "");
        cJSON_AddNumberToObject(entry, "count", count);
        cJSON_AddNumberToObject(entry, "active", active);
        /* Record ids of the non-live chains, ordered by chain creation —
         * the frontend's switch target is branch_ids[target-1] (the live
         * chain occupies `active` and has no record). */
        cJSON *ids_arr = cJSON_CreateArray();
        if (ids_arr)
        {
            for (int k = 0; k < count - 1; k++)
            {
                if (branch_ids[k])
                {
                    cJSON *sid = cJSON_CreateString(branch_ids[k]);
                    if (sid) cJSON_AddItemToArray(ids_arr, sid);
                }
            }
            cJSON_AddItemToObject(entry, "branch_ids", ids_arr);
        }
        if (!cJSON_AddItemToArray(arr, entry))
        {
            cJSON_Delete(entry);
            free(live_created);
            for (int k = 0; k < 64; k++)
            {
                free(created_ats[k]);
                free(branch_ids[k]);
            }
            goto cleanup;
        }
        free(live_created);
        for (int k = 0; k < 64; k++)
        {
            free(created_ats[k]);
            free(branch_ids[k]);
        }
    }

    result = cJSON_PrintUnformatted(arr);

cleanup:
    if (arr) cJSON_Delete(arr);
    session_free(s);
    session_manager_unlock(sm);
    return result;
}

char *session_manager_export_session(SessionManager *sm, const char *session_id)
{
    Session *s = session_manager_load_session(sm, session_id);
    if (!s) return NULL;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", s->id ? s->id : "");
    cJSON_AddStringToObject(json, "title", s->title ? s->title : "");
    cJSON *tga = cJSON_CreateNumber(s->title_generation_attempted);
    cJSON_AddItemToObject(json, "title_generation_attempted", tga);
    cJSON_AddStringToObject(json, "created_at", s->created_at ? s->created_at : "");

    if (s->messages_count > 0)
    {
        cJSON *arr = messages_to_json_array(s->messages, s->messages_count);
        if (arr) cJSON_AddItemToObject(json, "messages", arr);
    }

    if (s->metadata)
    {
        cJSON *meta = cJSON_Duplicate(s->metadata, 1);
        if (meta) cJSON_AddItemToObject(json, "metadata", meta);
    }

    if (s->events)
    {
        cJSON *ev = cJSON_Duplicate(s->events, 1);
        if (ev) cJSON_AddItemToObject(json, "events", ev);
    }

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    session_free(s);
    return str;
}

Session *session_manager_import_session(SessionManager *sm, const char *json_str)
{
    if (!sm || !sm->key_initialized || !json_str) return NULL;

    cJSON *json = cJSON_Parse(json_str);
    if (!json) return NULL;

    /* C8: previously we did a `load_session` precheck here to refuse
     * duplicates, then called `session_manager_save_session` (INSERT OR
     * REPLACE) afterward. The two were separate mutex acquisitions, so a
     * concurrent thread could insert the same id in between — the precheck
     * would pass, the save would silently overwrite. The atomic
     * save_session_core(SM_INSERT_IF_ABSENT) does the existence-check + insert
     * as a single SQLite statement (`INSERT ... ON CONFLICT(id) DO NOTHING`)
     * while already holding sm->lock, so the "reject duplicates" promise is
     * now load-bearing. Duplicate-id detection is dropped from this function
     * because it would just duplicate the SQL-level check that follows. */

    Session *s = session_create("Imported Session");
    if (!s) { cJSON_Delete(json); return NULL; }

    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item && id_item->valuestring)
    {
        free(s->id);
        s->id = str_dup(id_item->valuestring);
    }

    cJSON *title_item = cJSON_GetObjectItem(json, "title");
    if (title_item && title_item->valuestring)
    {
        free(s->title);
        s->title = str_dup(title_item->valuestring);
    }

    cJSON *tga_item = cJSON_GetObjectItem(json, "title_generation_attempted");
    if (tga_item && cJSON_IsNumber(tga_item))
        s->title_generation_attempted = tga_item->valueint;

    cJSON *created_item = cJSON_GetObjectItem(json, "created_at");
    if (created_item && created_item->valuestring)
    {
        free(s->created_at);
        s->created_at = str_dup(created_item->valuestring);
    }

    cJSON *msgs = cJSON_GetObjectItem(json, "messages");
    if (msgs && cJSON_IsArray(msgs))
    {
        /* J1: `agent_save_session` filters out "system" messages before
         * persisting — the system prompt is regenerated by
         * `inject_system_with_summary` on every `agent_run_streaming`
         * cycle, so storing a stale copy would either (a) get
         * overwritten on the next run, leaking memory and confusing the
         * agent's messages array, or (b) if a future `agent_run_streaming`
         * skipped `inject_system_with_summary`, it would send a stale
         * system prompt. The import path used to bypass this filter
         * (it called `session_deserialize_messages` directly on the
         * imported JSON). Now we walk the parsed cJSON messages array
         * and DETACH any "system"-role entry before deserializing, so
         * what's written to the DB matches what agent_save_session
         * would have written itself. Iterate by index, decrement on
         * detach. The detached item is deleted, severing it from the
         * cJSON tree; the rest of the array shifts down to fill the gap. */
        for (int i = 0; i < cJSON_GetArraySize(msgs); )
        {
            cJSON *item = cJSON_GetArrayItem(msgs, i);
            cJSON *role = item ? cJSON_GetObjectItem(item, "role") : NULL;
            if (role && cJSON_IsString(role) &&
                strcmp(role->valuestring, "system") == 0)
            {
                cJSON_DetachItemFromArray(msgs, i);
                cJSON_Delete(item);
                continue; /* don't advance — array shifted down */
            }
            i++;
        }

        char *msgs_str = cJSON_PrintUnformatted(msgs);
        if (msgs_str)
        {
            session_deserialize_messages(s, msgs_str);
            free(msgs_str);
        }
    }

    cJSON *meta = cJSON_GetObjectItem(json, "metadata");
    if (meta)
    {
        char *meta_str = cJSON_PrintUnformatted(meta);
        if (meta_str)
        {
            session_deserialize_metadata(s, meta_str);
            free(meta_str);
        }
    }

    cJSON *events = cJSON_GetObjectItem(json, "events");
    if (events && cJSON_IsArray(events))
    {
        char *ev_str = cJSON_PrintUnformatted(events);
        if (ev_str)
        {
            session_deserialize_events(s, ev_str);
            free(ev_str);
        }
    }

    cJSON_Delete(json);

    /* C10: hold sm->lock across the insert so the import+check are atomic
     * at the mutex level too (the SQL-level DO NOTHING is already atomic
     * per C8, but the import builds a fresh Session outside the lock; with
     * the lock here, no other writer can race the build+insert pair). */
    session_manager_lock(sm);
    int rc = save_session_core_locked(sm, s, SM_INSERT_IF_ABSENT);
    if (rc <= 0)
    {
        session_manager_unlock(sm);
        session_free(s);
        return NULL;
    }
    session_manager_unlock(sm);

    return s;
}

int session_manager_purge_sessions(SessionManager *sm, int older_than_days)
{
    if (!sm || !sm->db) return -1;

    /* B9: validate the input so the (time_t)older_than_days * 86400
     * multiplication can't overflow for huge or negative values. The purge
     * semantics only make sense for older_than_days >= 0; anything else is
     * operator error and refuses rather than risking UB or a flipped cutoff. */
    if (older_than_days < 0 || older_than_days > 365 * 100)
    {
        log_error("purge_sessions refusing bad older_than_days",
                  "days", "out of range [0, 36500]", NULL);
        return -1;
    }

    time_t cutoff = time(NULL) - (time_t)older_than_days * 86400;
    /* D3: localtime_r is the thread-safe variant — localtime under a
     * multi-threaded server races on the shared static buffer. */
    struct tm tm_storage;
    struct tm *tm_cutoff = localtime_r(&cutoff, &tm_storage);
    if (!tm_cutoff) return -1;

    char cutoff_str[64];
    strftime(cutoff_str, sizeof(cutoff_str), "%Y-%m-%dT%H:%M:%S", tm_cutoff);

    pthread_mutex_lock(&sm->lock);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM agent_sessions WHERE created_at < ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        /* H2: SQLite guarantees *ppStmt == NULL on prepare error so the
         * `sqlite3_finalize(stmt)` here is a no-op defensively kept for
         * review consistency (every other error path in this function
         * finalizes the stmt before returning). Also log the prepare
         * failure — the prior code returned -1 with no operator signal. */
        log_error("sqlite prepare purge", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&sm->lock);
        return -1;
    }

    int bind_rc = sqlite3_bind_text(stmt, 1, cutoff_str, -1, SQLITE_TRANSIENT);
    if (bind_rc != SQLITE_OK)
    {
        log_error("sqlite bind purge", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&sm->lock);
        return -1;
    }

    int step_rc = sqlite3_step(stmt);
    int deleted = 0;
    if (step_rc == SQLITE_DONE)
        deleted = sqlite3_changes(sm->db);
    else
        log_error("sqlite step purge", "err", sqlite3_errmsg(sm->db), NULL);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);

    return (step_rc == SQLITE_DONE) ? deleted : -1;
}

int session_manager_log_event(SessionManager *sm, const char *session_id,
                               const char *event_type, const char *data)
{
    /* C10: hold sm->lock across load→mutate→save. Also C11: an
     * agent_save_session that holds the lock across its own triad blocks
     * here, so its metadata/events written back ARE the freshest. */
    session_manager_lock(sm);
    Session *s = load_session_locked(sm, session_id);
    if (!s) { session_manager_unlock(sm); return -1; }

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
