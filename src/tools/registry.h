#ifndef ECHO_REGISTRY_H
#define ECHO_REGISTRY_H

#include "tool.h"
#include "../safety/safety.h"

void registry_init(SafetyConfig *safety);
void registry_register(Tool *tool);
Tool *registry_get(const char *name);
char *registry_schemas_json(void);
int registry_count(void);
void registry_destroy(void);

#endif
