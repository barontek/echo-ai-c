import axios, { AxiosError } from 'axios';
import type { Session, Config, Message, ApiError } from '../types';

const API_BASE = ''; // Use relative path - goes through Vite proxy in dev

const API_TIMEOUT = Math.max(
  1000,
  parseInt(import.meta.env.VITE_API_TIMEOUT || '10000', 10) || 10000
);

const UNLOCK_TOKEN_HEADER = 'X-Unlock-Token';

type TokenExpiredCallback = () => void;

class ApiClient {
  private client;
  private unlockToken: string | null = null;
  private onTokenExpired: TokenExpiredCallback | null = null;

  constructor() {
    this.client = axios.create({
      baseURL: API_BASE,
      timeout: API_TIMEOUT,
      headers: { 'Content-Type': 'application/json' },
    });

    // Attach unlock token to every request
    this.client.interceptors.request.use((config) => {
      if (this.unlockToken) {
        config.headers.set(UNLOCK_TOKEN_HEADER, this.unlockToken);
      }
      return config;
    });

    this.client.interceptors.response.use(
      (response) => response,
      (error: AxiosError<ApiError>) => {
        // No response — backend not reachable
        if (!error.response) {
          return Promise.reject(new Error('__BACKEND_UNREACHABLE__'));
        }

        const message = error.response.data?.error || error.message || 'Unknown error';
        console.error(`API Error: ${message}`);

        // If we get 401 and have a token, the token is invalid/expired
        if (error.response.status === 401) {
          this.unlockToken = null;
          if (this.onTokenExpired) {
            this.onTokenExpired();
          }
        }

        return Promise.reject(new Error(message));
      }
    );
  }

  /** Register a callback for when the unlock token expires. */
  setOnTokenExpired(cb: TokenExpiredCallback): void {
    this.onTokenExpired = cb;
  }

  /** Return whether a valid unlock token is held. */
  get isUnlocked(): boolean {
    return this.unlockToken !== null;
  }

  /** Return the current unlock token, or null. */
  get unlockTokenValue(): string | null {
    return this.unlockToken;
  }

  /** Store the unlock token from a setup or unlock response. */
  private setUnlockToken(token: string): void {
    this.unlockToken = token;
  }

  /** Clear the unlock token. */
  clearUnlockToken(): void {
    this.unlockToken = null;
  }

  async getModels(provider?: string): Promise<string[]> {
    const params = provider ? `?provider=${encodeURIComponent(provider)}` : '';
    const res = await this.client.get<{ models: string[] }>(`/api/models${params}`);
    return res.data.models || [];
  }

  async getConfig(): Promise<Config> {
    const res = await this.client.get<{ config: Config }>('/api/config');
    return res.data.config;
  }

  async getSessions(): Promise<Session[]> {
    const res = await this.client.get<{ sessions: Session[] }>('/api/sessions');
    return res.data.sessions || [];
  }

  async createSession(): Promise<{ session_id: string }> {
    const res = await this.client.post<{ session_id: string }>('/api/sessions');
    return res.data;
  }

  async loadSession(sessionId: string): Promise<{
    session_id: string;
    title: string | null;
    messages: Message[];
  }> {
    const res = await this.client.get<{
      session_id: string;
      title: string | null;
      messages: Message[];
    }>(`/api/sessions/${sessionId}`);
    return res.data;
  }

  async deleteSession(sessionId: string): Promise<void> {
    await this.client.delete(`/api/sessions/${sessionId}`);
  }

  async renameSession(sessionId: string, newTitle: string): Promise<void> {
    await this.client.post('/api/sessions/rename', { session_id: sessionId, new_title: newTitle });
  }

  async getPreferences(): Promise<{ model?: string; provider?: string }> {
    const res = await this.client.get<{ model?: string; provider?: string }>('/api/preferences');
    return res.data;
  }

  async setPreferences(prefs: { model: string; provider?: string }): Promise<void> {
    await this.client.post('/api/preferences', prefs);
  }

  async healthCheck(): Promise<boolean> {
    try {
      const res = await this.client.get('/api/health');
      return res.status === 200;
    } catch {
      return false;
    }
  }

  async getStatus(): Promise<{ locked: boolean; needs_setup: boolean }> {
    const res = await this.client.get<{ locked: boolean; needs_setup: boolean }>('/api/status');
    return res.data;
  }

  async unlock(password: string): Promise<void> {
    const res = await this.client.post<{ status: string; token?: string }>('/api/unlock', {
      password,
    });
    if (res.data.token) {
      this.setUnlockToken(res.data.token);
    }
  }

  async setup(password: string, confirm: string): Promise<void> {
    const res = await this.client.post<{ status: string; token?: string }>('/api/setup', {
      password,
      confirm,
    });
    if (res.data.token) {
      this.setUnlockToken(res.data.token);
    }
  }

  async logout(): Promise<void> {
    try {
      await this.client.post('/api/logout');
    } finally {
      this.unlockToken = null;
    }
  }

  async changePassword(
    currentPassword: string,
    newPassword: string,
    confirm: string
  ): Promise<void> {
    await this.client.post('/api/change-password', {
      current_password: currentPassword,
      new_password: newPassword,
      confirm,
    });
  }
}

export const api = new ApiClient();
