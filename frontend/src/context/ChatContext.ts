import { createContext, type Context } from 'react';
import type { ApprovalRequest, ToolCall } from '../types';

export type ConnectionStatus = 'connected' | 'connecting' | 'disconnected' | 'reconnecting';

export interface ChatContextValue {
  sessions: Array<{ id: string; title: string; created_at: string }>;
  activeSessionId: string | null;
  currentModel: string;
  currentProvider: string;
  models: string[];
  providers: string[];
  messages: Array<{
    role: 'user' | 'assistant';
    content: string;
    id?: string;
    timestamp?: string;
    thinking?: string;
    has_tools?: boolean;
    tool_calls?: ToolCall[];
    error?: string;
  }>;
  connectionStatus: ConnectionStatus;
  isConnected: boolean;
  isStreaming: boolean;
  currentThinking: string;
  sidebarOpen: boolean;
  setSidebarOpen: (open: boolean) => void;
  sendMessage: (content: string) => void;
  stopGeneration: () => void;
  editMessage: (index: number, newText: string, msgId?: string) => void;
  retryMessage: (index: number) => void;
  createSession: () => Promise<void>;
  selectSession: (sessionId: string) => Promise<void>;
  deleteSession: (sessionId: string) => Promise<void>;
  renameSession: (sessionId: string, newTitle: string) => Promise<void>;
  selectModel: (model: string) => void;
  selectProvider: (provider: string) => void;
  reconnect: () => void;
  pendingApproval: ApprovalRequest | null;
  resolveApproval: (requestId: string, approved: boolean) => void;
}

export const ChatContext: Context<ChatContextValue | null> = createContext<ChatContextValue | null>(
  null
);
