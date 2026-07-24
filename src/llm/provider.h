#ifndef ECHO_PROVIDER_H
#define ECHO_PROVIDER_H

#include "../agent/message.h"

typedef struct LLMProvider LLMProvider;

struct LLMProvider {
    LLMResponse *(*chat)(LLMProvider *self, Message *messages, int count,
                         const char *model, double temperature, int timeout);
    LLMResponse *(*chat_streaming)(LLMProvider *self, Message *messages, int count,
                                   const char *model, double temperature, int timeout,
                                   void (*on_chunk)(const char *chunk, void *userdata),
                                   void *userdata);
    LLMResponse *(*extract_structured)(LLMProvider *self, Message *messages, int count,
                                        const char *model, double temperature, int timeout,
                                        const char *json_schema);
    void (*destroy)(LLMProvider *self);
    void *ctx;
};

LLMProvider *get_provider(const char *name, const char *model,
                          const char *base_url, int num_ctx, int keep_alive_secs);

#endif
