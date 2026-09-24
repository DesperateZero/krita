# BR1 PageStore architecture audit

This directory is the machine-readable containment, R1 restructuring and
R2/M3 compact-page/shard-index/intrusive-record boundary for BR1. It does not grant handoff, zero-copy,
bounded-memory, or deletion eligibility.

Current work follows design 84 (2026-09-23), covering optimizations from design
52 onward, not just M1–M5. Milestone labels below are historical checkpoints,
not acceptance of the remaining protocol and budget gaps.

- `intrusion-manifest.json` records the current compatibility, duplicate, and
  misplaced ownership surface.  Active entries must name symbols that still
  exist; removed entries remain as tombstones for one milestone.
- `state-ownership.json` records the current and target writer for every state
  fact required by design 69.  `split` is debt, not an accepted final owner.
- `runtime-census.json` maps every manifest entry to operation/page/live/peak/
  terminal/fallback evidence.  `static-proven` records an inapplicable runtime
  route whose caller set is proven by the frozen graph; `deferred` remains an
  explicit gap, not runtime coverage.
- `reverse-callgraph.json` is the compilation-database-backed Clang IndexStore
  graph.  Overloads have distinct USRs; direct/dynamic calls, template
  specializations, macro-expanded callsites and address-taken registrations
  are retained.  Unresolved callback edges fail closed.
- `baseline-source.json` freezes the dirty source union against commit
  `f5fbbde618`, its hashes and structural census, and the PageStore intrusion
  ratchet.
- `convergence-start-source.json` is the fixed pre-cleanup comparison point;
  do not refresh it with the current source checkpoint.
- `convergence-retired-protocol-proof.json` preserves the relevant declarations,
  callers and override edges from the newly generated pre-deletion graph plus
  its digest. It is a historical subset, not a replacement gate graph.

Run the gate from the repository root:

```sh
python3 build-tools/ci-scripts/check-pagestore-architecture.py
```

Regenerate the reverse-call graph only after a successful configured build has
written `_build-br0-cpu/compile_commands.json`:

```sh
python3 build-tools/ci-scripts/generate-pagestore-callgraph.py \
    --compile-commands _build-br0-cpu/compile_commands.json
python3 build-tools/ci-scripts/check-pagestore-architecture.py
```

Generation reparses only translation units whose repository-local include
closure reaches PageStore.  It uses the original compile command plus
`-fsyntax-only` and a temporary index store, so build objects are untouched.
The checker binds every manifest entry to the graph, verifies all source
locations and counts, rejects unresolved callbacks, and requires graph
provenance to match the frozen compile-database hash. Source-selection and
content fingerprints include the image/backend source and header universe;
adding a caller or changing a header invalidates the graph. Generation checks
that source and audit inputs have not changed while indexing.

Only regenerate the baseline in an explicitly reviewed ratchet update:

```sh
python3 build-tools/ci-scripts/check-pagestore-architecture.py \
    --write-baseline --build-dir _build-br0-cpu
```

Before writing a new checkpoint the checker enforces the previous intrusion
ceilings; refreshing hashes cannot silently raise them.
Regeneration is not a way to make a violation pass.  A patch that reduces an
intrusion deletes or lowers the corresponding frozen entry.  A patch that
increases a count must first update designs 68/69 with the ownership, memory,
concurrency, and removal consequences.

The legacy tile lease, anonymous transaction, writer-admission, iterator, and
tile-refresh compatibility routes now have a mutex-protected runtime census.
Existing store/history/retirement diagnostics are mapped separately rather
than counted twice. The retired count-authorized physical-exclusive API is
gone. The graph now binds the move-only `KisCpuWriteBindingReservation::acquire`
used by fresh native writes and its physical-claim tests. It is not D0: the
physical-allocation ownership fact remains split until provider handoff. The
fail-closed Vulkan provider remains explicitly deferred
to BR3.  With that retain decision frozen, R0 is complete; R1 may begin, but
metadata replacement and production backing handoff remain outside R1.

R1 has completed all five ordered extractions. `KisPageDefaultStorage` owns
the immutable default-read cache and bounded materialization admission;
`KisPageRetirementQueue` owns completion-qualified detached retirement debt;
`KisPageHistoryCollector` owns bounded retained-root reachability scans;
`KisPageReadCoordinator` owns read requests, leases and last-use accounting;
and `KisPagePublicationCoordinator` owns staged publication state and the
commit/restore/abort algorithm. All are final, non-`QObject`, by-value
services. The manifest restructuring status is `r1-complete`.

R2/M1 is also complete. `KisVersionRecord`, `KisReplicaRecord` and
`KisMetadataOverflowNode` are the compact authoritative schema, use typed
8-byte generational ids, and satisfy their 128/128/64-byte layout ceilings.
The coordinator stores these records and reconstructs public/reference
snapshots at the boundary; `applyOwnerSequence()` continues to project only
touched records.

R2/M2 has moved authoritative version, replica and overflow records into the
three shard-owned typed arenas. Stable addresses, slot/block generation,
quarantine, hard budgets, intrusive empty-block return and complete footprint
counters are active. The transitional `VersionRecordNode` owner is gone.
M2B computes slot demand under the shard lock, allocates `PreparedBlock`
candidates after unlocking, then revalidates page revision before lock-local
attach. Publication candidates aggregate block growth once per shard, and the
K1/K16/H metadata-only fixture reports logical bytes, arena bytes, slack and
candidate/attach counts.

R2/M3 replaces the page-local exact/physical/history/prepared/mutable
`list/map/hash/set` indexes and the `shared_ptr<VersionRecords>` owner with two
shard-global `KisShardSlotIndex` values plus intrusive version, history and
mutable chains. Insertions use explicit capacity tokens; the physical index
resolves a replica slot back to its owning version, and revision-qualified
history cursors retain ordered continuation semantics. The public snapshot
header is no longer stored: `MetadataPage` is a 96-byte compact directory
record and active writer/handoff/materialization/claim state lives in a sparse
shard-owned compact activity table whose identities are typed arena slots.
M3 is closed; M4 prepared-commit and backing handoff remain unauthorized.
