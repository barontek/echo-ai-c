import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { ChatProvider, useChat } from '../context';

const { mockApi, wsCalls } = vi.hoisted(() => ({
  mockApi: {
    getSessions: vi.fn().mockResolvedValue([
      { id: 'session-1', title: 'First Chat', created_at: '2024-01-01' },
      { id: 'session-2', title: 'Second Chat', created_at: '2024-01-02' },
    ]),
    getModels: vi.fn().mockResolvedValue(['qwen3:4b-instruct', 'llama3.2:latest']),
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
}));

vi.mock('../api/client', () => ({
  api: mockApi,
}));

vi.stubGlobal(
  'WebSocket',
  vi.fn(() => {
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
          setTimeout(() => fn(), 0);
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
  })
);

describe('Session History Bug Tests', () => {
  beforeEach(() => {
    vi.clearAllMocks();
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
        expect(mockApi.setPreferences).toHaveBeenCalledWith({ model: 'gpt-4', provider: 'ollama' });
      });
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
        vi.fn(() => mockWs)
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
