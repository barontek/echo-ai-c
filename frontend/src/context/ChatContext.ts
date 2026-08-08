import { createContext, type Context } from 'react';
import type { ApprovalRequest, BranchInfo, ToolCall } from '../types';

export type ConnectionStatus = 'connected' | 'connecting' | 'disconnected' | 'reconnecting';

/* Fallback per-provider effort option lists, used only until /api/providers
 * resolves (the backend's effort_options map is authoritative). OpenAI gets
 * xhigh; openai_compatible, ollama and opencode_zen share the smaller set. */
export const EFFORT_OPTIONS_BY_PROVIDER: Record<string, string[]> = {
  openai: ['low', 'medium', 'high', 'xhigh', 'max', 'none'],
  openai_compatible: ['low', 'medium', 'high', 'max', 'none'],
  ollama: ['low', 'medium', 'high', 'max', 'none'],
  opencode_zen: ['low', 'medium', 'high', 'max', 'none'],
};
export type EffortOption = string;

export interface ChatContextValue {
  sessions: Array<{ id: string; title: string; created_at: string }>;
  activeSessionId: string | null;
  currentModel: string;
  currentProvider: string;
  models: string[];
  providers: string[];
  currentEffort: string;
  effortOptions: string[];
  selectEffort: (effort: string) => void;
  supportsEffort: boolean;
  messages: Array<{
    role: 'user' | 'assistant' | 'system' | 'tool';
    content: string;
    id?: string;
    parent_id?: string;
    fork_group_id?: string;
    timestamp?: string;
    thinking?: string;
    has_tools?: boolean;
    tool_calls?: ToolCall[];
    tool_call_id?: string;
    tool_name?: string;
    error?: string;
  }>;
  /* Fork-point counters for the active chain, keyed by message id. */
  branchInfo: BranchInfo[];
  connectionStatus: ConnectionStatus;
  isConnected: boolean;
  isStreaming: boolean;
  currentThinking: string;
  sidebarOpen: boolean;
  setSidebarOpen: (open: boolean) => void;
  sendMessage: (content: string) => void;
  stopGeneration: () => void;
  editMessage: (index: number, newText: string, msgId?: string) => void;
  regenerateMessage: (index: number, msgId?: string) => void;
  switchBranch: (messageId: string, direction: -1 | 1) => void;
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
