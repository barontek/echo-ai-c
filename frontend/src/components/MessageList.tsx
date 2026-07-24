import { memo, useEffect, useRef, useState } from 'react';
import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { Pencil, Copy, Check } from 'lucide-react';
import { useChat } from '../context';
import { parseThinkBlocks } from '../utils/thinkBlockParser';

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
                    {msg.has_tools && msg.tool_calls && msg.tool_calls.length > 0 && (
                      <div className="tool-calls">
                        {msg.tool_calls.map((tc, i) => {
                          const entries = Object.entries(tc.arguments);
                          const argsDisplay = entries
                            .map(([k, v]) => `${k}: ${JSON.stringify(v)}`)
                            .join('\n');
                          return (
                            <details key={i} className="tool-call">
                              <summary className="tool-name">{tc.name}</summary>
                              <pre className="tool-args">{argsDisplay}</pre>
                              {tc.result && (
                                <div className="tool-result">
                                  <div className="tool-result-label">Result:</div>
                                  <pre className="tool-result-content">
                                    {tc.result.content !== undefined && tc.result.content !== null
                                      ? tc.result.content
                                      : tc.result.error || '(empty)'}
                                  </pre>
                                </div>
                              )}
                            </details>
                          );
                        })}
                      </div>
                    )}
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
