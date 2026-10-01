# Capacity Observatory - Persistence

This document is the exact description of what the store writes, when a write
becomes durable, how a damaged log is handled, and what is not claimed. It
describes only behaviour that exists in `src/store_file.cpp`,
`src/platform.cpp`, `include/capacity_observatory/store.hpp`, and
`include/capacity_observatory/io.hpp`.

## 1. Store directory layout

`co::Store::open` takes a directory (`co::StoreOptions::directory`) and
creates it if it does not exist (`co::io::ensure_directory`). Four files live
there:

| file | role | created by |
|---|---|---|
| `evidence.log` | the append-only framed evidence log; the only file that grows | a writable open |
| `snapshot.json` | the atomically published derived snapshot; disposable | `co::Store::publish_snapshot` |
| `epoch` | the session epoch, atomically published | a writable open |
| `writer.lock` | the kernel-enforced single-writer lock file | a writable open |

Deleting `snapshot.json` loses nothing: evidence lives in the log, and the
snapshot can be recomputed. Deleting or truncating `evidence.log` loses
evidence, and a truncated log is either repaired as a torn tail or refused as
interior corruption (section 5).

## 2. The log header

The first 16 bytes of `evidence.log` are the header. All integers are
little-endian.

| offset | size | content |
|---|---|---|
| 0 | 8 | magic `COBSTORE` (bytes `43 4F 42 53 54 4F 52 45`) |
| 8 | 2 | format version (currently 1) |
| 10 | 2 | reserved, written as zero |
| 12 | 4 | CRC-32C (Castagnoli) of bytes 0..11 |

The header is written once, when a writable open finds a zero-length file, and
is flushed before the open returns. A read of a non-empty file that fails any of
these checks is refused:

* shorter than 16 bytes: `ReasonCode::BadMagic` with the observed size;
* wrong magic: `BadMagic`;
* version other than 1: `UnsupportedVersion`;
* header CRC mismatch: `IntegrityMismatch`.

## 3. The frame format

Every record after the header is a frame: an 8-byte frame header followed by a
payload.

| offset | size | content |
|---|---|---|
| 0 | 4 | payload length in bytes |
| 4 | 4 | CRC-32C of the payload bytes |
| 8 | length | the payload: canonical compact JSON |

The payload is produced by `co::json::dump(value, canonical = true,
indent = -1)`: object members are sorted by key and no insignificant
whitespace is emitted (`include/capacity_observatory/json.hpp`). The on-disk
bytes are therefore deterministic for the same logical record, and an external
tool can parse a payload with any strict JSON parser.

Record kinds (`co::StoreRecordKind`) and their payload members:

| kind | value | members |
|---|---|---|
| EpochAdvance | 1 | `v, kind="epoch", seq, epoch` |
| Evidence | 2 | `v, kind="evidence", seq, epoch, mutation, digest, record` |
| Snapshot | 3 | `v, kind="snapshot", seq, epoch, digest, bytes` |
| Marker | 4 | `v, kind="marker", seq, epoch, mutation, digest` |

`seq` is the durable sequence number (the first frame of a fresh log is
sequence 1), `epoch` is the writer session epoch of the frame (section 7), and
`record` is the encoded `co::EvidenceRecord`: authority, role, scope,
dimension, assertion, unit, unit_dimension, unit_numerator, unit_denominator,
declared, amount, epoch, generation, revision, provenance, has_observed_at,
observed_at, validity_from_present, validity_from, validity_until_present,
validity_until, source. The canonical writer sorts these members, so the byte
order on disk is the sorted order.

A payload longer than `StoreOptions::max_frame_bytes` (default 1 MiB) is
refused at append time with `ReasonCode::RecordTooLarge`, and a snapshot
document longer than that bound is refused the same way.

## 4. The commit point

`Store::append_frame` performs, in order:

1. write the 8-byte frame header;
2. write the payload;
3. `co::io::File::sync()`, which is `FlushFileBuffers` on Windows and
   `fsync` elsewhere;
4. return the outcome.

Step 3 is the commit point. Before the call returns, the frame is either fully
durable or not durable at all; there is no state in which a caller has been told
that a record committed while the bytes are still in a volatile cache
(`include/capacity_observatory/store.hpp`). The sequence number and, for
evidence, the mutation identity and its digest are inside the same payload, so a
retry can be answered without a second durable write.

If any step fails, the append returns a status (`IoFailure`, `ShortWrite`,
`FlushFailure`, `CapacityExceeded`, `RecordTooLarge`) and the store does not
update its in-memory sequence or mutation map. A partially written frame that
remains in the file after such a failure is a torn tail and is handled by the
next open (section 5).

## 5. Recovery: torn tail versus interior corruption

A writable open scans the file, and so does `co::inspect_store` (which takes no
lock and never writes). The scan is exactly this:

```
read the 16-byte header and validate magic, version, and header CRC
offset = 16
while offset < file_size:
    remaining = file_size - offset
    if remaining < 8:
        -> torn tail, discard 'remaining'
    read the 8-byte frame header (payload_length, stored_crc)
    if payload_length == 0:
        -> resync probe: valid frame later? interior corruption
           otherwise: torn tail, discard from offset
    if payload_length > max_frame_bytes:
        if the whole frame is present in the file:
            -> CapacityExceeded (configuration mismatch, nothing truncated)
        otherwise:
            -> torn tail, discard from offset
    if offset + 8 + payload_length > file_size:
        -> torn tail, discard from offset   (nothing can follow it)
    read the payload
    if crc32c(payload) != stored_crc:
        if bytes follow the complete frame:
            -> InteriorCorruption (nothing truncated)
        else:
            -> resync probe: valid frame later? interior corruption
               otherwise: torn tail, discard from offset
    accept the frame; offset += 8 + payload_length
```

The resync probe asks whether a well-formed frame exists after the damage. It
runs two bounded checks and is reached only where the length field itself may be
damaged, or where a complete-length frame fails its CRC at the very end of the
file:

```
find_later_valid_frame(damage_offset, file_size):
    # 1. the natural resynchronisation point: if the damaged frame's length field
    #    were intact, the next frame would begin here
    if the 8 bytes at damage_offset declare a length L with 0 < L <= max_frame_bytes
       and damage_offset + 8 + L <= file_size
       and the frame at damage_offset + 8 + L validates:
        return true
    # 2. a bounded byte-by-byte search, for a damaged length field
    limit = min(file_size, damage_offset + 4096)
    for candidate in [damage_offset + 1, limit):
        if the frame at candidate validates: return true
    return false

validates_as_frame(offset):
    a full 8-byte header fits before file_size
    length != 0 and length <= max_frame_bytes
    offset + 8 + length <= file_size
    crc32c(payload at offset + 8) == stored CRC
```

The classification that results:

* **Interior corruption**: a complete frame fails its CRC while further bytes
  remain, or the resync probe finds a valid frame after the damage. The open
  fails with `ReasonCode::InteriorCorruption` and nothing is truncated;
  `inspect_store` reports the same failure. Silently treating this as a tail
  would discard every committed record that follows the damage.
* **Configuration mismatch**: a frame that is fully present but declares more
  than `StoreOptions::max_frame_bytes` fails with
  `ReasonCode::CapacityExceeded` and a message naming the offset and the limit
  to raise. It is not corruption and is not truncated away.
* **Torn tail**: the damage is at the end of the file and no valid frame follows
  it. The scan stops at the end of the last complete frame,
  `RecoveryReport::torn_tail_recovered` is set, and `bytes_discarded` reports
  how many bytes were abandoned. On a writable open the file is truncated to that
  point and flushed; a read-only open changes nothing and only reports.

A complete length field followed by missing or unwritten payload bytes is a torn
tail by design: a process that died before its flush can leave exactly that
shape, and because nothing can follow an incomplete final frame, truncating at
its start loses nothing that was ever committed.

Consequences a caller should know:

* Truncation is conservative: everything from the first unreadable frame onward
  is discarded, including any later bytes. That is only ever done when no valid
  frame follows the damage within the probe window.
* Interior corruption is rejected, never skipped and never repaired: the log is
  refused rather than silently losing a record from the middle.
* A store written with a larger `max_frame_bytes` than the reader is configured
  with is refused with `CapacityExceeded`, not truncated.
* The scan is linear in the file size and re-parses and re-validates every
  payload, so recovery costs grow with the log; the benchmark in
  `bench/bench_main.cpp` measures a real reopen of a real log.
* The recovery counters describe the scan. The epoch frame that the current open
  appends is not part of `frames_read` or `epoch_frames`; a later
  `inspect_store` of the same directory counts it.

## 6. Atomic publication of `snapshot.json` and `epoch`

`co::io::write_file_atomic` is the only publication path:

1. open `<path>.publish.tmp` (for example `snapshot.json.publish.tmp`),
   truncate it to zero, write the bytes, flush, and close;
2. replace the destination atomically: `MoveFileExW` with
   `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` on Windows,
   `rename` on POSIX.

The replacement is the publication point: a reader observes either the previous
complete file or the new complete file, never a mixture. A failure before step 2
leaves the destination untouched, and the temporary file is removed. A stale
`.publish.tmp` left behind by a crash is never promoted to the published file;
the next publication truncates it.

`Store::publish_snapshot` writes `snapshot.json` first and then appends a
Snapshot frame carrying the document's SHA-256 digest and its length, and flushes
it. The two are not one transaction: a crash between them publishes a snapshot
whose digest record is missing. The file itself is still complete and readable
(`Store::read_snapshot` returns it, bounded by `max_frame_bytes`), and the
digest frame is a record of what was published, not the publication itself.

The `epoch` file is published the same way. It is exactly 12 bytes: an 8-byte
little-endian epoch followed by its CRC-32C. A missing file means epoch 0; a file
of any other length, or with a bad CRC, is refused with
`ReasonCode::IntegrityMismatch`.

## 7. Session epochs, and why the store epoch is not the authority epoch

The store's session epoch identifies the writer session; it is not a capacity
epoch and carries no authority.

On every writable open (`Store::open`):

1. the durable epoch is the larger of the newest epoch found in the log's
   EpochAdvance frames and the value in the `epoch` file;
2. if that value is the maximum representable epoch, the open fails with
   `ReasonCode::EpochExhausted`;
3. `next_epoch = durable_epoch + 1` is published to the `epoch` file;
4. an EpochAdvance frame carrying `next_epoch` (and the next sequence number)
   is appended and flushed before any other write of the session.

Publishing the epoch before any other write means a reader can always tell which
session produced a frame. A fresh store therefore starts at session epoch 1 and
its first open consumes sequence number 1 for the epoch frame.

`Store::append_evidence` deliberately does not compare an evidence record's
epoch with the store's session epoch. The implementation comment states the
reason: the record's epoch belongs to the authority that asserted it, and
conflating the two would refuse legitimate evidence after a store restart.
Authority epoch and generation staleness is decided by the acceptance window
(`co::EvidenceWindow::consider`, see `docs/ARCHITECTURE.md` section 5), which
compares against the per-slot watermark. What the store enforces at append time
is the mutation identity and the digest (section 8).

## 8. Idempotency lives in the same durable commit

`Store::append_evidence` requires a non-empty `co::MutationId` and computes
`co::EvidenceRecord::content_digest()` before writing. It then consults the
mutation map it built while scanning the log:

* same mutation, same digest: `applied = false`,
  `ReasonCode::IdempotentReplay`, no second append;
* same mutation, different digest: refused with
  `ReasonCode::IdempotencyConflict`;
* unknown mutation: the frame is written with the mutation id, the digest, and
  the sequence number in the same payload, and committed with one flush.

If the first attempt failed before its flush, the frame is a torn tail, the
mutation was never recorded, and a retry with the same identity simply commits.
If the first attempt committed, the retry is answered from the recovered map
without touching the disk. That is the whole retry contract, and it is why the
identity lives in the payload rather than in a side file.

`Store::append_idempotent_marker` records an identity and digest without an
evidence payload, for callers that commit their own payload in the same
operation. Marker frames restore the mutation map on the next open exactly like
evidence frames.

## 9. The single-writer lock

`co::io::ExclusiveLock` is a kernel-enforced advisory lock on
`writer.lock`:

* Windows: the file is opened with `OPEN_ALWAYS`, then
  `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 1 byte)`;
* POSIX: `open(O_RDWR | O_CREAT)` then `flock(LOCK_EX | LOCK_NB)`.

Acquisition is non-blocking. When another holder exists the result is
`ReasonCode::LockHeld` ("exclusive lock on <path> is held by another
process"), not a wait. The lock is held for the lifetime of the `co::Store`
object and released when the object is destroyed or the process dies, including
abnormal termination, because the operating system releases the handle. There is
no stale-lock file to clean up and no lock timeout.

A read-only open (`StoreOptions::read_only`) never creates the lock file and
never takes the lock, and never writes: it does not create a missing log (a
read-only open of a directory whose `evidence.log` does not exist fails with
`IoFailure`), and it never truncates a torn tail. `inspect_store` likewise
takes no lock. The lock excludes other cooperating writers; it cannot stop a
process that writes the log without taking it.

## 10. Bounded sizes

| bound | default | effect |
|---|---|---|
| `StoreOptions::max_frame_bytes` | 1 MiB | an appended payload larger than this is refused with `RecordTooLarge`; during recovery a frame that is fully present but larger is refused with `CapacityExceeded` (raise the option), and one that is not fully present is a torn tail |
| `StoreOptions::max_file_bytes` | 4 GiB | opening a larger log fails with `CapacityExceeded`; an append that would push the file past it fails with `CapacityExceeded` |
| `co::io::read_file` bound | caller supplied | a larger file is refused with `CapacityExceeded`; the `epoch` file is read with a 64-byte bound and must be exactly 12 bytes |
| JSON nesting depth | 64 | the parser's absolute ceiling is 64 levels; `co::json::Value::parse(text, max_depth)` can lower it but never raise it, so a deeper payload is refused. A leading UTF-8 byte order mark is skipped (RFC 8259 section 8.1 permits ignoring one) while every diagnostic still reports offsets into the caller's original bytes |

Nothing in the store grows without a bound: the log has `max_file_bytes`, each
frame has `max_frame_bytes`, the refusal audit has its own capacity
(`EvidenceWindow::kDefaultRefusalCapacity`, 4096), and the in-memory mutation
map is bounded by the number of distinct mutations the log contains.

## 11. What is not proven, and what is not claimed

For this document: every format, algorithm, and ordering rule described above is
**REAL** - it is implemented in the cited files and was exercised on this machine
against a real filesystem store (see `bench/bench_main.cpp`, rows
`store-durable-append`, `store-reopen-recovery`, and `store-inspect-scan`).
The *evidence records* used by those rows are **SYNTHETIC**: they are generated
from a printed seed, not captured from a facility. The POSIX durability path and
power-loss behaviour are **UNSUPPORTED** on this machine, as detailed below.

* **Power-loss atomicity beyond the operating system's flush guarantees is not
  claimed.** The commit point is `FlushFileBuffers` / `fsync` on the log
  file. The implementation does not claim anything about drive caches that ignore
  flush, sector remapping, or the ordering of filesystem metadata.
* **No directory flush after an atomic replace.** On POSIX, `rename` publishes
  the new file but the containing directory is not fsynced, so the rename itself
  may not survive a power loss; on Windows `MOVEFILE_WRITE_THROUGH` is used.
* **The POSIX code paths are compiled but unvalidated on this machine.** The
  build and every measurement reported in this repository were produced on
  Windows with MSVC 19.44; the `#else` branches in `src/platform.cpp`
  (`open`, `pread`, `ftruncate`, `fsync`, `flock`, `rename`,
  `mkdir`, `stat`) have not been executed here.
* **Interior corruption is detected, never repaired.** The file is rejected with
  `InteriorCorruption`.
* **Publication of `snapshot.json` and its log record are not one
  transaction.** A crash between them leaves the file published and the digest
  record absent.
* **Readers are not locked out.** A reader can open the log at any moment,
  including in the middle of a writer's append. Framing makes an incomplete tail
  detectable, but a reader can also observe a complete frame that was written but
  not yet flushed; durability is only guaranteed once the append call returns.
* **The lock is advisory.** It excludes writers that use the same protocol on the
  same file, and nothing else.
* **No compression, no encryption, no key management.** Payloads are plain
  UTF-8 JSON; anyone with read access to the directory can read every record.
* **Snapshot content is not validated against the log on read.**
  `Store::read_snapshot` returns the bytes; the digest recorded in the log is
  available for comparison but no automatic comparison is performed.
