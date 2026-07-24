#ifndef ECHO_CONTEXT_H
#define ECHO_CONTEXT_H

#include "message.h"

char *split_thinking_content(const char *raw);
Message *apply_context_window(Message *msgs, int *count,
                              int max_messages, int max_chars);
Message *smart_select(Message *msgs, int count, int keep_count);
Message *trim_messages_by_tokens(Message *msgs, int *count, int max_tokens);

#endif
