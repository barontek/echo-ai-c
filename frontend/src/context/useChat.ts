import { useContext } from 'react';
import { ChatContext, type ChatContextValue } from './ChatContext';

/**
 * useChat - access the chat state and actions provided by ChatProvider.
 *
 * Subscribes to the ChatContext only; it sets up no effects of its own,
 * so there is nothing to clean up here (all subscription/cleanup lives in
 * ChatProvider). Returns a new snapshot of the context value on every
 * render the provider re-renders.
 *
 * @returns The current ChatContextValue snapshot.
 * @throws {Error} When called outside of a ChatProvider — the error
 *   message is 'useChat must be used within ChatProvider'. Components
 *   that may render outside the provider must not call this hook.
 */
export function useChat(): ChatContextValue {
  const context = useContext(ChatContext);
  if (!context) {
    throw new Error('useChat must be used within ChatProvider');
  }
  return context;
}
