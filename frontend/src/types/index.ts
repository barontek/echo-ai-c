// Centralized types for the application

export interface Session {
  id: string;
  title: string;
  created_at: string;
}

export interface Message {
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
}

/* Branching state for one fork point on the active chain: how many chains
 * (live + snapshotted) share the fork point, and the live chain's position
 * among them (1-based). Keyed by the fork point message id as it appears in
 * the active chain. branchIds holds the snapshot record ids of the non-live
 * chains, ordered by chain creation (the live chain has no record and is
 * omitted), so switchBranch can resolve a target position to a branch_id. */
export interface BranchInfo {
  messageId: string;
  count: number;
  active: number;
  branchIds: string[];
}

export interface ToolCall {
  name: string;
  arguments: string | Record<string, unknown>;
  tool_call_id?: string;
  result?: {
    content: string;
    error: string | null;
  };
}

export interface Config {
  provider: string;
  model: string;
  temperature: number;
  max_iterations: number;
  session_enabled: boolean;
}

export interface ApprovalRequest {
  type: 'approval_request';
  request_id: string;
  tool_name: string;
  arguments: string;
}

export interface AskUserRequest {
  type: 'ask_user';
  question: string;
}

export interface StreamEvent {
  type:
    | 'ready'
    | 'message'
    | 'content'
    | 'done'
    | 'error'
    | 'pong'
    | 'session_start'
    | 'approval_request'
    | 'ask_user'
    | 'title_updated'
    | 'history'
    | 'tool_start'
    | 'tool_end'
    | 'branch_info';
  content?: string;
  has_tools?: boolean;
  tool_calls?: ToolCall[];
  messages?: Message[];
  session_id?: string;
  title?: string;
  timestamp?: string;
  role?: string;
  request_id?: string;
  tool_name?: string;
  arguments?: string;
  question?: string;
  tool_call_id?: string;
  result_content?: string;
  result_error?: string;
  /* done frame after an edit/regenerate fork: the fork point message's
   * fresh identity, so the pill can key on the new message id. fork_role
   * is the fork point's role ('user' for an edit, 'assistant' for a
   * regenerate) — it tells the frontend WHICH message carries the fresh
   * identity, so the pill lands on the edited message / regenerated
   * response instead of blindly on the last assistant message. */
  fork_message_id?: string;
  fork_group_id?: string;
  fork_role?: string;
  /* branch_info frame: fork-point counters for the active chain. */
  branches?: Array<{
    message_id: string;
    count: number;
    active: number;
    branch_ids?: string[];
  }>;
}

export interface ApiError {
  error: string;
  detail?: string;
}
