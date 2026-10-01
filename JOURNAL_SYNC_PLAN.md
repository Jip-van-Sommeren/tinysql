# Journal synchronization and database write-ordering plan

Date: 2026-09-15

Status: partially implemented; reviewed against commit `fcb8f88` on 2026-09-30.
Statement journaling, writeback barriers, rollback of existing tables and table
creation, and basic durable-offset tracking are implemented. Startup recovery,
complete failure handling, and per-page offset integration remain unfinished.
Checked items describe implemented pieces, not a claim of crash-safe statement
atomicity.

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
this plan. Recovery-format prerequisites are tracked below; their detailed
design remains to be done. Dirty eviction must remain disabled until the
recovery prerequisites below are satisfied.

## Current implementation

- `PageFile` and `JournalFile` use the descriptor-owning `LinuxFile` backend,
  with explicit `sync()` using `fsync()`. `PageFile::flush()` has been removed.
  `LinuxDirectory` provides directory synchronization.
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
  This synchronizes the journal and, on the first barrier, its directory.
  Table mutation methods write back and synchronize their table file before
  `commit()` synchronizes directories containing new files, removes the journal,
  and synchronizes the journal directory. The offset overload can skip a
  redundant barrier, but buffer-manager writeback still uses the no-argument
  full-journal barrier.
- `JournalFile::readNext()` streams bounded file-size and page-image records;
  `readPages()` has been removed. Rollback replays pages, restores recorded file
  sizes, synchronizes those files, removes newly created files, and synchronizes
  their directories before retiring the journal. Already-absent new files are
  accepted on retry. Recovery still does not run automatically when opening a DB.
- All 12 CTest tests pass. Recovery tests inject CREATE failures around journal
  sync, directory sync, file creation, page writes, and table sync; they also
  cover retry after deletion and rollback of an existing table after writeback.
  These tests do not yet simulate power loss or prove recovery after restart.

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
      tests now record create/write/sync events; full boundary coverage and
      simulated loss of unsynchronized writes remain pending in section 5.
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
      durable before database writes are authorized. `ensureDurable()` handles
      creation; commit and rollback synchronize after removal. File `fsync()`
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
      destructors. The remaining failure-state and participant-tracking work is
      listed below.

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
- [x] Identify pages by `(database-relative file path, page ID)` and deduplicate
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
work above. Its coordinator groundwork is implemented; the current writeback
paths still use the no-argument barrier to synchronize the whole journal.

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
- [ ] Qualify retained coverage with a statement/journal generation before
      passing it through longer-lived caches. Counter reset exists, but a bare
      offset does not identify the statement that produced it. (Later)

## 3. Gate every database write

- [x] Put a journal barrier before both `flushPage()` and `flushAll()` writes.
      `flushAll()` performs one barrier before its batch, not one per page.
      Recovery uses separate `PageFile` writes without generating undo records.
- [x] Verify each dirty page's journal coverage at the writeback boundary:
      existing page image or original-size/nonexistence metadata, as appropriate.
      Normal mutation and creation paths now capture the necessary records,
      but the flush boundary itself does not verify coverage for each page.
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
- [ ] Track participating/written files and make commit verify that all are
      synchronized, independently of cached-frame dirty flags. Today `commit()`
      trusts callers and removes the journal without checking table durability.
- [x] Fail the statement on journal-barrier synchronization errors and propagate
      them before dependent database writes. Keep the directory-sync-pending
      flag until synchronization succeeds.
- [x] Mark coordinator capture failures as failed and retain the journal.
      Both journal append methods and `sync()` reject `appendFailed_`; a partial
      append cannot be followed by another append or a successful sync.
- [ ] Apply fail-closed handling consistently to database-write and database-sync
      errors, including lower-level callers. Database catch paths attempt
      rollback, but buffer-manager write/sync failures do not themselves mark
      the coordinator failed. Gate normal reads as well as mutations while
      recovery is required.

Required batch ordering, not a complete commit algorithm:

```text
append original records A and B
    -> synchronize journal and any pending journal-creation directory entry
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
- [ ] Extend recovery-required access gating to the database/storage boundary,
      including SELECT, catalog/header reads, and previously opened handles.
      Coordinator guards alone do not prevent reading unresolved table contents.
- [ ] Specify a versioned journal format and recovery/cleanup protocol with
      statement/database identity. Keep before-images available until all
      affected files are durable and journal retirement is durable. Existing
      records have type and length fields but no format header or checksums.
- [ ] Make journal parsing distinguish clean EOF, incomplete trailing records,
      and corruption. Validate record boundaries, lengths, checksums, paths, and
      statement identity. Later appends must not invalidate previously durable
      recovery information, including through torn writes to a shared sector.
      Do not blindly ignore every malformed final record.
      Partial: clean EOF is distinct from errors, lengths are bounded, and
      truncated/malformed records throw without advancing the read cursor.
      There is no safe interrupted-append recovery policy yet; rollback can
      fail on a partial tail after replaying earlier records.
- [ ] Recover an existing journal before exposing tables or beginning another
      statement; opening an existing journal must never reset its append offset
      to zero and overwrite unresolved recovery information.
      `StorageEngine::open()` currently ignores an existing journal; a new
      statement then fails its exclusive journal creation instead of recovering.
      Low-level journal reopening does preserve the append position at EOF.
      Recovery must also respect exclusive database ownership so it cannot
      replay another live writer's journal.
- [x] Provide bounded streaming journal reads through `readNext()` rather than
      requiring the whole journal to be materialized.
      Preserve each record's file path as well as its page ID. Bound path/record
      lengths before allocation and reject invalid relative paths.
- [x] Connect `readNext()` to rollback using `std::visit` for page and file
      records. The file-record layout is now consistently
      `[type:u8][pathLength:u32][path][originalSize:u64][newFile:u8]`.
- [x] Validate the new-file flag, aligned original sizes, zero original size for
      a new file, and consistent duplicate file-metadata records. Require valid
      preceding metadata and an in-range page ID before replaying a page image;
      new files must not have page before-images.
- [ ] Complete corruption validation, including conflicting duplicate page
      images and the format/identity checks above. Streaming replay can still
      modify earlier pages before encountering an invalid later record.
- [ ] Confine replay to intended table files, not just lexical relative paths.
      Current decoding rejects absolute paths, `..`, NULs, and oversized paths,
      but does not enforce the tables subtree or prevent symlink escape.
- [x] Undo table creation by removing files that did not exist beforehand and
      synchronizing their directories before journal retirement. Accept an
      already-absent file, including when creation never completed or a previous
      rollback removed it before failing its directory sync.
- [ ] Invalidate or reject other live table/buffer-manager caches after rollback,
      once guards are released. Current Database mutation paths destroy their
      local Table before entering the rollback catch, but there is no general
      cache/handle coordination for lower-level users.
- [ ] Make recovery restartable after failure during replay, truncate, file
      sync, removal, or directory sync. Preserve unresolved evidence and block
      normal access, including reads, until recovery succeeds.
      Partial: retry after new-file deletion followed by a failed tables-directory
      sync is tested. General restart recovery and cleanup after journal unlink
      are not implemented.
- [ ] Treat a failure during final commit/cleanup synchronization as an uncertain
      outcome: preserve remaining evidence and require recovery before serving
      further operations. Do not report success or blindly retry the statement.
      Partial: `commit()` now marks the coordinator failed on cleanup errors, and
      Database commit remains outside the mutation catch. However, reads remain
      possible and journal unlink may already have succeeded when directory sync
      fails. Distinguish retrying cleanup from replaying undo; do not assume the
      journal pathname still exists or blindly roll back an uncertain commit.

## 5. Verification and completion criteria

- [x] Add end-to-end recovery event/failure injection using native-call wrappers.
      `db_statement_recovery_test` records create/write/sync attempts and file
      existence at each event. Low-level wrappers also test interrupted/short
      transfers and partial journal appends.
- [ ] Extend those probes to every persistence boundary, including journal
      append failures through the coordinator, resize, unlink, and final cleanup
      directory sync. They are not yet a complete simulated storage backend.
- [x] Verify metadata-only durable offsets, skipping an already-covered offset,
      counter reset after commit and rollback, empty statements, invalid offset
      rejection, and catchable journal-size mismatch errors.
- [ ] Verify page/file-specific coverage and batch barriers end to end once
      offset-aware writeback is connected. Include new appends after a sync,
      equal page IDs in different files, stale-generation coverage, and future
      eviction. Existing reader tests preserve paths/IDs but do not prove this
      writeback behavior.
- [x] Verify journal-file and journal-directory sync failures prevent table
      creation, and successful barriers precede the create call.
- [ ] Verify failed journal barriers prevent writes to existing tables too,
      and no failed barrier advances durable coverage.
- [x] Inject CREATE failures at file open, after an earlier page write succeeds,
      and at table-file sync. Verify rollback removes the new file, synchronizes
      its directory before journal retirement, and permits a subsequent statement.
- [ ] Inject write/sync failures after partial progress on existing tables and
      across multiple participating files. Check undo retention independently
      of which frames are still dirty or cached.
- [x] Test oversized/truncated records, path rejection, and bounded-memory
      journal record reading, including a large sparse journal.
- [x] Test both journal record types, consecutive records, reopening/appending,
      both new-file flag values, invalid flags, and preserving the read position
      after decoding failures. Reader fixtures encode the wire layout independently.
- [x] Test Database operations with a root different from the working directory,
      direct INSERT recovery integration, rollback after mid-batch validation
      failure, journal cleanup, and a subsequent successful statement.
- [x] Test reuse of a Table across committed statements, followed by rollback
      after an existing table's modified pages have been written and synchronized.
      Compare restored file bytes and rows; verify the file is not treated as new.
- [ ] Extend existing-table rollback tests to file growth, new-page link/header
      restoration, and truncation, including failures during those operations.
- [x] Test CREATE rollback after header construction fails, refusal to create
      without an active statement, begin failure without stale active state,
      rejection of failed-state commit/sync, and rollback when a recorded new
      file was never created.
- [x] Retry rollback after deletion followed by a failed tables-directory sync;
      require the journal to survive the failure and the retry to accept absence.
- [ ] Test final commit/cleanup failures, access blocked after recovery failure,
      and startup recovery of a leftover journal.
- [ ] Exercise malformed journals and cross-record validation through recovery,
      not just through the record reader. Extend recovery retries beyond the
      covered new-file deletion case.
- [ ] Inject failures before and after each persistence boundary and reopen.
      Observe either the previous complete state or the
      committed complete state, never a mixture. Process-termination tests alone
      do not simulate power loss because the OS cache survives; also use a fake
      backend that loses unsynchronized writes and exercises torn writes.
- [x] Include journal/recovery sources in `db_core` and pass the normal build
      and CTest suite. Reverified on 2026-09-30: all 12 tests pass.
- [x] Run focused ASan/UBSan checks. Reverified on 2026-09-30: statement-recovery,
      database-storage, storage-cursor, journal-file, and journal-reader tests
      pass. LeakSanitizer is disabled because of the sandbox's ptrace restriction.
- [x] Pass a fresh full warnings-as-errors build. Verified on 2026-09-30 with
      GCC 14 and `-Werror`, including the executable and all test targets. The
      previously reported qualifier and unused-variable/parameter warnings are
      fixed.

## Recommended next steps

1. Gate normal database access while recovery is required, including reads and
   existing handles. Propagate lower-level database write/sync failures into
   that state, and define what happens after journal unlink succeeds but its
   directory sync fails.
2. Enforce page/file coverage at writeback and make commit's all-files-durable
   requirement explicit. Add existing-table growth/truncation and partial-write
   failure tests alongside this work.
3. Specify the recoverable journal format, then recover leftover journals before
   exposing tables. Include interrupted appends, corruption, exclusive recovery
   ownership, safe target paths, and failure during recovery itself. Finish
   database-root parent-directory persistence.
4. Extend the existing failure-injection tests through all recovery/retirement
   boundaries and simulate loss of unsynchronized data; passing ordinary file
   tests is not a power-loss guarantee.
5. Connect the implemented 64-bit offset barrier to page/file-specific coverage
   with statement generations, then optimize batch and single-page writeback.

Complete this synchronization milestone only when all normal database-write
paths enforce journal coverage, batching is tested, and sync errors prevent
unsafe progress. Claim statement atomicity and crash recovery only after the
separate recovery prerequisites and persistence-boundary tests also pass.
