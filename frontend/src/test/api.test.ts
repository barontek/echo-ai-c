import { describe, it, expect, vi, beforeEach } from 'vitest';
import { api } from '../api/client';

// Mock axios
const { mockClient } = vi.hoisted(() => ({
  mockClient: {
    get: vi.fn(),
    post: vi.fn(),
    delete: vi.fn(),
    interceptors: {
      request: { use: vi.fn() },
      response: { use: vi.fn() },
    },
  },
}));

vi.mock('axios', () => ({
  default: {
    create: () => mockClient,
  },
}));

describe('API Client', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('should export api object', () => {
    expect(api).toBeDefined();
    expect(typeof api.getSessions).toBe('function');
    expect(typeof api.getModels).toBe('function');
    expect(typeof api.getProviders).toBe('function');
    expect(typeof api.createSession).toBe('function');
    expect(typeof api.loadSession).toBe('function');
    expect(typeof api.deleteSession).toBe('function');
    expect(typeof api.renameSession).toBe('function');
    expect(typeof api.healthCheck).toBe('function');
  });

  it('sends the login ID when polling and cancelling OpenAI login', async () => {
    const signal = new AbortController().signal;
    mockClient.get.mockResolvedValueOnce({ data: { state: 'pending' } });
    mockClient.post.mockResolvedValueOnce({ data: { signed_out: true } });

    await api.getOpenAIOAuthStatus('opaque-login-id', signal);
    await api.logoutOpenAIOAuth('opaque-login-id', signal);

    expect(mockClient.get).toHaveBeenCalledWith('/api/auth/openai/status', {
      params: { login_id: 'opaque-login-id' },
      signal,
    });
    expect(mockClient.post).toHaveBeenCalledWith(
      '/api/auth/openai/logout',
      { login_id: 'opaque-login-id' },
      { signal }
    );
  });
});
