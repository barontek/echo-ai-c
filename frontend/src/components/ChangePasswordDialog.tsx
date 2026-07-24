import { useState, type FormEvent } from 'react';
import { api } from '../api/client';

interface Props {
  onClose: () => void;
}

export function ChangePasswordDialog({ onClose }: Props) {
  const [currentPassword, setCurrentPassword] = useState('');
  const [newPassword, setNewPassword] = useState('');
  const [confirm, setConfirm] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState<string | null>(null);

  function validate(): string | null {
    if (newPassword.length < 8) {
      return 'New password must be at least 8 characters long.';
    }
    if (newPassword !== confirm) {
      return 'New passwords do not match.';
    }
    return null;
  }

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    if (confirming) return;
    const validationError = validate();
    if (validationError) {
      setError(validationError);
      return;
    }
    setError(null);
    setConfirming(true);
  }

  async function handleConfirmed() {
    setSubmitting(true);
    setError(null);
    setSuccess(null);
    try {
      await api.changePassword(currentPassword, newPassword, confirm);
      setSuccess('Password changed successfully. All sessions have been re-encrypted.');
      setCurrentPassword('');
      setNewPassword('');
      setConfirm('');
      setConfirming(false);
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Unknown error';
      if (msg.includes('401') || msg.toLowerCase().includes('incorrect')) {
        setError('Current password is incorrect.');
      } else if (msg.includes('400') && msg.toLowerCase().includes('match')) {
        setError('New passwords do not match.');
      } else if (msg.includes('400') && msg.toLowerCase().includes('8 characters')) {
        setError('New password must be at least 8 characters long.');
      } else {
        setError(`Failed to change password: ${msg}`);
      }
      setConfirming(false);
    } finally {
      setSubmitting(false);
    }
  }

  return (
    <div className="unlock-overlay" onClick={onClose}>
      <div className="unlock-dialog" onClick={(e) => e.stopPropagation()}>
        <div className="unlock-title">Change Password</div>
        <div className="unlock-subtitle">This will re-encrypt all stored sessions.</div>

        {confirming ? (
          <div>
            <div
              style={{
                marginBottom: '16px',
                padding: '12px',
                background: 'rgba(239, 68, 68, 0.1)',
                border: '1px solid var(--danger)',
                borderRadius: '8px',
                color: 'var(--danger)',
                fontSize: '13px',
                lineHeight: '1.5',
                textAlign: 'left',
              }}
            >
              <strong>Warning:</strong> This will re-encrypt all stored sessions. Do not close the
              app until this completes. If you forget the new password, your data will be
              permanently inaccessible.
            </div>
            <div style={{ display: 'flex', gap: '12px', justifyContent: 'center' }}>
              <button
                className="confirm-cancel"
                onClick={() => setConfirming(false)}
                disabled={submitting}
              >
                Cancel
              </button>
              <button
                className="unlock-submit"
                style={{ width: 'auto' }}
                onClick={handleConfirmed}
                disabled={submitting}
              >
                {submitting ? 'Re-encrypting…' : 'Change Password'}
              </button>
            </div>
          </div>
        ) : (
          <form onSubmit={handleSubmit}>
            <input
              className="unlock-input"
              type="password"
              placeholder="Current password"
              value={currentPassword}
              onChange={(e) => setCurrentPassword(e.target.value)}
              autoFocus
              disabled={submitting}
              style={{ marginBottom: '12px' }}
            />
            <input
              className="unlock-input"
              type="password"
              placeholder="New password (min. 8 characters)"
              value={newPassword}
              onChange={(e) => setNewPassword(e.target.value)}
              disabled={submitting}
              style={{ marginBottom: '12px' }}
            />
            <input
              className="unlock-input"
              type="password"
              placeholder="Confirm new password"
              value={confirm}
              onChange={(e) => setConfirm(e.target.value)}
              disabled={submitting}
            />

            {error && <div className="unlock-error">{error}</div>}
            {success && (
              <div
                style={{
                  marginTop: '12px',
                  padding: '10px 12px',
                  background: 'rgba(34, 197, 94, 0.1)',
                  border: '1px solid var(--success)',
                  borderRadius: '8px',
                  color: 'var(--success)',
                  fontSize: '13px',
                }}
              >
                {success}
              </div>
            )}

            <div
              style={{ display: 'flex', gap: '12px', marginTop: '16px', justifyContent: 'center' }}
            >
              <button
                className="confirm-cancel"
                type="button"
                onClick={onClose}
                disabled={submitting}
              >
                Cancel
              </button>
              <button
                className="unlock-submit"
                style={{ width: 'auto' }}
                type="submit"
                disabled={submitting || !currentPassword || !newPassword || !confirm}
              >
                {submitting ? 'Re-encrypting…' : 'Next'}
              </button>
            </div>
          </form>
        )}
      </div>
    </div>
  );
}
