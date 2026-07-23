#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <unistd.h>
#include <sys/stat.h>

#include "config/config.h"
#include "utils/logging.h"
#include "utils/string_utils.h"
#include "agent/agent.h"
#include "tools/registry.h"
#include "safety/safety.h"
#include "session/session_manager.h"
#include "session/encryption.h"
#include "server/server.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  --cli        Interactive REPL with rich-rendered chat\n");
    printf("  --chat       Lightweight interactive chat\n");
    printf("  --web        HTTP server on port 8080 (default)\n");
    printf("  --config PATH  Path to config file\n");
    printf("  --debug        Enable debug-level logging\n");
    printf("  --help         Show this help message\n");
}

static SafetyConfig *load_safety_config(Conf *conf)
{
    SafetyConfig *safety = safety_config_create();
    if (!safety) return NULL;

    const char *v = conf_get(conf, "safety.workspace");
    safety->workspace = str_dup(v ? v : ".");

    v = conf_get(conf, "safety.allow_network");
    safety->allow_network = v ? (strcmp(v, "true") == 0 || strcmp(v, "1") == 0) : 1;

    safety->max_file_size = (size_t)conf_get_int(conf, "safety.max_file_size", 10485760);
    safety->max_execution_time = conf_get_int(conf, "safety.max_execution_time", 300);

    return safety;
}

static AgentConfig *load_agent_config(Conf *conf)
{
    AgentConfig *cfg = calloc(1, sizeof(AgentConfig));
    if (!cfg) return NULL;

    const char *v = conf_get(conf, "agent.provider");
    cfg->provider = v ? v : "ollama";

    v = conf_get(conf, "agent.model");
    if (!v) { log_error("agent.model required in config", NULL); free(cfg); return NULL; }
    cfg->model = v;

    v = conf_get(conf, "ollama.base_url");
    cfg->base_url = v ? v : "http://localhost:11434";

    v = conf_get(conf, "agent.system_prompt");
    cfg->system_prompt = v ? v : "You are a helpful AI assistant.";

    cfg->temperature = conf_get_int(conf, "agent.temperature", 70) / 100.0;
    cfg->timeout = conf_get_int(conf, "agent.timeout", 120);
    cfg->max_iterations = conf_get_int(conf, "agent.max_iterations", 50);
    cfg->max_context_messages = conf_get_int(conf, "agent.max_context_messages", 50);
    cfg->max_context_chars = conf_get_int(conf, "agent.max_context_chars", 100000);
    cfg->num_ctx = conf_get_int(conf, "ollama.num_ctx", 4096);
    cfg->keep_alive_secs = conf_get_int(conf, "ollama.keep_alive_secs", 120);

    return cfg;
}

static SessionManager *init_session_manager(Conf *conf)
{
    const char *enabled = conf_get(conf, "session.enabled");
    if (!enabled || strcmp(enabled, "true") != 0)
    {
        log_info("session persistence disabled", NULL);
        return NULL;
    }

    const char *home = getenv("HOME");
    if (!home) { log_error("HOME not set, cannot initialize session manager", NULL); return NULL; }

    char *data_dir = NULL;
    if (asprintf(&data_dir, "%s/.config/echo-ai", home) < 0) return NULL;

    mkdir(data_dir, 0755);
    char *sessions_dir = NULL;
    if (asprintf(&sessions_dir, "%s/sessions", data_dir) < 0) { free(data_dir); return NULL; }
    mkdir(sessions_dir, 0755);
    free(sessions_dir);

    char *password = encryption_resolve_password();
    if (!password)
    {
        log_error("no password available (set ECHO_PASSWORD or create ~/.config/echo-ai/password)", NULL);
        free(data_dir);
        return NULL;
    }

    SessionManager *sm = session_manager_create(data_dir, password);
    free(data_dir);

    size_t pw_len = strlen(password);
    memset(password, 0, pw_len);
    free(password);

    if (!sm)
    {
        log_error("failed to create session manager", NULL);
        return NULL;
    }

    log_info("session manager initialized", "data_dir", sm->data_dir, NULL);
    return sm;
}

static SessionManager *g_session_manager = NULL;

static void run_cli(Conf *conf)
{
    SafetyConfig *safety = load_safety_config(conf);
    if (!safety) { log_error("failed to load safety config", NULL); return; }

    registry_init(safety);

    AgentConfig *cfg = load_agent_config(conf);
    if (!cfg) { log_error("failed to load agent config", NULL); registry_destroy(); safety_config_free(safety); return; }

    Agent *agent = agent_create(cfg);
    if (!agent) { log_error("failed to create agent", NULL); free(cfg); registry_destroy(); safety_config_free(safety); return; }

    g_session_manager = init_session_manager(conf);
    if (g_session_manager)
    {
        agent_set_session_manager(agent, g_session_manager);
        Session *s = session_manager_create_session(g_session_manager, "CLI Session");
        if (s)
        {
            free(agent->session_id);
            agent->session_id = str_dup(s->id);
            log_info("session created", "id", agent->session_id, NULL);
            session_free(s);
        }
    }

    free(cfg);

    printf("Echo AI -- CLI mode (type '/exit' to quit, '/new' to reset)\n");
    printf("Model: %s\n", agent->model);
    printf("Tools: %d registered\n", registry_count());
    if (g_session_manager)
        printf("Session: %s\n\n", agent->session_id ? agent->session_id : "(none)");
    else
        printf("Session: disabled\n\n");

    char *line = NULL;
    size_t line_cap = 0;

    while (1)
    {
        printf("> ");
        fflush(stdout);

        ssize_t len = getline(&line, &line_cap, stdin);
        if (len < 0) break;

        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) break;

        if (strcmp(line, "/new") == 0)
        {
            agent_destroy(agent);
            AgentConfig *new_cfg = load_agent_config(conf);
            if (new_cfg)
            {
                agent = agent_create(new_cfg);
                if (agent && g_session_manager)
                {
                    agent_set_session_manager(agent, g_session_manager);
                    Session *s = session_manager_create_session(g_session_manager, "CLI Session");
                    if (s)
                    {
                        free(agent->session_id);
                        agent->session_id = str_dup(s->id);
                        session_free(s);
                    }
                }
                free(new_cfg);
            }
            if (!agent) { log_error("failed to recreate agent", NULL); break; }
            printf("Session reset.\n\n");
            continue;
        }

        if (strcmp(line, "/sessions") == 0)
        {
            if (!g_session_manager)
            {
                printf("Session persistence disabled.\n\n");
                continue;
            }
            SessionList *list = session_manager_list_sessions(g_session_manager);
            if (!list)
            {
                printf("No sessions found.\n\n");
                continue;
            }
            printf("Sessions (%d):\n", list->count);
            for (int i = 0; i < list->count; i++)
            {
                printf("  %s | %s | %s\n",
                       list->ids[i], list->titles[i], list->created_ats[i]);
            }
            printf("\n");
            session_list_free(list);
            continue;
        }

        if (line[0] == '\0') continue;

        printf("\n");
        LLMResponse *resp = agent_run(agent, line);

        if (resp && resp->content)
        {
            printf("%s\n\n", resp->content);
        }
        else if (resp)
        {
            printf("[no text response]\n\n");
        }
        else
        {
            printf("[error getting response]\n\n");
        }

        llm_response_free(resp);
    }

    free(line);
    agent_destroy(agent);
    if (g_session_manager) session_manager_free(g_session_manager);
    registry_destroy();
    safety_config_free(safety);
}

static void run_web(Conf *conf, const char *config_path)
{
    SafetyConfig *safety = load_safety_config(conf);
    if (!safety) { log_error("failed to load safety config", NULL); return; }

    registry_init(safety);

    AgentConfig *cfg = load_agent_config(conf);
    if (!cfg) { log_error("failed to load agent config", NULL); registry_destroy(); safety_config_free(safety); return; }

    Agent *agent = agent_create(cfg);
    if (!agent) { log_error("failed to create agent", NULL); free(cfg); registry_destroy(); safety_config_free(safety); return; }
    free(cfg);

    g_session_manager = init_session_manager(conf);

    int port = conf_get_int(conf, "server.port", 8080);

    ServerContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = g_session_manager ? STATE_LOCKED : STATE_UNLOCKED;
    if (!g_session_manager)
    {
        ctx.state = STATE_UNLOCKED;
        ctx.unlock_token = str_dup("noop");
    }
    ctx.config_path = str_dup(config_path);
    ctx.agent = agent;
    ctx.sm = g_session_manager;
    ctx.safety = safety;
    ctx.conf = (Conf *)conf;
    ctx.port = port;
    ctx.rate_limiter = rate_limiter_create(60, 60);

    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d", port);
    log_info("starting web server", "port", pbuf, NULL);
    server_start(&ctx);

    rate_limiter_destroy(ctx.rate_limiter);
    agent_destroy(agent);
    if (g_session_manager) session_manager_free(g_session_manager);
    registry_destroy();
    safety_config_free(safety);
}

int main(int argc, char *argv[])
{
    enum mode { MODE_CLI, MODE_CHAT, MODE_WEB } mode = MODE_WEB;
    const char *config_path = "config.conf";

    static struct option long_opts[] = {
        {"cli",    no_argument,       0, 'c'},
        {"chat",   no_argument,       0, 't'},
        {"web",    no_argument,       0, 'w'},
        {"config", required_argument, 0, 'f'},
        {"debug",  no_argument,       0, 'd'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int debug = 0;
    while ((opt = getopt_long(argc, argv, "ctwf:dh", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 'c': mode = MODE_CLI;  break;
        case 't': mode = MODE_CHAT; break;
        case 'w': mode = MODE_WEB;  break;
        case 'f': config_path = optarg; break;
        case 'd': debug = 1; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    log_init();
    if (debug) log_set_level(LOG_DEBUG);
    log_info("starting echo-ai", "mode", mode == MODE_CLI ? "cli" :
                                     mode == MODE_CHAT ? "chat" : "web", NULL);
    log_info("loading config", "path", config_path, NULL);

    Conf *conf = conf_load(config_path);
    if (!conf)
    {
        log_error("failed to load config", "path", config_path, NULL);
        log_cleanup();
        return 1;
    }

    switch (mode)
    {
    case MODE_CLI:
        run_cli(conf);
        break;
    case MODE_CHAT:
        log_info("chat mode not yet implemented", NULL);
        break;
    case MODE_WEB:
        run_web(conf, config_path);
        break;
    }

    conf_free(conf);
    log_cleanup();
    return 0;
}
