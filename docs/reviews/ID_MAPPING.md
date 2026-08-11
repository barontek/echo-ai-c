# Fix-ID Mapping — CHAT_DB_DISCREPANCIES.md vs AGENTS_COMPLIANCE_REMEDIATION_PLAN.md

**Date:** 2026-08-11

The repository uses two letter-based fix-ID schemes that DO NOT correspond:

1. **CHAT_DB_DISCREPANCIES.md** (deleted from the tree in commit c2c56a8,
   git-recoverable) — the scheme referenced by comments in src/ and tests/:
   A1-A13, B1-B10, C1-C15, D1-D5, E1-E2, F1-F6, G, H2, I2-I3, J1-J5.
2. **AGENTS_COMPLIANCE_REMEDIATION_PLAN.md** (docs/no_longer_in_use/) —
   a different scheme reusing the same letters with different numbers:
   A1-A5, B1-B4, C1, D1-D2, E1-E13, F1-F9.

**Rule:** when reading a fix ID in code or a test comment, it refers to
scheme (1). When reading the remediation plan, IDs refer to scheme (2).
Never match IDs across the two schemes; the review doc
(docs/reviews/AGENTS_COMPLIANCE_REVIEW.md) references rule numbers and
the L1-L7 bug IDs, which are scheme-independent.

The mapping between the two schemes is only recoverable by restoring
CHAT_DB_DISCREPANCIES.md (git show c2c56a8^:CHAT_DB_DISCREPANCIES.md).
If a cross-document match is ever needed, restore that file and build
the mapping table at that point rather than guessing.
