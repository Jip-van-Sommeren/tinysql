# Database roadmap

Date: 2026-09-10

This roadmap prioritizes storage reliability before expanding SQL features.
Milestones are ordered by dependency and include acceptance criteria. They
describe future work; creating this document does not implement them.

## Current baseline

The database supports persistent table pages, guarded page access, cursor scans,
INSERT, DELETE, filtered SELECT, scalar expressions, SUM, and COUNT. The most
recent validation passed the warnings-as-errors build and all four CTest tests.

Remaining limitations include unbounded page caching, materialized query results,
incomplete constraint enforcement, and no crash-recovery mechanism.

## 1. Make storage failures visible

- [ ] Check file creation, opening, seeking, writing, flushing, and closing.
      Report failures with file and page context.
- [ ] Clear a page's dirty flag only after a successful write.
- [ ] Introduce injectable storage failures for deterministic tests.
- [ ] Replace the destructive demo startup with an explicit database-path option;
      isolate demo resets from ordinary startup.

**Acceptance criteria:** Failed writes raise errors, affected cached pages remain
dirty, and restarting the executable preserves existing data. Distinguish
successful writes from power-loss durability.

## 2. Harden validation and expression correctness

- [x] Reject rows that cannot fit on an empty page before allocating additional
      pages.
- [x] Validate page identifiers, page types, slot boundaries, and page chains;
      detect cycles and identifier overflow.
- [x] Implement three-valued NULL logic, with WHERE accepting only TRUE.
- [x] Reject unsupported constraints explicitly until enforcement exists.
- [x] Reject duplicate INSERT column names and add parenthesized expressions to
      the parser.

**Acceptance criteria:** Malformed pages and oversized rows fail predictably,
scans cannot loop indefinitely through cyclic chains, and NULL truth-table tests
pass.

## 3. Establish statement atomicity and crash recovery

- [ ] Design and implement a single-writer, per-statement recovery mechanism
      before allowing eviction to write partially modified statements.
- [ ] Cover page changes, header counters, file growth, and table creation.
- [ ] Define commit durability, required filesystem synchronization, and recovery
      on reopening.
- [ ] Ensure an INSERT or DELETE that fails midway cannot leave a partially
      applied statement.

**Acceptance criteria:** Injected exceptions and process termination at
persistence boundaries recover either the complete previous state or the
complete committed state.

## 4. Bound the page cache

- [ ] Add configurable buffer capacity and LRU eviction of unpinned pages.
- [ ] Count the header page within the capacity and keep schema access valid
      during decoding and writeback.
- [ ] Integrate dirty-page eviction with the recovery mechanism.
- [ ] Report buffer exhaustion when all eligible frames are pinned.
- [ ] Expose cached-frame, hit, miss, and eviction counters for tests.

**Acceptance criteria:** Scans and writes exceeding the cache capacity succeed
with a small buffer, pinned pages remain valid, and residency never exceeds the
configured frame limit.

## 5. Stream query results

- [ ] Add a result cursor that owns the table and execution state needed to
      remain valid.
- [ ] Preserve the existing materialized result API as a convenience wrapper.
- [ ] Update the CLI to print rows incrementally and release resources on early
      termination.

**Acceptance criteria:** A large ungrouped SELECT runs with memory bounded by the
cache and current processing state.

## 6. Improve storage utilization and enforce constraints

- [ ] Compact fragmented pages and track reusable free space.
- [ ] Implement DEFAULT, CHECK, UNIQUE, and PRIMARY KEY behavior, including
      duplicates within one INSERT batch.
- [ ] Keep foreign keys explicitly unsupported until cross-table enforcement is
      implemented.

**Acceptance criteria:** Repeated insert/delete cycles reuse space, constraints
survive reopening, and rejected writes remain atomic.

## 7. Expand SQL capabilities

Implement these features in order:

- [ ] MIN, MAX, and AVG.
- [ ] LIMIT.
- [ ] ORDER BY.
- [ ] GROUP BY and HAVING.
- [ ] UPDATE.
- [ ] Joins.
- [ ] Indexes and scan-versus-index planning.

**Acceptance criteria:** Each feature includes parser, binding, execution,
persistence where applicable, and regression tests. Sorting and grouping specify
memory limits or spill behavior.

## Validation and assumptions

- Keep the build and CTest suite green after every milestone.
- Add fault-injection, corruption, reopen, and recovery tests alongside storage
  changes.
- Use sanitizer checks for ownership changes and cache counters for memory
  claims.
- Assume single-process, single-writer operation initially. Defer concurrent
  transactions and network access.
- Record any on-disk format change with versioning and an explicit compatibility
  policy.

Baseline verification commands:

```sh
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```
