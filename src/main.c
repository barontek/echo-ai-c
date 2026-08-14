/*
 * main.c - entry point: parses CLI flags, loads config, wires up the agent,
 * session manager, safety config, and tools, then starts the HTTP server.
 * Depends on: config, agent, session, safety, server, utils.
 */

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
#include "llm/openai_oauth.h"
#include "tools/registry.h"
#include "tools/search_provider.h"
#include "llm/factory.h"
#include "safety/safety.h"
#include "session/session_manager.h"
#include "session/encryption.h"
#include "session/memory.h"
#include "change_tracker/change_tracker.h"
#include "tui/tui.h"
#include "server/server.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  --cli        Interactive TUI (notcurses-based chat)\n");
    printf("  --chat       Lightweight line-based chat (piped input ok)\n");
    printf("  --web        HTTP server on port 8080 (default)\n");
    printf("  --config PATH  Path to config file\n");
    printf("  --debug        Enable debug-level logging\n");
    printf("  --help         Show this help message\n");
}

static SafetyConfig *load_safety_config(Conf *conf)
{
    SafetyConfig *safety = safety_config_create();
    if (!safety) return NULL;
    if (safety_load_from_conf(safety, conf) != 0)
    {
        log_error("failed to load safety config (allocation failure)", NULL);
        safety_config_free(safety);
        return NULL;
    }
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
    if (!v) {
        log_error("agent.model required in config", NULL);
        free(cfg);
        return NULL;
    }
    cfg->model = v;

    /* Per-provider base URL: [<provider>.base_url] override, else the
     * provider's canonical default (single source of truth in
     * factory.c's provider_default_base_url — same mapping the WS
     * provider-switch path uses). */
    char provider_key[64];
    snprintf(provider_key, sizeof(provider_key), "%s.base_url", cfg->provider);
    v = conf_get(conf, provider_key);
    cfg->base_url = v ? v : provider_default_base_url(cfg->provider);
    if (!cfg->base_url)
    {
        log_error("unknown provider in config", "provider", cfg->provider, NULL);
        free(cfg);
        return NULL;
    }

    /* Optional per-provider API token from the [providers] section.
     * opencode_zen reads the shared "opencode" key. */
    /* OpenAI is OAuth-only. Static provider tokens remain available through
     * openai_compatible, but are never accepted by the OpenAI provider. */
    cfg->api_token = strcmp(cfg->provider, "openai") == 0
        ? NULL : conf_provider_token(conf, cfg->provider);

    v = conf_get(conf, "agent.system_prompt");
    cfg->system_prompt = v ? v : "You are a helpful AI assistant.";

    cfg->temperature = conf_get_int(conf, "agent.temperature", 70) / 100.0;
    cfg->timeout = conf_get_int(conf, "agent.timeout", 120);
    cfg->max_iterations = conf_get_int(conf, "agent.max_iterations", 50);
    cfg->max_context_messages = conf_get_int(conf, "agent.max_context_messages", 50);
    cfg->max_context_chars = conf_get_int(conf, "agent.max_context_chars", 100000);
    cfg->max_tool_result_chars = conf_get_int(conf, "agent.max_tool_result_chars", 25000);
    cfg->num_ctx = conf_get_int(conf, "ollama.num_ctx", 4096);
    cfg->keep_alive_secs = conf_get_int(conf, "ollama.keep_alive_secs", 120);

    /* Optional reasoning-effort hint for providers that support it
     * (currently openai); validated by the provider at create time. */
    cfg->effort = conf_get(conf, "agent.effort");

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
    if (!home) {
        log_error("HOME not set, cannot initialize session manager", NULL);
        return NULL;
    }

    char *data_dir = NULL;
    if (asprintf(&data_dir, "%s/.config/echo-ai", home) < 0) return NULL;

    char *password = encryption_resolve_password_alloc();
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

static int run_openai_device_login(OpenAIOAuth *auth);

/*
 * RuntimeCtx - result of the shared bootstrap sequence (setup_runtime).
 * Owns agent, openai_auth, and safety; cfg_copy aliases Conf strings for
 * web mode's AgentConfig copy. Caller must release with teardown_runtime().
 */
typedef struct {
    Agent *agent;
    OpenAIOAuth *openai_auth;
    SafetyConfig *safety;
    AgentConfig cfg_copy;
    int registry_failed;
} RuntimeCtx;

static RuntimeCtx *setup_runtime(Conf *conf)
{
    RuntimeCtx *rt = calloc(1, sizeof(RuntimeCtx));
    if (!rt) return NULL;

    rt->safety = load_safety_config(conf);
    if (!rt->safety)
    {
        log_error("failed to load safety config", NULL);
        free(rt);
        return NULL;
    }

    rt->registry_failed = registry_init(rt->safety);
    if (rt->registry_failed > 0)
        log_error("tool registry partially initialized", "failed_count", NULL);

    {
        const char *enabled = conf_get(conf, "tools.enabled");
        if (enabled && registry_set_enabled(enabled) != 0)
            log_error("failed to enable tools from config", "names", enabled, NULL);
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

    rt->openai_auth = openai_oauth_create();
    if (!rt->openai_auth)
    {
        registry_destroy();
        safety_config_free(rt->safety);
        free(rt);
        return NULL;
    }
    registry_set_openai_oauth(rt->openai_auth);

    AgentConfig *cfg = load_agent_config(conf);
    if (cfg) cfg->openai_auth = rt->openai_auth;
    if (!cfg)
    {
        log_error("failed to load agent config", NULL);
        openai_oauth_destroy(rt->openai_auth);
        registry_destroy();
        safety_config_free(rt->safety);
        free(rt);
        return NULL;
    }

    rt->agent = agent_create(cfg);
    if (!rt->agent)
    {
        log_error("failed to create agent", NULL);
        free(cfg);
        openai_oauth_destroy(rt->openai_auth);
        registry_destroy();
        safety_config_free(rt->safety);
        free(rt);
        return NULL;
    }
    agent_set_safety(rt->agent, rt->safety);

    registry_set_delegate_config(cfg->provider, cfg->base_url, cfg->api_token, cfg->model,
                                  cfg->num_ctx, cfg->keep_alive_secs,
                                  cfg->temperature, cfg->timeout, cfg->max_iterations);

    /* Inner strings alias Conf strings which outlive every mode's run */
    rt->cfg_copy = *cfg;
    free(cfg);
    return rt;
}

static void teardown_runtime(RuntimeCtx *rt)
{
    if (!rt) return;
    agent_destroy(rt->agent);
    openai_oauth_destroy(rt->openai_auth);
    registry_destroy();
    safety_config_free(rt->safety);
    free(rt);
}

static void run_chat(Conf *conf)
{
    RuntimeCtx *rt = setup_runtime(conf);
    if (!rt) return;

    Agent *agent = rt->agent;
    OpenAIOAuth *openai_auth = rt->openai_auth;

    SessionManager *sm = NULL;
    if (strcmp(rt->cfg_copy.provider, "openai") == 0)
    {
        sm = init_session_manager(conf);
        if (sm)
        {
            registry_set_session_manager(sm);
            agent_set_session_manager(agent, sm);
            if (openai_oauth_attach_session(openai_auth, sm) != 0)
                log_error("failed to load stored OpenAI credentials", NULL);
        }
    }

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
        if (strcmp(line, "/openai-login") == 0)
        {
            if (!sm)
                printf("OpenAI login requires agent.provider=openai and encrypted storage.\n\n");
            else
                (void)run_openai_device_login(openai_auth);
            continue;
        }
        if (strcmp(line, "/openai-logout") == 0)
        {
            printf(openai_oauth_logout(openai_auth) == 0 ?
                       "OpenAI signed out.\n\n" : "OpenAI sign-out failed.\n\n");
            continue;
        }
        if (line[0] == '\0') continue;

        printf("\n");
        LLMResponse *resp = agent_run_new(agent, line);

        if (resp && resp->content)
            printf("%s\n\n", resp->content);
        else if (resp)
            printf("[no text response]\n\n");
        else
            printf("[error getting response]\n\n");

        llm_response_free(resp);
    }

    free(line);
    if (sm) registry_set_session_manager(NULL);
    session_manager_free(sm);
    teardown_runtime(rt);
}


static int run_openai_device_login(OpenAIOAuth *auth)
{
    char *verification_url = NULL;
    char *user_code = NULL;
    char *login_id = NULL;
    unsigned int interval = 0;
    if (openai_oauth_device_start(auth, &verification_url, &user_code,
                                  &login_id, &interval) != 0)
    {
        printf("OpenAI device login could not be started.\n\n");
        return -1;
    }
    printf("Open %s and enter code: %s\n", verification_url, user_code);
    printf("Waiting for OpenAI authorization...\n");
    free(verification_url);
    free(user_code);
    OpenAIOAuthDeviceResult result = OPENAI_OAUTH_DEVICE_PENDING;
    while (result == OPENAI_OAUTH_DEVICE_PENDING ||
           result == OPENAI_OAUTH_DEVICE_TRANSIENT)
    {
        sleep(interval > 0 ? interval : 1U);
        result = openai_oauth_device_poll(auth, login_id);
    }
    free(login_id);
    if (result == OPENAI_OAUTH_DEVICE_COMPLETE)
    {
        printf("OpenAI sign-in complete.\n\n");
        return 0;
    }
    printf("OpenAI device login failed or expired.\n\n");
    return -1;
}

typedef struct {
    Conf *conf;
    OpenAIOAuth *oauth;
    SafetyConfig *safety;
    SessionManager *sm;
} TuiFactoryCtx;

static Agent *tui_agent_factory(void *userdata)
{
    TuiFactoryCtx *fc = userdata;
    AgentConfig *cfg = load_agent_config(fc->conf);
    if (cfg) cfg->openai_auth = fc->oauth;
    if (!cfg)
    {
        log_error("failed to load agent config", NULL);
        return NULL;
    }
    Agent *agent = agent_create(cfg);
    free(cfg);
    if (!agent)
    {
        log_error("failed to create agent", NULL);
        return NULL;
    }
    agent_set_safety(agent, fc->safety);
    if (fc->sm) agent_set_session_manager(agent, fc->sm);
    return agent;
}

static void run_tui(Conf *conf)
{
    RuntimeCtx *rt = setup_runtime(conf);
    if (!rt) return;

    const char *style = conf_get(conf, "tui.style");
    const char *density = conf_get(conf, "tui.density");
    const char *accent = conf_get(conf, "tui.accent");
    const char *transparent = conf_get(conf, "tui.transparent");

    TuiEvents *evs = tui_events_init(1024);
    TuiEvents *jobs = tui_events_init(64);
    if (!evs || !jobs)
    {
        log_error("tui: event rings unavailable", NULL);
        tui_events_destroy(evs);
        tui_events_destroy(jobs);
        teardown_runtime(rt);
        return;
    }

    char *data_dir = NULL;
    const char *home = getenv("HOME");
    if (home && asprintf(&data_dir, "%s/.config/echo-ai", home) >= 0)
        (void)mkdir(data_dir, 0700);

    TuiFactoryCtx fctx = {.conf = conf, .oauth = rt->openai_auth,
                          .safety = rt->safety, .sm = NULL};
    TuiAppCtx actx;
    memset(&actx, 0, sizeof(actx));
    actx.agent = rt->agent;
    actx.evs = evs;
    actx.jobs = jobs;
    actx.oauth = rt->openai_auth;
    actx.safety = rt->safety;
    actx.conf = conf;
    actx.ct = ct_create();
    /* Web mode does this too: without it the write_file tool never
     * snapshots, so /undo and /redo would always report "Nothing to
     * undo" in the TUI. */
    registry_set_change_tracker(actx.ct);
    actx.agent_factory = tui_agent_factory;
    actx.agent_factory_userdata = &fctx;
    actx.model = rt->agent->model;
    actx.provider = rt->cfg_copy.provider;
    actx.session_id = rt->agent->session_id;
    actx.tool_count = registry_enabled_count();
    actx.style = style;
    actx.density = density;
    actx.accent = accent;
    actx.transparent = transparent && strcmp(transparent, "true") == 0;

    TuiApp *app = tui_app_create(&actx);
    if (!app)
    {
        log_error("tui: app init failed", NULL);
        ct_destroy(actx.ct);
        tui_events_destroy(evs);
        tui_events_destroy(jobs);
        teardown_runtime(rt);
        free(data_dir);
        return;
    }

    SessionManager *sm = NULL;
    int password_cancelled = 0;
    if (data_dir)
    {
        const char *enabled = conf_get(conf, "session.enabled");
        int session_enabled = !enabled || strcmp(enabled, "false") != 0;
        if (session_enabled)
        {
            /* Unlock loop: wrong passwords retry with visible feedback,
             * Esc/empty aborts startup cleanly. */
            while (sm == NULL && !password_cancelled)
            {
                char *password = encryption_resolve_password_alloc();
                if (!password)
                {
                    const char *prompt = encryption_first_run_detect(data_dir)
                        ? "Create a database password: "
                        : "Enter database password: ";
                    password = tui_app_prompt_password(app, prompt);
                }
                if (!password)
                {
                    password_cancelled = 1;
                    break;
                }
                SessionManagerCreateResult result = SESSION_MANAGER_CREATE_OK;
                sm = session_manager_create_ex(data_dir, password, &result);
                size_t pw_len = strlen(password);
                memset(password, 0, pw_len);
                free(password);
                if (sm) break;
                if (result == SESSION_MANAGER_CREATE_AUTH_FAILED)
                {
                    (void)tui_app_notice(app, "Wrong password",
                                         "The database password did not match. Press any key to try again.");
                    continue;
                }
                log_error("failed to create session manager", NULL);
                (void)tui_app_notice(app, "Unlock failed",
                                     "Could not open the session store. Press any key to exit.");
                password_cancelled = 1;
            }
            if (sm)
            {
                fctx.sm = sm;
                if (openai_oauth_attach_session(rt->openai_auth, sm) != 0)
                    log_error("failed to load stored OpenAI credentials", NULL);
                log_info("session manager ready", NULL);
            }
        }
    }
    if (password_cancelled)
    {
        tui_app_destroy(app);
        ct_destroy(actx.ct);
        tui_events_destroy(evs);
        tui_events_destroy(jobs);
        teardown_runtime(rt);
        free(data_dir);
        return;
    }

    int tui_rc = tui_app_run(app, sm);
    if (tui_rc == 0)
    {
        /* The worker took ownership of the agent (tui_worker_destroy
         * frees it, and /new replaces it mid-run), so the runtime must
         * not destroy it again. On a run failure (worker never started)
         * the runtime still owns it. */
        rt->agent = NULL;
    }

    tui_app_destroy(app);
    /* The terminal is restored by now: bid farewell on a clean quit so
     * the shell sees it after the app closes. */
    if (tui_rc == 0)
        printf("Goodbye from Echo AI!\n");
    if (sm) session_manager_free(sm);
    ct_destroy(actx.ct);
    tui_events_destroy(evs);
    tui_events_destroy(jobs);
    teardown_runtime(rt);
    free(data_dir);
}
static void run_web(Conf *conf, const char *config_path)
{
    RuntimeCtx *rt = setup_runtime(conf);
    if (!rt) return;

    Agent *agent = rt->agent;
    OpenAIOAuth *openai_auth = rt->openai_auth;

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
    ctx.agent_cfg = rt->cfg_copy;
    ctx.config_path = str_dup(config_path);
    ctx.agent = agent;
    ctx.openai_oauth = openai_auth;
    ctx.sm = NULL;
    ctx.safety = rt->safety;
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
    if (ctx.sm) session_manager_free(ctx.sm);
    teardown_runtime(rt);
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

    if (log_init() != 0)
    {
        fprintf(stderr, "fatal: logging subsystem unavailable (stderr not writable)\n");
        return 1;
    }
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
        /* TUI mode owns the terminal; piped input is the --chat REPL's
         * job (AGENTS.md decision 7: no silent fallback). */
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        {
            log_error("--cli requires an interactive terminal; use --chat for piped input", NULL);
            conf_free(conf);
            log_cleanup();
            return 1;
        }
        run_tui(conf);
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
