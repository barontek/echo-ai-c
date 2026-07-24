import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { parseThinkBlocks } from '../utils/thinkBlockParser';

const { mockApi } = vi.hoisted(() => ({
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
      messages: [],
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
}));

vi.mock('../api/client', () => ({
  api: mockApi,
}));

vi.stubGlobal(
  'WebSocket',
  vi.fn(() => ({
    send: vi.fn(),
    close: vi.fn(),
    readyState: 1,
    set onopen(fn: () => void) {
      setTimeout(fn, 0);
    },
    set onmessage(_fn: ((event: MessageEvent) => void) | null) {},
    set onclose(_fn: ((event: CloseEvent) => void) | null) {},
    set onerror(_fn: ((event: Event) => void) | null) {},
  }))
);

describe('Components', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('ChatInput renders correctly', async () => {
    const { ChatInput } = await import('../components/ChatInput');
    const { ChatProvider } = await import('../context');

    expect(ChatInput).toBeDefined();
    expect(ChatProvider).toBeDefined();
  });

  it('context exports work', async () => {
    const context = await import('../context');
    expect(context.ChatProvider).toBeDefined();
    expect(context.useChat).toBeDefined();
    expect(context.ChatContext).toBeDefined();
  });

  it('types file exists', async () => {
    const types = await import('../types');
    expect(types).toBeDefined();
  });

  it('api client exports work', async () => {
    const { api } = await import('../api/client');
    expect(api).toBeDefined();
    expect(typeof api.getSessions).toBe('function');
    expect(typeof api.getModels).toBe('function');
  });

  describe('Sidebar rename UI', () => {
    it('renders rename buttons for each session', async () => {
      const { Sidebar } = await import('../components/Sidebar');
      const { ChatProvider } = await import('../context');

      render(
        <ChatProvider>
          <Sidebar />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(mockApi.getSessions).toHaveBeenCalled();
      });

      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });

      const renameButtons = screen.getAllByTitle('Rename');
      expect(renameButtons).toHaveLength(2);
    });

    it('shows inline input when rename button is clicked', async () => {
      const { Sidebar } = await import('../components/Sidebar');
      const { ChatProvider } = await import('../context');

      render(
        <ChatProvider>
          <Sidebar />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });

      const renameButtons = screen.getAllByTitle('Rename');
      await userEvent.click(renameButtons[0]);

      const input = screen.getByDisplayValue('First Chat');
      expect(input).toBeInTheDocument();
      expect(input.tagName).toBe('INPUT');
    });

    it('calls renameSession on Enter key', async () => {
      const { Sidebar } = await import('../components/Sidebar');
      const { ChatProvider } = await import('../context');

      render(
        <ChatProvider>
          <Sidebar />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });

      const renameButtons = screen.getAllByTitle('Rename');
      await userEvent.click(renameButtons[0]);

      const input = screen.getByDisplayValue('First Chat');
      await userEvent.clear(input);
      await userEvent.type(input, 'Renamed{Enter}');

      await waitFor(() => {
        expect(mockApi.renameSession).toHaveBeenCalledWith('session-1', 'Renamed');
      });
    });

    it('cancels rename on Escape key without calling API', async () => {
      const { Sidebar } = await import('../components/Sidebar');
      const { ChatProvider } = await import('../context');

      render(
        <ChatProvider>
          <Sidebar />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });

      const renameButtons = screen.getAllByTitle('Rename');
      await userEvent.click(renameButtons[0]);

      const input = screen.getByDisplayValue('First Chat');
      await userEvent.clear(input);
      await userEvent.type(input, 'ShouldNotSave{Escape}');

      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });
      expect(mockApi.renameSession).not.toHaveBeenCalled();
    });

    it('does not show rename input next to delete button during rename', async () => {
      const { Sidebar } = await import('../components/Sidebar');
      const { ChatProvider } = await import('../context');

      render(
        <ChatProvider>
          <Sidebar />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });

      const renameButtons = screen.getAllByTitle('Rename');
      await userEvent.click(renameButtons[0]);

      // During rename, the rename button for this session should be hidden
      const visibleRenameButtons = screen.getAllByTitle('Rename');
      expect(visibleRenameButtons).toHaveLength(1); // Only the other session's button
    });

    it('does not call renameSession on empty title submission', async () => {
      const { Sidebar } = await import('../components/Sidebar');
      const { ChatProvider } = await import('../context');

      render(
        <ChatProvider>
          <Sidebar />
        </ChatProvider>
      );

      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });

      const renameButtons = screen.getAllByTitle('Rename');
      await userEvent.click(renameButtons[0]);

      const input = screen.getByDisplayValue('First Chat');
      await userEvent.clear(input);
      await userEvent.type(input, '{Enter}');

      expect(mockApi.renameSession).not.toHaveBeenCalled();
      await waitFor(() => {
        expect(screen.getByText('First Chat')).toBeInTheDocument();
      });
    });
  });
});

describe('parseThinkBlocks — streaming thinking classification', () => {
  it('classifies a complete <think> block', () => {
    const blocks = parseThinkBlocks('<think>Let me solve this</think>Here is the answer');
    expect(blocks).toEqual([
      { type: 'thinking', text: 'Let me solve this' },
      { type: 'content', text: 'Here is the answer' },
    ]);
  });

  it('classifies content before, after, and between multiple think blocks', () => {
    const blocks = parseThinkBlocks('start <think>first</think> middle <think>second</think> end');
    expect(blocks).toEqual([
      { type: 'content', text: 'start ' },
      { type: 'thinking', text: 'first' },
      { type: 'content', text: ' middle ' },
      { type: 'thinking', text: 'second' },
      { type: 'content', text: ' end' },
    ]);
  });

  it('handles unclosed <think> tag at end of stream (partial)', () => {
    const blocks = parseThinkBlocks('Some text <think>still thinking');
    expect(blocks).toEqual([
      { type: 'content', text: 'Some text ' },
      { type: 'thinking', text: 'still thinking' },
    ]);
  });

  it('handles only thinking without content', () => {
    const blocks = parseThinkBlocks('<think>just thinking</think>');
    expect(blocks).toEqual([{ type: 'thinking', text: 'just thinking' }]);
  });

  it('returns a single content block when no thinking is present', () => {
    const blocks = parseThinkBlocks('plain response without thinking');
    expect(blocks).toEqual([{ type: 'content', text: 'plain response without thinking' }]);
  });

  it('progressive chunks: tag split across multiple appends', () => {
    // Simulate the frontend accumulating content chunk by chunk
    const chunks = [
      'Here is my ',
      '<thi',
      'nk>Let me process ',
      'this step by step',
      '</think>',
      'The answer is 42.',
    ];
    const streamed: string[] = [];
    for (const chunk of chunks) {
      streamed.push(chunk);
      // parseThinkBlocks is called on the full accumulated content each time
      const blocks = parseThinkBlocks(streamed.join(''));
      // After the closing </think> tag, a content block should appear
      if (chunk === '</think>') {
        const thinkingBlock = blocks.find((b) => b.type === 'thinking');
        expect(thinkingBlock).toBeDefined();
        expect(thinkingBlock!.text).toBe('Let me process this step by step');
      }
    }
    const final = parseThinkBlocks(streamed.join(''));
    expect(final).toEqual([
      { type: 'content', text: 'Here is my ' },
      { type: 'thinking', text: 'Let me process this step by step' },
      { type: 'content', text: 'The answer is 42.' },
    ]);
  });

  it('progressive chunks: empty content after thinking', () => {
    const blocks = parseThinkBlocks('<think>I am thinking</think>');
    expect(blocks).toEqual([{ type: 'thinking', text: 'I am thinking' }]);
  });

  it('progressive chunks: partial <think tag not yet classified as thinking', () => {
    const partial = parseThinkBlocks('prefix <thi');
    // '<thi' is not a complete <think> so it stays as content
    expect(partial).toEqual([{ type: 'content', text: 'prefix <thi' }]);
    // Once nk> arrives it becomes thinking
    const complete = parseThinkBlocks('prefix <think>thought</think>answer');
    expect(complete).toEqual([
      { type: 'content', text: 'prefix ' },
      { type: 'thinking', text: 'thought' },
      { type: 'content', text: 'answer' },
    ]);
  });
});
