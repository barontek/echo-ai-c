import { useChat } from '../context';

const DESTRUCTIVE_TOOLS = new Set(['bash', 'write_file', 'memory', 'sqlite_query']);

export function ApprovalDialog() {
  const { pendingApproval, resolveApproval } = useChat();

  if (!pendingApproval) return null;

  const isDangerous = DESTRUCTIVE_TOOLS.has(pendingApproval.tool_name);

  return (
    <div className="approval-overlay" onClick={() => {}}>
      <div className="approval-dialog" onClick={(e) => e.stopPropagation()}>
        <div className="approval-title">Approval Required</div>
        <div className="approval-tool-name">{pendingApproval.tool_name}</div>

        <div className="approval-section-label">Arguments</div>
        <div className="approval-args">{pendingApproval.arguments}</div>

        {isDangerous && (
          <div className="approval-warning">
            This operation may be destructive. Review the arguments carefully.
          </div>
        )}

        <div className="approval-actions">
          <button
            className="approval-deny"
            onClick={() => resolveApproval(pendingApproval.request_id, false)}
          >
            Deny
          </button>
          <button
            className="approval-approve"
            onClick={() => resolveApproval(pendingApproval.request_id, true)}
          >
            Approve
          </button>
        </div>
      </div>
    </div>
  );
}
