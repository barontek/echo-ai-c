import { createContext, type Context } from 'react';
import type { ApprovalRequest, ToolCall } from '../types';

export type ConnectionStatus = 'connected' | 'connecting' | 'disconnected' | 'reconnecting';

export const EFFORT_OPTIONS = ['minimal', 'low', 'medium', 'high'] as const;
export type EffortOption = (typeof EFFORT_OPTIONS)[number];

export interface ChatContextValue {
  sessions: Array<{ id: string; title: string; created_at: string }>;
  activeSessionId: string | null;
  currentModel: string;
  currentProvider: string;
  models: string[];
  providers: string[];
  currentEffort: string;
  selectEffort: (effort: string) => void;
  supportsEffort: boolean;
  messages: Array<{
    role: 'user' | 'assistant' | 'system' | 'tool';
    content: string;
    id?: string;
    timestamp?: string;
    thinking?: string;
    has_tools?: boolean;
    tool_calls?: ToolCall[];
    tool_call_id?: string;
    tool_name?: string;
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
  createSession: () => void;
  selectSession: (sessionId: string) => Promise<void>;
  deleteSession: (sessionId: string) => Promise<void>;
  renameSession: (sessionId: string, newTitle: string) => Promise<void>;
  selectModel: (model: string) => void;
  selectProvider: (provider: string, prefetchedModels?: string[]) => Promise<void>;
  reconnect: () => void;
  pendingApproval: ApprovalRequest | null;
  resolveApproval: (requestId: string, approved: boolean) => void;
  pendingQuestion: string | null;
  resolveAskUser: (answer: string) => void;
}

export const ChatContext: Context<ChatContextValue | null> = createContext<ChatContextValue | null>(
  null
);
