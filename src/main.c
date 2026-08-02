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
#include "tools/search_provider.h"
#include "safety/safety.h"
#include "session/session_manager.h"
#include "session/encryption.h"
#include "session/memory.h"
#include "change_tracker/change_tracker.h"
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
    safety_load_from_conf(safety, conf);
    if (!safety->workspace) safety->workspace = str_dup(".");
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

    if (strcmp(cfg->provider, "openai") == 0)
    {
        /* OpenAI-compatible provider (also covers LM Studio etc. via a
         * local openai.base_url). */
        v = conf_get(conf, "openai.base_url");
        cfg->base_url = v ? v : "https://api.openai.com";
    }
    else
    {
        v = conf_get(conf, "ollama.base_url");
        cfg->base_url = v ? v : "http://localhost:11434";
    }

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
    if (!enabled || strcmp(enabled, "false") != 0)
    {
        log_info("session persistence enabled", NULL);
    } else {
        log_info("session persistence disabled", NULL);
        return NULL;
    }

    const char *home = getenv("HOME");
    if (!home) { log_error("HOME not set, cannot initialize session manager", NULL); return NULL; }

    char *data_dir = NULL;
    if (asprintf(&data_dir, "%s/.config/echo-ai", home) < 0) return NULL;

    mkdir(data_dir, 0755);

    char *password = encryption_resolve_password();
    if (!password)
    {
        int is_first_run = encryption_first_run_detect(data_dir);
        const char *prompt = is_first_run ? "Create a database password: " : "Enter database password: ";
        password = getpass(prompt);
        if (!password)
        {
            log_error("no password provided", NULL);
            free(data_dir);
            return NULL;
        }
        password = str_dup(password);
        if (!password)
        {
            free(data_dir);
            return NULL;
        }
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

static void run_chat(Conf *conf)
{
    SafetyConfig *safety = load_safety_config(conf);
    if (!safety) { log_error("failed to load safety config", NULL); return; }

    registry_init(safety);

    {
        const char *enabled = conf_get(conf, "tools.enabled");
        if (enabled) registry_set_enabled(enabled);
    }

    const char *sp_name = conf_get(conf, "search.provider");
    if (sp_name)
    {
        const char *api_key = conf_get(conf, "search.api_key");
        SearchProvider *sp = search_provider_create(sp_name, api_key);
        if (sp) registry_set_search_provider(sp);
    }

    AgentConfig *cfg = load_agent_config(conf);
    if (!cfg) { log_error("failed to load agent config", NULL); registry_destroy(); safety_config_free(safety); return; }

    Agent *agent = agent_create(cfg);
    if (!agent) { log_error("failed to create agent", NULL); free(cfg); registry_destroy(); safety_config_free(safety); return; }
    agent_set_safety(agent, safety);

    registry_set_delegate_config(cfg->provider, cfg->base_url, cfg->model,
                                  cfg->num_ctx, cfg->keep_alive_secs,
                                  cfg->temperature, cfg->timeout, cfg->max_iterations);
    free(cfg);

    printf("Echo AI -- Chat mode (type '/exit' to quit)\n");
    printf("Model: %s\n", agent->model);
    printf("Tools: %d registered\n\n", registry_count());

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
        if (line[0] == '\0') continue;

        printf("\n");
        LLMResponse *resp = agent_run(agent, line);

        if (resp && resp->content)
            printf("%s\n\n", resp->content);
        else if (resp)
            printf("[no text response]\n\n");
        else
            printf("[error getting response]\n\n");

        llm_response_free(resp);
    }

    free(line);
    agent_destroy(agent);
    registry_destroy();
    safety_config_free(safety);
}

static void print_cli_help(void)
{
    printf("Commands:\n");
    printf("  /exit           Quit the REPL\n");
    printf("  /new            Reset conversation\n");
    printf("  /save <name>    Save current session\n");
    printf("  /load <id>      Load a session by ID\n");
    printf("  /model <name>   Switch model\n");
    printf("  /undo           Undo last file change\n");
    printf("  /redo           Redo last undone file change\n");
    printf("  /clear          Clear the screen\n");
    printf("  /sessions       List saved sessions\n");
    printf("  /help           Show this message\n");
}

static void run_cli(Conf *conf)
{
    SafetyConfig *safety = load_safety_config(conf);
    if (!safety) { log_error("failed to load safety config", NULL); return; }

    registry_init(safety);

    {
        const char *enabled = conf_get(conf, "tools.enabled");
        if (enabled) registry_set_enabled(enabled);
    }

    {
        const char *sp_name = conf_get(conf, "search.provider");
        if (sp_name)
        {
            const char *api_key = conf_get(conf, "search.api_key");
            SearchProvider *sp = search_provider_create(sp_name, api_key);
            if (sp) registry_set_search_provider(sp);
        }
    }

    AgentConfig *cfg = load_agent_config(conf);
    if (!cfg) { log_error("failed to load agent config", NULL); registry_destroy(); safety_config_free(safety); return; }

    Agent *agent = agent_create(cfg);
    if (!agent) { log_error("failed to create agent", NULL); free(cfg); registry_destroy(); safety_config_free(safety); return; }
    agent_set_safety(agent, safety);

    registry_set_delegate_config(cfg->provider, cfg->base_url, cfg->model,
                                  cfg->num_ctx, cfg->keep_alive_secs,
                                  cfg->temperature, cfg->timeout, cfg->max_iterations);

    g_session_manager = init_session_manager(conf);
    if (g_session_manager)
    {
        registry_set_session_manager(g_session_manager);
        agent_set_session_manager(agent, g_session_manager);
        log_info("session manager ready", NULL);
    }

    ChangeTracker *ct = ct_create();

    free(cfg);

    printf("Echo AI -- CLI mode (type '/help' for commands)\n");
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
                }
                free(new_cfg);
            }
            if (!agent) { log_error("failed to recreate agent", NULL); break; }
            printf("Session reset.\n\n");
            continue;
        }

        if (strcmp(line, "/help") == 0)
        {
            print_cli_help();
            printf("\n");
            continue;
        }

        if (strcmp(line, "/clear") == 0)
        {
            printf("\033[2J\033[H");
            continue;
        }

        if (strcmp(line, "/undo") == 0)
        {
            int rc = ct_undo(ct);
            if (rc < 0)
                printf("Nothing to undo.\n\n");
            else
                printf("Undone (%d bytes restored).\n\n", rc);
            continue;
        }

        if (strcmp(line, "/redo") == 0)
        {
            int rc = ct_redo(ct);
            if (rc < 0)
                printf("Nothing to redo.\n\n");
            else
                printf("Redone (%d bytes written).\n\n", rc);
            continue;
        }

        if (strncmp(line, "/save ", 6) == 0)
        {
            if (!g_session_manager)
            {
                printf("Session persistence disabled.\n\n");
                continue;
            }
            const char *name = line + 6;
            if (name[0] == '\0')
            {
                printf("Usage: /save <name>\n\n");
                continue;
            }
            Session *s = session_manager_load_session(g_session_manager, agent->session_id);
            if (s)
            {
                free(s->title);
                s->title = str_dup(name);
                if (session_manager_save_session(g_session_manager, s) == 0)
                    printf("Session saved as '%s'.\n\n", name);
                else
                    printf("Failed to save session.\n\n");
                session_free(s);
            }
            else
                printf("No active session to save.\n\n");
            continue;
        }

        if (strncmp(line, "/load ", 6) == 0)
        {
            if (!g_session_manager)
            {
                printf("Session persistence disabled.\n\n");
                continue;
            }
            const char *sid = line + 6;
            if (sid[0] == '\0')
            {
                printf("Usage: /load <id>\n\n");
                continue;
            }
            Session *s = session_manager_load_session(g_session_manager, sid);
            if (!s)
            {
                printf("Session '%s' not found.\n\n", sid);
                continue;
            }
            free(agent->session_id);
            agent->session_id = str_dup(sid);
            printf("Loaded session: %s (%s)\n\n", s->title, sid);
            session_free(s);
            continue;
        }

        if (strncmp(line, "/model ", 7) == 0)
        {
            const char *model = line + 7;
            if (model[0] == '\0')
            {
                printf("Current model: %s\n\n", agent->model);
                continue;
            }
            free(agent->model);
            agent->model = str_dup(model);
            printf("Switched to model: %s\n\n", model);
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
    ct_destroy(ct);
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

    {
        const char *enabled = conf_get(conf, "tools.enabled");
        if (enabled) registry_set_enabled(enabled);
    }

    {
        const char *sp_name = conf_get(conf, "search.provider");
        if (sp_name)
        {
            const char *api_key = conf_get(conf, "search.api_key");
            SearchProvider *sp = search_provider_create(sp_name, api_key);
            if (sp) registry_set_search_provider(sp);
        }
    }

    AgentConfig *cfg = load_agent_config(conf);
    if (!cfg) { log_error("failed to load agent config", NULL); registry_destroy(); safety_config_free(safety); return; }

    Agent *agent = agent_create(cfg);
    if (!agent) { log_error("failed to create agent", NULL); free(cfg); registry_destroy(); safety_config_free(safety); return; }
    agent_set_safety(agent, safety);

    registry_set_delegate_config(cfg->provider, cfg->base_url, cfg->model,
                                  cfg->num_ctx, cfg->keep_alive_secs,
                                  cfg->temperature, cfg->timeout, cfg->max_iterations);
    /* D2: copy before freeing cfg; assign to ctx later once it's declared. */
    AgentConfig cfg_copy = *cfg;
    free(cfg);

    const char *session_enabled_str = conf_get(conf, "session.enabled");
    int session_enabled = !session_enabled_str || strcmp(session_enabled_str, "false") != 0;

    int port = conf_get_int(conf, "server.port", 8080);

    ServerContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (session_enabled)
    {
        const char *home = getenv("HOME");
        if (home)
        {
            char *data_dir = NULL;
            if (asprintf(&data_dir, "%s/.config/echo-ai", home) >= 0)
            {
                int is_first_run = encryption_first_run_detect(data_dir);
                free(data_dir);
                ctx.state = is_first_run ? STATE_SETUP : STATE_LOCKED;
            }
            else
            {
                ctx.state = STATE_LOCKED;
            }
        }
        else
        {
            log_error("HOME not set, cannot determine auth state", NULL);
            ctx.state = STATE_LOCKED;
        }
    }
    else
    {
        ctx.state = STATE_UNLOCKED;
        ctx.unlock_token = str_dup("noop");
    }

    /* D2: inner string pointers alias Conf strings which outlive the server */
    ctx.agent_cfg = cfg_copy;
    ctx.config_path = str_dup(config_path);
    ctx.agent = agent;
    ctx.sm = NULL;
    ctx.safety = safety;
    ctx.conf = (Conf *)conf;
    ctx.port = port;
    {
        const char *home = getenv("HOME");
        char *rl_db = NULL;
        if (home && asprintf(&rl_db, "%s/.config/echo-ai/rate_limits.db", home) >= 0)
        {
            ctx.rate_limiter = rate_limiter_create(60, 60, rl_db);
            free(rl_db);
        }
        else
        {
            ctx.rate_limiter = rate_limiter_create(60, 60, NULL);
        }
    }
    ctx.metrics = metrics_create();
    if (agent) agent_set_metrics(agent, ctx.metrics);
    ctx.change_tracker = ct_create();

    registry_set_change_tracker(ctx.change_tracker);

    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d", port);
    log_info("starting web server", "port", pbuf, NULL);
    server_start(&ctx);

    rate_limiter_destroy(ctx.rate_limiter);
    metrics_destroy(ctx.metrics);
    ct_destroy(ctx.change_tracker);
    agent_destroy(agent);
    if (ctx.sm) session_manager_free(ctx.sm);
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
        run_chat(conf);
        break;
    case MODE_WEB:
        run_web(conf, config_path);
        break;
    }

    conf_free(conf);
    log_cleanup();
    return 0;
}
