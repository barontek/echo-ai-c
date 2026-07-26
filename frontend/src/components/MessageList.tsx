import { memo, useEffect, useRef, useState } from 'react';
import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { Pencil, Copy, Check } from 'lucide-react';
import { useChat } from '../context';
import { parseThinkBlocks } from '../utils/thinkBlockParser';
import type { ToolCall } from '../types';

interface SearchResult {
  title: string;
  url: string;
  snippet: string;
}

function parseSearchResults(content: string): SearchResult[] {
  try {
    const parsed = JSON.parse(content);
    if (Array.isArray(parsed)) {
      return parsed.map((r) => ({
        title: r.title || '',
        url: r.url || '',
        snippet: r.snippet || '',
      }));
    }
  } catch {}
  return [];
}

function ToolIcon({ name }: { name: string }) {
  const cls = 'tool-icon-svg';
  switch (name) {
    case 'web_search':
    case 'deep_search':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <circle cx="11" cy="11" r="8" />
          <path d="M21 21l-4.35-4.35" />
        </svg>
      );
    case 'web_fetch':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <path d="M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4" />
          <polyline points="7,10 12,15 17,10" />
          <line x1="12" y1="15" x2="12" y2="3" />
        </svg>
      );
    case 'bash':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <polyline points="4,17 10,11 4,5" />
          <line x1="12" y1="19" x2="20" y2="19" />
        </svg>
      );
    case 'read_file':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <path d="M14 2H6a2 2 0 00-2 2v16a2 2 0 002 2h12a2 2 0 002-2V8z" />
          <polyline points="14,2 14,8 20,8" />
          <line x1="16" y1="13" x2="8" y2="13" />
          <line x1="16" y1="17" x2="8" y2="17" />
        </svg>
      );
    case 'write_file':
    case 'replace_in_file':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <path d="M12 20h9" />
          <path d="M16.5 3.5a2.121 2.121 0 013 3L7 19l-4 1 1-4L16.5 3.5z" />
        </svg>
      );
    case 'python_execute':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <polyline points="16,18 22,12 16,6" />
          <polyline points="8,6 2,12 8,18" />
        </svg>
      );
    case 'git':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <circle cx="12" cy="18" r="3" />
          <circle cx="6" cy="6" r="3" />
          <circle cx="18" cy="6" r="3" />
          <path d="M18 9v1a2 2 0 01-2 2H8a2 2 0 01-2-2V9" />
          <path d="M12 15v-3" />
        </svg>
      );
    case 'list_dir':
    case 'glob':
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <path d="M22 19a2 2 0 01-2 2H4a2 2 0 01-2-2V5a2 2 0 012-2h5l2 3h9a2 2 0 012 2z" />
        </svg>
      );
    default:
      return (
        <svg className={cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <path d="M14.7 6.3a1 1 0 000 1.4l1.6 1.6a1 1 0 001.4 0l3.77-3.77a6 6 0 01-7.94 7.94l-6.91 6.91a2.12 2.12 0 01-3-3l6.91-6.91a6 6 0 017.94-7.94l-3.76 3.76z" />
        </svg>
      );
  }
}

function ToolCallEntry({ tc }: { tc: ToolCall }) {
  const searchResults = parseSearchResults(tc.result?.content || '');
  const hasSearchResults = searchResults.length > 0;
  const hasResult = tc.result && (tc.result.content || tc.result.error);
  const inProgress = !hasResult;

  let argsStr = '';
  if (typeof tc.arguments === 'string') {
    try {
      const parsed = JSON.parse(tc.arguments);
      argsStr = JSON.stringify(parsed, null, 2);
    } catch {
      argsStr = tc.arguments;
    }
  } else {
    argsStr = JSON.stringify(tc.arguments, null, 2);
  }

  const label = tc.name.replace(/_/g, ' ');

  return (
    <details className="tool-call-card" open={inProgress}>
      <summary className="tool-call-summary">
        <ToolIcon name={tc.name} />
        <span className="tool-call-label">{label}</span>
        {inProgress && <span className="tool-call-spinner" />}
      </summary>
      <div className="tool-call-body">
        {argsStr && (
          <pre className="tool-call-args">{argsStr}</pre>
        )}
        {hasSearchResults && (
          <div className="search-results">
            <div className="search-results-header">
              <span className="search-results-count">{searchResults.length} results</span>
            </div>
            {searchResults.map((r, i) => (
              <a
                key={i}
                className="search-result-item"
                href={r.url}
                target="_blank"
                rel="noopener noreferrer"
              >
                <span className="search-result-title">{r.title}</span>
                <span className="search-result-url">{r.url}</span>
                {r.snippet && (
                  <span className="search-result-snippet">{r.snippet}</span>
                )}
              </a>
            ))}
          </div>
        )}
        {hasResult && !hasSearchResults && (
          <div className="tool-call-result">
            {tc.result!.error && (
              <div className="tool-call-error">{tc.result!.error}</div>
            )}
            {tc.result!.content && (
              <pre className="tool-call-result-text">{tc.result!.content}</pre>
            )}
          </div>
        )}
      </div>
    </details>
  );
}

export const MessageList = memo(function MessageList() {
  const { messages, isStreaming, editMessage } = useChat();
  const containerRef = useRef<HTMLDivElement>(null);
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editText, setEditText] = useState('');
  const [copiedIndex, setCopiedIndex] = useState<number | null>(null);
  const userScrolledUpRef = useRef(false);
  const thinkingContainerRef = useRef<HTMLDivElement | null>(null);
  const thinkingScrolledUpRef = useRef(false);
  const copyTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const isAtBottom = (el: HTMLElement) => {
    return el.scrollHeight - el.scrollTop - el.clientHeight < 80;
  };

  const handleScroll = () => {
    userScrolledUpRef.current = !isAtBottom(containerRef.current!);
  };

  const handleThinkingScroll = () => {
    const el = thinkingContainerRef.current;
    if (el) thinkingScrolledUpRef.current = !isAtBottom(el);
  };

  useEffect(() => {
    if (!userScrolledUpRef.current && containerRef.current) {
      containerRef.current.scrollTop = containerRef.current.scrollHeight;
    }
    const thinkingEl = thinkingContainerRef.current;
    if (thinkingEl && !thinkingScrolledUpRef.current) {
      thinkingEl.scrollTop = thinkingEl.scrollHeight;
    }
  }, [messages, isStreaming]);

  const handleEditKeyDown = (e: React.KeyboardEvent, idx: number, msgId?: string) => {
    if (e.key === 'Enter' && e.ctrlKey) {
      editMessage(idx, editText, msgId);
      setEditingIndex(null);
    }
    if (e.key === 'Escape') {
      setEditingIndex(null);
    }
  };

  return (
    <div className="message-list" ref={containerRef} onScroll={handleScroll}>
      {messages.length === 0 && !isStreaming && (
        <div className="empty-state">
          <div className="empty-logo">✦</div>
          <h2 className="empty-title">Echo AI</h2>
          <p className="empty-subtitle">Ask me anything. I'm running locally.</p>
        </div>
      )}

      {messages.map((msg, idx) => {
        const isEditing = editingIndex === idx;
        const msgKey = msg.timestamp ? `${msg.role}-${msg.timestamp}-${idx}` : `msg-${idx}`;

        return (
          <div key={msgKey} className={`message message-${msg.role}`}>
            <div className="message-bubble">
              {isEditing && (
                <div className="message-content">
                  <textarea
                    className="edit-input"
                    value={editText}
                    onChange={(e) => setEditText(e.target.value)}
                    autoFocus
                    spellCheck={false}
                    onKeyDown={(e) => handleEditKeyDown(e, idx, msg.id)}
                  />
                </div>
              )}

              {!isEditing && (
                <>
                  <div className="message-content">
                    {(() => {
                      const blocks = parseThinkBlocks(msg.content);
                      const thinkingText = blocks
                        .filter((b) => b.type === 'thinking')
                        .map((b) => b.text)
                        .join('\n');
                      const contentText = blocks
                        .filter((b) => b.type === 'content')
                        .map((b) => b.text)
                        .join('\n');
                      return (
                        <>
                          {thinkingText && (
                            <details className="thinking-collapsible" open>
                              <summary className="thinking-label">Thinking</summary>
                              <div
                                className="markdown-content"
                                ref={
                                  idx === messages.length - 1
                                    ? (el) => {
                                        thinkingContainerRef.current = el;
                                      }
                                    : undefined
                                }
                                onScroll={
                                  idx === messages.length - 1 ? handleThinkingScroll : undefined
                                }
                              >
                                <ReactMarkdown remarkPlugins={[remarkGfm]}>
                                  {thinkingText}
                                </ReactMarkdown>
                              </div>
                            </details>
                          )}
                          {msg.tool_calls && msg.tool_calls.length > 0 && (
                            <div className="tool-calls-section">
                              <div className="tool-calls-label">
                                Using tool{msg.tool_calls.length > 1 ? 's' : ''}
                              </div>
                              {msg.tool_calls.map((tc, i) => (
                                <ToolCallEntry key={i} tc={tc} />
                              ))}
                            </div>
                          )}
                          {contentText && (
                            <div className="markdown-content">
                              <ReactMarkdown remarkPlugins={[remarkGfm]}>
                                {contentText}
                              </ReactMarkdown>
                            </div>
                          )}
                        </>
                      );
                    })()}
                    {msg.error && <div className="message-error">{msg.error}</div>}
                    {isStreaming && idx === messages.length - 1 && (
                      <div className="typing-indicator">
                        <span></span>
                        <span></span>
                        <span></span>
                      </div>
                    )}
                  </div>
                  <div className="message-footer">
                    {msg.timestamp && <div className="message-time">{msg.timestamp}</div>}
                    {msg.role === 'assistant' && msg.content && (
                      <button
                        className="icon-button"
                        onClick={() => {
                          const doCopy = () => {
                            setCopiedIndex(idx);
                            if (copyTimeoutRef.current) clearTimeout(copyTimeoutRef.current);
                            copyTimeoutRef.current = setTimeout(() => {
                              setCopiedIndex(null);
                              copyTimeoutRef.current = null;
                            }, 2000);
                          };
                          if (navigator.clipboard) {
                            navigator.clipboard
                              .writeText(msg.content)
                              .then(doCopy)
                              .catch((err) => {
                                console.error('Copy failed:', err);
                              });
                          } else {
                            const textarea = document.createElement('textarea');
                            textarea.value = msg.content;
                            textarea.style.position = 'fixed';
                            textarea.style.opacity = '0';
                            document.body.appendChild(textarea);
                            textarea.select();
                            try {
                              document.execCommand('copy');
                              doCopy();
                            } catch (err) {
                              console.error('Copy failed:', err);
                            }
                            document.body.removeChild(textarea);
                          }
                        }}
                        title="Copy"
                      >
                        {copiedIndex === idx ? <Check size={16} /> : <Copy size={16} />}
                      </button>
                    )}
                    {msg.role === 'user' && !isStreaming && (
                      <button
                        className="icon-button"
                        onClick={() => {
                          setEditingIndex(idx);
                          setEditText(msg.content);
                        }}
                        title="Edit"
                      >
                        <Pencil size={16} />
                      </button>
                    )}
                  </div>
                </>
              )}

              {isEditing && (
                <div className="edit-actions">
                  <button
                    className="edit-save"
                    onClick={() => {
                      editMessage(idx, editText, msg.id);
                      setEditingIndex(null);
                    }}
                  >
                    Regenerate (Ctrl+Enter)
                  </button>
                  <button className="edit-cancel" onClick={() => setEditingIndex(null)}>
                    Cancel (Esc)
                  </button>
                </div>
              )}
            </div>
          </div>
        );
      })}
    </div>
  );
});
