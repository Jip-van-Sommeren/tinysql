# Journal synchronization and database write-ordering plan

Date: 2026-09-15

Status: implementation and persistence-boundary verification completed on
2026-10-21. Journal version 2 isolates recovery information by sector and stages
initialization before publication. Recovery failure/restart tests and a storage
model that discards unsynchronized writes cover the remaining prerequisites.
The guarantees and explicit storage assumptions are recorded below.

## Goal and scope

Keep `JournalFile::writePage()` responsible for appending before-image records,
without synchronizing after every append. A statement-level coordinator decides
when a batch must become durable before allowing database-file writes.

The core requirement is that an existing page's original contents must be
durable in the journal **before issuing the corresponding modified database-page
write**, not merely before flushing the database. Saving several original pages
and synchronizing them together is valid when their database writes are held
back until synchronization succeeds. See
[SQLite's rollback-journal ordering](https://sqlite.org/atomiccommit.html#_flushing_the_rollback_journal_file_to_mass_storage).

Assume Linux, C++23, and one active modifying statement per database. Keep the
database page format and generic `ByteReader`/`ByteWriter` encoding unchanged.
Concurrent transactions, LRU implementation, and new SQL features are outside
this plan. Dirty eviction remains disabled; a future implementation must use the
same generation-qualified, checked writeback path.

Storage assumptions: successful file/directory `fsync()` persists the corresponding
contents/namespace; same-directory rename and unlink have atomic namespace
outcomes; torn data writes can damage any bytes within an affected aligned sector,
whose size divides 4096 bytes (including 512-byte and 4096-byte sectors). Database
pages and journal allocations align to that 4096-byte unit. Larger damage units,
lying synchronization acknowledgements, and checksum collisions are outside this
model. This does not assume atomic sector writes or powersafe overwrite. See the
[sector assumptions](https://sqlite.org/atomiccommit.html#_hardware_assumptions)
and [Linux file/directory synchronization semantics](https://man7.org/linux/man-pages/man2/fsync.2.html).

Version 2 changes only the journal wire format: the 60-byte header occupies sector
0, the two 20-byte boundary payloads occupy sectors 1 and 2, and records start at
byte 12288. Each record has a logical length and a zero-padded, sector-aligned
physical length; its checksum covers the padding too. Boundary checksums cover
an entire slot sector. Published sectors are never reused by later appends.
Version 1 journals are rejected without replay; unresolved version 1 work must
be recovered using the previous binary before upgrading. Database metadata,
database pages, and the generic byte encoding remain unchanged.

## Current implementation

- `PageFile` and `JournalFile` use the descriptor-owning `LinuxFile` backend,
  with explicit `sync()` using `fsync()`. `PageFile::flush()` has been removed.
  `LinuxDirectory` provides directory synchronization. Descriptor-relative
  opens use `openat2()` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`.
- `db.meta` persists a database UUID, version, and checksum. Journal version 2
  has a checksummed header with database/statement UUIDs and page size, two
  checksummed recovery-boundary slots, and sequenced, checksummed records.
- `StorageEngine` owns `StatementRecovery`, shared by participating buffer
  managers. SQL INSERT, DELETE, and CREATE TABLE call begin/commit/rollback;
  direct `Database::insertRows()` uses the same INSERT path. The bypassing
  `Database::createTable()` API has been removed; callers can execute a
  `BoundCreateTable` or SQL. Lower-level creation requires an active statement.
- `PageGuard::page()` and `as()` provide read-only access. Writable access calls
  `BufferManager::prepareForWrite()` before modification: capture original file
  size, then capture an existing page's raw before-image once per path/page ID.
  Ordinary data-page allocation goes through a writable header first, so its
  original file size is captured before growth. Newly appended pages need no
  page before-image.
- Before opening a table file with `CreateNew`, the buffer-manager helper
  captures and synchronizes a nonexistence record. `FileBeforeImage` contains
  path, original size, and `newFile`; this state lives in the coordinator for
  one statement, not in a persistent buffer-manager flag.
- `flushPage()` and `flushAll()` call `ensureDurable()` before database writes.
  This synchronizes records, publishes the completed end offset to the older
  boundary slot, synchronizes the journal again, and persists its directory
  when needed. `begin()` writes and synchronizes the header and initial empty
  boundaries in `journal.pending`, renames it to `journal.log`, and synchronizes
  the directory before authorizing mutations. Startup discards any leftover
  staging file; failed initialization also supports in-process cleanup.
  Table mutation methods write back and synchronize their table file before
  `commit()` synchronizes directories containing new files, removes the journal,
  and synchronizes the journal directory. Buffer-manager writeback validates
  each frame's generation-qualified coverage and synchronizes the batch's highest
  required offset. Already-covered single-page writes skip redundant barriers.
  Commit verifies every participating handle has flushed and synchronized its
  work; pending state survives destruction of its buffer manager.
- `JournalFile::readNext()` streams bounded file-size and page-image records;
  `readPages()` has been removed. Rollback replays pages, restores recorded file
  sizes, synchronizes those files, removes newly created files, and synchronizes
  their directories before retiring the journal. Already-absent new files are
  accepted on retry. Opening a DB takes an exclusive directory lock, validates
  `db.meta`, and recovers a leftover journal before returning. Recovery validates
  the entire selected prefix before replay, including identity, checksums,
  sequence, metadata relationships, and duplicate page records. Startup uses
  the newest checksum-valid published boundary and ignores its unpublished
  suffix; in-process rollback uses the known completed-append boundary,
  including after a partial failed append. Header, boundary slots, and record
  allocations have disjoint sector ownership, so later writes cannot tear a
  previously published record or both boundary slots.
- Database writes, file syncs, and journal barriers fail closed. SQL, catalog
  reads, table handles, page guards, and cursors reject access until recovery
  succeeds. Rollback invalidates existing handles; callers must reopen them.
  After journal unlink, rollback retries only directory cleanup. An uncertain
  commit rejects rollback and requires reopening the database.
- Recovery tests inject CREATE failures around journal
  sync, directory sync, file creation, page writes, and table sync; they also
  cover stale coverage generations, batch barriers, partial native writes,
  multiple participants, file growth/truncation, failed recovery retries, and
  read gating after cleanup failures. New tests cover startup undo, validation
  before replay, unpublished tails, boundary publication failures, and partial
  boundary-slot overwrites. Recovery faults now include replay writes, resize,
  unlink, partial header creation, rename, and reopening after interruption.
  `db_power_loss_test` checks 3385 storage-loss images, including 2184 sector
  tears, at 100 points before and after persistence operations. It covers
  multi-file commit, growth, creation, and interrupted startup recovery.
  Current build/test results are recorded in section 5.

## 1. Add explicit durable storage operations

- [x] Introduce a small shared Linux file backend owning one native file
      descriptor per open file. Use it underneath `PageFile` and `JournalFile`,
      retaining their distinct page-oriented and record-oriented responsibilities.
      Do not depend on nonstandard access to a C++23 fstream's native handle or
      reopen a pathname on each sync.
- [x] Provide exact positional reads, complete positional writes, file-size
      queries, resizing, and `sync()`. Handle interrupted operations and short
      transfers; report errors with operation and file context. Keep the native
      backend behind a testable interface. Native positional writes can be short
      and must be completed explicitly; see the
      [Linux positional I/O documentation](https://man7.org/linux/man-pages/man2/pwrite.2.html).
      Tests inject native-call failures through link-time wrappers. Recovery
      tests record create/write/resize/rename/unlink/sync events; section 5 also
      covers storage loss and torn writes at persistence boundaries.
- [x] Define `sync()` as draining any application buffer first, then successfully
      completing `fsync()` on the same open file. The native positional-I/O
      backend has no C++ stream buffer, but still uses the OS cache; do not use
      `O_DIRECT`. Ordinary successful writes are not durability acknowledgements.
- [x] Separate cache writeback from durability: remove the no-op
      `PageFile::flush()` and use explicit `sync()` at durability boundaries.
      `BufferManager::flushAll()` writes pages; `BufferManager::sync()`
      synchronizes the underlying file.
- [x] Add directory synchronization for journal creation and removal. A newly
      created journal must have both its contents and its directory entry made
      durable before database writes are authorized. `begin()` handles staged
      creation/publication; commit and rollback synchronize after removal. File `fsync()`
      alone does not persist directory entries; see
      [Linux fsync semantics](https://man7.org/linux/man-pages/man2/fsync.2.html).
- [x] Synchronize directories containing newly created files before commit
      retires the journal, including lower-level StorageEngine callers. Rollback
      also synchronizes those directories after removing new files.
- [x] Synchronize the database root after creating `tables/` and `journal/`.
      `StorageEngine::create()` now calls `root.sync()` explicitly.
- [x] Synchronize the database root's parent after creating the root, so the
      database directory itself has a durable directory entry.
- [x] Throw on file synchronization failures. File destructors only close their
      descriptors and do not synchronize or silently commit pending work.
- [x] Keep normal commit and rollback synchronization explicit, outside
      destructors. Failure-state and participant tracking are covered below.

## 2. Track journal coverage within a statement

- [x] Add a storage-owned `StatementRecovery` shared with buffer managers;
      reject nested statements on the same coordinator. Wire SQL mutations and
      direct `Database::insertRows()` through it.
- [x] Close the direct CREATE bypass by removing `Database::createTable()` and
      using the bound-query/SQL execution path instead. Lower-level table
      creation now requires an active, nonfailed statement before creating the
      file; `initializeNewTable()` requires a new, unwritten file belonging to
      that statement.
- [x] Capture existing pages before writable access through
      `PageGuard::write()` / `writePage()`, rather than relying on a dirty flag
      set after modification. Reject writable access without an active statement.
- [x] Identify pages by `(tables-relative file path, page ID)` and deduplicate
      in the normal buffer-manager path, including page zero and linked-page
      changes. Clear capture bookkeeping after successful commit/rollback.
- [x] Enforce active/nonfailed state and deduplication inside the coordinator's
      capture methods. Page capture also requires original file metadata and a
      page ID within the original file size.
- [x] Capture original file size once before normal existing-table mutations
      and data-page allocation. Distinguish existing pages from newly appended
      pages using that size; retain metadata until statement resolution.
- [x] Record original file existence as well as size. Before `CreateNew`, append
      a `FileBeforeImage` with `newFile = true` and original size zero, then sync
      the journal and its directory. Do not open a nonexistent file to query its
      size. Clear metadata on successful statement completion so reusing a Table
      in another statement cannot misclassify it as new.

Offset tracking is a sync optimization, not a substitute for the correctness
work above. Writeback uses generation-qualified per-page/file coverage; a batch
checks every dirty frame before synchronizing its highest required end offset.

- [x] Return a `uint64_t` end offset after a complete append from both
      `JournalFile::writePage()` and `writeFileBeforeImage()`. Neither method
      automatically synchronizes.
- [x] Track completed-append and successfully synchronized offsets separately
      in the coordinator. Initialize and reset both on begin/finish; update the
      append offset for both record types. Reject journal-size mismatches with
      a catchable error and mark the statement failed.
- [x] Implement `ensureDurable(uint64_t requiredEnd)` for the active statement.
      Reject inactive/failed state and offsets beyond completed appends; sync
      only when coverage or journal-directory persistence is missing. Advance
      durable coverage only after the required synchronizations succeed.
- [x] Retain the required end offset against each page/file-metadata identity,
      rather than keeping only the latest append offset and a page-ID set.
- [x] Qualify retained coverage with a statement/journal generation before
      passing it through longer-lived caches. Generations are never reset on
      statement completion. Dirty frames retain their capture token, and both
      single-page and batch writeback reject stale or mismatched coverage.

## 3. Gate every database write

- [x] Put a journal barrier before both `flushPage()` and `flushAll()` writes.
      `flushAll()` performs one barrier before its batch, not one per page.
      Recovery uses separate `PageFile` writes without generating undo records.
- [x] Verify each dirty page's journal coverage at the writeback boundary:
      existing page image or original-size/nonexistence metadata, as appropriate.
      Both flush paths validate each frame against the coordinator's capture
      identity and generation before issuing any dependent page write.
- [x] Connect per-identity offset tracking to buffer-manager writeback: collect
      the batch's highest required end offset and avoid redundant syncs for
      already-covered single-page writes. Future eviction must use the same
      checked writeback path.
- [x] Keep cache writeback separate from durability. `flushPage()` clears its
      frame only after a complete write; `flushAll()` clears flags only after
      its write loop succeeds. A write failure leaves affected dirty state and
      does not itself discard the journal.
- [x] Synchronize the same open table file after writeback in current Table
      creation, insertion, and deletion paths, before Database calls commit.
- [x] Track participating/written files and make commit verify that all are
      synchronized, independently of cached-frame dirty flags. Per-handle
      participants retain dirty page IDs and pending sync state even after a
      buffer manager is destroyed. Commit rejects unflushed or unsynchronized
      work; syncing another handle does not acknowledge the original writer.
- [x] Fail the statement on journal-barrier synchronization errors and propagate
      them before dependent database writes. Keep the directory-sync-pending
      flag until synchronization succeeds.
- [x] Mark coordinator capture failures as failed and retain the journal.
      Both journal append methods and `sync()` reject `appendFailed_`; a partial
      append cannot be followed by another append or a successful sync.
- [x] Apply fail-closed handling consistently to database-write and database-sync
      errors, including lower-level callers. Buffer-manager write/sync failures
      mark the coordinator failed and retain undo and pending-write state. Gate
      normal reads and mutations at the database/storage boundary and through
      already-open tables, guards, and cursors until recovery succeeds.

Required batch ordering, not a complete commit algorithm:

```text
append original records A and B
    -> synchronize journal records
    -> publish the completed end offset to the older recovery-boundary slot
    -> synchronize journal again and any pending journal-creation directory entry
    -> write modified database pages A and B
    -> synchronize every modified database file at commit
    -> durably finalize the statement's journal state
    -> report success
```

## 4. Recovery integration prerequisites

Synchronization barriers alone are not a complete crash-recovery protocol.
The basic writeback path is already enabled. Complete and test the following
before claiming crash recovery or enabling dirty-page eviction:

- [x] Implement the basic existing-table rollback sequence: stream page images,
      restore recorded file lengths, synchronize those files, remove the journal,
      then synchronize its directory. Clear bookkeeping only after success.
- [x] Harden coordinator lifecycle transitions: publish active state only after
      journal creation succeeds; reject inactive/failed capture, commit, and
      durability calls; clear bookkeeping and offsets only on successful finish.
- [x] Extend recovery-required access gating to the database/storage boundary,
      including SELECT, catalog/header reads, and previously opened handles.
      Covered by the step 3 failure-injection tests.
- [x] Specify a versioned journal format and recovery/cleanup protocol with
      statement/database identity. Keep before-images available until all
      affected files are durable and journal retirement is durable. Version 2
      uses a 60-byte header, two 20-byte boundary payloads, and 17-byte record
      headers containing type, logical length, sequence, and checksum, with
      sector isolation as specified above. Record and boundary CRCs include
      both UUIDs and sector padding. `db.meta` persists the database identity;
      commit/rollback explicitly synchronize files and directory cleanup.
- [x] Make journal parsing distinguish clean EOF, incomplete trailing records,
      and corruption. Validate record boundaries, lengths, checksums, paths, and
      statement identity. Clean EOF is distinct from errors; malformed records
      in the selected prefix throw without advancing the cursor. Startup selects
      the newest checksum-valid boundary, rejects invalid offsets or records
      within it, and ignores only the unpublished suffix. In-process rollback
      uses its known completed-append boundary.
- [x] Ensure later appends and boundary updates cannot invalidate previously
      durable recovery information through torn writes to a shared sector.
      Version 2 isolates the immutable header, both boundary slots, and each
      padded record allocation. Independent wire fixtures verify the layout;
      the power-loss model damages entire dirty sectors, including neighboring
      bytes outside a requested write. Both slot rotations and appends after
      earlier database writeback are exercised.
- [x] Recover an existing journal before exposing tables or beginning another
      statement; opening an existing journal must never reset its append offset
      to zero and overwrite unresolved recovery information.
      `StorageEngine::open()` locks the database root exclusively, validates the
      published journal prefix, and replays it before returning. Low-level
      reopening preserves EOF and validates the record stream before appending.
      Startup restoration and rejection of invalid journals are tested.
- [x] Provide bounded streaming journal reads through `readNext()` rather than
      requiring the whole journal to be materialized.
      Preserve each record's file path as well as its page ID. Bound path/record
      lengths before allocation and reject invalid relative paths.
- [x] Connect `readNext()` to rollback using `std::visit` for page and file
      records. The file-record layout is now consistently
      `[type:u8][recordSize:u64][sequence:u32][checksum:u32]`
      followed by `[pathLength:u32][path][originalSize:u64][newFile:u8]`.
- [x] Validate the new-file flag, aligned original sizes, zero original size for
      a new file, and consistent duplicate file-metadata records. Require valid
      preceding metadata and an in-range page ID before replaying a page image;
      new files must not have page before-images.
- [x] Complete corruption validation, including conflicting duplicate page
      images and the format/identity checks above. `validateJournalForRecovery()`
      validates the complete selected prefix before any replay writes, rejects
      all duplicate page identities, and checks preceding file metadata and
      page ranges. Recovery-level cross-record fixtures are listed below.
- [x] Confine replay to intended table files, not just lexical relative paths.
      Recovery opens `tables/` relative to the locked root and resolves replay
      filenames beneath that directory with `RESOLVE_BENEATH` and
      `RESOLVE_NO_SYMLINKS`; removals use `unlinkat()` with a filename.
      Coordinator capture and recovery require a single filename component.
      Validation resolves all existing targets before any replay, so a later
      symlink or invalid path cannot cause earlier records to be replayed.
      Decoding also rejects absolute paths, `..`, NULs, and oversized paths.
- [x] Undo table creation by removing files that did not exist beforehand and
      synchronizing their directories before journal retirement. Accept an
      already-absent file, including when creation never completed or a previous
      rollback removed it before failing its directory sync.
- [x] Invalidate or reject other live table/buffer-manager caches after rollback.
      A recovery epoch rejects old tables, cursors, and even pinned guards
      without destroying referenced frames. They can be safely released, and
      callers reopen tables to read or mutate restored contents.
- [x] Make recovery restartable after failure during replay, truncate, file
      sync, removal, or directory sync. Preserve unresolved evidence and block
      normal access, including reads, until recovery succeeds.
      One-shot failures cover replay/short writes, resize, file sync, table and
      journal unlink, and both directory syncs. Each case retries both in process
      and after reopening, compares restored bytes, checks read gating, and
      begins another statement. The storage-loss model also interrupts startup
      recovery at every persistence operation and restarts it.
- [x] Treat a failure during final commit/cleanup synchronization as an uncertain
      outcome: preserve remaining evidence and require recovery before serving
      further operations. Do not report success or blindly retry the statement.
      Commit marks the coordinator failed on cleanup errors and stays
      outside the Database mutation catch. Reads and mutations are blocked.
      After commit unlinks the journal, rollback is rejected and the caller must
      reopen the database. Rollback retirement retries only directory cleanup.
      Storage-loss tests verify complete old/new outcomes during retirement,
      and require the complete new state after an acknowledged commit.

## 5. Verification and completion criteria

- [x] Add end-to-end recovery event/failure injection using native-call wrappers.
      `db_statement_recovery_test` records create/write/sync attempts and file
      existence at each event. Low-level wrappers also test interrupted/short
      transfers and partial journal appends.
- [x] Extend those probes to every persistence boundary, including journal
      append failures through the coordinator, resize, unlink, and final cleanup
      directory sync. Probes cover record/boundary writes and syncs, recovery
      resize/unlink, staging header/slot creation, rename, and directory cleanup.
      Partial initialization failures block reads and support cleanup/reopen.
      The separate native-call storage model captures before/after crash states.
- [x] Verify metadata-only durable offsets, skipping an already-covered offset,
      counter reset after commit and rollback, empty statements, invalid offset
      rejection, and catchable journal-size mismatch errors.
- [x] Verify page/file-specific coverage and batch barriers end to end. Tests
      include new appends after a sync, equal page IDs in different files,
      identical offsets across generations, cached-frame reuse, and redundant
      barrier avoidance. Dirty eviction remains disabled and must use the same
      checked writeback path when implemented.
- [x] Verify journal-file and journal-directory sync failures prevent table
      creation, and successful barriers precede the create call.
- [x] Verify failed journal barriers prevent writes to existing tables too,
      and no failed barrier advances durable coverage.
- [x] Inject CREATE failures at file open, after an earlier page write succeeds,
      and at table-file sync. Verify rollback removes the new file, synchronizes
      its directory before journal retirement, and permits a subsequent statement.
- [x] Inject write/sync failures after partial progress on existing tables and
      across multiple participating files. Check undo retention independently
      of which frames are still dirty or cached.
- [x] Test oversized/truncated records, path rejection, and bounded-memory
      journal record reading, including a large sparse journal.
- [x] Test both journal record types, consecutive records, reopening/appending,
      both new-file flag values, invalid flags, and preserving the read position
      after decoding failures. Reader fixtures encode the wire layout independently.
- [x] Test versioned headers, database/statement identity, checksums, sequences,
      boundary selection/conflicts, and interrupted updates of either boundary
      slot. Verify record-sync / boundary-write / boundary-sync ordering and
      that failures at each stage prevent dependent database writes.
- [x] Test Database operations with a root different from the working directory,
      direct INSERT recovery integration, rollback after mid-batch validation
      failure, journal cleanup, and a subsequent successful statement.
- [x] Test reuse of a Table across committed statements, followed by rollback
      after an existing table's modified pages have been written and synchronized.
      Compare restored file bytes and rows; verify the file is not treated as new.
- [x] Extend existing-table rollback tests to file growth, new-page link/header
      restoration, and truncation, including failures during those operations.
      Grown files are restored byte-for-byte after write, sync, and resize faults,
      including after reopening. Storage-loss cases cover torn data pages and
      lost/retained unsynchronized length changes during recovery.
- [x] Test CREATE rollback after header construction fails, refusal to create
      without an active statement, begin failure without stale active state,
      rejection of failed-state commit/sync, and rollback when a recorded new
      file was never created.
- [x] Retry rollback after deletion followed by a failed tables-directory sync;
      require the journal to survive the failure and the retry to accept absence.
- [x] Test final commit/cleanup failures, access blocked after recovery failure,
      and startup recovery of a leftover journal.
      Final cleanup, read gating through all public handle types, and in-process
      recovery/cleanup retries are tested. `db_database_storage_test` now reopens
      a database with durable undo, restores existing table bytes, removes an
      uncommitted table, retires the journal, and starts a subsequent statement.
      Header-only journals after failed begin barriers are also recovered.
- [x] Exercise malformed journals and cross-record validation through recovery,
      not just through the record reader. Startup rejects foreign identity,
      checksum corruption, truncation, and invalid published boundaries before
      changing tables; unpublished tails are ignored. Additional independent
      fixtures cover conflicting metadata, duplicate pages, missing/out-of-range
      metadata, images for new files, invalid sizes, nested/dot/absolute/parent/NUL
      paths, and symlink targets. Two failed opens preserve all table bytes,
      outside files, and journal evidence in each case.
- [x] Inject failures before and after each persistence boundary and reopen.
      `db_power_loss_test` separates volatile/durable inode data and directory
      names behind native-call wrappers. Rebuilding files from those snapshots
      loses unsynchronized writes even though the host OS cache survives.
      At every create/write/rename/sync/resize/unlink cut it tests lost writes,
      early writeback, alternating persisted sectors, independent table/journal
      namespace persistence, and each dirty sector torn with garbage or either
      old/new half. Multi-file statements must recover to complete old/new file
      sets; acknowledged commits require the new set; interrupted undo requires
      the old set. All comparisons include file bytes, lengths, and existence.
      This is deterministic model coverage under the stated storage assumptions,
      not exhaustive enumeration of every possible device failure.
- [x] Include journal/recovery sources in `db_core` and pass the CTest suite.
      Reverified on 2026-10-21: all 13 tests pass in a fresh GCC 14 Debug build,
      including the new storage-loss test, with no warning downgrades.
- [x] Run focused ASan/UBSan checks. Reverified on 2026-10-21: byte-I/O,
      statement-recovery, database-storage, storage-cursor, journal-file,
      journal-reader, and power-loss tests all pass (7/7). Both sanitizers are
      enabled together; LeakSanitizer is disabled for the sandbox's ptrace
      restriction. Non-PIE executables keep ASan shadow mapping deterministic.
- [x] Pass a fresh full warnings-as-errors build. GCC 14 Debug builds pass with
      `-Wall -Wextra -Wpedantic -Werror -Wconversion`, also with ASan/UBSan.
      Parser reservation counts use `size_t`; column IDs are range-checked before
      narrowing; boundary slot conversions are explicit; byte-reader integer
      promotions are narrowed explicitly without changing the wire encoding.

## Completion and future work

The synchronization/recovery milestone is complete under the storage assumptions
above. Dirty eviction is still disabled and was not added by this work. Any future
eviction must reuse the same checked coverage/writeback path; concurrency, cache
replacement, and SQL extensions remain separate work.

Reproduction (GCC 14, Linux):

```sh
cmake -S . -B /tmp/db-journal-final-check -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/db-journal-final-check -j 4
ctest --test-dir /tmp/db-journal-final-check --output-on-failure -j 4

cmake -S . -B /tmp/db-journal-sanitize -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build /tmp/db-journal-sanitize -j 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/db-journal-sanitize --output-on-failure \
  -R 'db_(byte_io|statement_recovery|database_storage|storage_cursor|journal_file|journal_reader|power_loss)_test'
```
