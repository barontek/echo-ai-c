import { describe, it, expect, vi, beforeEach } from 'vitest';
import { api } from '../api/client';

// Mock axios
vi.mock('axios', () => ({
  default: {
    create: () => ({
      get: vi.fn(),
      post: vi.fn(),
      delete: vi.fn(),
      interceptors: {
        request: {
          use: vi.fn(),
        },
        response: {
          use: vi.fn(),
        },
      },
    }),
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
    expect(typeof api.createSession).toBe('function');
    expect(typeof api.loadSession).toBe('function');
    expect(typeof api.deleteSession).toBe('function');
    expect(typeof api.renameSession).toBe('function');
    expect(typeof api.healthCheck).toBe('function');
  });
});
