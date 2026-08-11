# src/change_tracker — file undo/redo

What this owns: snapshot-based undo/redo for tool-driven file edits —
`ct_snapshot` before a write, `ct_undo`/`ct_redo` to restore previous
content, wired into the write-file and replace-in-file tools.

Why it exists: an edit that destroys the only copy of a user's file
without a way back is data loss; the tracker guarantees one undo step
per edited file, session-scoped.
