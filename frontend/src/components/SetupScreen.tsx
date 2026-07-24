import { useState, type FormEvent } from 'react';
import { api } from '../api/client';

interface Props {
  onComplete: () => void;
}

export function SetupScreen({ onComplete }: Props) {
  const [password, setPassword] = useState('');
  const [confirm, setConfirm] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);

  function validate(): string | null {
    if (password.length < 8) {
      return 'Password must be at least 8 characters long.';
    }
    if (password !== confirm) {
      return 'Passwords do not match.';
    }
    return null;
  }

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    const validationError = validate();
    if (validationError) {
      setError(validationError);
      return;
    }

    setError(null);
    setSubmitting(true);
    try {
      await api.setup(password, confirm);
      setPassword('');
      setConfirm('');
      onComplete();
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Unknown error';
      if (msg.includes('409') || msg.toLowerCase().includes('already initialized')) {
        setError('Database already initialized. Please use the unlock screen.');
      } else if (msg.includes('400') || msg.toLowerCase().includes('passwords do not match')) {
        setError('Passwords do not match.');
      } else if (msg.toLowerCase().includes('8 characters')) {
        setError('Password must be at least 8 characters long.');
      } else {
        setError('Setup failed. Please try again.');
      }
      setPassword('');
      setConfirm('');
    } finally {
      setSubmitting(false);
    }
  }

  return (
    <div className="unlock-overlay">
      <div className="unlock-dialog">
        <div className="unlock-title">Echo AI</div>
        <div className="unlock-subtitle">Create a database password</div>

        <div className="setup-note">
          This password encrypts your conversation history. If you forget it, your existing sessions
          cannot be recovered.
        </div>

        <form onSubmit={handleSubmit}>
          <input
            className="unlock-input"
            type="password"
            placeholder="Password (min. 8 characters)"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoFocus
            disabled={submitting}
          />

          <input
            className="unlock-input setup-confirm"
            type="password"
            placeholder="Confirm password"
            value={confirm}
            onChange={(e) => setConfirm(e.target.value)}
            disabled={submitting}
          />

          {error && <div className="unlock-error">{error}</div>}

          <button
            className="unlock-submit"
            type="submit"
            disabled={submitting || !password || !confirm}
          >
            {submitting ? 'Setting up…' : 'Set Password'}
          </button>
        </form>
      </div>
    </div>
  );
}
