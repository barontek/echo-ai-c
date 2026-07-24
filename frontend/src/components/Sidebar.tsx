import { memo, useState, useEffect, useRef } from 'react';
import { useChat } from '../context';

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
  const renameInputRef = useRef<HTMLInputElement>(null);

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
                    selectProvider(p);
                    setShowProviderDropdown(false);
                  }}
                >
                  {p}
                </button>
              ))}
            </div>
          )}
        </div>

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
