# Session log

This log records significant work and non-obvious findings that apply only to
`cartolina-tileserver`. Project-wide and cross-repository work belongs in
`cartolina-js/docs/wiki/session-log.md`:
<https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/session-log.md>.

## 2026-06-30 — Establish tileserver documentation ownership

Created [index.md](index.md) as the tileserver documentation entry point and
moved the metanode-store operator guide, tools inventory, and metatile
production guide from the shared cartolina-js wiki. Their old wiki paths now
contain relocation links so historical references remain valid.

The documentation split is now explicit: operational and implementation
documentation specific to the tileserver lives here; project-wide RFCs,
architecture, backlog, session history, and frontend/backend interface
documentation remain in the shared wiki.

The resource-definition document remains available but is identified as an
older, incomplete reference that must be checked against the implementation.

Created [backlog.md](backlog.md) for work confined to the tileserver and
moved the applicable entries from the shared backlog without changing their
content. Added a top-posted task to audit all of `resources.md` against the
current implementation; documenting missing generators is one part of that
audit.

The migration includes the spatially varying bottom-LOD task. Although that
entry describes client effects, its proposed implementation and acceptance
work are confined to tileserver tiling and resource configuration. Its
relationship to the per-node bottom-LOD task now uses an explicit link.

Applied the same public-documentation boundary used by `cartolina-js`: public
docs may contain software facts and references used by committed demos or
examples, but not private integrations, APIs, hosts, paths, datasets, or
validation details. Replaced the dataset-specific tile URL in `AGENTS.md`
with placeholders while preserving the filename example.

Extended the committed pre-commit hook to require `docs/session-log.md` in
the staged changes, matching the cartolina-js workflow. Trivial changes can
bypass the check with `SKIP_SESSION_LOG=1`; the existing sensitive-content
scan and its `SKIP_SENSITIVE=1` override remain unchanged. Added the email
and recovery-code checks present in cartolina-js so both repositories protect
the same common PII and secret patterns.

Added RFC 7 to the design-history section at the bottom of
[index.md](index.md) and linked it from [backlog.md](backlog.md) as the
specification behind the metanode-store and unified-tiling work.
