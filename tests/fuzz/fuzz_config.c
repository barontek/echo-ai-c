/* libFuzzer harness for conf_load — the .conf file parser.
 *
 * Writes fuzz data to a temp file, parses it, exercises conf_get on a few
 * keys that are likely to be in the fuzz data, then frees everything.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_config -max_len=4096
 */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "config/config.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    if (size > 65536) return 0;

    char tmp_path[] = "/tmp/fuzz_conf_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return 0;

    size_t written = 0;
    while (written < size)
    {
        ssize_t rc = write(fd, data + written, size - written);
        if (rc <= 0) break;
        written += (size_t)rc;
    }
    close(fd);

    if (written == 0)
    {
        unlink(tmp_path);
        return 0;
    }

    Conf *conf = conf_load(tmp_path);
    if (conf)
    {
        /* Exercise common conf_get paths with plausible keys. */
        conf_get(conf, "provider");
        conf_get(conf, "model");
        conf_get(conf, "base_url");
        conf_get(conf, "temperature");
        conf_get(conf, "nonexistent_key");
        conf_get_int(conf, "temperature", 0);
        conf_get_int(conf, "nonexistent_int", 42);
        conf_free(conf);
    }

    unlink(tmp_path);
    return 0;
}
