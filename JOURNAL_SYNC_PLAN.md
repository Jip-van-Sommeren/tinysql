# Journal synchronization and database write-ordering plan

Date: 2026-09-15

Status: partially implemented; reviewed against the working tree, including
uncommitted changes, on 2026-09-30. Statement journaling, writeback barriers, and
basic rollback of existing tables are implemented. Startup recovery, complete
failure handling, and recovery of table creation are not complete. Checked items
describe implemented pieces, not a claim of crash-safe statement atomicity.

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
  direct `Database::insertRows()` now uses the same INSERT path. Direct
  `Database::createTable()` still bypasses it.
- `PageGuard::page()` and `as()` provide read-only access. Writable access calls
  `BufferManager::prepareForWrite()` before modification: capture original file
  size, then capture an existing page's raw before-image once per path/page ID.
  Ordinary data-page allocation goes through a writable header first, so its
  original file size is captured before growth. Newly appended pages need no
  page before-image.
- `flushPage()` and `flushAll()` call `ensureDurable()` before database writes.
  This synchronizes the journal and, on the first barrier, its directory.
  Table mutation methods write back and synchronize their table file before
  `commit()` removes the journal and synchronizes the journal directory.
- `JournalFile::readNext()` streams bounded file-size and page-image records;
  `readPages()` has been removed. Rollback replays pages, restores recorded file
  sizes, synchronizes those files, and removes the journal with directory sync.
  It does not yet undo file creation or run automatically when opening a DB.
- All 11 CTest tests pass. There is a regression test for rollback after a
  mid-batch validation error, but it fails before database writeback. It does
  not establish rollback after on-disk partial updates or crash safety.

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
      Tests currently inject native-call failures through link-time wrappers;
      the event-recording recovery backend in section 5 is still pending.
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
- [x] Synchronize the tables directory on the SQL CREATE TABLE success path.
      Undoing creation and covering the direct API remain pending below.
- [ ] Make database-directory creation durable too: synchronize the database
      root after creating `tables/` and `journal/`, and its parent after creating
      the root. Merely opening a `LinuxDirectory` does not synchronize it.
- [x] Throw on file synchronization failures. File destructors only close their
      descriptors and do not synchronize or silently commit pending work.
- [x] Keep normal commit and rollback synchronization explicit, outside
      destructors. The remaining failure-state and participant-tracking work is
      listed below.

## 2. Track journal coverage within a statement

- [x] Add a storage-owned `StatementRecovery` shared with buffer managers;
      reject nested statements on the same coordinator. Wire SQL mutations and
      direct `Database::insertRows()` through it.
- [ ] Cover all mutation entry points. Route direct `Database::createTable()`
      through the statement lifecycle, and require a valid active statement
      before lower-level creation changes the filesystem. Do not let public
      `initializeNewTable()` overwrite an existing table without undo coverage.
- [x] Capture existing pages before writable access through
      `PageGuard::write()` / `writePage()`, rather than relying on a dirty flag
      set after modification. Reject writable access without an active statement.
- [x] Identify pages by `(database-relative file path, page ID)` and deduplicate
      in the normal buffer-manager path, including page zero and linked-page
      changes. Clear capture bookkeeping after successful commit/rollback.
- [ ] Enforce active-state checks and deduplication inside the coordinator's
      capture methods too. `capturePageOnce()` currently relies on its caller
      checking `hasCapturedPage()`; the method itself appends unconditionally.
- [x] Capture original file size once before normal existing-table mutations
      and data-page allocation. Distinguish existing pages from newly appended
      pages using that size; retain metadata until statement resolution.
- [ ] Record original file existence as well as size. Make a nonexistence
      record durable before creating a table file. Current `FileBeforeImage`
      contains only a path and size, and `initializeNewTable()` captures neither.

The following offsets are a sync optimization, not a substitute for the
correctness work above. The current no-argument barrier synchronizes the whole
journal every time and is used by the existing writeback paths.

- [ ] Make `JournalFile::writePage(const PageBeforeImage&)` return a `uint64_t`
      end offset after a complete append succeeds. Store that offset against the
      page identity, and provide equivalent coverage for file-metadata records.
      Appending already avoids automatic synchronization.
- [ ] Track the last completely appended offset and the successfully synchronized
      offset separately. Qualify these offsets with the current statement/journal
      generation so coverage from an earlier statement cannot be reused.
      Only the append offset exists today.
- [ ] Provide `ensureDurable(requiredEnd)`: synchronize pending journal records
      when the required offset is not yet covered. Advance the durable offset
      only after all required file/directory synchronization succeeds. Already
      covered records require no additional sync, even if unrelated new records
      have since been appended.
      The current `uint32_t` overload is an empty stub: remove it until usable,
      or implement it with `uint64_t` offsets. Do not call it as a barrier yet.

## 3. Gate every database write

- [x] Put a journal barrier before both `flushPage()` and `flushAll()` writes.
      `flushAll()` performs one barrier before its batch, not one per page.
      Recovery uses separate `PageFile` writes without generating undo records.
- [ ] Verify each dirty page's journal coverage at the writeback boundary:
      existing page image or original-size/nonexistence metadata, as appropriate.
      Synchronizing a journal does not establish that a particular page is
      covered; new-table initialization currently demonstrates this gap.
- [ ] Once offset tracking exists, collect the batch's highest required end
      offset and avoid redundant syncs for already-covered single-page writes.
      Future eviction must use the same checked writeback path.
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
- [ ] Apply fail-closed handling consistently to append, database-write, and
      database-sync errors, including lower-level callers. Retain undo state and
      forbid normal access until rollback/recovery succeeds. The Database catch
      paths attempt rollback, but not every failure marks coordinator state.
- [ ] Make both journal append methods reject a previously failed append.
      `writePage()` and `sync()` check `appendFailed_`;
      `writeFileBeforeImage()` currently does not. No append may proceed past a
      partially written record.

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
- [ ] Harden lifecycle transitions and invalid calls. Set active state only
      after journal creation succeeds; validate active/failed state in capture
      and commit; reject normal reads and writes while recovery is required.
      Currently `begin()` can leave `active_` true after creation fails, and
      `commit()` does not reject failed/inactive state.
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
      records. Reject conflicting original-size records for the same path.
- [ ] Validate recovery records together: require file metadata for every page
      image, page-aligned original sizes, valid page ranges, and consistent
      duplicates. A page-only target is currently written but omitted from the
      final file-size/sync loop. Reject such journals before unsafe replay.
- [ ] Confine replay to intended table files, not just lexical relative paths.
      Current decoding rejects absolute paths, `..`, NULs, and oversized paths,
      but does not enforce the tables subtree or prevent symlink escape.
- [ ] Undo table creation by removing files that did not exist beforehand and
      synchronizing the tables directory before journal retirement. SQL CREATE
      currently has a lifecycle wrapper but no file-existence undo record.
- [ ] Invalidate or reject other live table/buffer-manager caches after rollback,
      once guards are released. Current Database mutation paths destroy their
      local Table before entering the rollback catch, but there is no general
      cache/handle coordination for lower-level users.
- [ ] Make recovery restartable after failure during replay, truncate, file
      sync, removal, or directory sync. Preserve unresolved evidence and block
      normal access, including reads, until recovery succeeds.
- [ ] Treat a failure during final commit/cleanup synchronization as an uncertain
      outcome: preserve remaining evidence and require recovery before serving
      further operations. Do not report success or blindly retry the statement.
      `commit()` currently throws on cleanup errors but does not mark recovery
      required; journal unlink may already have succeeded when directory sync
      fails. The Database commit calls correctly remain outside the mutation
      catch blocks, avoiding an automatic rollback after an uncertain commit.

## 5. Verification and completion criteria

- [ ] Use an injectable storage backend that records append, write, file-sync,
      directory-sync, resize, and removal events and can fail each boundary.
      Low-level LinuxFile wrappers already cover interrupted/short transfers
      and failures; end-to-end recovery event/failure injection is still missing.
- [ ] Verify that multiple journal appends cause no immediate sync, one batch
      barrier covers multiple database writes, and already covered pages do not
      trigger redundant barriers.
- [ ] Verify that append, journal-sync, and journal-directory-sync failures
      prevent all dependent database writes and do not advance durable coverage.
- [ ] Exercise repeated modification/eviction of one page, new records after a
      sync, and equal page IDs in different files. Verify generation reset
      between statements and coverage of original file size/existence.
- [ ] Fail database writes and database sync after earlier pages have succeeded.
      Ensure the coordinator retains undo information and tracks written files
      independently of cached-frame dirty flags.
- [x] Test oversized/truncated records, path rejection, and bounded-memory
      journal record reading, including a large sparse journal.
- [x] Test both journal record types, consecutive records, reopening/appending,
      and preserving the read position after decoding failures.
- [x] Test Database operations with a root different from the working directory,
      direct INSERT recovery integration, rollback after mid-batch validation
      failure, journal cleanup, and a subsequent successful statement.
- [ ] Test rollback after modified pages have actually reached the database
      file, including file growth, header/link restoration, and truncation.
      The existing validation-failure regression happens before writeback.
- [ ] Test CREATE rollback, the direct CREATE API, begin/commit failure states,
      reads blocked after recovery failure, and startup with a leftover journal.
- [ ] Exercise malformed journals and cross-record validation through recovery,
      not just through the record reader. Retry recovery after an injected
      recovery failure and verify that undo information is not lost.
- [ ] Inject failures before and after each persistence boundary and reopen.
      Observe either the previous complete state or the
      committed complete state, never a mixture. Process-termination tests alone
      do not simulate power loss because the OS cache survives; also use a fake
      backend that loses unsynchronized writes and exercises torn writes.
- [x] Include journal/recovery sources in `db_core` and pass the normal build
      and CTest suite. Verified on 2026-09-30: all 11 tests pass.
- [x] Run focused ASan/UBSan checks. Verified on 2026-09-30: database-storage,
      storage-cursor, select-executor, and decimal tests pass; LeakSanitizer is
      disabled because of the sandbox's ptrace restriction.
- [ ] Restore a clean warnings-as-errors build. The 2026-09-30 GCC 14 check fails
      on the ignored top-level `const` return qualifier of
      `getOriginalFileSize()` and unused parameters/variables in recovery and
      buffer-manager code. Normal build success does not cover this check.

## Recommended next steps

1. Harden coordinator state and entry points: begin failure, capture/commit
   preconditions, failed-state access blocking, both append failure guards, and
   direct CREATE integration. Remove the empty offset-barrier overload until
   it can provide a real guarantee.
2. Add file-existence undo for CREATE and enforce page/file coverage at every
   writeback boundary. Make commit's all-files-durable requirement explicit.
3. Specify and validate the recovery format, then recover leftover journals
   before exposing tables. Include safe handling of interrupted append/cleanup
   and restartable recovery; complete directory-creation persistence.
4. Add deterministic end-to-end failure tests alongside those changes,
   especially failures after database writes and during recovery/retirement.
5. Optimize repeated journal synchronization with 64-bit record-end offsets
   and statement generations after the correctness path is established.

Complete this synchronization milestone only when all normal database-write
paths enforce journal coverage, batching is tested, and sync errors prevent
unsafe progress. Claim statement atomicity and crash recovery only after the
separate recovery prerequisites and persistence-boundary tests also pass.
