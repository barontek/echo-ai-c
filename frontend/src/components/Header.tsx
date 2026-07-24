import { memo, useState } from 'react';
import { useChat } from '../context';
import { api } from '../api/client';
import { ChangePasswordDialog } from './ChangePasswordDialog';

interface HeaderProps {
  onLogout?: () => void;
}

function getInitialTheme(): 'dark' | 'light' {
  const saved = localStorage.getItem('theme');
  if (saved === 'light' || saved === 'dark') {
    document.documentElement.setAttribute('data-theme', saved);
    return saved;
  }
  return 'dark';
}

export const Header = memo(function Header({ onLogout }: HeaderProps) {
  const chat = useChat();
  const [showDebug, setShowDebug] = useState(false);
  const [showChangePassword, setShowChangePassword] = useState(false);
  const [theme, setTheme] = useState<'dark' | 'light'>(getInitialTheme);
  const [copyMsg, setCopyMsg] = useState<string | null>(null);

  const toggleTheme = () => {
    const newTheme = theme === 'dark' ? 'light' : 'dark';
    setTheme(newTheme);
    document.documentElement.setAttribute('data-theme', newTheme);
    localStorage.setItem('theme', newTheme);
  };

  const statusText = {
    connected: 'Connected',
    connecting: 'Connecting...',
    disconnected: 'Disconnected',
    reconnecting: 'Reconnecting...',
  }[chat.connectionStatus];

  return (
    <>
      <div className="chat-header">
        <div className="header-left">
          <button className="menu-button" onClick={() => chat.setSidebarOpen(!chat.sidebarOpen)}>
            Menu
          </button>
          <span className="model-badge">{chat.currentModel}</span>
        </div>
        <div className="header-right">
          <div className="connection-status">
            <span
              className={`status-dot ${chat.connectionStatus === 'connected' ? '' : 'disconnected'}`}
            ></span>
            <span>{statusText}</span>
          </div>
          {onLogout && (
            <button className="header-icon-btn" onClick={onLogout} title="Lock database">
              🔒
            </button>
          )}
          <button className="header-icon-btn" onClick={toggleTheme} title="Toggle theme">
            {theme === 'dark' ? '☀' : '☾'}
          </button>
          <button
            className="header-icon-btn"
            onClick={() => setShowDebug(!showDebug)}
            title="Debug"
          >
            ⚙
          </button>
        </div>
      </div>
      {showDebug && (
        <div className="debug-panel">
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '8px' }}>
            <strong>Debug Panel</strong>
            <button
              onClick={() => setShowDebug(false)}
              style={{
                background: 'transparent',
                border: 'none',
                color: 'var(--text-primary)',
                cursor: 'pointer',
              }}
            >
              x
            </button>
          </div>
          <div style={{ marginBottom: '8px' }}>
            <strong>State:</strong>
            <pre style={{ margin: '4px 0', fontSize: '10px', whiteSpace: 'pre-wrap' }}>
              {JSON.stringify(
                {
                  isConnected: chat.isConnected,
                  isStreaming: chat.isStreaming,
                  sessionId: chat.activeSessionId,
                  messages: chat.messages.length,
                  model: chat.currentModel,
                },
                null,
                2
              )}
            </pre>
          </div>
          <div style={{ marginBottom: '8px' }}>
            <button
              disabled={!chat.activeSessionId || chat.messages.length === 0}
              onClick={async () => {
                setCopyMsg(null);
                try {
                  const headers: Record<string, string> = {};
                  const token = api.unlockTokenValue;
                  if (token) headers['X-Unlock-Token'] = token;
                  const res = await fetch(`/api/sessions/${chat.activeSessionId}/debug-export`, {
                    headers,
                  });
                  if (!res.ok) {
                    const body = await res.json().catch(() => null);
                    throw new Error(body?.detail || `HTTP ${res.status}`);
                  }
                  const data = await res.json();
                  await navigator.clipboard.writeText(JSON.stringify(data, null, 2));
                  setCopyMsg('Copied to clipboard');
                } catch (err) {
                  setCopyMsg(`Failed: ${err instanceof Error ? err.message : 'Unknown error'}`);
                }
                setTimeout(() => setCopyMsg(null), 3000);
              }}
              style={{
                padding: '4px 8px',
                background:
                  chat.activeSessionId && chat.messages.length > 0 ? 'var(--accent)' : '#666',
                color: 'white',
                border: 'none',
                borderRadius: '4px',
                cursor:
                  chat.activeSessionId && chat.messages.length > 0 ? 'pointer' : 'not-allowed',
                fontSize: '11px',
                width: '100%',
                opacity: chat.activeSessionId && chat.messages.length > 0 ? 1 : 0.5,
              }}
            >
              Copy session JSON
            </button>
            {copyMsg && (
              <div
                style={{
                  marginTop: '4px',
                  fontSize: '10px',
                  color: copyMsg.startsWith('Failed') ? '#ff6b6b' : '#8f8',
                }}
              >
                {copyMsg}
              </div>
            )}
          </div>

          <div>
            <strong>Recent Logs:</strong>
            <div style={{ color: 'var(--text-muted)', fontSize: '9px' }}>Enable debug logging</div>
          </div>

          <hr style={{ border: 'none', borderTop: '1px solid var(--border)', margin: '8px 0' }} />

          <button
            onClick={() => {
              setShowDebug(false);
              setShowChangePassword(true);
            }}
            style={{
              padding: '6px 8px',
              background: 'var(--accent)',
              color: 'white',
              border: 'none',
              borderRadius: '4px',
              cursor: 'pointer',
              fontSize: '11px',
              width: '100%',
            }}
          >
            Change Password
          </button>
        </div>
      )}

      {showChangePassword && <ChangePasswordDialog onClose={() => setShowChangePassword(false)} />}
    </>
  );
});
