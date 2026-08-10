import { useState, useEffect, useCallback, useMemo, useRef, type ReactNode } from 'react';
import {
  ChatContext,
  EFFORT_OPTIONS_BY_PROVIDER,
  type ChatContextValue,
  type ConnectionStatus,
} from './ChatContext';
import { api } from '../api/client';
import type { ApprovalRequest, BranchInfo, StreamEvent, ToolCall } from '../types';

const DEBUG = import.meta.env.DEV;

function debugLog(action: string, data?: unknown) {
  if (DEBUG) {
    console.log(`[Chat:${action}]`, data ?? '');
  }
}

type ChatPreferences = {
  model?: string;
  provider?: string;
  models?: Record<string, string>;
  effort?: string;
};

const CHAT_PREFERENCES_KEY = 'echo-ai-chat-preferences';
const FALLBACK_PROVIDERS = ['ollama', 'openai', 'openai_compatible', 'opencode_zen'];

function readChatPreferences(): ChatPreferences {
  try {
    const raw = localStorage.getItem(CHAT_PREFERENCES_KEY);
    if (!raw) return {};
    const value = JSON.parse(raw) as ChatPreferences;
    return value && typeof value === 'object' ? value : {};
  } catch {
    return {};
  }
}

function writeChatPreferences(preferences: ChatPreferences): void {
  try {
    localStorage.setItem(CHAT_PREFERENCES_KEY, JSON.stringify(preferences));
  } catch {
    return;
  }
}

function validateModels(models: string[]): string[] {
  return models.filter((model) => typeof model === 'string' && model.trim().length > 0);
}

function normalizeToolCallResult(tc: ToolCall): ToolCall {
  /* Wire shape carries results as result_content/result_error; the app's
   * canonical shape is result.{content,error}. Normalize on ingestion so
   * reloaded messages render results the same way live ones do. */
  if (tc.result) return tc;
  if (tc.result_content === undefined && tc.result_error === undefined) return tc;
  return {
    ...tc,
    result: {
      content: tc.result_content || '',
      error: tc.result_error ?? null,
    },
  };
}

function combineAssistantMessages(
  messages: ChatContextValue['messages']
): ChatContextValue['messages'] {
  const combined: ChatContextValue['messages'] = [];

  for (const raw of messages) {
    /* Normalize the wire shape once per message so every branch below
     * (push as-is or merge into a previous assistant) sees canonical
     * tc.result. */
    const msg = raw.tool_calls
      ? { ...raw, tool_calls: raw.tool_calls.map(normalizeToolCallResult) }
      : raw;

    if (msg.role === 'system') continue;

    if (msg.role === 'tool') {
      const last = combined[combined.length - 1];
      if (last && last.role === 'assistant' && last.tool_calls && last.tool_calls.length > 0) {
        /* Match by tool_call_id first so concurrent/parallel tool calls
         * don't collapse onto the first unresolved call. */
        const target = msg.tool_call_id
          ? last.tool_calls.find((tc) => tc.tool_call_id === msg.tool_call_id && !tc.result)
          : last.tool_calls.find((tc) => !tc.result);
        if (target) target.result = { content: msg.content || '', error: null };
      }
      continue;
    }

    const last = combined[combined.length - 1];
    if (last && last.role === 'assistant' && msg.role === 'assistant') {
      last.content += (last.content && msg.content) ? '\n' + msg.content : (msg.content || '');
      if (msg.has_tools) last.has_tools = msg.has_tools;
      if (msg.tool_calls && msg.tool_calls.length > 0) {
        last.tool_calls = last.tool_calls
          ? [...last.tool_calls, ...msg.tool_calls]
          : msg.tool_calls;
      }
      if (msg.thinking) {
        last.thinking = last.thinking
          ? last.thinking + '\n' + msg.thinking
          : msg.thinking;
      }
    } else {
      combined.push(msg);
    }
  }

  return combined;
}

/* Wire format of branch_info frames and the REST branches array:
 * {message_id, count, active, branch_ids?}. */
type BranchEntry = {
  message_id: string;
  count: number;
  active: number;
  branch_ids?: string[];
};

function toBranchInfo(branches?: BranchEntry[]): BranchInfo[] {
  if (!branches) return [];
  return branches
    .filter((b) => b && typeof b.message_id === 'string' && b.message_id.length > 0)
    .map((b) => ({
      messageId: b.message_id,
      count: b.count,
      active: b.active,
      branchIds: Array.isArray(b.branch_ids) ? b.branch_ids : [],
    }));
}

/**
 * ChatProvider - owns the entire chat session lifecycle for the app.
 *
 * Renders its children inside a ChatContext whose value changes whenever
 * sessions, the active session, model/provider selection, streaming
 * messages, or approval/ask-user prompts change. Owns the WebSocket to
 * /ws/chat (created on mount or when model/provider/session change), the
 * SSE-equivalent message stream, and the initial data load.
 *
 * Effect ownership (cleanup contract):
 * - The initial-data effect aborts its AbortController and any in-flight
 *   model request on unmount.
 * - The websocket effect closes the socket AND removes the
 *   'visibilitychange' listener on unmount; the listener reconnects a
 *   dropped socket when the tab becomes visible again.
 * - modelRequestControllerRef/wsRef are nulled on unmount so late
 *   async callbacks no-op instead of touching dead state.
 */
export function ChatProvider({ children }: { children: ReactNode }) {
  const [sessions, setSessions] = useState<ChatContextValue['sessions']>([]);
  const [activeSessionId, setActiveSessionId] = useState<string | null>(null);
  const [currentModel, setCurrentModel] = useState<string>('');
  const [currentProvider, setCurrentProvider] = useState<string>('ollama');
  const [modelByProvider, setModelByProvider] = useState<Record<string, string>>({});
  const [models, setModels] = useState<string[]>([]);
  /* Provider list comes from the backend (/api/providers); the fallback
   * below only covers the window before the fetch resolves. */
  const [providers, setProviders] = useState<string[]>(FALLBACK_PROVIDERS);
  /* Providers whose backend accepts a reasoning-effort hint; the effort
   * selector in the chat input is gated on the current provider being in
   * this list. Empty until /api/providers resolves. */
  const [effortSupportedProviders, setEffortSupportedProviders] = useState<string[]>([]);
  /* Per-provider effort value lists from /api/providers; falls back to
   * EFFORT_OPTIONS_BY_PROVIDER until the fetch resolves. */
  const [effortOptionsByProvider, setEffortOptionsByProvider] = useState<Record<string, string[]>>(
    EFFORT_OPTIONS_BY_PROVIDER
  );
  /* '' means "provider default"; otherwise one of the current provider's
   * accepted effort values. */
  const [currentEffort, setCurrentEffort] = useState<string>('');
  const [messages, setMessages] = useState<ChatContextValue['messages']>([]);
  const [branchInfo, setBranchInfo] = useState<BranchInfo[]>([]);
  const [connectionStatus, setConnectionStatus] = useState<ConnectionStatus>('disconnected');
  const [isConnected, setIsConnected] = useState(false);
  const [isStreaming, setIsStreaming] = useState(false);
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const [pendingApproval, setPendingApproval] = useState<ApprovalRequest | null>(null);
  const [pendingQuestion, setPendingQuestion] = useState<string | null>(null);

  const wsRef = useRef<WebSocket | null>(null);
  const wsGenRef = useRef(0);
  const modelRequestGenerationRef = useRef(0);
  const modelRequestControllerRef = useRef<AbortController | null>(null);
  const isReadyRef = useRef(false);
  const messageQueueRef = useRef<string[]>([]);
  const connectRef = useRef<() => void>(() => {});
  const reconnectDelayRef = useRef(500);
  const activeSessionIdRef = useRef(activeSessionId);
  const staleSessionIdRef = useRef<string | null>(null);
  const MAX_RECONNECT_DELAY = 30_000;

  const connect = useCallback(() => {
    debugLog('connect', { model: currentModel, provider: currentProvider });

    if (!currentModel) {
      debugLog('connect', 'no model selected, deferring');
      return;
    }

    /* Guard against a null ws explicitly: `wsRef.current?.readyState ===
     * WebSocket.OPEN` would be `undefined === undefined` when OPEN is
     * unavailable (e.g. a test shim without statics), silently making
     * every connect a no-op. */
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      debugLog('connect', 'already connected');
      return;
    }

    try {
      // Close existing if closing
      if (wsRef.current) {
        wsRef.current.close();
      }

      setConnectionStatus('connecting');

      const gen = ++wsGenRef.current;
      const token = api.unlockTokenValue;
      const protocols = token ? ['echo-ai', `echo-ai-token-${token}`] : [];
      const ws = new WebSocket('/ws/chat', protocols);
      wsRef.current = ws;

      ws.onopen = () => {
        if (gen !== wsGenRef.current) return; // Stale handler
        debugLog('ws:open');
        reconnectDelayRef.current = 500;
        setIsConnected(true);
        setConnectionStatus('connected');
        // Send config to initialize
        const configMsg = JSON.stringify({
          provider: currentProvider,
          model: currentModel,
          // undefined drops the key; the backend then uses the config
          // default effort
          effort: currentEffort || undefined,
        });
        debugLog('ws:send', { type: 'config', provider: currentProvider, model: currentModel, effort: currentEffort });
        ws.send(configMsg);
      };

      ws.onmessage = (event) => {
        try {
          const data: StreamEvent = JSON.parse(event.data);
          debugLog('ws:message', { type: data.type, hasContent: !!data.content });

          switch (data.type) {
            case 'ready':
              debugLog('ready', data);
              isReadyRef.current = true;
              if (data.session_id) {
                setActiveSessionId(data.session_id);
                debugLog('session:set', data.session_id);
              }
              // Process queued messages
              while (messageQueueRef.current.length > 0) {
                const msg = messageQueueRef.current.shift();
                if (msg) {
                  debugLog('ws:send:queued', msg.substring(0, 50));
                  ws.send(msg);
                }
              }
              break;

            case 'session_start':
              // AI has started responding - refresh session list
              if (data.session_id) {
                setActiveSessionId(data.session_id);
                api.getSessions().then(setSessions).catch(console.error);
              }
              break;

            case 'approval_request':
              debugLog('approval_request', {
                tool_name: data.tool_name,
                request_id: data.request_id,
              });
              setIsStreaming(false);
              if (data.request_id && data.tool_name) {
                setPendingApproval({
                  type: 'approval_request',
                  request_id: data.request_id,
                  tool_name: data.tool_name,
                  arguments: data.arguments || '',
                });
              }
              break;

            case 'ask_user':
              /* The agent is blocked waiting for a freeform text answer;
               * pause streaming and surface the question. The backend
               * gives up after `ask_user_timeout` (default 60s) if we
               * never reply, so this can't hang forever. */
              debugLog('ask_user', { question: data.question });
              setIsStreaming(false);
              setPendingQuestion(data.question ?? '');
              break;

            case 'title_updated':
              debugLog('title_updated', data);
              if (data.session_id && data.title) {
                setSessions((prev) =>
                  prev.map((s) => (s.id === data.session_id ? { ...s, title: data.title! } : s))
                );
              }
              break;

            case 'tool_start':
              debugLog('tool_start', { name: data.tool_name });
              setMessages((prev) => {
                const last = prev[prev.length - 1];
                const newTc = {
                  name: data.tool_name || 'unknown',
                  arguments: data.arguments || '{}',
                };
                if (last?.role === 'assistant') {
                  return [
                    ...prev.slice(0, -1),
                    {
                      ...last,
                      has_tools: true,
                      tool_calls: last.tool_calls
                        ? [...last.tool_calls, newTc]
                        : [newTc],
                    },
                  ];
                }
                return [
                  ...prev,
                  {
                    role: 'assistant' as const,
                    content: '',
                    has_tools: true,
                    tool_calls: [newTc],
                  },
                ];
              });
              break;

            case 'tool_end':
              debugLog('tool_end', { name: data.tool_name });
              setMessages((prev) => {
                const last = prev[prev.length - 1];
                if (last?.role !== 'assistant' || !last.tool_calls) return prev;
                return [
                  ...prev.slice(0, -1),
                  {
                    ...last,
                    tool_calls: last.tool_calls.map((tc) =>
                      tc.name === data.tool_name && !tc.result
                        ? {
                            ...tc,
                            result: {
                              content: data.result_content || '',
                              error: data.result_error || null,
                            },
                          }
                        : tc
                    ),
                  },
                ];
              });
              break;

            case 'content':
              if (data.session_id) {
                if (activeSessionIdRef.current === null) {
                  // New Chat is active — reject in-flight events from the
                  // session we just left, accept anything else (the new session).
                  if (data.session_id === staleSessionIdRef.current) break;
                } else if (data.session_id !== activeSessionIdRef.current) {
                  break;
                }
              }
              debugLog('message:content', {
                content: data.content?.substring(0, 50),
                len: data.content?.length,
              });
              setIsStreaming(true);
              setMessages((prev) => {
                const last = prev[prev.length - 1];
                // If last message is from assistant (created by 'thinking'), update it
                if (last?.role === 'assistant') {
                  return [...prev.slice(0, -1), { ...last, content: last.content + (data.content || '') }];
                }
                return [
                  ...prev,
                  { role: 'assistant', content: data.content || '', has_tools: false },
                ];
              });
              break;

            case 'done':
              debugLog('done', {
                session_id: data.session_id,
                title: data.title,
                content: data.content?.substring(0, 30),
                has_tools: data.has_tools,
                tool_calls: data.tool_calls,
              });
              // Ignore done events for sessions that are no longer active.
              // This prevents stale responses from a previous chat overwriting
              // the cleared state after the user clicked New Chat.
              if (data.session_id) {
                if (activeSessionIdRef.current === null) {
                  if (data.session_id === staleSessionIdRef.current) {
                    debugLog('done:ignored:stale', {
                      event: data.session_id,
                      stale: staleSessionIdRef.current,
                    });
                    setIsStreaming(false);
                    break;
                  }
                } else if (data.session_id !== activeSessionIdRef.current) {
                  debugLog('done:ignored', {
                    event: data.session_id,
                    current: activeSessionIdRef.current,
                  });
                  setIsStreaming(false);
                  break;
                }
              }
              setIsStreaming(false);
              isReadyRef.current = true;
              {
                /* After an edit/regenerate fork the fork point message got a
                 * fresh identity; adopt it so the branch pill (keyed by
                 * message id) attaches to the right message. fork_role tells
                 * us which message that is: the edited user message (edit),
                 * or the regenerated assistant response (regenerate). */
                const forkMsgId = data.fork_message_id;
                const forkGroupId = data.fork_group_id;
                const isUserFork = !!(forkMsgId && data.fork_role === 'user');
                setMessages((prev) => {
                  const lastAssistantIdx = prev.findLastIndex((m) => m.role === 'assistant');
                  /* For an edit the fork point is the edited user message
                   * (the last user message of the rebuilt chain); anything
                   * else — regenerate and plain chat completions — lands on
                   * the last assistant message. */
                  const targetIdx = isUserFork
                    ? prev.findLastIndex((m) => m.role === 'user')
                    : lastAssistantIdx;
                  if (targetIdx >= 0) {
                    const last = prev[targetIdx];

                    if (isUserFork) {
                      /* The edited message's content was already applied
                       * locally at submit; the done content is the assistant
                       * reply and must NOT overwrite the user message. Only
                       * the fork identity is adopted — and the tail (the
                       * freshly streamed assistant reply) is preserved. */
                      return [
                        ...prev.slice(0, targetIdx),
                        {
                          ...last,
                          id: forkMsgId,
                          fork_group_id: forkGroupId,
                        },
                        ...prev.slice(targetIdx + 1),
                      ];
                    }

                    const mergedToolCalls = (last.tool_calls || []).map((existing) => {
                      if (existing.result) return existing;
                      const doneMatch = data.tool_calls?.find(
                        (dtc) => dtc.name === existing.name
                      );
                      if (!doneMatch) return existing;
                      return normalizeToolCallResult({
                        ...existing,
                        result_content: doneMatch.result_content,
                        result_error: doneMatch.result_error,
                      });
                    });

                    /* Only use data.tool_calls to enrich existing tool_calls
                     * (mergedToolCalls path above). Never add tool_calls
                     * to a message that didn't already have them — the
                     * backend may attach stale tool_calls from previous
                     * turns onto a text-only response. */
                    const finalToolCalls =
                      mergedToolCalls.length > 0
                        ? mergedToolCalls
                        : last.tool_calls || [];

                    return [
                      ...prev.slice(0, targetIdx),
                      {
                        ...last,
                        content: data.content || last.content,
                        has_tools: data.has_tools ?? (finalToolCalls.length > 0),
                        tool_calls: finalToolCalls,
                        timestamp: data.timestamp || last.timestamp,
                        id: forkMsgId || last.id,
                        fork_group_id: forkGroupId || last.fork_group_id,
                      },
                    ];
                  }
                  return [
                    ...prev,
                    {
                      role: 'assistant',
                      content: data.content || '',
                      has_tools: data.has_tools,
                      tool_calls: data.tool_calls,
                      timestamp: data.timestamp,
                      id: forkMsgId,
                      fork_group_id: forkGroupId,
                    },
                  ];
                });
              }
              if (data.session_id) {
                setActiveSessionId(data.session_id);
              }
              // Always refresh session list after chat completes
              api.getSessions().then(setSessions).catch(console.error);
              if (data.title) {
                debugLog('title:generated', data.title);
              }
              break;

            case 'history':
              debugLog('history', { messages: data.messages?.length });
              if (data.messages && data.messages.length > 0) {
                const combined = combineAssistantMessages(data.messages);
                setMessages(combined);
              }
              break;

            case 'branch_info':
              debugLog('branch_info', { branches: data.branches?.length });
              setBranchInfo(toBranchInfo(data.branches));
              break;

            case 'error':
              debugLog('error', data.content);
              setIsStreaming(false);
              isReadyRef.current = true;
              setMessages((prev) => {
                const lastUserIdx = prev.findLastIndex((m) => m.role === 'user');
                if (lastUserIdx < 0) return prev;
                const lastUser = prev[lastUserIdx];
                return [
                  ...prev.slice(0, lastUserIdx),
                  { ...lastUser, error: data.content || 'An error occurred' },
                  ...prev.slice(lastUserIdx + 1),
                ];
              });
              break;
          }
        } catch (err) {
          console.error('[Chat] Failed to parse message:', err);
        }
      };

      ws.onclose = (e) => {
        if (gen !== wsGenRef.current) return; // Stale handler
        debugLog('ws:close', { code: e.code, reason: e.reason });
        setIsConnected(false);
        isReadyRef.current = false;

        // Auto-reconnect unless cleanly closed
        if (e.code !== 1000) {
          setConnectionStatus('reconnecting');
          const delay = reconnectDelayRef.current;
          reconnectDelayRef.current = Math.min(
            Math.round(delay * (1.5 + Math.random())), // 1.5-2.5x jitter
            MAX_RECONNECT_DELAY
          );
          debugLog('ws:reconnect:scheduled', { delay });
          setTimeout(() => {
            if (
              gen === wsGenRef.current &&
              (!wsRef.current || wsRef.current.readyState === WebSocket.CLOSED)
            ) {
              connectRef.current();
            }
          }, delay);
        } else {
          reconnectDelayRef.current = 500;
          setConnectionStatus('disconnected');
        }
      };

      ws.onerror = () => {
        if (gen !== wsGenRef.current) return; // Stale handler
        // ws.onclose will handle reconnection; no need to schedule here
      };
    } catch (err) {
      console.error('[Chat] Failed to connect:', err);
    }
  }, [currentModel, currentProvider, currentEffort]);

  useEffect(() => {
    connectRef.current = connect;
  }, [connect]);

  // Keep a ref in sync with activeSessionId so WebSocket message handlers
  // (which live inside the connect closure) can detect stale events.
  useEffect(() => {
    activeSessionIdRef.current = activeSessionId;
  }, [activeSessionId]);

  const sendMessage = useCallback(
    (content: string) => {
      const preview = content.substring(0, 30);
      debugLog('sendMessage:start', preview);

      // Add user message immediately to UI
      const timestamp = new Date().toLocaleTimeString('en-US', {
        hour: '2-digit',
        minute: '2-digit',
        hour12: false,
      });
      setMessages((prev) => [...prev, { role: 'user', content, timestamp }]);

      const ws = wsRef.current;
      const payload = activeSessionId
        ? { type: 'message', content, session_id: activeSessionId }
        : { type: 'message', content };
      const payloadStr = JSON.stringify(payload);

      if (ws && ws.readyState === WebSocket.OPEN && isReadyRef.current) {
        debugLog('ws:send:message', content.substring(0, 30));
        ws.send(payloadStr);
        setIsStreaming(true);
        isReadyRef.current = false;
      } else {
        debugLog('ws:queue-or-reconnect', {
          readyState: ws?.readyState,
          isReady: isReadyRef.current,
        });
        messageQueueRef.current.push(payloadStr);
        if (!ws || ws.readyState === WebSocket.CLOSED) {
          debugLog('ws:reconnect-needed');
          connect();
        }
      }
    },
    [connect, activeSessionId]
  );

  const reconnect = useCallback(() => {
    debugLog('reconnect');
    messageQueueRef.current = [];
    isReadyRef.current = false;
    wsRef.current?.close();
    connect();
    // Refetch session state after reconnection
    if (activeSessionIdRef.current) {
      api
        .loadSession(activeSessionIdRef.current)
        .then((data) => {
          setMessages(combineAssistantMessages(data.messages));
          setBranchInfo(toBranchInfo(data.branches));
        })
        .catch(console.error);
    }
  }, [connect]);

  const stopGeneration = useCallback(() => {
    debugLog('stopGeneration');
    const ws = wsRef.current;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'stop' }));
    }
  }, []);

  const resolveApproval = useCallback((requestId: string, approved: boolean) => {
    const ws = wsRef.current;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'approval_response', request_id: requestId, approved }));
    }
    setPendingApproval(null);
  }, []);

  const resolveAskUser = useCallback((answer: string) => {
    const ws = wsRef.current;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'ask_user_response', answer }));
    }
    setPendingQuestion(null);
  }, []);

  const editMessage = useCallback(
    (index: number, newText: string, msgId?: string) => {
      setMessages((prev) => {
        if (index < 0 || index >= prev.length) return prev;
        /* Truncate all messages after the edited index so stale
         * assistant responses (and their tool_calls) don't bleed
         * into the new streaming response. Matches the backend's
         * session_manager_truncate_history behaviour. */
        return prev.slice(0, index + 1).map((m, i) =>
          (i === index ? { ...m, content: newText, error: undefined } : m)
        );
      });
      setIsStreaming(true);

      // Send edit over WebSocket
      const ws = wsRef.current;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(
          JSON.stringify({
            type: 'edit',
            index: index,
            content: newText,
            session_id: activeSessionId,
            message_id: msgId || undefined,
          })
        );
      }
    },
    [activeSessionId]
  );

  // Load initial data
  useEffect(() => {
    debugLog('mount');
    const controller = new AbortController();
    const initialProviderGeneration = modelRequestGenerationRef.current;
    const isMounted = () => !controller.signal.aborted;
    const providerSelectionUnchanged = () =>
      isMounted() && initialProviderGeneration === modelRequestGenerationRef.current;

    const loadData = async () => {
      try {
        debugLog('loadData:start');
        const [providersData, sessionsData] = await Promise.all([
          api.getProviders(),
          api.getSessions(),
        ]);
        const storedPrefs = readChatPreferences();
        const savedModels = {
          ...(storedPrefs.models || {}),
        };
        debugLog('loadData:sessions', sessionsData.length);
        const availableProviders = providersData.providers.length > 0
          ? providersData.providers
          : FALLBACK_PROVIDERS;
        if (!isMounted()) return;
        setProviders(availableProviders);
        setEffortSupportedProviders(providersData.effortSupported);
        if (providersData.effortOptions && Object.keys(providersData.effortOptions).length > 0) {
          setEffortOptionsByProvider(providersData.effortOptions);
        }
        setSessions(sessionsData);
        if (!providerSelectionUnchanged()) return;

        // Validate saved provider; fall back to first available
        let savedProvider =
          storedPrefs.provider &&
          availableProviders.includes(storedPrefs.provider || '')
            ? storedPrefs.provider || availableProviders[0]
            : availableProviders[0];
        if (savedProvider === 'openai') {
          try {
            const status = await api.getOpenAIOAuthStatus(undefined, controller.signal);
            if (!providerSelectionUnchanged()) return;
            if (status.state !== 'signed_in') {
              savedProvider = availableProviders.find((provider) => provider !== 'openai') || '';
            }
          } catch (error) {
            if (!providerSelectionUnchanged()) return;
            debugLog('loadData:openai-auth-unavailable', error);
            savedProvider = availableProviders.find((provider) => provider !== 'openai') || '';
          }
        }

        if (!savedProvider) {
          setModels([]);
          setCurrentModel('');
          return;
        }

        const legacyModel = storedPrefs.model;
        if (legacyModel && !savedModels[savedProvider]) {
          savedModels[savedProvider] = legacyModel;
        }

        const modelsData = validateModels(await api.getModels(savedProvider, controller.signal));
        if (!providerSelectionUnchanged()) return;
        if (modelsData.length === 0) {
          setModels([]);
          setCurrentModel('');
          return;
        }

        setCurrentProvider(savedProvider);
        setModels(modelsData);

        const savedModel =
          savedModels[savedProvider] && modelsData.includes(savedModels[savedProvider])
            ? savedModels[savedProvider]
            : modelsData[0] || '';
        setCurrentModel(savedModel);
        savedModels[savedProvider] = savedModel;
        setModelByProvider(savedModels);

        /* Restore the saved effort hint; anything outside the current
         * provider's accepted set is dropped so a stale localStorage value
         * can never reach the wire. */
        const savedEffort =
          storedPrefs.effort &&
          (providersData.effortOptions?.[savedProvider] ||
            EFFORT_OPTIONS_BY_PROVIDER[savedProvider] ||
            []).includes(storedPrefs.effort)
            ? storedPrefs.effort
            : '';
        setCurrentEffort(savedEffort);

        const resolvedPrefs: ChatPreferences = {
          provider: savedProvider,
          model: savedModel,
          models: savedModels,
          effort: savedEffort || undefined,
        };
        writeChatPreferences(resolvedPrefs);
      } catch (err) {
        if (!providerSelectionUnchanged()) return;
        console.error('[Chat] Failed to load data:', err);
      }
    };

    void loadData();
    return () => {
      controller.abort();
      modelRequestControllerRef.current?.abort();
    };
  }, []);

  useEffect(() => {
    connect();

    const onVisibilityChange = () => {
      if (document.visibilityState === 'visible') {
        const ws = wsRef.current;
        if (!ws || ws.readyState !== WebSocket.OPEN) {
          debugLog('visibilitychange:reconnect');
          reconnect();
        }
      }
    };
    document.addEventListener('visibilitychange', onVisibilityChange);

    return () => {
      debugLog('unmount');
      document.removeEventListener('visibilitychange', onVisibilityChange);
      wsRef.current?.close();
    };
  }, [connect, reconnect]);

  const createSession = useCallback(() => {
    debugLog('createSession:start');
    // Stop any active generation first — stale streaming events from the old
    // session would otherwise overwrite the cleared state and switch the active
    // session_id back to the old conversation after New Chat is clicked.
    stopGeneration();
    // Remember the session we're leaving so in-flight stale events from it
    // are rejected by the content / done handlers even while active is null.
    staleSessionIdRef.current = activeSessionIdRef.current;
    activeSessionIdRef.current = null;
    setActiveSessionId(null);
    setMessages([]);
    setBranchInfo([]);
    messageQueueRef.current = [];
    // Session is created lazily on the backend when the first message is sent.
  }, [stopGeneration]);

  const selectSession = useCallback(async (sessionId: string) => {
    debugLog('selectSession:start', sessionId);
    activeSessionIdRef.current = sessionId;
    try {
      const data = await api.loadSession(sessionId);
      debugLog('selectSession:messages', data.messages.length);

      // Combine consecutive assistant messages into one
      const combinedMessages = combineAssistantMessages(data.messages);

      setActiveSessionId(sessionId);
      setMessages(combinedMessages);
      setBranchInfo(toBranchInfo(data.branches));
    } catch (err) {
      console.error('[Chat] Failed to load session:', err);
    }
  }, []);

  const deleteSession = useCallback(
    async (sessionId: string) => {
      debugLog('deleteSession:start', sessionId);
      try {
        await api.deleteSession(sessionId);
        if (activeSessionId === sessionId) {
          setActiveSessionId(null);
          setMessages([]);
          setBranchInfo([]);
        }
        const sessionsData = await api.getSessions();
        setSessions(sessionsData);
      } catch (err) {
        console.error('[Chat] Failed to delete session:', err);
      }
    },
    [activeSessionId]
  );

  const renameSession = useCallback(async (sessionId: string, newTitle: string) => {
    debugLog('renameSession:start', { sessionId, newTitle });
    try {
      await api.renameSession(sessionId, newTitle);
      setSessions((prev) => prev.map((s) => (s.id === sessionId ? { ...s, title: newTitle } : s)));
    } catch (err) {
      console.error('[Chat] Failed to rename session:', err);
    }
  }, []);

  const selectModel = useCallback(
    (model: string) => {
      debugLog('selectModel', model);
      setCurrentModel(model);
      const models = { ...modelByProvider, [currentProvider]: model };
      setModelByProvider(models);
      const preferences = { model, provider: currentProvider, models };
      writeChatPreferences(preferences);
      // The useEffect on connect() will detect the model change and reconnect
    },
    [currentProvider, modelByProvider]
  );

  const selectEffort = useCallback(
    (effort: string) => {
      debugLog('selectEffort', effort);
      setCurrentEffort(effort);
      /* Merge into whatever prefs already exist so the effort change never
       * clobbers provider/model state that was written elsewhere. */
      const preferences = { ...readChatPreferences(), effort: effort || undefined };
      writeChatPreferences(preferences);
      /* Resend the config over the live socket so the change applies
       * immediately; the backend rebuilds the provider when the effort
       * differs. Fresh connections pick it up from the initial config
       * message instead. */
      const ws = wsRef.current;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(
          JSON.stringify({
            provider: currentProvider,
            model: currentModel,
            effort: effort || undefined,
          })
        );
      }
    },
    [currentProvider, currentModel]
  );

  const selectProvider = useCallback(
    async (provider: string, prefetchedModels?: string[]) => {
      debugLog('selectProvider', provider);
      modelRequestControllerRef.current?.abort();
      const controller = new AbortController();
      const generation = ++modelRequestGenerationRef.current;
      modelRequestControllerRef.current = controller;
      const preferredModel = modelByProvider[provider] || '';

      try {
        const nextModels = validateModels(
          prefetchedModels ?? (await api.getModels(provider, controller.signal))
        );
        if (controller.signal.aborted || generation !== modelRequestGenerationRef.current) return;
        if (nextModels.length === 0) {
          throw new Error(`No models are available for ${provider}.`);
        }

        setModels(nextModels);
        const model =
          preferredModel && nextModels.includes(preferredModel)
            ? preferredModel
            : nextModels[0] || '';
        setCurrentProvider(provider);
        setCurrentModel(model);
        /* A provider switch can invalidate the current effort (e.g. xhigh
         * is openai-only); drop it so an unsupported value never reaches
         * the wire. */
        if (
          currentEffort &&
          !(effortOptionsByProvider[provider] || []).includes(currentEffort)
        ) {
          setCurrentEffort('');
        }
        const models = { ...modelByProvider, [provider]: model };
        setModelByProvider(models);
        const preferences = { model, provider, models };
        writeChatPreferences(preferences);
      } catch (error) {
        if (controller.signal.aborted || generation !== modelRequestGenerationRef.current) return;
        throw error;
      } finally {
        if (generation === modelRequestGenerationRef.current) {
          modelRequestControllerRef.current = null;
        }
      }
      // The useEffect on connect() will detect the provider change and reconnect
    },
    [modelByProvider, currentEffort, effortOptionsByProvider]
  );

  /* Regenerate the assistant message at `index`: the backend forks AT that
   * message (the fresh response becomes the new chain's fork point, pill
   * included) and streams a new answer. The local tail from the message
   * onwards is dropped so stale content doesn't bleed into the new
   * stream — dropping the old message itself is what lets the content
   * frames stream into a fresh assistant message. */
  const regenerateMessage = useCallback(
    (index: number, msgId?: string) => {
      setMessages((prev) => {
        if (index < 0 || index >= prev.length) return prev;
        return prev.slice(0, index);
      });
      setIsStreaming(true);

      const ws = wsRef.current;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(
          JSON.stringify({
            type: 'regenerate',
            index: index,
            session_id: activeSessionId,
            message_id: msgId || undefined,
          })
        );
      }
    },
    [activeSessionId]
  );

  /* Move the active chain one step in `direction` at the fork point of the
   * message identified by `messageId`. The target position's branch_id is
   * resolved from the branchInfo entry (branch_ids omits the live chain,
   * which has no record). */
  const switchBranch = useCallback(
    (messageId: string, direction: -1 | 1) => {
      const entry = branchInfo.find((b) => b.messageId === messageId);
      if (!entry || entry.count <= 1) return;
      const target = entry.active + direction;
      if (target < 1 || target > entry.count || target === entry.active) return;
      /* branch_ids[k] is the chain at position (k < active ? k+1 : k+2). */
      const branchIndex = target < entry.active ? target - 1 : target - 2;
      const branchId = entry.branchIds[branchIndex];
      if (!branchId) return;

      const ws = wsRef.current;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(
          JSON.stringify({
            type: 'branch_switch',
            session_id: activeSessionId,
            branch_id: branchId,
          })
        );
      }
    },
    [branchInfo, activeSessionId]
  );

  const supportsEffort = effortSupportedProviders.includes(currentProvider);
  /* `|| []` must be memoized: a bare `|| []` here would mint a fresh
   * array each render when the provider isn't in the map, churning the
   * context value's effortOptions reference and re-rendering consumers. */
  const effortOptions = useMemo(
    () => effortOptionsByProvider[currentProvider] || [],
    [effortOptionsByProvider, currentProvider]
  );

  const value = useMemo<ChatContextValue>(
    () => ({
      sessions,
      activeSessionId,
      currentModel,
      currentProvider,
      models,
      providers,
      currentEffort,
      effortOptions,
      selectEffort,
      supportsEffort,
      messages,
      branchInfo,
      connectionStatus,
      isConnected,
      isStreaming,
      currentThinking: '',
      sidebarOpen,
      setSidebarOpen,
      sendMessage,
      stopGeneration,
      editMessage,
      regenerateMessage,
      switchBranch,
      createSession,
      selectSession,
      deleteSession,
      renameSession,
      selectModel,
      selectProvider,
      reconnect,
      pendingApproval,
      resolveApproval,
      pendingQuestion,
      resolveAskUser,
    }),
    [
      sessions,
      activeSessionId,
      currentModel,
      currentProvider,
      models,
      providers,
      currentEffort,
      effortOptions,
      selectEffort,
      supportsEffort,
      messages,
      branchInfo,
      connectionStatus,
      isConnected,
      isStreaming,
      sidebarOpen,
      sendMessage,
      stopGeneration,
      editMessage,
      regenerateMessage,
      switchBranch,
      createSession,
      selectSession,
      deleteSession,
      renameSession,
      selectModel,
      selectProvider,
      reconnect,
      pendingApproval,
      resolveApproval,
      pendingQuestion,
      resolveAskUser,
    ]
  );

  return <ChatContext.Provider value={value}>{children}</ChatContext.Provider>;
}
