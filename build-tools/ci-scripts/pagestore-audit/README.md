# BR1 PageStore architecture audit

This directory records BR1 production containment, ownership, runtime evidence
bindings, source identity and the indexed call graph. It does not grant
behavioral correctness, handoff, zero-copy, allocation-free installation,
bounded-memory, or deletion eligibility by itself.

The current implementation prepares default compatibility backing before canonical
updates/restores, rejects unconstructible managers with `std::bad_alloc`, prepares
flat shard-index buckets and descriptor nodes before installation, and validates
immutable roots once during construction. These repair the M5 baseline review's
budget-failure and install-allocation counterexamples. Reproducible behavioral
and allocator evidence, the qualified source identity, and the sole active
M6 → M7 → M8 → BR1-PERF plan are maintained in workspace design 92. Designs
47–91 are historical inputs, not competing current milestones or API templates.

Current counts come from the generated graph and architecture checker output;
the frozen execution evidence is linked from workspace design 92. Candidate
classifications are review inputs, not deletion permission.
The source gate cannot replace behavior, actual allocator interposition,
resource-pressure tests or performance qualification.

- `intrusion-manifest.json` records the current compatibility, duplicate, and
  misplaced ownership surface.  Active entries must name symbols that still
  exist; removed entries remain as tombstones for one milestone.
- `state-ownership.json` records the current and target writer for every state
  fact, governed by current design 92. `split` is debt, not an accepted final owner.
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
increases a count must first update current design 92 with the ownership, memory,
concurrency, and removal consequences.

The old mutex-protected compatibility census and retired writer-claim metrics
are not current production authorities. Runtime metric identifiers are checked
against production identifiers; that structural check does not prove all
budget/failure behavior. The ownership facts distinguish canonical
writers by responsibility; provider allocation identity, physical pin lifetime,
Store classification and compatibility group lifetime are separate facts.
The fail-closed Vulkan provider is still deferred to BR3. Do not recreate an
old split or compatibility protocol to match these historical descriptions.

M6–M8 extend the existing owners and shared protocols. Historical sketches do
not authorize recreating deleted placeholder APIs. Source inventory, call graph,
ownership, runtime census and lifecycle/failure evidence must agree before a
compatibility implementation is removed. Same-source correctness and bounded
resource behavior must accompany every new handoff qualification.
