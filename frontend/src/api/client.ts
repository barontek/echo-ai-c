import axios, { AxiosError } from 'axios';
import type { Session, Config, Message, ApiError } from '../types';

const API_BASE = ''; // Use relative path - goes through Vite proxy in dev

const API_TIMEOUT = Math.max(
  1000,
  parseInt(import.meta.env.VITE_API_TIMEOUT || '10000', 10) || 10000
);

const UNLOCK_TOKEN_HEADER = 'X-Unlock-Token';
const UNLOCK_TOKEN_STORAGE_KEY = 'echo-ai-unlock-token';

type TokenExpiredCallback = () => void;

/**
 * ApiClient - thin axios wrapper around the echo-ai backend REST API.
 *
 * Owns the unlock token (mirrored into localStorage under the
 * 'echo-ai-unlock-token' key, so a page refresh stays unlocked — this
 * state is browser-only and does NOT exist server-side).
 *
 * Failure convention: every method except `healthCheck` throws an
 * `Error`. The message is the backend's `error` field when present. If
 * the backend is unreachable (no HTTP response at all), the thrown
 * message is exactly `'__BACKEND_UNREACHABLE__'` — consumers pattern
 * match on that sentinel string. A 401 additionally clears the token and
 * fires the `setOnTokenExpired` callback.
 */
class ApiClient {
  private client;
  private unlockToken: string | null = null;
  private onTokenExpired: TokenExpiredCallback | null = null;

  constructor() {
    /* Restore the token from a previous session so a page refresh keeps the
     * user unlocked; the server stays STATE_UNLOCKED across reloads. */
    this.unlockToken = localStorage.getItem(UNLOCK_TOKEN_STORAGE_KEY);

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
          this.clearUnlockToken();
          if (this.onTokenExpired) {
            this.onTokenExpired();
          }
        }

        return Promise.reject(new Error(message));
      }
    );
  }

  /**
   * Register a callback invoked when the backend rejects the unlock token
   * with a 401 (the token is cleared first).
   *
   * @param cb - Callback to run on expiry; replaces any previous one.
   * @returns void. Never throws.
   */
  setOnTokenExpired(cb: TokenExpiredCallback): void {
    this.onTokenExpired = cb;
  }

  /**
   * Whether an unlock token is currently held (memory or localStorage).
   * Note this is client-side bookkeeping only — the token may already be
   * rejected by the server; `getStatus()` is the source of truth.
   */
  get isUnlocked(): boolean {
    return this.unlockToken !== null;
  }

  /**
   * The current unlock token, or null when none is held (fresh load with
   * no stored token, or after clearUnlockToken()).
   *
   */
  get unlockTokenValue(): string | null {
    return this.unlockToken;
  }

  /**
   * Store the unlock token from a setup or unlock response, mirroring it
   * into localStorage so a page refresh stays unlocked.
   *
   * @param token - The token issued by the backend.
   * @returns void.
   */
  private setUnlockToken(token: string): void {
    this.unlockToken = token;
    localStorage.setItem(UNLOCK_TOKEN_STORAGE_KEY, token);
  }

  /**
   * Forget the unlock token in memory and in localStorage. Called on 401
   * and on logout; safe to call repeatedly.
   *
   * @returns void. Never throws.
   */
  clearUnlockToken(): void {
    this.unlockToken = null;
    localStorage.removeItem(UNLOCK_TOKEN_STORAGE_KEY);
  }

  /**
   * Fetch the available model names for a provider.
   *
   * @param provider - Optional provider name filter (e.g. 'ollama');
   *   omitted to list models for the default provider.
   * @param signal - AbortSignal to cancel the request on unmount.
   * @returns The model name list (empty array when the backend returns
   *   none — never null).
   * @throws {Error} Per the class contract; '`__BACKEND_UNREACHABLE__`'
   *   when the backend is down.
   */
  async getModels(provider?: string, signal?: AbortSignal): Promise<string[]> {
    const params = provider ? `?provider=${encodeURIComponent(provider)}` : '';
    const res = await this.client.get<{ models: string[] }>(`/api/models${params}`, { signal });
    return res.data.models || [];
  }

  /**
   * Fetch the provider catalog: names, which support "effort" levels, and
   * each provider's effort options.
   *
   * @returns Provider names, effort-supported flags, and the effort
   *   options map; absent backend fields default to [] / {} — never null.
   * @throws {Error} Per the class contract.
   */
  async getProviders(): Promise<{
    providers: string[];
    effortSupported: string[];
    effortOptions: Record<string, string[]>;
  }> {
    const res = await this.client.get<{
      providers: string[];
      effort_supported?: string[];
      effort_options?: Record<string, string[]>;
    }>('/api/providers');
    return {
      providers: res.data.providers || [],
      effortSupported: res.data.effort_supported || [],
      effortOptions: res.data.effort_options || {},
    };
  }

  /**
   * Fetch the OpenAI OAuth login state.
   *
   * @param loginId - Optional id of a started login; when present the
   *   backend reports that login's progress instead of the global state.
   * @param signal - AbortSignal to cancel on unmount.
   * @returns `{ state }` where state is 'signed_out' | 'pending' |
   *   'signed_in'; `error` is present only when the backend reports one.
   *   `account_id`/`plan_type` are present only when signed in.
   * @throws {Error} Per the class contract.
   */
  async getOpenAIOAuthStatus(
    loginId?: string,
    signal?: AbortSignal
  ): Promise<{
    state: 'signed_out' | 'pending' | 'signed_in';
    account_id?: string;
    plan_type?: string;
    error?: string;
  }> {
    const res = await this.client.get<{
      state: 'signed_out' | 'pending' | 'signed_in';
      account_id?: string;
      plan_type?: string;
      error?: string;
    }>('/api/auth/openai/status', {
      params: loginId ? { login_id: loginId } : undefined,
      signal,
    });
    return res.data;
  }

  /**
   * Start an OpenAI OAuth login, returning the authorization URL to open
   * in a popup and a login id to poll with getOpenAIOAuthStatus().
   *
   * @param signal - AbortSignal to cancel on unmount.
   * @returns `{ authorization_url, login_id }`.
   * @throws {Error} Per the class contract.
   */
  async startOpenAIOAuth(
    signal?: AbortSignal
  ): Promise<{ authorization_url: string; login_id: string }> {
    const res = await this.client.post<{ authorization_url: string; login_id: string }>(
      '/api/auth/openai/start',
      undefined,
      { signal }
    );
    return res.data;
  }

  /**
   * Sign out of OpenAI OAuth.
   *
   * @param loginId - Optional login id to sign out; omitted signs out the
   *   current account.
   * @param signal - AbortSignal to cancel on unmount.
   * @returns void.
   * @throws {Error} Per the class contract.
   */
  async logoutOpenAIOAuth(loginId?: string, signal?: AbortSignal): Promise<void> {
    await this.client.post('/api/auth/openai/logout', loginId ? { login_id: loginId } : undefined, {
      signal,
    });
  }

  /**
   * Fetch the server config (settings the frontend renders).
   *
   * @returns The full {@link Config}.
   * @throws {Error} Per the class contract.
   */
  async getConfig(): Promise<Config> {
    const res = await this.client.get<{ config: Config }>('/api/config');
    return res.data.config;
  }

  /**
   * Fetch the session list (ids, titles, creation timestamps).
   *
   * @returns The sessions; empty array when none exist — never null.
   * @throws {Error} Per the class contract.
   */
  async getSessions(): Promise<Session[]> {
    const res = await this.client.get<{ sessions: Session[] }>('/api/sessions');
    return res.data.sessions || [];
  }

  /**
   * Create a new empty session.
   *
   * @returns `{ session_id }` of the created session.
   * @throws {Error} Per the class contract.
   */
  async createSession(): Promise<{ session_id: string }> {
    const res = await this.client.post<{ session_id: string }>('/api/sessions');
    return res.data;
  }

  /**
   * Fetch one session's full record (messages plus branch metadata).
   *
   * @param sessionId - Id of the session to load.
   * @returns The session record. `title` is null when the session was
   *   never titled (backend behavior — not an error); `branches` is
   *   present only when the session has been forked.
   * @throws {Error} Per the class contract; a 404 means the session was
   *   deleted server-side.
   */
  async loadSession(sessionId: string): Promise<{
    session_id: string;
    title: string | null;
    messages: Message[];
    branches?: Array<{
      message_id: string;
      count: number;
      active: number;
      branch_ids?: string[];
    }>;
  }> {
    const res = await this.client.get<{
      session_id: string;
      title: string | null;
      messages: Message[];
      branches?: Array<{
        message_id: string;
        count: number;
        active: number;
        branch_ids?: string[];
      }>;
    }>(`/api/sessions/${sessionId}`);
    return res.data;
  }

  /**
   * Delete a session permanently.
   *
   * @param sessionId - Id of the session to delete.
   * @returns void.
   * @throws {Error} Per the class contract.
   */
  async deleteSession(sessionId: string): Promise<void> {
    await this.client.delete(`/api/sessions/${sessionId}`);
  }

  /**
   * Rename a session.
   *
   * @param sessionId - Id of the session.
   * @param newTitle - New title; sent verbatim to the backend.
   * @returns void.
   * @throws {Error} Per the class contract.
   */
  async renameSession(sessionId: string, newTitle: string): Promise<void> {
    await this.client.post('/api/sessions/rename', { session_id: sessionId, new_title: newTitle });
  }

  /**
   * Ping the backend health endpoint. This is the ONE method that does
   * NOT throw: any failure (backend down, 4xx/5xx) resolves to `false`.
   *
   * @returns true only when the backend answered with HTTP 200; false
   *   on every other outcome.
   */
  async healthCheck(): Promise<boolean> {
    try {
      const res = await this.client.get('/api/health');
      return res.status === 200;
    } catch {
      return false;
    }
  }

  /**
   * Fetch the vault status gate.
   *
   * @returns `{ locked, needs_setup }` — `locked` means the vault is
   *   password-protected and must be unlocked; `needs_setup` means no
   *   password exists yet and setup is required.
   * @throws {Error} Per the class contract.
   */
  async getStatus(): Promise<{ locked: boolean; needs_setup: boolean }> {
    const res = await this.client.get<{ locked: boolean; needs_setup: boolean }>('/api/status');
    return res.data;
  }

  /**
   * Unlock the vault with a password. On success the returned token is
   * stored and attached to all subsequent requests.
   *
   * @param password - The vault password.
   * @returns void.
   * @throws {Error} Per the class contract; the backend reports a wrong
   *   password as an error message on the 401/403 path.
   */
  async unlock(password: string): Promise<void> {
    const res = await this.client.post<{ status: string; token?: string }>('/api/unlock', {
      password,
    });
    if (res.data.token) {
      this.setUnlockToken(res.data.token);
    }
  }

  /**
   * Initialize the vault with a new password (first-run setup). On
   * success the returned token is stored like unlock().
   *
   * @param password - The new vault password.
   * @param confirm - Must equal `password`; the backend rejects a
   *   mismatch.
   * @returns void.
   * @throws {Error} Per the class contract.
   */
  async setup(password: string, confirm: string): Promise<void> {
    const res = await this.client.post<{ status: string; token?: string }>('/api/setup', {
      password,
      confirm,
    });
    if (res.data.token) {
      this.setUnlockToken(res.data.token);
    }
  }

  /**
   * Log out: tells the backend and always clears the local token, even
   * when the backend request fails.
   *
   * @returns void; resolves after the token is cleared regardless of the
   *   backend call's outcome (the backend error is swallowed by design).
   */
  async logout(): Promise<void> {
    try {
      await this.client.post('/api/logout');
    } finally {
      this.unlockToken = null;
    }
  }

  /**
   * Change the vault password.
   *
   * @param currentPassword - The current password (server-verified).
   * @param newPassword - The new password.
   * @param confirm - Must equal `newPassword`.
   * @returns void.
   * @throws {Error} Per the class contract; a wrong current password is
   *   reported by the backend as an error.
   */
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
