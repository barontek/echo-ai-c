#ifndef ECHO_MEMORY_H
#define ECHO_MEMORY_H

#include <sqlite3.h>

typedef struct {
    char *key;
    char *value;
} MemoryFact;

int memory_table_init(sqlite3 *db);
int memory_set(sqlite3 *db, const char *key, const char *value);
char *memory_get(sqlite3 *db, const char *key);
int memory_delete(sqlite3 *db, const char *key);
MemoryFact *memory_list_all(sqlite3 *db, int *count);
void memory_facts_free(MemoryFact *facts, int count);

#ifdef MEMORY_TEST
void memory_test_set_alloc_fail(int nth_allocation);
#endif

#endif