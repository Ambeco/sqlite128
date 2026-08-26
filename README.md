<h1 align="center">sqlite128 — a SQLite fork with wide-key rowid tables</h1>

This is a fork of [SQLite](https://sqlite.org/). For everything about building, testing, and
navigating the underlying SQLite source tree (version control, compiling, source tree map, the
amalgamation, etc.), see mainline SQLite's own
**[README.md](https://github.com/sqlite/sqlite/blob/master/README.md)** — none of that is
duplicated here. This file covers only what's specific to this fork.

## 1. Goal

Mainline SQLite only lets a `PRIMARY KEY` become an alias for the table's rowid — getting the
table-b-tree storage layout a rowid table's key already gets (interior pages hold only key + child
pointer, so fanout is high and the tree stays shallow) — when that key is a single `INTEGER`
column. Every other primary key is forced into a worse option: an ordinary rowid table plus a
separate `UNIQUE` index and an extra `O(log n)` lookup on every access, or a `WITHOUT ROWID` table,
whose index-b-tree format stores the *full row payload in every node, interior and leaf alike*,
collapsing fanout for anything but the narrowest rows.

This fork has two goals:

1. **Extend rowid-aliasing to `REAL` and fixed-width `BLOB`/`TEXT` primary keys, up to 8 bytes.**
   A `NOT NULL REAL`, `BLOB(N)`, or `TEXT(N)` (N≤8) primary key that satisfies specific `CHECK`
   constraints (§2) becomes a genuine rowid alias, exactly like `INTEGER PRIMARY KEY` today — no
   separate index, no extra lookup hop, and a smaller database file.
2. **Widen the rowid itself to 128 bits, extending that same treatment up to 16-byte `BLOB`/`TEXT`
   primary keys.** A UUID is exactly 16 bytes — the motivating case for this whole fork — and this
   is what lets a UUID primary key get the same compact, high-fanout storage an `INTEGER PRIMARY
   KEY` already gets, instead of a hidden index or a bloated `WITHOUT ROWID` tree.

## 2. Design and limitations

The underlying mechanism is a 128-bit rowid: `rowid_t` widens from a 64-bit to a 128-bit
signed integer (opt-in via the `SQLITE_128BIT_ROWID` compile flag; a default build is completely
unaffected — every bit of this work is `#ifdef`-gated out). That gives enough room to embed a
16-byte value as a table's rowid directly, rather than needing a variable-length b-tree key change
throughout the storage engine.

**Which primary keys qualify.** A single-column `PRIMARY KEY` gets rowid-aliasing if its declared
type is *exactly* `BLOB`, `TEXT`, or `REAL` (not synonyms like `VARCHAR`/`CHAR`/`DOUBLE`/`FLOAT` —
see §4) **and** it carries the specific `CHECK` constraints that prove it's always exactly the
right fixed width and type at runtime:

| Type | Required constraints |
|---|---|
| `BLOB(W)`, W≤16 | `NOT NULL`, `CHECK(typeof(col)='blob')`, `CHECK(length(col)=W)` |
| `TEXT(W)`, W≤16 | `NOT NULL`, `CHECK(typeof(col)='text')`, `CHECK(length(CAST(col AS BLOB))=W)`, and effective collation must resolve to `BINARY` (the default, unless overridden) |
| `REAL` | `NOT NULL`, `CHECK(typeof(col)='real')` |

The 16-byte cap only applies under `SQLITE_128BIT_ROWID`; in a default build the effective cap is 8
bytes, since a default build's rowid is still a plain 64-bit integer (only `SQLITE_128BIT_ROWID`
widens it to 128 bits) and only widths that actually fit are ever accepted. `REAL` is always exactly
8 bytes and qualifies under either build.

This mirrors, deliberately, how `INTEGER PRIMARY KEY` itself already works in mainline SQLite: it's
spelling-sensitive (`INT PRIMARY KEY` does *not* get rowid-aliasing, only literally `INTEGER`
does), and it disallows `NULL` because a rowid can never be null. The `CHECK` constraints are the
explicit, schema-visible way a developer opts into the tradeoffs — this fork deliberately does
**not** auto-coerce or auto-enforce these types under the hood; the constraints are how SQLite's
existing engine (which already always enforces `CHECK`) does the enforcement, not a new bespoke
mechanism.

The `typeof(col)=...` checks matter more than they might look: `BLOB` and `TEXT` are the two
SQLite affinities that never coerce *each other* — a column declared `BLOB` can, at runtime,
actually hold a `TEXT` value and vice versa (SQLite's manifest typing allows it, and neither
affinity's coercion path touches the other's type). Without the `typeof()` check, a same-byte-length
value of the wrong type could silently satisfy the length `CHECK` and slip into what's supposed to
be a strictly-typed rowid-backing column.

**Byte/bit encoding.** Every source type applies a canonicalizing transform on the way into and out
of the 128-bit rowid, so that one uniform *signed* 128-bit integer comparison gives the correct
order for every case, without the b-tree/VDBE/comparison layers needing to know which source type
a given table's rowid came from:
- `BLOB`/`TEXT`: raw bytes, high-aligned, zero-padded low, with the top bit of the first byte
  flipped (standard unsigned-to-signed order-preserving transform) so plain signed comparison
  matches true `memcmp` order.
- `REAL`: the sign bit is flipped for positive values, all bits are flipped for negative values
  (the standard IEEE-754-bits-to-monotonic-orderable-int transform) so numeric order — including
  among negative values — is preserved, rather than the reversed order raw IEEE-754 bit patterns
  would otherwise give for negatives.

**Safety-net for near-miss schemas.** If a single-column primary key's declared type *looks like*
an attempt at this optimization (the right type spelling plus a width specifier or a related
`CHECK`) but doesn't fully satisfy every requirement above, `CREATE TABLE` fails outright with an
error explaining what to add or remove — rather than silently falling back to an unoptimized table
and letting the gap go unnoticed until it shows up as a production performance problem. Critically,
this only fires for a live `CREATE TABLE` statement, never while SQLite is merely *opening* an
existing database file — so a database created before this feature existed, whose schema happens
to coincidentally resemble a near-miss, still opens exactly as it always did.

**Known limitations (by design, not yet closed):**
- Only exact `BLOB`/`TEXT`/`REAL` spellings qualify for now — no synonyms (see §4).
- No UUID-formatted-string or blob auto-coercion — this is an explicit rowid *storage* mechanism,
  not an implicit type-conversion feature. `uuid_str()`/`uuid_blob()` (mainline SQLite functions,
  see `ext/misc/uuid.c`) remain the explicit way to produce a UUID string or the 16-byte blob you'd
  store in a `BLOB(16)` primary key.
- Width is capped at 8 bytes in a default build, 16 bytes under `SQLITE_128BIT_ROWID`.

## 3. Remaining work

The design in §1/§2 is fully implemented for both widths (up to 8 bytes in any build, up to 16
bytes under `SQLITE_128BIT_ROWID`), including secondary indexes on a narrow-PK table. What remains
is a single confirmed, reproducible bug — not unimplemented design:

- **The near-miss heuristic fires on ordinary, non-narrow-PK schemas.** `build.c`'s
  `narrowPKFinalize()` treats *any* width annotation on a `PRIMARY KEY`'s declared type
  (`TEXT(36)`, `VARCHAR(50)`, etc.) as a narrow-PK "near miss," even with no `CHECK` constraint on
  the column at all. Width-annotated primary keys with no such `CHECK` are an extremely common,
  ordinary, mainline-compatible pattern (e.g. schemas ported from other databases) — this fork
  currently rejects them outright. Minimal repro: `CREATE TABLE t(id text(36) not null primary
  key, v text)` raises a near-miss error with no `CHECK` anywhere in the statement. Confirmed
  fork-introduced (not pre-existing) via `test/tkt1449.test`. Likely fix: require an actual
  narrow-PK-shaped `CHECK` hit before treating a column as a near-miss, not a bare width
  annotation on the declared type.

Everything else originally suspected of being a `SQLITE_128BIT_ROWID` bug — across `btree.c`'s
cell-size and balancing code, `PRAGMA integrity_check`, `newDatabase()`'s meta-page setup,
`ext/recover/*`'s interaction with a wrapper VFS, and large swaths of `test/pragma.test` and the
`corruptL`/`corruptN`/`dbpage`/`diskfull`/`fts3corrupt4`/`fts3fuzz001` test clusters — has been
triaged to one of: already fixed, intentional by-design behavior (this fork's own wide magic
header, or the `BTS_LEGACY_NARROW` read-only-for-legacy-files mechanism), or confirmed transient
test-environment flakiness. None of that history is repeated here; see `git log` for the
individual fixes.

One further item was root-caused and fixed, but the fix lives in the *test*, not the engine, since
there was nothing wrong to fix in the engine itself: **`ext/fts5/test/fts5origintext4.test`**
(`fts5origintext4-1.2.1`) asserts that a broad, low-selectivity query uses more than a hardcoded
250000-byte page-cache threshold; under `SQLITE_128BIT_ROWID` it used only ~70KB (vs. ~327KB in a
default build). Root cause: FTS5's own internal leaf-page target size
(`FTS5_DEFAULT_PAGE_SIZE`=4050 bytes, unrelated to and unaware of this fork) sits just below this
build's table-leaf local-payload cap (`usableSize-35` bytes, e.g. 4061 for a 4096-byte page) in a
default build, so FTS5's ~4051-4055-byte leaf blobs fit entirely locally — but this fork's
table-leaf cap is intentionally 10 bytes smaller under `SQLITE_128BIT_ROWID`
(`usableSize-35-10`, `btree.c`), reserving room for the wide rowid key's worst-case 19-byte varint
(vs. 9 narrow) so fanout guarantees still hold for genuinely large keys — a real, necessary
correctness margin, confirmed by `test/boundary1.test`/`test/boundary3.test` needing exactly this
margin. That 10-byte difference was just enough to push FTS5's specific leaf size over the
threshold, forcing those blobs to spill into overflow pages. Once a payload overflows, mainline
SQLite's own `SQLITE_DIRECT_OVERFLOW_READ` optimization (on by default, confirmed via a build with
`-DSQLITE_DIRECT_OVERFLOW_READ=0` restoring the page-cache usage to ~366KB) reads overflow content
directly from the file, deliberately bypassing the page cache — which is exactly why less showed up
in `sqlite3_db_status(..., CACHE_USED, ...)`. No data was ever lost or misread (verified:
`ft('the')`'s result set has the identical count/sum/min/max as an unfiltered scan in both builds);
this was purely two independently-correct behaviors — a necessary wide-rowid safety margin, and an
unrelated, deliberate SQLite cache-bypass optimization — intersecting at a page-size constant
neither side has any awareness of the other choosing. Reducing the wide-rowid margin to avoid this
would reopen the real corruption risk it exists to prevent, so instead the *test* was adjusted,
gated so a default build's copy is untouched: `SQLITE_128BIT_ROWID` was added to the compile-option
list `sqlite3_compileoption_used()` reports (`tool/mkctimec.tcl`), and the test now uses that (via
`sqlite_compileoption_used('128BIT_ROWID')`) to force FTS5's own `pgsz` config down to 4000 bytes
—  comfortably under the reduced local-payload cap — only under `SQLITE_128BIT_ROWID`, restoring
every leaf to fitting entirely locally (and thus back in the page cache) in both builds. A default
build's test run is unaffected byte-for-byte (the added block is skipped entirely).

## 4. Potential follow-up work

- **Type-spelling synonyms.** Only exact `BLOB`/`TEXT`/`REAL` qualify today; `CHAR`/`VARCHAR`/
  `NCHAR`/`NVARCHAR`/`CLOB` (text-like) and `DOUBLE`/`FLOAT`/`DOUBLE PRECISION` (real-like) are
  deliberately out of scope for the initial implementation. Follow-up: either extend eligibility to
  cover them, or at minimum make sure they trigger the same near-miss error as an exact-spelling
  near-miss, rather than silently not qualifying.
- **A `UUID` column type.** Sugar for `BLOB(16) NOT NULL CHECK(length(col)=16)`, declared as its own
  type name. Deliberately *not* designed yet: a `UUID`-declared column would likely be expected to
  also accept UUID-*formatted strings* directly (auto-parse-and-convert on write), which reopens
  the general question of implicit, format-sniffing-based type coercion — a real safety/ergonomics
  tradeoff (a 32-hex-character string with dashes silently becoming a large integer on insert)
  that needs its own dedicated design pass before being built.
- **Wider fixed hash-shaped rowids.** The same mechanism generalizes past 16 bytes if `rowid_t`
  were widened further: 128 bits (16 bytes) covers UUID/MD5; 160 bits (20 bytes) would cover
  SHA-1; 256 bits (32 bytes) would cover SHA-256; 512 bits (64 bytes) would cover SHA-512 — letting
  a table keyed by a content hash get the same compact table-b-tree treatment. Not scoped or
  started; would need its own width-generalization pass through the `sqlite3_uint128`-shaped
  arithmetic/comparison/storage layers this fork already built for 128 bits.
- **Known bugs.** See §3.

## 5. Attribution

This fork's direction, goals, and design decisions — including every constraint, tradeoff, and
safety mechanism described in §2 — are [Ambeco](https://github.com/Ambeco)'s. Claude (Anthropic)
fleshed out the technical details, wrote all of the code, and ran all verification, under
Ambeco's direct guidance and review at each step.
