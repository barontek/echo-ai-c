import { useState, useEffect, useCallback, useMemo, useRef, type ReactNode } from 'react';
import { ChatContext, type ChatContextValue, type ConnectionStatus } from './ChatContext';
import { api } from '../api/client';
import type { ApprovalRequest, StreamEvent } from '../types';

const DEBUG = import.meta.env.DEV;

function debugLog(action: string, data?: unknown) {
  if (DEBUG) {
    console.log(`[Chat:${action}]`, data ?? '');
  }
}

function combineAssistantMessages(
  messages: ChatContextValue['messages']
): ChatContextValue['messages'] {
  const combined: ChatContextValue['messages'] = [];

  for (const msg of messages) {
    const last = combined[combined.length - 1];
    // Combine consecutive assistant messages
    if (last && last.role === 'assistant' && msg.role === 'assistant') {
      last.content += '\n' + msg.content;
      if (msg.has_tools) last.has_tools = msg.has_tools;
      if (msg.tool_calls && msg.tool_calls.length > 0) last.tool_calls = msg.tool_calls;
    } else {
      combined.push(msg);
    }
  }

  return combined;
}

export function ChatProvider({ children }: { children: ReactNode }) {
  const [sessions, setSessions] = useState<ChatContextValue['sessions']>([]);
  const [activeSessionId, setActiveSessionId] = useState<string | null>(null);
  const [currentModel, setCurrentModel] = useState<string>('');
  const [currentProvider, setCurrentProvider] = useState<string>('ollama');
  const [models, setModels] = useState<string[]>([]);
  const [providers] = useState<string[]>(['ollama', 'openai', 'anthropic', 'lm_studio']);
  const [messages, setMessages] = useState<ChatContextValue['messages']>([]);
  const [connectionStatus, setConnectionStatus] = useState<ConnectionStatus>('disconnected');
  const [isConnected, setIsConnected] = useState(false);
  const [isStreaming, setIsStreaming] = useState(false);
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const [pendingApproval, setPendingApproval] = useState<ApprovalRequest | null>(null);

  const wsRef = useRef<WebSocket | null>(null);
  const wsGenRef = useRef(0);
  const isReadyRef = useRef(false);
  const messageQueueRef = useRef<string[]>([]);
  const connectRef = useRef<() => void>(() => {});
  const reconnectDelayRef = useRef(500);
  const activeSessionIdRef = useRef(activeSessionId);
  const MAX_RECONNECT_DELAY = 30_000;

  const connect = useCallback(() => {
    debugLog('connect', { model: currentModel, provider: currentProvider });

    if (!currentModel) {
      debugLog('connect', 'no model selected, deferring');
      return;
    }

    if (wsRef.current?.readyState === WebSocket.OPEN) {
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
      const ws = new WebSocket('/ws/chat');
      wsRef.current = ws;

      ws.onopen = () => {
        if (gen !== wsGenRef.current) return; // Stale handler
        debugLog('ws:open');
        reconnectDelayRef.current = 500;
        setIsConnected(true);
        setConnectionStatus('connected');
        // Send config to initialize
        const configMsg = JSON.stringify({ provider: currentProvider, model: currentModel });
        debugLog('ws:send', { type: 'config', provider: currentProvider, model: currentModel });
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

            case 'title_updated':
              debugLog('title_updated', data);
              if (data.session_id && data.title) {
                setSessions((prev) =>
                  prev.map((s) => (s.id === data.session_id ? { ...s, title: data.title! } : s))
                );
              }
              break;

            case 'content':
              if (data.session_id && data.session_id !== activeSessionIdRef.current) break;
              debugLog('message:content', {
                content: data.content?.substring(0, 50),
                len: data.content?.length,
              });
              setIsStreaming(true);
              setMessages((prev) => {
                const last = prev[prev.length - 1];
                // If last message is from assistant (created by 'thinking'), update it
                if (last?.role === 'assistant') {
                  return [...prev.slice(0, -1), { ...last, content: data.content || '' }];
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
              if (
                data.session_id &&
                activeSessionIdRef.current != null &&
                data.session_id !== activeSessionIdRef.current
              ) {
                debugLog('done:ignored', {
                  event: data.session_id,
                  current: activeSessionIdRef.current,
                });
                setIsStreaming(false);
                break;
              }
              setIsStreaming(false);
              isReadyRef.current = true;
              // Find last assistant message and update it (not user message)
              setMessages((prev) => {
                const lastAssistantIdx = prev.findLastIndex((m) => m.role === 'assistant');
                if (lastAssistantIdx >= 0) {
                  const last = prev[lastAssistantIdx];
                  return [
                    ...prev.slice(0, lastAssistantIdx),
                    {
                      ...last,
                      content: data.content || last.content,
                      has_tools: data.has_tools ?? last.has_tools,
                      tool_calls:
                        data.tool_calls && data.tool_calls.length > 0
                          ? data.tool_calls
                          : last.tool_calls,
                      timestamp: data.timestamp || last.timestamp,
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
                  },
                ];
              });
              if (data.session_id) {
                setActiveSessionId(data.session_id);
              }
              // Always refresh session list after chat completes
              api.getSessions().then(setSessions).catch(console.error);
              if (data.title) {
                debugLog('title:generated', data.title);
              }
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
  }, [currentModel, currentProvider]);

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

  const editMessage = useCallback(
    (index: number, newText: string, msgId?: string) => {
      setMessages((prev) => {
        if (index < 0 || index >= prev.length) return prev;
        return prev.map((m, i) => (i === index ? { ...m, content: newText, error: undefined } : m));
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

    const loadData = async () => {
      try {
        debugLog('loadData:start');
        const [sessionsData, modelsData, prefsData] = await Promise.all([
          api.getSessions(),
          api.getModels(),
          api.getPreferences(),
        ]);
        debugLog('loadData:sessions', sessionsData.length);
        debugLog('loadData:models', modelsData.length);
        debugLog('loadData:prefs', prefsData);
        setSessions(sessionsData);
        setModels(modelsData);

        // Validate saved provider; fall back to first available
        const savedProvider =
          prefsData.provider && providers.includes(prefsData.provider)
            ? prefsData.provider
            : providers[0];
        setCurrentProvider(savedProvider);

        // Validate saved model; fall back to first available model
        const savedModel =
          prefsData.model && modelsData.includes(prefsData.model)
            ? prefsData.model
            : modelsData[0] || '';
        setCurrentModel(savedModel);

        // Persist resolved values to overwrite any stale data (e.g. "new-model" from a test)
        if (savedProvider !== prefsData.provider || savedModel !== prefsData.model) {
          api.setPreferences({ model: savedModel, provider: savedProvider }).catch(console.error);
        }
      } catch (err) {
        console.error('[Chat] Failed to load data:', err);
      }
    };

    loadData();
  }, [providers]);

  useEffect(() => {
    // eslint-disable-next-line react-hooks/set-state-in-effect
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

  const createSession = useCallback(async () => {
    debugLog('createSession:start');
    // Stop any active generation first — stale streaming events from the old
    // session would otherwise overwrite the cleared state and switch the active
    // session_id back to the old conversation after New Chat is clicked.
    stopGeneration();
    try {
      const { session_id } = await api.createSession();
      debugLog('createSession:done', session_id);
      activeSessionIdRef.current = session_id;
      setActiveSessionId(session_id);
      setMessages([]);
      messageQueueRef.current = [];
      const sessionsData = await api.getSessions();
      setSessions(sessionsData);
    } catch (err) {
      console.error('[Chat] Failed to create session:', err);
    }
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
      api.setPreferences({ model, provider: currentProvider }).catch(console.error);
      // The useEffect on connect() will detect the model change and reconnect
    },
    [currentProvider]
  );

  const selectProvider = useCallback(
    (provider: string) => {
      debugLog('selectProvider', provider);
      setCurrentProvider(provider);
      api.setPreferences({ model: currentModel, provider }).catch(console.error);
      // Refetch models for the new provider
      api.getModels(provider).then(setModels).catch(console.error);
      // The useEffect on connect() will detect the provider change and reconnect
    },
    [currentModel]
  );

  const retryMessage = useCallback(
    (index: number) => {
      const msg = messages[index];
      if (msg && msg.role === 'user') {
        setMessages((prev) => prev.map((m, i) => (i === index ? { ...m, error: undefined } : m)));
        sendMessage(msg.content);
      }
    },
    [messages, sendMessage]
  );

  const value = useMemo<ChatContextValue>(
    () => ({
      sessions,
      activeSessionId,
      currentModel,
      currentProvider,
      models,
      providers,
      messages,
      connectionStatus,
      isConnected,
      isStreaming,
      currentThinking: '',
      sidebarOpen,
      setSidebarOpen,
      sendMessage,
      stopGeneration,
      editMessage,
      retryMessage,
      createSession,
      selectSession,
      deleteSession,
      renameSession,
      selectModel,
      selectProvider,
      reconnect,
      pendingApproval,
      resolveApproval,
    }),
    [
      sessions,
      activeSessionId,
      currentModel,
      currentProvider,
      models,
      providers,
      messages,
      connectionStatus,
      isConnected,
      isStreaming,
      sidebarOpen,
      sendMessage,
      stopGeneration,
      editMessage,
      retryMessage,
      createSession,
      selectSession,
      deleteSession,
      renameSession,
      selectModel,
      selectProvider,
      reconnect,
      pendingApproval,
      resolveApproval,
    ]
  );

  return <ChatContext.Provider value={value}>{children}</ChatContext.Provider>;
}
