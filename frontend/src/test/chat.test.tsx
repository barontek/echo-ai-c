import { describe, it, expect, vi, beforeEach } from 'vitest';
import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { ChatProvider, useChat } from '../context';

const { mockApi, wsCalls, wsOnOpenTimers } = vi.hoisted(() => ({
  mockApi: {
    getSessions: vi.fn().mockResolvedValue([
      { id: 'session-1', title: 'First Chat', created_at: '2024-01-01' },
      { id: 'session-2', title: 'Second Chat', created_at: '2024-01-02' },
    ]),
    getModels: vi.fn().mockResolvedValue(['qwen3:4b-instruct', 'llama3.2:latest']),
    getProviders: vi.fn().mockResolvedValue({ providers: ['ollama', 'openai', 'opencode_zen'], effortSupported: ['openai'] }),
    getOpenAIOAuthStatus: vi.fn().mockResolvedValue({ state: 'signed_out' }),
    createSession: vi.fn().mockResolvedValue({ session_id: 'new-session-456' }),
    loadSession: vi.fn().mockResolvedValue({
      session_id: 'session-1',
      title: 'First Chat',
      messages: [
        { role: 'user', content: 'Hello', timestamp: '10:00' },
        { role: 'assistant', content: 'Hi there!', timestamp: '10:01' },
      ],
    }),
    deleteSession: vi.fn().mockResolvedValue(undefined),
    renameSession: vi.fn().mockResolvedValue(undefined),
    updateConfig: vi.fn().mockResolvedValue(undefined),
    getPreferences: vi.fn().mockResolvedValue({}),
    setPreferences: vi.fn().mockResolvedValue(undefined),
    getConfig: vi.fn().mockResolvedValue({
      provider: 'ollama',
      model: 'qwen3:4b-instruct',
      temperature: 0.3,
      max_iterations: 50,
      session_enabled: true,
    }),
    healthCheck: vi.fn().mockResolvedValue({ status: 'healthy', version: '0.1.0' }),
  },
  wsCalls: [] as string[],
  /* Pending onopen timers across all mock ws instances: a timer scheduled
   * by one test can otherwise fire into the next test's wsCalls. */
  wsOnOpenTimers: [] as ReturnType<typeof setTimeout>[],
}));

vi.mock('../api/client', () => ({
  api: mockApi,
}));

const MOCK_WS_STATICS = { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 };

function installPrimaryWebSocketMock(): void {
  vi.stubGlobal(
    'WebSocket',
    Object.assign(
      /* Regular function (not an arrow) so `new WebSocket(...)` works;
       * the returned object replaces `this`, mimicking the real API. */
      vi.fn(function () {
        const handlers: Record<string, ((...args: unknown[]) => unknown) | null | undefined> = {};
        return {
          send: vi.fn((data: string) => {
            wsCalls.push(data);
          }),
          close: vi.fn(),
          readyState: 1,
          get onopen() {
            return handlers.onopen;
          },
          set onopen(fn: ((...args: unknown[]) => unknown) | null | undefined) {
            handlers.onopen = fn;
            if (fn) {
              wsOnOpenTimers.push(setTimeout(() => fn(), 0));
            }
          },
          get onclose() {
            return handlers.onclose;
          },
          set onclose(fn: ((...args: unknown[]) => unknown) | null | undefined) {
            handlers.onclose = fn;
          },
          get onmessage() {
            return handlers.onmessage;
          },
          set onmessage(fn: ((...args: unknown[]) => unknown) | null | undefined) {
            handlers.onmessage = fn;
          },
          get onerror() {
            return handlers.onerror;
          },
          set onerror(fn: ((...args: unknown[]) => unknown) | null | undefined) {
            handlers.onerror = fn;
          },
        };
      }),
      MOCK_WS_STATICS
    )
  );
}

installPrimaryWebSocketMock();

describe('Session History Bug Tests', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    /* Drop stale ws-open timers from earlier tests before re-installing
     * the primary WebSocket mock, so a previous test's onopen can't
     * append to this test's wsCalls. */
    wsOnOpenTimers.splice(0).forEach(clearTimeout);
    /* Re-install the primary WebSocket mock: individual tests may have
     * stubbed the global themselves, and stubGlobal leaves the last
     * stub in place for later tests. */
    installPrimaryWebSocketMock();
    if (typeof localStorage !== 'undefined') localStorage.clear();
    wsCalls.length = 0;
    mockApi.getSessions.mockResolvedValue([
      { id: 'session-1', title: 'First Chat', created_at: '2024-01-01' },
      { id: 'session-2', title: 'Second Chat', created_at: '2024-01-02' },
    ]);
    mockApi.loadSession.mockResolvedValue({
      session_id: 'session-1',
      title: 'First Chat',
      messages: [
        { role: 'user', content: 'Hello', timestamp: '10:00' },
        { role: 'assistant', content: 'Hi there!', timestamp: '10:01' },
      ],
    });
    mockApi.getProviders.mockResolvedValue({
      providers: ['ollama', 'openai', 'opencode_zen'],
      effortSupported: ['openai'],
    });
    mockApi.getModels.mockResolvedValue(['qwen3:4b-instruct', 'llama3.2:latest']);
    mockApi.getPreferences.mockResolvedValue({});
    mockApi.setPreferences.mockResolvedValue(undefined);
    mockApi.getOpenAIOAuthStatus.mockResolvedValue({ state: 'signed_out' });
  });

  describe('BUG: Session history not appearing', () => {
    it('should show sessions in sidebar after loading', async () => {
      function TestSidebar() {
        const { sessions } = useChat();
        return (
          <div>
            {sessions.map((s) => (
              <span key={s.id} data-testid={`session-${s.id}`}>
                {s.title}
              </span>
            ))}
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestSidebar />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(mockApi.getSessions).toHaveBeenCalled();
      });

      await waitFor(() => {
        expect(screen.getByTestId('session-session-1')).toBeInTheDocument();
      });

      expect(screen.getByTestId('session-session-1').textContent).toBe('First Chat');
    });

    it('should display loaded session messages', async () => {
      function TestComponent() {
        const { messages, selectSession } = useChat();
        return (
          <div>
            <button onClick={() => selectSession('session-1')}>Load</button>
            <span data-testid="msg-count">{messages.length}</span>
            {messages.map((m, i) => (
              <span key={i} data-testid={`msg-${i}`}>
                {m.role}:{m.content}
              </span>
            ))}
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      expect(screen.getByTestId('msg-count').textContent).toBe('0');

      await userEvent.click(screen.getByText('Load'));

      await waitFor(() => {
        expect(mockApi.loadSession).toHaveBeenCalledWith('session-1');
      });

      await waitFor(() => {
        expect(screen.getByTestId('msg-count').textContent).toBe('2');
      });

      expect(screen.getByTestId('msg-0').textContent).toBe('user:Hello');
      expect(screen.getByTestId('msg-1').textContent).toBe('assistant:Hi there!');
    });

    it('should switch between sessions without accumulating messages', async () => {
      mockApi.loadSession
        .mockResolvedValueOnce({
          session_id: 'session-1',
          title: 'First',
          messages: [{ role: 'user', content: 'Msg1', timestamp: '10:00' }],
        })
        .mockResolvedValueOnce({
          session_id: 'session-2',
          title: 'Second',
          messages: [{ role: 'user', content: 'Msg2', timestamp: '11:00' }],
        });

      function TestComponent() {
        const { messages, selectSession, activeSessionId } = useChat();
        return (
          <div>
            <span data-testid="active-session">{activeSessionId || 'none'}</span>
            <span data-testid="msg-count">{messages.length}</span>
            <span data-testid="first-msg">{messages[0]?.content || ''}</span>
            <button onClick={() => selectSession('session-1')}>Load1</button>
            <button onClick={() => selectSession('session-2')}>Load2</button>
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Load1'));
      await waitFor(() => {
        expect(screen.getByTestId('active-session').textContent).toBe('session-1');
      });
      expect(screen.getByTestId('msg-count').textContent).toBe('1');
      expect(screen.getByTestId('first-msg').textContent).toBe('Msg1');

      await userEvent.click(screen.getByText('Load2'));
      await waitFor(() => {
        expect(screen.getByTestId('active-session').textContent).toBe('session-2');
      });
      expect(screen.getByTestId('msg-count').textContent).toBe('1');
      expect(screen.getByTestId('first-msg').textContent).toBe('Msg2');
    });

    it('should clear messages when creating new session', async () => {
      function TestComponent() {
        const { messages, createSession } = useChat();
        return (
          <div>
            <span data-testid="msg-count">{messages.length}</span>
            <button onClick={createSession}>New Chat</button>
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('New Chat'));

      await waitFor(() => {
        expect(screen.getByTestId('msg-count').textContent).toBe('0');
      });
    });
  });

  describe('BUG: AI not replying to messages', () => {
    it('should queue message when WebSocket not ready', async () => {
      function TestComponent() {
        const { sendMessage, messages } = useChat();
        return (
          <div>
            <span data-testid="msg-count">{messages.length}</span>
            <button onClick={() => sendMessage('Hello AI')}>Send</button>
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      // Even if WS is not fully ready, message should be added to queue
      await userEvent.click(screen.getByText('Send'));

      await waitFor(() => {
        expect(screen.getByTestId('msg-count').textContent).toBe('1');
      });
    });

    it('should add user message to messages immediately when sending', async () => {
      function TestComponent() {
        const { sendMessage, messages } = useChat();
        return (
          <div>
            <span data-testid="msg-count">{messages.length}</span>
            <span data-testid="first-content">{messages[0]?.content || ''}</span>
            <button onClick={() => sendMessage('My message')}>Send</button>
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Send'));

      await waitFor(() => {
        expect(screen.getByTestId('msg-count').textContent).toBe('1');
      });
      expect(screen.getByTestId('first-content').textContent).toBe('My message');
    });

    it('should handle send when WebSocket is closed gracefully', async () => {
      // Test that sendMessage doesn't crash even if WS is not available
      function TestComponent() {
        const { sendMessage } = useChat();
        return (
          <div>
            <button onClick={() => sendMessage('Test')}>Send</button>
          </div>
        );
      }

      // Should not throw
      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Send'));
      // Message should be added to queue and attempted to send
      expect(screen.getByText('Send')).toBeInTheDocument();
    });
  });

  describe('Data flow verification', () => {
    it('should load models on mount', async () => {
      function TestComponent() {
        const { models } = useChat();
        return <span data-testid="model-count">{models.length}</span>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(mockApi.getModels).toHaveBeenCalled();
      });

      await waitFor(() => {
        expect(screen.getByTestId('model-count').textContent).toBe('2');
      });
    });

    it('should track current model', async () => {
      mockApi.getPreferences.mockResolvedValueOnce({ model: 'qwen3:4b-instruct' });

      function TestComponent() {
        const { currentModel } = useChat();
        return <span data-testid="current-model">{currentModel}</span>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByTestId('current-model').textContent).toBe('qwen3:4b-instruct');
      });
    });

    it('should load model from API preferences on mount', async () => {
      mockApi.getPreferences.mockResolvedValueOnce({ model: 'llama3.2:latest' });

      function TestComponent() {
        const { currentModel } = useChat();
        return <span data-testid="pref-model">{currentModel}</span>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(mockApi.getPreferences).toHaveBeenCalled();
      });

      await waitFor(() => {
        expect(screen.getByTestId('pref-model').textContent).toBe('llama3.2:latest');
      });
    });

    it('should persist model via API on selectModel', async () => {
      function TestComponent() {
        const { selectModel } = useChat();
        return <button onClick={() => selectModel('gpt-4')}>Use GPT-4</button>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Use GPT-4'));

      await waitFor(() => {
        expect(mockApi.setPreferences).toHaveBeenCalledWith({
          model: 'gpt-4',
          provider: 'ollama',
          models: { ollama: 'gpt-4' },
        });
      });
    });

    it('should restore the last model for each provider', async () => {
      mockApi.getProviders.mockResolvedValueOnce({ providers: ['ollama', 'openai'], effortSupported: ['openai'] });
      mockApi.getPreferences.mockResolvedValueOnce({
        provider: 'ollama',
        model: 'qwen3:4b-instruct',
        models: { ollama: 'qwen3:4b-instruct', openai: 'gpt-4o-mini' },
      });
      mockApi.getModels.mockImplementation((provider?: string) =>
        Promise.resolve(provider === 'openai' ? ['gpt-4o-mini', 'gpt-4o'] : ['qwen3:4b-instruct', 'llama3.2:latest'])
      );

      function TestComponent() {
        const { currentProvider, currentModel, selectProvider } = useChat();
        return (
          <>
            <span data-testid="selected-provider">{currentProvider}</span>
            <span data-testid="selected-model">{currentModel}</span>
            <button onClick={() => selectProvider('openai')}>OpenAI</button>
            <button onClick={() => selectProvider('ollama')}>Ollama</button>
          </>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByTestId('selected-model').textContent).toBe('qwen3:4b-instruct');
      });
      await userEvent.click(screen.getByText('OpenAI'));
      await waitFor(() => {
        expect(screen.getByTestId('selected-provider').textContent).toBe('openai');
        expect(screen.getByTestId('selected-model').textContent).toBe('gpt-4o-mini');
      });
      await userEvent.click(screen.getByText('Ollama'));
      await waitFor(() => {
        expect(screen.getByTestId('selected-provider').textContent).toBe('ollama');
        expect(screen.getByTestId('selected-model').textContent).toBe('qwen3:4b-instruct');
      });
    });

    it('does not restore OpenAI while signed out', async () => {
      localStorage.setItem(
        'echo-ai-chat-preferences',
        JSON.stringify({
          provider: 'openai',
          model: 'gpt-5-codex',
          models: { openai: 'gpt-5-codex' },
        })
      );
      mockApi.getProviders.mockResolvedValueOnce({ providers: ['ollama', 'openai'], effortSupported: ['openai'] });
      mockApi.getOpenAIOAuthStatus.mockResolvedValueOnce({ state: 'signed_out' });

      function TestComponent() {
        const { currentProvider, currentModel } = useChat();
        return (
          <>
            <span data-testid="restored-provider">{currentProvider}</span>
            <span data-testid="restored-model">{currentModel}</span>
          </>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByTestId('restored-model').textContent).toBe('qwen3:4b-instruct');
      });
      expect(screen.getByTestId('restored-provider').textContent).toBe('ollama');
      expect(mockApi.getOpenAIOAuthStatus).toHaveBeenCalled();
      expect(mockApi.getModels).not.toHaveBeenCalledWith('openai', expect.anything());
    });

    it('keeps models in response order and ignores an older provider completion', async () => {
      let resolveOpenAI: ((models: string[]) => void) | undefined;
      let resolveZen: ((models: string[]) => void) | undefined;
      let openAISignal: AbortSignal | undefined;
      mockApi.getModels.mockImplementation((provider?: string, signal?: AbortSignal) => {
        if (provider === 'openai') {
          openAISignal = signal;
          return new Promise<string[]>((resolve) => {
            resolveOpenAI = resolve;
          });
        }
        if (provider === 'opencode_zen') {
          return new Promise<string[]>((resolve) => {
            resolveZen = resolve;
          });
        }
        return Promise.resolve(['qwen3:4b-instruct']);
      });

      function TestComponent() {
        const { currentProvider, currentModel, models, selectProvider } = useChat();
        return (
          <>
            <span data-testid="ordered-provider">{currentProvider}</span>
            <span data-testid="ordered-model">{currentModel}</span>
            <span data-testid="ordered-models">{models.join(',')}</span>
            <button onClick={() => void selectProvider('openai')}>OpenAI request</button>
            <button onClick={() => void selectProvider('opencode_zen')}>Zen request</button>
          </>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );
      await waitFor(() =>
        expect(screen.getByTestId('ordered-model').textContent).toBe('qwen3:4b-instruct')
      );

      fireEvent.click(screen.getByText('OpenAI request'));
      expect(screen.getByTestId('ordered-provider').textContent).toBe('ollama');
      expect(screen.getByTestId('ordered-models').textContent).toBe('qwen3:4b-instruct');
      fireEvent.click(screen.getByText('Zen request'));
      expect(openAISignal?.aborted).toBe(true);

      await act(async () => {
        resolveZen?.(['zen-second', 'zen-first']);
      });
      expect(screen.getByTestId('ordered-provider').textContent).toBe('opencode_zen');
      expect(screen.getByTestId('ordered-model').textContent).toBe('zen-second');
      expect(screen.getByTestId('ordered-models').textContent).toBe('zen-second,zen-first');

      await act(async () => {
        resolveOpenAI?.(['stale-openai']);
      });
      expect(screen.getByTestId('ordered-provider').textContent).toBe('opencode_zen');
      expect(screen.getByTestId('ordered-models').textContent).toBe('zen-second,zen-first');
      expect(mockApi.setPreferences).not.toHaveBeenCalledWith(
        expect.objectContaining({ provider: 'openai' })
      );
      expect(wsCalls).not.toContainEqual(expect.stringContaining('"provider":"openai"'));
    });

    it('keeps startup sessions and providers when a provider is selected early', async () => {
      let resolveProviders: ((providers: {
        providers: string[];
        effortSupported: string[];
      }) => void) | undefined;
      mockApi.getProviders.mockImplementationOnce(
        () =>
          new Promise<{ providers: string[]; effortSupported: string[] }>((resolve) => {
            resolveProviders = resolve;
          })
      );
      mockApi.getModels.mockImplementation((provider?: string) =>
        Promise.resolve(provider === 'opencode_zen' ? ['zen-model'] : ['ollama-model'])
      );

      function TestComponent() {
        const { sessions, providers, currentProvider, selectProvider } = useChat();
        return (
          <>
            <span data-testid="startup-session-count">{sessions.length}</span>
            <span data-testid="startup-providers">{providers.join(',')}</span>
            <span data-testid="startup-provider">{currentProvider}</span>
            <button onClick={() => void selectProvider('opencode_zen')}>Select early</button>
          </>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );
      fireEvent.click(screen.getByText('Select early'));
      await waitFor(() =>
        expect(screen.getByTestId('startup-provider').textContent).toBe('opencode_zen')
      );

      await act(async () => {
        resolveProviders?.({ providers: ['ollama', 'openai', 'opencode_zen'], effortSupported: ['openai'] });
      });
      await waitFor(() =>
        expect(screen.getByTestId('startup-session-count').textContent).toBe('2')
      );
      expect(screen.getByTestId('startup-providers').textContent).toBe(
        'ollama,openai,opencode_zen'
      );
      expect(screen.getByTestId('startup-provider').textContent).toBe('opencode_zen');
    });

    it('should handle empty sessions list', async () => {
      mockApi.getSessions.mockResolvedValueOnce([]);

      function TestComponent() {
        const { sessions } = useChat();
        return <span data-testid="session-count">{sessions.length}</span>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByTestId('session-count').textContent).toBe('0');
      });
    });

    it('should handle session with no messages', async () => {
      mockApi.loadSession.mockResolvedValueOnce({
        session_id: 'empty-session',
        title: 'Empty',
        messages: [],
      });

      function TestComponent() {
        const { selectSession, messages } = useChat();
        return (
          <div>
            <span data-testid="msg-count">{messages.length}</span>
            <button onClick={() => selectSession('empty-session')}>Load</button>
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Load'));

      await waitFor(() => {
        expect(screen.getByTestId('msg-count').textContent).toBe('0');
      });
    });

    it('should delete session via API and refresh list', async () => {
      function TestComponent() {
        const { deleteSession } = useChat();
        return <button onClick={() => deleteSession('session-1')}>Delete</button>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Delete'));

      await waitFor(() => {
        expect(mockApi.deleteSession).toHaveBeenCalledWith('session-1');
      });
    });

    it('renameSession calls API with session_id and new_title', async () => {
      function TestComponent() {
        const { renameSession } = useChat();
        return <button onClick={() => renameSession('session-1', 'Renamed Chat')}>Rename</button>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Rename'));

      await waitFor(() => {
        expect(mockApi.renameSession).toHaveBeenCalledWith('session-1', 'Renamed Chat');
      });
    });

    it('renameSession updates title in sessions list after rename', async () => {
      function TestComponent() {
        const { sessions, renameSession } = useChat();
        return (
          <div>
            <span data-testid="session-title">
              {sessions.find((s) => s.id === 'session-1')?.title}
            </span>
            <button onClick={() => renameSession('session-1', 'Renamed Chat')}>Rename</button>
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByTestId('session-title').textContent).toBe('First Chat');
      });

      await userEvent.click(screen.getByText('Rename'));

      await waitFor(() => {
        expect(screen.getByTestId('session-title').textContent).toBe('Renamed Chat');
      });
    });

    it('renameSession handles API error without crashing', async () => {
      mockApi.renameSession.mockRejectedValueOnce(new Error('API error'));

      function TestComponent() {
        const { renameSession } = useChat();
        return <button onClick={() => renameSession('session-1', 'Will Fail')}>RenameError</button>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('RenameError'));

      await waitFor(() => {
        expect(mockApi.renameSession).toHaveBeenCalledWith('session-1', 'Will Fail');
      });
    });

    it('renameSession with empty string still calls API (backend validates min_length)', async () => {
      function TestComponent() {
        const { renameSession } = useChat();
        return <button onClick={() => renameSession('session-1', '')}>RenameEmpty</button>;
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('RenameEmpty'));

      await waitFor(() => {
        expect(mockApi.renameSession).toHaveBeenCalledWith('session-1', '');
      });
    });
  });

  describe('Session Continuity Tests', () => {
    it('should refresh session list after chat completes', async () => {
      let onmessageHandler: ((event: { data: string }) => void) | null = null;
      const mockWs = {
        send: vi.fn(),
        close: vi.fn(),
        readyState: 1,
        set onopen(fn: () => void) {
          setTimeout(fn, 0);
        },
        get onmessage() {
          return onmessageHandler;
        },
        set onmessage(fn: ((event: { data: string }) => void) | null) {
          onmessageHandler = fn;
        },
      };

      vi.stubGlobal(
        'WebSocket',
        Object.assign(vi.fn(() => mockWs), MOCK_WS_STATICS)
      );

      function TestComponent() {
        const { sendMessage } = useChat();
        return (
          <div>
            <button onClick={() => sendMessage('test')}>Send</button>
          </div>
        );
      }

      render(
        <ChatProvider>
          <TestComponent />
        </ChatProvider>
      );

      await userEvent.click(screen.getByText('Send'));

      // Simulate 'done' message from server
      if (onmessageHandler) {
        (onmessageHandler as (event: { data: string }) => void)({
          data: JSON.stringify({
            type: 'done',
            content: 'response',
            session_id: 'chat-123',
          }),
        });
      }

      // Session list should be refreshed after chat completes
      await waitFor(() => {
        expect(mockApi.getSessions).toHaveBeenCalled();
      });
    });
  });
});

describe('Reasoning effort setting', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    /* Same hygiene as the main describe: drop stale ws-open timers and
     * re-install the primary WebSocket mock, since earlier tests stub the
     * global themselves and leave the last stub in place. */
    wsOnOpenTimers.splice(0).forEach(clearTimeout);
    installPrimaryWebSocketMock();
    if (typeof localStorage !== 'undefined') localStorage.clear();
    wsCalls.length = 0;
    mockApi.getProviders.mockResolvedValue({
      providers: ['ollama', 'openai', 'opencode_zen'],
      effortSupported: ['openai'],
    });
    mockApi.getModels.mockResolvedValue(['qwen3:4b-instruct', 'llama3.2:latest']);
    mockApi.getPreferences.mockResolvedValue({});
    mockApi.setPreferences.mockResolvedValue(undefined);
    mockApi.getOpenAIOAuthStatus.mockResolvedValue({ state: 'signed_out' });
    mockApi.getSessions.mockResolvedValue([]);
    mockApi.loadSession.mockResolvedValue({ session_id: 's', title: 't', messages: [] });
  });

  it('shows the effort selector only when the current provider supports it', async () => {
    const { ChatInput } = await import('../components/ChatInput');
    function TestComponent() {
      const { currentProvider, selectProvider, supportsEffort } = useChat();
      return (
        <>
          <span data-testid="provider">{currentProvider}</span>
          <span data-testid="supports">{String(supportsEffort)}</span>
          <button onClick={() => void selectProvider('openai')}>OpenAI</button>
          <ChatInput />
        </>
      );
    }
    render(
      <ChatProvider>
        <TestComponent />
      </ChatProvider>
    );

    await waitFor(() => {
      expect(screen.getByTestId('provider').textContent).toBe('ollama');
    });
    expect(screen.getByTestId('supports').textContent).toBe('false');
    expect(screen.queryByRole('combobox', { name: 'Reasoning effort' })).toBeNull();

    await userEvent.click(screen.getByText('OpenAI'));
    await waitFor(() => {
      expect(screen.getByTestId('supports').textContent).toBe('true');
    });
    expect(screen.getByRole('combobox', { name: 'Reasoning effort' })).toBeDefined();
  });

  it('selecting an effort persists it and resends the config over the live socket', async () => {
    function TestComponent() {
      const { currentEffort, selectProvider, selectEffort } = useChat();
      return (
        <>
          <span data-testid="effort">{currentEffort}</span>
          <button onClick={() => void selectProvider('openai')}>OpenAI</button>
          <button onClick={() => selectEffort('low')}>Effort low</button>
          <button onClick={() => selectEffort('')}>Effort default</button>
        </>
      );
    }
    render(
      <ChatProvider>
        <TestComponent />
      </ChatProvider>
    );

    await waitFor(() => {
      expect(screen.getByTestId('effort').textContent).toBe('');
    });
    await userEvent.click(screen.getByText('OpenAI'));
    await waitFor(() => {
      expect(mockApi.getModels).toHaveBeenCalledWith('openai', expect.anything());
    });
    wsCalls.length = 0;
    await userEvent.click(screen.getByText('Effort low'));
    expect(screen.getByTestId('effort').textContent).toBe('low');
    await waitFor(() => {
      expect(mockApi.setPreferences).toHaveBeenCalledWith(
        expect.objectContaining({ effort: 'low' })
      );
    });
    expect(
      wsCalls.some((c) => c.includes('"provider":"openai"') && c.includes('"effort":"low"'))
    ).toBe(true);

    wsCalls.length = 0;
    await userEvent.click(screen.getByText('Effort default'));
    expect(screen.getByTestId('effort').textContent).toBe('');
    await waitFor(() => {
      expect(mockApi.setPreferences).toHaveBeenCalledWith(
        expect.objectContaining({ effort: undefined })
      );
    });
    expect(wsCalls.some((c) => c.includes('"effort":'))).toBe(false);
  });

  it('restores a saved effort and sends it in the initial config message', async () => {
    localStorage.setItem(
      'echo-ai-chat-preferences',
      JSON.stringify({
        provider: 'ollama',
        model: 'qwen3:4b-instruct',
        models: { ollama: 'qwen3:4b-instruct' },
        effort: 'high',
      })
    );
    function TestComponent() {
      const { currentEffort } = useChat();
      return <span data-testid="effort">{currentEffort}</span>;
    }
    render(
      <ChatProvider>
        <TestComponent />
      </ChatProvider>
    );

    await waitFor(() => {
      expect(screen.getByTestId('effort').textContent).toBe('high');
    });
    await waitFor(() => {
      expect(wsCalls.some((c) => c.includes('"effort":"high"'))).toBe(true);
    });
  });

  it('drops an invalid saved effort instead of sending it', async () => {
    localStorage.setItem(
      'echo-ai-chat-preferences',
      JSON.stringify({
        provider: 'ollama',
        model: 'qwen3:4b-instruct',
        models: { ollama: 'qwen3:4b-instruct' },
        effort: 'extreme',
      })
    );
    function TestComponent() {
      const { currentEffort } = useChat();
      return <span data-testid="effort">{currentEffort}</span>;
    }
    render(
      <ChatProvider>
        <TestComponent />
      </ChatProvider>
    );

    await waitFor(() => {
      expect(screen.getByTestId('effort').textContent).toBe('');
    });
    await waitFor(() => {
      expect(wsCalls.some((c) => c.includes('"effort"'))).toBe(false);
    });
  });
});
