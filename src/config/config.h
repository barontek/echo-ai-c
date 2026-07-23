#ifndef ECHO_CONFIG_H
#define ECHO_CONFIG_H

typedef struct Conf Conf;

Conf *conf_load(const char *path);
const char *conf_get(const Conf *conf, const char *key);
int conf_get_int(const Conf *conf, const char *key, int def);
void conf_free(Conf *conf);

#endif
