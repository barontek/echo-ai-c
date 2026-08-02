import { useState } from 'react';
import { useChat } from '../context';

export function AskUserDialog() {
  const { pendingQuestion, resolveAskUser } = useChat();
  const [value, setValue] = useState('');

  if (pendingQuestion === null) return null;

  const answer = (text: string) => {
    resolveAskUser(text);
    setValue('');
  };

  return (
    <div className="approval-overlay" onClick={() => {}}>
      <div className="approval-dialog" onClick={(e) => e.stopPropagation()}>
        <div className="approval-title">Question from agent</div>
        <div className="ask-user-question">{pendingQuestion}</div>

        <div className="approval-section-label">Your answer</div>
        <input
          className="ask-user-input"
          value={value}
          onChange={(e) => setValue(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') answer(value);
          }}
          placeholder="Type your answer..."
          autoFocus
        />

        <div className="approval-actions">
          {/* Cancel replies with an empty answer so the agent unblocks
           * immediately instead of waiting out the 60s timeout. */}
          <button className="approval-deny" onClick={() => answer('')}>
            Cancel
          </button>
          <button className="approval-approve" onClick={() => answer(value)}>
            Send
          </button>
        </div>
      </div>
    </div>
  );
}
