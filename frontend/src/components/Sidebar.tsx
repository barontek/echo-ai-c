import { memo, useState, useEffect, useRef, useCallback } from 'react';
import { useChat } from '../context';
import { api } from '../api/client';

const OPENAI_AUTH_POLL_MS = 1000;
const OPENAI_AUTH_TIMEOUT_MS = 5 * 60 * 1000;

type OpenAIAuthAttempt = {
  generation: number;
  controller: AbortController;
  loginId?: string;
  loginWindow: Window | null;
  signedIn: boolean;
};

function waitForOpenAIAuthPoll(signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(new DOMException('OpenAI login cancelled', 'AbortError'));
      return;
    }
    const timer = window.setTimeout(() => {
      signal.removeEventListener('abort', onAbort);
      resolve();
    }, OPENAI_AUTH_POLL_MS);
    const onAbort = () => {
      window.clearTimeout(timer);
      reject(new DOMException('OpenAI login cancelled', 'AbortError'));
    };
    signal.addEventListener('abort', onAbort, { once: true });
  });
}

function getOpenAIAuthError(error: unknown): string {
  if (!(error instanceof Error)) return 'OpenAI login failed. Please try again.';
  if (error.message === '__BACKEND_UNREACHABLE__') {
    return 'Cannot reach Echo. Check that the server is running and try again.';
  }
  return error.message || 'OpenAI login failed. Please try again.';
}

/**
 * Sidebar - session list, rename/delete controls, and the OpenAI OAuth
 * sign-in flow.
 *
 * Owns the OpenAI login popup + poll loop: on unmount it aborts any
 * in-flight poll and cancels the auth attempt (mountedRef gate), so no
 * setState happens after teardown. Also adds/removes a keydown listener
 * for closing the delete confirmation with Escape.
 */
export const Sidebar = memo(function Sidebar() {
  const {
    sessions,
    activeSessionId,
    models,
    currentModel,
    currentProvider,
    providers,
    selectSession,
    createSession,
    selectModel,
    selectProvider,
    deleteSession,
    renameSession,
    sidebarOpen,
    setSidebarOpen,
  } = useChat();
  const [searchTerm, setSearchTerm] = useState('');
  const [showModelDropdown, setShowModelDropdown] = useState(false);
  const [showProviderDropdown, setShowProviderDropdown] = useState(false);
  const [deleteConfirm, setDeleteConfirm] = useState<string | null>(null);
  const [renamingId, setRenamingId] = useState<string | null>(null);
  const [renameValue, setRenameValue] = useState('');
  const [openAIAuthBusy, setOpenAIAuthBusy] = useState(false);
  const [openAISignOutBusy, setOpenAISignOutBusy] = useState(false);
  const [openAIAuthError, setOpenAIAuthError] = useState<string | null>(null);
  const renameInputRef = useRef<HTMLInputElement>(null);
  const authGenerationRef = useRef(0);
  const authAttemptRef = useRef<OpenAIAuthAttempt | null>(null);
  const mountedRef = useRef(true);

  const filteredSessions = !searchTerm
    ? sessions
    : sessions.filter((s) => s.title?.toLowerCase().includes(searchTerm.toLowerCase()));

  const handleDelete = (sessionId: string) => {
    deleteSession(sessionId);
    setDeleteConfirm(null);
  };

  const handleSelectSession = (id: string) => {
    selectSession(id);
    setSidebarOpen(false);
  };

  const handleStartRename = (
    sessionId: string,
    currentTitle: string | null,
    e: React.MouseEvent
  ) => {
    e.stopPropagation();
    setRenamingId(sessionId);
    setRenameValue(currentTitle || '');
  };

  const handleFinishRename = () => {
    if (renamingId && renameValue.trim()) {
      renameSession(renamingId, renameValue.trim());
    }
    setRenamingId(null);
    setRenameValue('');
  };

  const cancelOpenAIAuthAttempt = useCallback(() => {
    const attempt = authAttemptRef.current;
    if (!attempt) return;

    authGenerationRef.current++;
    authAttemptRef.current = null;
    attempt.controller.abort();
    attempt.loginWindow?.close();
    if (attempt.loginId && !attempt.signedIn) {
      void api.logoutOpenAIOAuth(attempt.loginId).catch(() => {});
    }
    if (mountedRef.current) setOpenAIAuthBusy(false);
  }, []);

  const handleSelectProvider = async (provider: string) => {
    if (openAISignOutBusy) return;
    if (provider !== 'openai') {
      cancelOpenAIAuthAttempt();
      setOpenAIAuthError(null);
      setShowProviderDropdown(false);
      try {
        await selectProvider(provider);
      } catch (error: unknown) {
        if (mountedRef.current) setOpenAIAuthError(getOpenAIAuthError(error));
      }
      return;
    }

    if (authAttemptRef.current) return;

    setOpenAIAuthError(null);
    setOpenAIAuthBusy(true);
    setShowProviderDropdown(false);
    const loginWindow = window.open('about:blank', 'echo-ai-openai-login');
    const generation = ++authGenerationRef.current;
    const controller = new AbortController();
    const attempt: OpenAIAuthAttempt = {
      generation,
      controller,
      loginWindow,
      signedIn: false,
    };
    authAttemptRef.current = attempt;
    const isCurrent = () =>
      mountedRef.current &&
      authAttemptRef.current === attempt &&
      authGenerationRef.current === generation;

    try {
      const status = await api.getOpenAIOAuthStatus(undefined, controller.signal);
      if (!isCurrent()) return;
      if (status.state === 'signed_in') {
        attempt.signedIn = true;
        loginWindow?.close();
        await selectProvider(provider);
        if (!isCurrent()) return;
        authAttemptRef.current = null;
        setOpenAIAuthBusy(false);
        return;
      }
      if (status.state === 'pending') {
        throw new Error('Another OpenAI login is already in progress. Wait for it to finish or cancel it.');
      }

      if (!loginWindow) {
        throw new Error('Your browser blocked the OpenAI login popup. Allow popups and try again.');
      }

      const login = await api.startOpenAIOAuth(controller.signal);
      attempt.loginId = login.login_id;
      if (!isCurrent()) {
        void api.logoutOpenAIOAuth(login.login_id).catch(() => {});
        return;
      }
      loginWindow.location.href = login.authorization_url;

      const deadline = Date.now() + OPENAI_AUTH_TIMEOUT_MS;
      while (Date.now() < deadline) {
        await waitForOpenAIAuthPoll(controller.signal);
        if (!isCurrent()) return;
        const next = await api.getOpenAIOAuthStatus(login.login_id, controller.signal);
        if (!isCurrent()) return;
        if (next.state === 'signed_in') {
          attempt.signedIn = true;
          loginWindow.close();
          await selectProvider(provider);
          if (!isCurrent()) return;
          authAttemptRef.current = null;
          setOpenAIAuthBusy(false);
          return;
        }
        if (next.state === 'signed_out') {
          throw new Error(next.error || 'OpenAI sign-in did not complete. Please try again.');
        }
        if (loginWindow.closed) {
          throw new Error('The OpenAI login window was closed before sign-in completed.');
        }
      }
      throw new Error('OpenAI login timed out. Please try again.');
    } catch (err: unknown) {
      if (!isCurrent()) return;
      authAttemptRef.current = null;
      authGenerationRef.current++;
      controller.abort();
      loginWindow?.close();
      if (attempt.loginId && !attempt.signedIn) {
        void api.logoutOpenAIOAuth(attempt.loginId).catch(() => {});
      }
      setOpenAIAuthError(getOpenAIAuthError(err));
      setOpenAIAuthBusy(false);
    }
  };

  const handleOpenAISignOut = async () => {
    cancelOpenAIAuthAttempt();
    setOpenAISignOutBusy(true);
    setOpenAIAuthError(null);
    try {
      const fallback = providers.find((provider) => provider !== 'openai');
      const fallbackModels = fallback ? await api.getModels(fallback) : [];
      const validFallbackModels = fallbackModels.filter(
        (model): model is string => typeof model === 'string' && model.trim().length > 0
      );
      if (fallback && validFallbackModels.length === 0) {
        throw new Error(`No models are available for ${fallback}.`);
      }
      await api.logoutOpenAIOAuth();
      if (fallback) await selectProvider(fallback, validFallbackModels);
    } catch (error: unknown) {
      if (mountedRef.current) setOpenAIAuthError(getOpenAIAuthError(error));
    } finally {
      if (mountedRef.current) setOpenAISignOutBusy(false);
    }
  };

  const handleRenameKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') {
      handleFinishRename();
    } else if (e.key === 'Escape') {
      setRenamingId(null);
      setRenameValue('');
    }
  };

  // Focus the rename input when it appears and select text
  useEffect(() => {
    if (renamingId && renameInputRef.current) {
      renameInputRef.current.focus();
      renameInputRef.current.select();
    }
  }, [renamingId]);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      cancelOpenAIAuthAttempt();
    };
  }, [cancelOpenAIAuthAttempt]);

  // Close delete confirmation on Escape
  useEffect(() => {
    if (!deleteConfirm) return;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setDeleteConfirm(null);
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
  }, [deleteConfirm]);

  return (
    <>
      {sidebarOpen && <div className="sidebar-overlay" onClick={() => setSidebarOpen(false)} />}
      <div className={`sidebar ${sidebarOpen ? 'open' : ''}`}>
        <div className="sidebar-header">
          <h2>Echo AI</h2>
        </div>

        <div className="model-selector">
          <button
            className="model-button"
            onClick={() => setShowModelDropdown(!showModelDropdown)}
            aria-expanded={showModelDropdown}
            aria-haspopup="listbox"
            aria-label="Select model"
          >
            <span className="model-name">{currentModel}</span>
            <span className="dropdown-arrow">▼</span>
          </button>
          {showModelDropdown && (
            <div className="model-dropdown" role="listbox" aria-label="Available models">
              {models.map((m) => (
                <button
                  key={m}
                  role="option"
                  aria-selected={m === currentModel}
                  className={`model-option ${m === currentModel ? 'active' : ''}`}
                  onClick={() => {
                    selectModel(m);
                    setShowModelDropdown(false);
                  }}
                >
                  {m}
                </button>
              ))}
            </div>
          )}
        </div>

        <div className="model-selector">
          <button
            className="model-button"
            onClick={() => setShowProviderDropdown(!showProviderDropdown)}
            aria-expanded={showProviderDropdown}
            aria-haspopup="listbox"
            aria-label="Select provider"
            disabled={openAISignOutBusy}
          >
            <span className="model-name">{currentProvider}</span>
            <span className="dropdown-arrow">▼</span>
          </button>
          {showProviderDropdown && (
            <div className="model-dropdown" role="listbox" aria-label="Available providers">
              {providers.map((p) => (
                <button
                  key={p}
                  role="option"
                  aria-selected={p === currentProvider}
                  className={`model-option ${p === currentProvider ? 'active' : ''}`}
                  onClick={() => {
                    void handleSelectProvider(p);
                  }}
                >
                  {p}
                </button>
              ))}
            </div>
          )}
        </div>

        {openAIAuthBusy && (
          <div className="provider-auth-notice" role="status">
            Complete the OpenAI login in your browser...
          </div>
        )}
        {currentProvider === 'openai' && !openAIAuthBusy && (
          <button
            className="provider-auth-notice"
            disabled={openAISignOutBusy}
            onClick={() => void handleOpenAISignOut()}
          >
            {openAISignOutBusy ? 'Signing out of OpenAI...' : 'Sign out of OpenAI'}
          </button>
        )}
        {openAIAuthError && (
          <div className="provider-auth-error" role="alert">
            {openAIAuthError}
          </div>
        )}

        <button className="new-chat-button" onClick={createSession}>
          <span>+</span> New Chat
        </button>

        <div className="search-container">
          <input
            id="search-conversations"
            type="text"
            className="search-input"
            placeholder="Search conversations..."
            value={searchTerm}
            onChange={(e) => setSearchTerm(e.target.value)}
          />
        </div>

        <div className="sessions-list">
          {filteredSessions.map((session) => (
            <div
              key={session.id}
              className={`session-item ${session.id === activeSessionId ? 'active' : ''}`}
              onClick={() => handleSelectSession(session.id)}
            >
              {renamingId === session.id ? (
                <input
                  ref={renameInputRef}
                  className="rename-input"
                  value={renameValue}
                  onChange={(e) => setRenameValue(e.target.value)}
                  onBlur={handleFinishRename}
                  onKeyDown={handleRenameKeyDown}
                  onClick={(e) => e.stopPropagation()}
                />
              ) : (
                <span className="session-title">{session.title || 'New Chat'}</span>
              )}
              <div className="session-actions">
                {renamingId !== session.id && (
                  <button
                    className="rename-button"
                    onClick={(e) => handleStartRename(session.id, session.title, e)}
                    title="Rename"
                  >
                    ✎
                  </button>
                )}
                <button
                  className="delete-button"
                  onClick={(e) => {
                    e.stopPropagation();
                    setDeleteConfirm(session.id);
                  }}
                >
                  ×
                </button>
              </div>
            </div>
          ))}
          {filteredSessions.length === 0 && (
            <div className="empty-state" style={{ padding: '20px', fontSize: '13px' }}>
              {searchTerm
                ? `No matching conversations for "${searchTerm}"`
                : sessions.length === 0
                  ? 'No conversations yet'
                  : 'No matching conversations'}
            </div>
          )}
        </div>

        {deleteConfirm && (
          <div className="confirm-overlay" onClick={() => setDeleteConfirm(null)}>
            <div className="confirm-dialog" onClick={(e) => e.stopPropagation()}>
              <p>Delete this conversation?</p>
              <div className="confirm-actions">
                <button className="confirm-cancel" onClick={() => setDeleteConfirm(null)}>
                  Cancel
                </button>
                <button className="confirm-delete" onClick={() => handleDelete(deleteConfirm)}>
                  Delete
                </button>
              </div>
            </div>
          </div>
        )}
      </div>
    </>
  );
});
