import { useContext } from 'react';
import { ChatContext, type ChatContextValue } from './ChatContext';

export function useChat(): ChatContextValue {
  const context = useContext(ChatContext);
  if (!context) {
    throw new Error('useChat must be used within ChatProvider');
  }
  return context;
}
