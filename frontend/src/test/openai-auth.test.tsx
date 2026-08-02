import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const { mockApi, mockChat } = vi.hoisted(() => ({
  mockApi: {
    getOpenAIOAuthStatus: vi.fn(),
    startOpenAIOAuth: vi.fn(),
    logoutOpenAIOAuth: vi.fn().mockResolvedValue(undefined),
    getModels: vi.fn().mockResolvedValue(['llama3.2:latest']),
  },
  mockChat: {
    sessions: [],
    activeSessionId: null,
    models: ['llama3.2:latest'],
    currentModel: 'llama3.2:latest',
    currentProvider: 'ollama',
    providers: ['ollama', 'openai'],
    selectSession: vi.fn(),
    createSession: vi.fn(),
    selectModel: vi.fn(),
    selectProvider: vi.fn().mockResolvedValue(undefined),
    deleteSession: vi.fn(),
    renameSession: vi.fn(),
    sidebarOpen: true,
    setSidebarOpen: vi.fn(),
  },
}));

vi.mock('../api/client', () => ({ api: mockApi }));
vi.mock('../context', () => ({ useChat: () => mockChat }));

import { Sidebar } from '../components/Sidebar';

type Popup = {
  close: () => void;
  closed: boolean;
  location: Location;
};

function createPopup(): Popup {
  return {
    close: vi.fn(),
    closed: false,
    location: { href: 'about:blank' } as Location,
  };
}

function selectOpenAI(): void {
  fireEvent.click(screen.getByRole('button', { name: 'Select provider' }));
  fireEvent.click(screen.getByRole('option', { name: 'openai' }));
}

describe('OpenAI OAuth provider selection', () => {
  beforeEach(() => {
    vi.useRealTimers();
    vi.clearAllMocks();
    mockApi.logoutOpenAIOAuth.mockResolvedValue(undefined);
    mockApi.getModels.mockResolvedValue(['llama3.2:latest']);
    mockChat.selectProvider.mockResolvedValue(undefined);
    mockChat.currentProvider = 'ollama';
  });

  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it('selects OpenAI immediately when already signed in', async () => {
    const popup = createPopup();
    vi.stubGlobal(
      'open',
      vi.fn(() => popup)
    );
    mockApi.getOpenAIOAuthStatus.mockResolvedValue({ state: 'signed_in' });

    render(<Sidebar />);
    selectOpenAI();

    await waitFor(() => expect(mockChat.selectProvider).toHaveBeenCalledWith('openai'));
    expect(mockApi.startOpenAIOAuth).not.toHaveBeenCalled();
    expect(popup.close).toHaveBeenCalled();
  });

  it('polls with the returned login ID before selecting OpenAI', async () => {
    vi.useFakeTimers();
    const popup = createPopup();
    vi.stubGlobal(
      'open',
      vi.fn(() => popup)
    );
    mockApi.getOpenAIOAuthStatus
      .mockResolvedValueOnce({ state: 'signed_out' })
      .mockResolvedValueOnce({ state: 'signed_in' });
    mockApi.startOpenAIOAuth.mockResolvedValue({
      authorization_url: 'https://auth.openai.test/login',
      login_id: 'login-123',
    });

    render(<Sidebar />);
    selectOpenAI();
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1000);
    });

    expect(mockApi.getOpenAIOAuthStatus).toHaveBeenNthCalledWith(
      2,
      'login-123',
      expect.any(AbortSignal)
    );
    expect(mockChat.selectProvider).toHaveBeenCalledWith('openai');
    expect(popup.location.href).toBe('https://auth.openai.test/login');
  });

  it('reports a blocked popup without navigating or starting login', async () => {
    vi.stubGlobal(
      'open',
      vi.fn(() => null)
    );
    mockApi.getOpenAIOAuthStatus.mockResolvedValue({ state: 'signed_out' });

    render(<Sidebar />);
    selectOpenAI();

    expect(await screen.findByRole('alert')).toHaveTextContent('blocked the OpenAI login popup');
    expect(mockApi.startOpenAIOAuth).not.toHaveBeenCalled();
    expect(mockChat.selectProvider).not.toHaveBeenCalled();
  });

  it('cancels with the login ID when the popup is closed', async () => {
    vi.useFakeTimers();
    const popup = createPopup();
    vi.stubGlobal(
      'open',
      vi.fn(() => popup)
    );
    mockApi.getOpenAIOAuthStatus
      .mockResolvedValueOnce({ state: 'signed_out' })
      .mockImplementation(() => Promise.resolve({ state: 'pending' }));
    mockApi.startOpenAIOAuth.mockResolvedValue({
      authorization_url: 'https://auth.openai.test/login',
      login_id: 'closed-login',
    });

    render(<Sidebar />);
    selectOpenAI();
    await act(async () => {});
    expect(mockApi.startOpenAIOAuth).toHaveBeenCalled();
    popup.closed = true;
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1000);
    });

    expect(screen.getByRole('alert')).toHaveTextContent('closed before sign-in completed');
    expect(mockApi.logoutOpenAIOAuth).toHaveBeenCalledWith('closed-login');
    expect(mockChat.selectProvider).not.toHaveBeenCalled();
  });

  it('cancels a timed-out login', async () => {
    vi.useFakeTimers();
    const popup = createPopup();
    vi.stubGlobal(
      'open',
      vi.fn(() => popup)
    );
    mockApi.getOpenAIOAuthStatus
      .mockResolvedValueOnce({ state: 'signed_out' })
      .mockImplementation(() => Promise.resolve({ state: 'pending' }));
    mockApi.startOpenAIOAuth.mockResolvedValue({
      authorization_url: 'https://auth.openai.test/login',
      login_id: 'timeout-login',
    });

    render(<Sidebar />);
    selectOpenAI();
    await act(async () => {});
    expect(mockApi.startOpenAIOAuth).toHaveBeenCalled();
    await act(async () => {
      await vi.advanceTimersByTimeAsync(5 * 60 * 1000);
    });

    expect(screen.getByRole('alert')).toHaveTextContent('timed out');
    expect(mockApi.logoutOpenAIOAuth).toHaveBeenCalledWith('timeout-login');
    expect(mockChat.selectProvider).not.toHaveBeenCalled();
  });

  it('cancels a pending login when another provider is selected', async () => {
    const popup = createPopup();
    vi.stubGlobal(
      'open',
      vi.fn(() => popup)
    );
    mockApi.getOpenAIOAuthStatus.mockResolvedValue({ state: 'signed_out' });
    mockApi.startOpenAIOAuth.mockResolvedValue({
      authorization_url: 'https://auth.openai.test/login',
      login_id: 'changed-provider-login',
    });

    render(<Sidebar />);
    selectOpenAI();
    await waitFor(() => expect(mockApi.startOpenAIOAuth).toHaveBeenCalled());
    fireEvent.click(screen.getByRole('button', { name: 'Select provider' }));
    fireEvent.click(screen.getByRole('option', { name: 'ollama' }));

    await waitFor(() => {
      expect(mockApi.logoutOpenAIOAuth).toHaveBeenCalledWith('changed-provider-login');
    });
    expect(mockChat.selectProvider).toHaveBeenCalledWith('ollama');
    expect(mockChat.selectProvider).not.toHaveBeenCalledWith('openai');
  });

  it('cancels a login that starts after the user switches providers', async () => {
    const popup = createPopup();
    let resolveStart:
      | ((login: { authorization_url: string; login_id: string }) => void)
      | undefined;
    vi.stubGlobal(
      'open',
      vi.fn(() => popup)
    );
    mockApi.getOpenAIOAuthStatus.mockResolvedValue({ state: 'signed_out' });
    mockApi.startOpenAIOAuth.mockImplementationOnce(
      () =>
        new Promise((resolve) => {
          resolveStart = resolve;
        })
    );

    render(<Sidebar />);
    selectOpenAI();
    await waitFor(() => expect(mockApi.startOpenAIOAuth).toHaveBeenCalled());
    fireEvent.click(screen.getByRole('button', { name: 'Select provider' }));
    fireEvent.click(screen.getByRole('option', { name: 'ollama' }));
    await act(async () => {
      resolveStart?.({
        authorization_url: 'https://auth.openai.test/login',
        login_id: 'late-login',
      });
    });

    await waitFor(() =>
      expect(mockApi.logoutOpenAIOAuth).toHaveBeenCalledWith('late-login')
    );
    expect(mockChat.selectProvider).not.toHaveBeenCalledWith('openai');
  });

  it('aborts and cancels on unmount without accepting a stale completion', async () => {
    vi.useFakeTimers();
    const popup = createPopup();
    let pollSignal: AbortSignal | undefined;
    let completePoll: ((value: { state: 'signed_in' }) => void) | undefined;
    vi.stubGlobal(
      'open',
      vi.fn(() => popup)
    );
    mockApi.getOpenAIOAuthStatus
      .mockResolvedValueOnce({ state: 'signed_out' })
      .mockImplementationOnce((_loginId: string, signal: AbortSignal) => {
        pollSignal = signal;
        return new Promise((resolve) => {
          completePoll = resolve;
        });
      });
    mockApi.startOpenAIOAuth.mockResolvedValue({
      authorization_url: 'https://auth.openai.test/login',
      login_id: 'unmount-login',
    });

    const view = render(<Sidebar />);
    selectOpenAI();
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1000);
    });
    view.unmount();
    completePoll?.({ state: 'signed_in' });
    await act(async () => {
      await vi.advanceTimersByTimeAsync(0);
    });

    expect(pollSignal?.aborted).toBe(true);
    expect(mockApi.logoutOpenAIOAuth).toHaveBeenCalledWith('unmount-login');
    expect(mockChat.selectProvider).not.toHaveBeenCalled();
  });

  it('signs out an established OpenAI account and selects a fallback', async () => {
    mockChat.currentProvider = 'openai';
    render(<Sidebar />);

    fireEvent.click(screen.getByRole('button', { name: 'Sign out of OpenAI' }));

    await waitFor(() => expect(mockApi.logoutOpenAIOAuth).toHaveBeenCalledWith());
    expect(mockChat.selectProvider).toHaveBeenCalledWith('ollama', ['llama3.2:latest']);
  });

  it('keeps credentials when the fallback provider cannot be selected', async () => {
    mockChat.currentProvider = 'openai';
    mockApi.getModels.mockRejectedValueOnce(new Error('fallback unavailable'));
    render(<Sidebar />);

    fireEvent.click(screen.getByRole('button', { name: 'Sign out of OpenAI' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('fallback unavailable');
    expect(mockApi.logoutOpenAIOAuth).not.toHaveBeenCalled();
  });

  it('keeps credentials when fallback model IDs are invalid', async () => {
    mockChat.currentProvider = 'openai';
    mockApi.getModels.mockResolvedValueOnce(['', '   ']);
    render(<Sidebar />);

    fireEvent.click(screen.getByRole('button', { name: 'Sign out of OpenAI' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('No models are available');
    expect(mockApi.logoutOpenAIOAuth).not.toHaveBeenCalled();
    expect(mockChat.selectProvider).not.toHaveBeenCalled();
  });

  it('keeps the OpenAI selection when credential deletion fails', async () => {
    mockChat.currentProvider = 'openai';
    mockApi.logoutOpenAIOAuth.mockRejectedValueOnce(new Error('delete failed'));
    render(<Sidebar />);

    fireEvent.click(screen.getByRole('button', { name: 'Sign out of OpenAI' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('delete failed');
    expect(mockChat.selectProvider).not.toHaveBeenCalled();
  });
});
