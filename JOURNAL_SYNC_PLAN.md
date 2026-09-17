# Journal synchronization and database write-ordering plan

Date: 2026-09-15

Status: partially implemented; progress reviewed on 2026-09-16. The native file
backend and journal record I/O are implemented, but statement journaling,
protected writeback, and crash recovery are not enabled. Partially implemented
items remain unchecked, with notes describing the completed groundwork.

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
Concurrent transactions, LRU implementation, new SQL features, and the complete
journal recovery format are outside this plan. Dirty eviction must remain
disabled until the recovery prerequisites below are satisfied.

## Current implementation

- `PageFile` and `JournalFile` share the descriptor-owning `LinuxFile` backend.
  Both expose explicit `sync()` using `fsync()`; `PageFile::flush()` is a
  documented non-durable no-op because there is no application write buffer.
- `JournalFile` is included in `db_core`. It appends encoded before-images
  without automatically synchronizing, reopens existing journals at their end,
  and rejects further appends or synchronization after an append failure.
  `readNext()` buffers one bounded record and preserves its path and page ID;
  `readPages()` remains a convenience method that materializes all records.
- `BufferManager::flushPage()` writes a page and immediately clears its dirty
  flag. `flushAll()` writes all dirty pages, calls the non-durable `flush()`, and
  then clears flags. Neither path coordinates with a journal or commits a
  statement durably.
- `Table` currently flushes from its mutation methods. There is no shared
  statement coordinator protecting SQL and direct storage calls.
- Low-level tests cover interrupted/short transfers, injected I/O failures,
  malformed records, path rejection, and large-journal streaming. There is no
  directory synchronization or end-to-end recovery/failure-ordering test yet.

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
      completing `fsync()` on the same open file. The proposed direct-I/O-call
      backend has no C++ stream buffer, but still uses the OS cache; do not use
      `O_DIRECT`. Ordinary successful writes are not durability acknowledgements.
- [x] Keep `flush()` only as a documented non-durable compatibility operation
      for draining application buffers; it is a no-op for an unbuffered native
      backend. Durability-sensitive callers must use `sync()` explicitly.
- [ ] Add directory synchronization for journal creation and removal. A newly
      created journal must have both its contents and its directory entry made
      durable before database writes are authorized. Synchronize affected table
      directories when committing new table files as well. File `fsync()` alone
      does not persist directory entries; see
      [Linux fsync semantics](https://man7.org/linux/man-pages/man2/fsync.2.html).
- [x] Throw on file synchronization failures. File destructors only close their
      descriptors and do not synchronize or silently commit pending work.
- [ ] Keep commit and rollback synchronization explicit when those protocols
      are implemented. Unsupported platforms must report that durable mode is
      unavailable rather than treating a stream flush as equivalent.

## 2. Track journal coverage within a statement

- [ ] Add a database-scoped `StatementRecovery` coordinator owned by the storage
      layer and shared with participating tables/buffer managers. Reject nested
      modifying statements initially. Both SQL execution and direct mutation
      APIs must use the same coordinator or reject writes without one.
- [ ] Capture each existing page's original raw bytes before its first mutable
      access in the statement. Introduce an explicit before-modification hook at
      mutation sites; `markDirty()` alone is too late because callers currently
      invoke it after changing the page.
- [ ] Identify a page by `(database-relative file path, page ID)`, not page ID
      alone. Retain only one original image per identity per statement, including
      page zero and page-link/header-counter changes.
      Partial: journal records and readers preserve both fields, but statement
      capture and per-statement deduplication do not exist yet.
- [ ] Make `JournalFile::writePage(const PageBeforeImage&)` return a `uint64_t`
      end offset after a complete append succeeds. Store that offset against the
      page identity. Appending must not call `sync()` automatically.
- [ ] Track the last completely appended offset and the successfully synchronized
      offset separately. Qualify these offsets with the current statement/journal
      generation so coverage from an earlier statement cannot be reused.
- [ ] Provide `ensureDurable(requiredEnd)`: synchronize pending journal records
      when the required offset is not yet covered. Advance the durable offset
      only after all required file/directory synchronization succeeds. Already
      covered records require no additional sync, even if unrelated new records
      have since been appended.
- [ ] Journal original file size/existence before file growth or creation. New
      pages have no before-image; their writes depend on durable file-metadata
      records instead. Keep this metadata until the statement is resolved.

## 3. Gate every database write

- [ ] Add one coordinator-controlled writeback path used by `flushPage()`,
      `flushAll()`, and future eviction. Missing journal coverage must reject a
      modified page write, not silently bypass protection. Recovery writes use a
      separate explicit path so they do not generate new undo records.
- [ ] For `flushAll()`, prepare the dirty-page batch, collect its journal
      requirements, and call `ensureDurable()` once with the highest required
      offset. Only then issue the database writes. No journal fsync belongs
      inside the ordinary per-page append loop.
- [ ] For a single-page flush or future eviction, check that page's coverage
      first. If its original record is already durable, writeback needs no new
      journal sync. Otherwise synchronize pending records before writing it.
- [ ] Keep cache writeback state separate from statement durability. A successful
      complete write can make a frame clean relative to the OS-visible file, but
      does not commit the statement. Track every written table file separately
      so commit synchronizes it even when its dirty pages have been evicted.
- [ ] If a database write fails or is partial, retain the affected dirty state,
      journal, and transaction bookkeeping. A later failure must still restore
      previously written or evicted pages, not just the frames still marked dirty.
- [ ] On journal append/sync failure, issue no dependent database writes. Mark
      the statement failed and stop ordinary writes; do not advance offsets,
      discard undo information, or continue appending past a partial record.
      Explicit recovery must resolve the statement before reuse.
      Partial: a failed append leaves the append offset unchanged and blocks
      further appends/sync on that `JournalFile` object. Database writes are not
      yet gated by journal state, and there is no statement failure state.

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
Complete and test the following before enabling the protected writeback path in
normal operation or allowing dirty-page eviction:

- [ ] Specify a versioned journal format and durable commit/cleanup protocol.
      Before-images remain available until all database files are synchronized
      and the statement is durably finalized. Never clear the journal merely
      because database page writes returned successfully.
- [ ] Make journal parsing distinguish clean EOF, incomplete trailing records,
      and corruption. Validate record boundaries, lengths, checksums, paths, and
      statement identity. Later appends must not invalidate previously durable
      recovery information, including through torn writes to a shared sector.
      Do not blindly ignore every malformed final record.
      Partial: clean EOF, truncated records, bounded lengths, and relative paths
      are checked. Checksums, statement identity, and torn-write protection are
      not implemented.
- [ ] Recover an existing journal before exposing tables or beginning another
      statement; opening an existing journal must never reset its append offset
      to zero and overwrite unresolved recovery information.
      Partial: reopening preserves the append position at EOF; automatic
      recovery and access gating are not implemented.
- [x] Provide bounded streaming journal reads through `readNext()` rather than
      requiring the whole journal to be materialized.
      Preserve each record's file path as well as its page ID. Bound path/record
      lengths before allocation and reject invalid relative paths.
- [ ] Connect the streaming reader to recovery, including the remaining record
      validation above, and confine recovery writes to database files.
- [ ] Restore original pages, file lengths, and file existence on rollback;
      synchronize restored files and directory changes before retiring the
      journal. Invalidate affected cached pages after guards are released.
      Recovery itself must be restartable after another failure.
- [ ] Treat a failure during final commit/cleanup synchronization as an uncertain
      outcome: preserve remaining evidence and require recovery before serving
      further operations. Do not report success or blindly retry the statement.

## 5. Verification and completion criteria

- [ ] Use an injectable storage backend that records append, write, file-sync,
      directory-sync, resize, and removal events and can fail each boundary.
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
- [ ] Exercise those reader checks through the recovery path once connected.
- [ ] Once recovery exists, inject failures before and after each persistence
      boundary and reopen. Observe either the previous complete state or the
      committed complete state, never a mixture. Process-termination tests alone
      do not simulate power loss because the OS cache survives; also use a fake
      backend that loses unsynchronized writes and exercises torn writes.
- [x] Run the warnings-as-errors build, existing CTest suite, and focused
      ASan/UBSan checks. Track pre-existing failures separately. Add journal
      sources to `db_core` only when their interfaces and implementation build
      cleanly together.
      Verified on 2026-09-16: GCC 14 with `-Werror`, all 9 CTest tests passing,
      and the byte-I/O, Linux-file, journal-file, and storage-cursor tests passing
      with ASan/UBSan (leak detection disabled). Repeat after recovery changes.

Complete this synchronization milestone only when all normal database-write
paths enforce journal coverage, batching is tested, and sync errors prevent
unsafe progress. Claim statement atomicity and crash recovery only after the
separate recovery prerequisites and persistence-boundary tests also pass.
