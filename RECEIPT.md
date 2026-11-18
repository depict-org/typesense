# Dropped collection compaction — local only

`POST /operations/db/compact_dropped` uses the existing authenticated operations
dispatch and member-local execution. It returns success, processed range bounds
and count, elapsed milliseconds, and live SST bytes before/after when available.

An ID below `next_collection_id` is dropped when absent from both the live ID map
and persisted collection metadata. The latter protects creation between its
metadata write and in-memory registration. Creation increments the counter and
persists it with metadata; dropping never decrements it. Recreating a name gets
a fresh ID (subject to the existing uint32 counter limit).

Ranges are ``[id_, id`)``. Allocated prefixes are sorted in RocksDB byte order
(thus `10_` precedes `1_`); consecutive dropped entries merge until a live entry.
N dropped collections produce 0 ranges for N=0, otherwise 1 through N ranges,
at most live-count + 1. Planning uses O(next-ID log next-ID) time and O(next-ID)
memory. Manager and Store locks are shared, permitting searches while preventing
new ID allocation/registration during the operation.

Each range first calls `DeleteFilesInRange` with an exclusive end, reapplies its
range tombstone, then calls `CompactRange`. Reapplying is necessary: a real
RocksDB probe reproduced resurrection when a deleted file held the tombstone
and an older mixed SST survived. The new tombstone prevents that resurrection.

Live ranges are excluded from deletion bounds. Whole-file deletion removes
only files wholly inside those bounds and skips L0; fallback compaction may
rewrite mixed files containing live keys and preserves their values. Whole-file
deletion needs near-zero temporary SST space. Fallback compaction has no small
fixed space bound: overlapping mixed SSTs/L0 can require database-scale rewrite
space, plus flush output. SST properties measure current-version SST bytes,
not filesystem free space; snapshots/checkpoints can delay physical reclamation.

Validation: `git diff --check` passed. The single repository regression test
creates three collections, drops two without compaction, checks empty dropped
ranges and live count/search/read, then checks fresh IDs, merging, and tombstone
resurrection. `bazel test //:typesense-test
--test_filter=CollectionManagerTest.CompactDroppedCollections` could not run:
Bazel is absent (exit 127); CMake/Make and project dependencies are also absent.

Supplemental probes compiled the actual new manager/Store method bodies with
minimal surrounding types against real Ubuntu RocksDB 8.9.1, using
`g++ -std=c++17 -Wall -Wextra -Werror`. All passed: 3,143,169 → 1,048,808 SST
bytes for two drops, all 1,000 live values unchanged, decimal-prefix merging,
pending creation protection, malformed-metadata rejection, all-dropped merging,
and the tombstone regression. These do not validate full Typesense compilation
or search execution. Bazel pins RocksDB 7.8.3; CMake pins 6.20.3; their checked
convenience headers provide the same `DeleteFilesInRange` signature.
Probe sources/output are saved under
`/Users/daniel/.codex/fleet/w28/lanes-20261118/tscompact/probe/`.

No push or production calls were made.
