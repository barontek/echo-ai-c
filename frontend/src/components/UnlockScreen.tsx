import { useState, type FormEvent } from 'react';
import { api } from '../api/client';

interface Props {
  onUnlocked: () => void;
}

/**
 * UnlockScreen - password gate shown when the vault is locked.
 *
 * @param props.onUnlocked - Called after a successful unlock.
 *
 * Submits to api.unlock(); maps the '__BACKEND_UNREACHABLE__' sentinel
 * to a human "Backend is not running" message. Owns no effects.
 */
export function UnlockScreen({ onUnlocked }: Props) {
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    if (!password) return;
    setError(null);
    setSubmitting(true);
    try {
      await api.unlock(password);
      setPassword('');
      onUnlocked();
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Unknown error';
      if (msg === '__BACKEND_UNREACHABLE__') {
        setError('Backend is not running');
      } else if (msg.toLowerCase().includes('429') || msg.toLowerCase().includes('too many')) {
        setError('Too many attempts, please wait.');
      } else {
        setError('Incorrect password');
      }
      setPassword('');
    } finally {
      setSubmitting(false);
    }
  }

  return (
    <div className="unlock-overlay">
      <div className="unlock-dialog">
        <div className="unlock-title">Echo AI</div>
        <div className="unlock-subtitle">Enter your database password to continue</div>

        <form onSubmit={handleSubmit}>
          <input
            className="unlock-input"
            type="password"
            placeholder="Database password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoFocus
            disabled={submitting}
          />

          {error && <div className="unlock-error">{error}</div>}

          <button className="unlock-submit" type="submit" disabled={submitting || !password}>
            {submitting ? 'Unlocking…' : 'Unlock'}
          </button>
        </form>
      </div>
    </div>
  );
}
