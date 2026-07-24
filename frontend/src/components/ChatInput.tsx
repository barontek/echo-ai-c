import { memo, useState, useRef, useEffect, type KeyboardEvent, type FormEvent } from 'react';
import { Square, ArrowUp } from 'lucide-react';
import { useChat } from '../context';

interface ChatInputProps {
  placeholder?: string;
}

export const ChatInput = memo(function ChatInput({
  placeholder = 'Type your message...',
}: ChatInputProps) {
  const { sendMessage, isConnected, isStreaming, stopGeneration } = useChat();
  const [input, setInput] = useState('');
  const textareaRef = useRef<HTMLTextAreaElement>(null);

  useEffect(() => {
    const el = textareaRef.current;
    if (!el) return;
    el.style.height = 'auto';
    el.style.height = `${Math.min(el.scrollHeight, 120)}px`;
  }, [input]);

  const disabled = !isConnected;
  const placeholderText = isStreaming ? 'Generating...' : placeholder;

  const handleSend = () => {
    const trimmed = input.trim();
    if (trimmed && !disabled) {
      sendMessage(trimmed);
      setInput('');
      if (textareaRef.current) {
        textareaRef.current.style.height = 'auto';
      }
    }
  };

  const handleKeyDown = (e: KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    handleSend();
  };

  return (
    <form className="chat-input-container" onSubmit={handleSubmit}>
      <div className="input-wrapper">
        <textarea
          ref={textareaRef}
          id="chat-input"
          className="chat-input"
          value={input}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={handleKeyDown}
          onFocus={() => {
            const el = document.querySelector<HTMLElement>('.message-list');
            if (el) el.scrollTop = el.scrollHeight;
          }}
          placeholder={placeholderText}
          disabled={disabled || isStreaming}
          rows={1}
        />
        {isStreaming ? (
          <button
            type="button"
            className="icon-button"
            onClick={stopGeneration}
            title="Stop generation"
            aria-label="Stop generation"
          >
            <Square size={18} fill="currentColor" />
          </button>
        ) : (
          <button
            type="submit"
            className="icon-button send-button-icon"
            disabled={disabled || !input.trim()}
          >
            <ArrowUp size={20} />
          </button>
        )}
      </div>
    </form>
  );
});
