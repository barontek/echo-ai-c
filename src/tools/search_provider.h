#ifndef ECHO_SEARCH_PROVIDER_H
#define ECHO_SEARCH_PROVIDER_H

typedef struct SearchProvider SearchProvider;

struct SearchProvider {
    char *name;
    char *(*search)(SearchProvider *self, const char *query, int num_results);
    void (*destroy)(SearchProvider *self);
    void *ctx;
};

SearchProvider *search_provider_create(const char *provider_name, const char *api_key);

#endif
