<h1 align="center">sqlite128 — a SQLite fork with wide-key rowid tables</h1>

This is a fork of [SQLite](https://sqlite.org/). For everything about building, testing, and
navigating the underlying SQLite source tree (version control, compiling, source tree map, the
amalgamation, etc.), see mainline SQLite's own
**[README.md](https://github.com/sqlite/sqlite/blob/master/README.md)** — none of that is
duplicated here. This file covers only what's specific to this fork.

## 1. Goal

Mainline SQLite gives a table's primary key the fast, compact "table b-tree" storage layout
(interior pages hold only key + child pointer, so fanout is high and the tree stays shallow) only
when that key is a single `INTEGER PRIMARY KEY` column — the classic rowid-alias case. Any other
primary key forces a choice between two worse options:

- An ordinary rowid table plus a `UNIQUE` index on the real key — a hidden rowid, a second index,
  and an extra `O(log n)` hop on every lookup.
- A `WITHOUT ROWID` table — avoids the extra index, but uses SQLite's "index b-tree" format, which
  stores the *full row payload in every b-tree node, interior and leaf alike*, not just leaves.
  That collapses fanout and makes the tree deeper for anything but the narrowest rows.

**UUID primary keys are the motivating case.** A UUID is exactly 16 bytes, well within what a
table b-tree's key could hold — SQLite just doesn't let a key be anything but a 64-bit integer
today. This fork's goal is to close that gap: let a `REAL`, `TEXT(N)`, or `BLOB(N)` primary key
(for `N` ≤ 16 bytes, under specific safety constraints — see §2) get the exact same compact,
high-fanout table-b-tree storage `INTEGER PRIMARY KEY` already gets, instead of falling back to a
hidden index or a bloated `WITHOUT ROWID` tree.

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

## 3. Step-by-step plan and progress

This fork's earliest work (an approach storing a UUID as an actual 128-bit SQL `INTEGER` value) has
been reverted in full — it turned out to be unnecessary once the design converged on storing a UUID
primary key directly in its native `BLOB`/`TEXT` form (§2), which needs only the lower-level
`rowid_t`/`sqlite3_uint128` arithmetic, not a SQL-visible 128-bit `INTEGER` type. `INTEGER` is,
today, exactly the same 64-bit type it is in mainline SQLite. What follows is the plan going
forward, with step 1 already done.

1. **Wide-rowid foundation (done).** `rowid_t` typedef and core rowid abstraction; disk-I/O rowid
   handling and file-format width detection; `sqlite3_uint128` arithmetic primitives
   (`sqliteInt128.h`); `SQLITE_128BIT_ROWID` compiles; page-1 magic-header gate for wide-rowid
   files; whole-database wide-rowid codec with convert-only-via-SQL migration. This is the only
   piece of infrastructure the narrow-PK-as-rowid feature (§1/§2) actually depends on.
2. **Schema-level detection and validation (done).** At `CREATE TABLE` time, a single-column
   `BLOB`/`TEXT`/`REAL` primary key with the required `CHECK`/`NOT NULL`/collation constraints now
   becomes a rowid alias (`Table.iPKey`), same as `INTEGER PRIMARY KEY` — no separate index is
   built. Implemented as a deferred decision: `sqlite3AddPrimaryKey()` records a candidate rather
   than building an index immediately, since qualification depends on `CHECK` constraints that may
   not be parsed yet at that point; `sqlite3EndTable()` resolves it once constraint resolution is
   complete, into qualifies / near-miss error / silent fallback to an ordinary index, per §2's near
   -miss policy including the `db->init.busy` carve-out. Verified against 26 concrete test-case
   schemas under both a default and a `SQLITE_128BIT_ROWID` build. **Not yet implemented:** the
   actual storage encoding and read/write codegen (steps 3–9 below) — a qualifying table's schema
   is accepted, but `INSERT`/`SELECT` on its PK column still hits the unmodified INTEGER-only
   codegen and fails gracefully (`datatype mismatch`), not a crash.
   - `BLOB`/`TEXT` width is capped at 8 bytes in a default build and 16 bytes under
     `SQLITE_128BIT_ROWID` — a default build's `rowid_t` is still a plain 64-bit integer, so only
     `SQLITE_128BIT_ROWID` unlocks the full UUID-sized (16-byte) case. `REAL` (always exactly 8
     bytes) is unaffected either way.
3. **Value↔rowid conversion primitives (done).** `rowidFromNarrowBytes()`/`rowidToNarrowBytes()`
   (`BLOB`/`TEXT`) and `rowidFromReal()`/`rowidToReal()` (`REAL`) in `sqliteInt.h`, implementing
   §2's bit-canonicalization transforms. Verified standalone (round-trip and order-matches-memcmp/
   numeric-order checks across widths, boundary values, and randomized pairs) before any VDBE
   integration — not yet wired into `INSERT`/`SELECT`/`UPDATE` codegen (steps 4+).
4. **INSERT codegen (done).** Step 3's conversion is wired into `insert.c`/`vdbe.c`/`vdbemem.c` (via
   the new `sqlite3VdbeMemToRowid()` dispatch), so a qualifying table now gets compact table-b-tree
   storage on write. Fixed three real bugs surfaced while verifying this against the full regression
   suite in `narrowPKFinalize()` (`build.c`) — all pre-existing in step 2, not introduced by step 4,
   but only found now: (a) its fallback-index path inherited `sqlite3CreateIndex()`'s "abort if
   `pParse->nErr` already nonzero" guard, silently skipping the fallback index whenever an unrelated
   earlier error existed in the same `CREATE TABLE` statement, corrupting the table's rowid/PK
   invariant; (b) that same fallback path built a synthetic, non-source-positioned token instead of
   retaining the parser's original `PRIMARY KEY(col)` `ExprList` (now `Table.pNarrowPKList`), so
   `ALTER TABLE ... RENAME COLUMN` silently failed to find/rewrite that occurrence; (c) passing
   `NULL` through to `sqlite3CreateIndex()`'s own `pList==0` fallback (correct only when called
   immediately, synchronously, as mainline does) built the index on the wrong column once our
   deferred call ran after later columns had already been added — plus the `pNarrowPKList` retention
   itself needed `sqlite3DeleteTable()` to free it on early-abort paths, or it leaked. **Known
   limitation, expected until step 5 lands:** `SELECT`ing the rowid-aliased column back currently
   returns garbage (raw rowid bits, untransformed) — write-only for now, by design of the roadmap.
5. **SELECT read-back codegen (done).** The single chokepoint predicted by the original audit,
   `expr.c`'s `sqlite3ExprCodeGetColumnOfTable()`, now emits `OP_Rowid` with a new P4 operand (the
   `Table*`) when the column qualifies; `vdbe.c`'s `OP_Rowid` reverses the step 3 encoding via a new
   `sqlite3VdbeMemSetRowid()` (the mirror of step 4's `sqlite3VdbeMemToRowid()`) instead of handing
   back raw rowid bits. Every other `OP_Rowid` emission site (internal row-identity bookkeeping —
   `update.c`, `where.c`, `window.c`, etc. — not a user-visible column read) omits the new P4 and is
   unchanged. Verified with a 16-byte `BLOB` under `SQLITE_128BIT_ROWID` carrying high entropy in
   both halves, confirming full 128-bit precision survives the read path, not just the low 64 bits.
   **Known correctness gap found (not introduced) by this step, deliberately deferred to step 7:**
   `rowidTruncateToI64()`, used unchanged at every *other* `OP_Rowid` site, extracts only the low 64
   bits of `rowid_t` — but narrow-PK's encoding embeds data in the *high* bits, so that truncation
   produces all zeros (not a graceful degradation) whenever one of those sites touches an actual
   narrow-PK table's real key. This has been true since Phase 1-4 introduced the high-bit embedding;
   it surfaced now because step 5 prompted the question, not because step 5 caused it. It matters
   concretely for `update.c`'s old-rowid tracking (step 7, UPDATE codegen, below) — a general fix
   needs `VdbeCursor` itself to know its table's narrow-PK kind/width (populated at cursor-open
   time), not a per-call-site opt-in like step 5 uses; step 7 is the first place this becomes
   reachable, so it's the natural place to fix it structurally rather than repeat step 5's
   narrower pattern.
6. **WHERE-clause/seek integration (done).** `wherecode.c`'s equality (`WHERE_IPK`) and range
   (`OP_SeekGT`/`GE`/`LT`/`LE` start bound, `OP_Rowid`-based end bound) codegen now emits the same
   `P4_TABLE`-gated opcodes as step 5 whenever the seeked column qualifies; `vdbe.c`'s
   `OP_SeekRowid` and `OP_SeekGT`/`GE`/`LT`/`LE` reverse the step 3 encoding via `rowidFromReal()`/
   `rowidFromNarrowBytes()` when they see that P4 kind, falling back to the untouched classic
   integer path otherwise. Cross-type comparisons (a numeric or wrong-kind literal compared against
   a `BLOB`/`TEXT`/`REAL`-keyed column) fall back on SQLite's existing type-ordering rules
   (`NULL < numeric < TEXT < BLOB`) rather than attempting a conversion: a value that can't convert
   to the column's declared kind is treated as sorting before or after every possible value of that
   column, exactly mirroring how the classic `OP_SeekGT`-family already treats a `REAL` literal that
   doesn't exactly convert to the `INTEGER` PK's domain. Wrong-*width* `BLOB`/`TEXT` literals (e.g.
   `WHERE blobcol > x'01'` against a `BLOB(8)` column) are handled by embedding the literal at its
   own length (bit-identical to zero-padding it to the column's width first) and then bumping the
   boundary operator by one step (`>` becomes `>=`, `<` becomes `<=`, or vice versa) exactly as if
   the embedding had produced an exact tie — because it does, and mainland `BLOB` `memcmp`-with-
   length semantics say the shorter value is the lesser one whenever a real prefix match ties out.
   Verified with equality/range probes (`=`, `<`, `<=`, `>`, `>=`) against `BLOB`/`TEXT`-keyed tables,
   including wrong-width literals and cross-type literals in both directions (numeric vs. `BLOB`-
   keyed, `TEXT` vs. `BLOB`-keyed and back), plus the full regression suite under both a default and
   a `SQLITE_128BIT_ROWID` build. **Not a regression, but a pre-existing 128-bit-rowid-foundation gap
   surfaced during this step's verification (out of scope for step 6, unrelated to narrow-PK):**
   several `test/boundary*.test`, `ext/fts5/test/fts5contentless2.test`/`fts5origintext4.test`,
   `test/amatch1.test`, and `test/altercorrupt.test` fail under `SQLITE_128BIT_ROWID` regardless of
   whether step 6's (or even step 4/5's) changes are present — confirmed by reverting to the step 5
   commit and reproducing the identical failures. These exercise huge plain-`INTEGER` rowid values,
   FTS5's internal rowid bookkeeping, and corruption-recovery paths, none of which involve a
   narrow-PK table; they're a gap in the wide-rowid foundation (step 1) itself, not something this
   feature introduced.
7. **UPDATE codegen (done).** `update.c`'s old/new-rowid bookkeeping now correctly round-trips a
   qualifying `BLOB`/`TEXT`/`REAL` rowid alias through an `UPDATE`, including when the PK column
   itself is the one being changed. Three sites needed the same `P4_TABLE`-gated `OP_Rowid` treatment
   as step 5's SELECT read-back: the initial "read the WHERE-scan's current row's old rowid"
   (`iDataCur`), and — less obviously — the *two* places that read that same value back out of the
   ephemeral FIFO table (`OP_Rowid, iEph, ...`) used for the non-onepass update loop. That second
   case matters even though `iEph` itself is never a narrow-PK table: its stored key is bit-identical
   to `pTab`'s real key (both went through the same `sqlite3VdbeMemToRowid()` encode), so decoding it
   back needs `P4_TABLE=pTab` regardless of which physical cursor is being read — it's the encoding's
   origin table that matters, not the cursor. `OP_MustBeInt` on the new-rowid register is skipped for
   a qualifying table exactly as `insert.c`'s `bClassicIntRowid` guard already does. Verified: single-
   and multi-row (`ONEPASS_OFF`/multi-row self-`UPDATE`) PK-value changes, PK-collision rejection,
   `DELETE`, and `REAL`/`TEXT`/`BLOB(16)` variants, plus the full regression suite under both a
   default and a `SQLITE_128BIT_ROWID` build (only the two known/expected failures).
   **Significant pre-existing gap discovered during this step's verification, NOT step-7-specific
   (present since step 4's INSERT codegen, just never exercised by a test table with a secondary
   index until now) and guarded against rather than fixed:** a table with a qualifying narrow-PK
   *and* any other index (an explicit `CREATE INDEX`, or a `UNIQUE` column/table constraint) silently
   corrupted data before this step. Root cause: every index b-tree entry in SQLite's on-disk format
   ends with a plain-`INTEGER`-typed trailing "rowid" field (`sqlite3VdbeIdxRowid()` explicitly
   rejects any other serial type as corruption) — but `insert.c`'s index-key-construction code
   (`OP_IntCopy` at the `iField==pTab->iPKey` case) blindly treats the source register as already
   holding that integer, when for a narrow-PK table it instead holds the natural decoded `BLOB`/
   `TEXT`/`REAL` value. A `rowid_t` also generally cannot losslessly fit in that field's 64-bit width
   at all under `SQLITE_128BIT_ROWID`. Rather than attempt a fix (which needs either restricting to
   the ≤8-byte case with a real conversion, or a genuine on-disk-format change for the wide case —
   both out of scope here), `build.c` now refuses to let a table become narrow-PK if it already has
   another index (an inline `UNIQUE` constraint parsed earlier in the same `CREATE TABLE`), and
   refuses a later separate `CREATE INDEX`/`UNIQUE` against an already-qualified narrow-PK table —
   both with a clear error message, falling back to (or requiring) an ordinary, fully mainline-
   compatible indexed `PRIMARY KEY` instead of silently corrupting the database. This is a real,
   user-facing limitation of the feature as it stands (no secondary indexes on a narrow-PK table)
   that a future step would need to lift.
8. **Foreign-key verification (done).** `fkey.c`'s child-side existence check (the `pIdx==0` branch,
   taken for a narrow-PK parent exactly as it is for `INTEGER PRIMARY KEY`, since a qualifying
   narrow-PK column gets no index either) now skips `OP_MustBeInt` for a narrow-PK parent, mirroring
   `insert.c`'s `bClassicIntRowid` guard. Verified: child-row insert/delete against a narrow-PK
   parent, `ON DELETE`/`ON UPDATE CASCADE` across a narrow-PK parent (including a genuine 16-byte
   `BLOB` under `SQLITE_128BIT_ROWID`), and multi-row cascades.
   **Major pre-existing gap discovered and fixed by this step (not FK-specific in root cause, but
   found here because FK enforcement is what forced it into the open):** `delete.c`'s two-pass
   `DELETE` strategy (`ONEPASS_OFF` — used whenever the statement has a trigger, a required FK check,
   or a subquery in the `WHERE` clause; FK enforcement on almost any narrow-PK table trips this)
   collects the rowids of matching rows into a `RowSet` before deleting them. `RowSet` only stores
   plain 64-bit integers (`OP_RowSetAdd`/`OP_RowSetRead` assert and read `Mem.u.i` directly) — wrong
   for a narrow-PK table's decoded `BLOB`/`TEXT`/`REAL` rowid value, and never even attempted a
   fallback: it silently inserted garbage into the set, causing the delete loop to match zero rows.
   The practical effect: **any `DELETE` against a narrow-PK table that had a trigger, that touched a
   table with `PRAGMA foreign_keys=ON` and an FK relationship, or whose `WHERE` clause contained a
   subquery, silently deleted nothing at all** (`changes()` reported 0, no error). Fixed by giving
   `delete.c` a `bNarrowPk`-gated alternate path that uses an ephemeral table for the two-pass rowid
   FIFO instead of a `RowSet` — the exact same mechanism `update.c` already uses for its own two-pass
   FIFO (§7), including the same "decode needs `P4_TABLE=pTab` even though the physical cursor being
   read is the ephemeral table, not the real one" reasoning. Verified: `DELETE` with an ordinary
   `AFTER`/`BEFORE` trigger present, `DELETE ... WHERE col IN (SELECT ...)`, multi-row cascades, and
   the full regression suite under both a default and a `SQLITE_128BIT_ROWID` build (only the two
   known/expected failures) — including `delete.test`/`fkey1-4.test`, which exercise the classic
   `RowSet` path heavily and confirmed it is untouched for ordinary `INTEGER PRIMARY KEY` tables.
9. **Remaining edge cases (done, verification only — no code changes needed).** All three items
   turned out to already be correctly handled by step 2's original design, each for a distinct
   reason:
   - `AUTOINCREMENT` combined with a `BLOB`/`TEXT`/`REAL` PK: `sqlite3AddPrimaryKey()`'s
     narrow-PK-candidate branch is an `else if` reached only when the pre-existing (unmodified)
     `else if(autoInc){ sqlite3ErrorMsg(pParse, "AUTOINCREMENT is only allowed on an INTEGER PRIMARY
     KEY"); }` check has already NOT fired — i.e. this is exactly mainline SQLite's decades-old
     `AUTOINCREMENT`-requires-`INTEGER PRIMARY KEY` restriction, completely undisturbed by this
     feature's code being appended after it. Verified for all three kinds (`BLOB`/`TEXT`/`REAL`),
     with and without otherwise-qualifying `CHECK`/width constraints present.
   - `WITHOUT ROWID` tables and composite (multi-column) primary keys: the narrow-PK-candidate branch
     requires `nTerm==1` (single-column PK) checked before the `WITHOUT ROWID` flag is even
     available, and `narrowPKFinalize()` separately bails out via `bWithoutRowid` — both explicit
     opt-outs from step 2's original design (see the design memory's near-miss trigger definition),
     not something that needed step 9 to add. Verified: a `WITHOUT ROWID` table with a
     `BLOB(8)`-shaped single-column PK has no `rowid` pseudo-column (genuinely without rowid, not
     silently rowid-aliased); a composite `PRIMARY KEY(x,y)` with a `BLOB(16)`-shaped `x` allows
     duplicate `x` values with distinct `y` (proving `x` alone isn't being enforced as a unique rowid
     alias). Also verified `PRIMARY KEY DESC` on an otherwise-qualifying column falls back to an
     ordinary indexed PK (the `sortOrder!=SQLITE_SO_DESC` guard), with uniqueness correctly enforced
     via the ordinary index rather than the narrow-PK rowid mechanism.
   - `PRAGMA table_info` introspection: verified it reports the column's exact original declared
     type string (`BLOB(8)`, `TEXT(4)`, `REAL`) for a qualifying narrow-PK column, unchanged from
     what an ordinary column of that declared type would show — nothing about the rowid-aliasing
     internals leaks into the declared-type string.
10. **Secondary indexes on a narrow-PK table.** Step 7 discovered that a narrow-PK table combined
    with any secondary index or `UNIQUE` constraint silently corrupted data, and refused the
    combination outright as a stopgap. Root cause: every index b-tree entry's trailing "rowid"
    field is required, by `sqlite3VdbeIdxRowid()`, to be a plain classic-integer record serial
    type — but `insert.c`'s index-key-construction code (`OP_IntCopy`) blindly assumed the source
    register already held one, which is false for a narrow-PK table's decoded `BLOB`/`TEXT`/`REAL`
    value. Scoped as two stages (full technical writeup, including exact call sites and the
    reasoning for each design choice, is in the project's design memory):
    - **Step 10a (≤8-byte narrow-PK, no on-disk format change, works in both build configs) —
      done.** The raw `rowid_t` bits for a narrow-PK width up to 8 bytes already fit a normal
      64-bit integer serial type, via two new helpers (`rowidToIdxInt64()`/`rowidFromIdxInt64()`
      in `sqliteInt.h`) that project a rowid_t to/from that representation — `OP_IntCopy` now does
      this conversion (`P4_TABLE`-gated) instead of blindly copying, fixing the actual step-7 bug.
      Reading it back correctly needed the `P4_TABLE`-decode mechanism (already proven for
      `OP_Rowid` since step 5) extended to two more opcodes: `OP_IdxRowid` (covering-index reads,
      `upsert.c`'s conflict-resolution seek) and `OP_DeferredSeek` (which shares the same `vdbe.c`
      case block as `OP_IdxRowid` but needed a separate fix, since its `P4` slot was already used
      for an unrelated `P4_INTARRAY` optimization — the two are now mutually exclusive at the
      `wherecode.c` emission site, with a documented, narrow, known-gap corner case where both
      would apply simultaneously). `expr.c`'s `sqlite3ExprCodeLoadIndexColumn()` needed its own
      fix too: it independently used the *decoded natural-type* chokepoint to build an index
      search/comparison key, which no longer matched what `insert.c` actually stores on disk — this
      was the root cause of spurious "row missing from index" errors in `pragma.c`'s
      `integrity_check` and in `DELETE`'s index cleanup. `build.c`'s two `CREATE TABLE`/`CREATE
      INDEX` guards are now conditional on `pTab->pKeyWidth` (≤8 allowed, >8 still refused pending
      step 10b) instead of an unconditional refusal. Verified: covering-index reads, `DELETE`
      (including the two-pass path), `UPDATE`, `UPSERT` via a secondary unique index, `ANALYZE`,
      inline `UNIQUE` column constraints, and the full regression suite under both a default and a
      `SQLITE_128BIT_ROWID` build (targeted files clean under both; a complete `veryquick.test` run
      under `SQLITE_128BIT_ROWID` — the first one in this project to ever run to completion instead
      of being cut off partway — surfaced a large cluster of genuinely pre-existing wide-rowid gaps
      unrelated to narrow-PK, see the note below).
      **Newly-discovered pre-existing gap (not step-10a-caused, confirmed via `git stash` A/B
      testing against the step-9 commit):** `PRAGMA integrity_check` itself reports a false "Rowid 0
      out of order" for *any* narrow-PK table under `SQLITE_128BIT_ROWID` — with or without a
      secondary index — because `btree.c`'s `checkTreePage()` uses the assert-on-overflow
      `rowidToI64()` (not the safe `rowidTruncateToI64()`) on table b-tree cell keys, which can't
      represent a narrow-PK rowid_t's high-aligned embedding. A much larger set of pre-existing,
      completely unrelated `SQLITE_128BIT_ROWID` gaps (spanning `VACUUM`, the `decimal` extension,
      generated columns, incremental vacuum, page overflow/size handling, sorting, file-format
      detection, disk-full handling, several FTS3/FTS4 corruption-fuzz suites, and more) was also
      newly surfaced by that first-ever complete `veryquick.test` run — all confirmed pre-existing
      via the same A/B method, none related to narrow-PK or any step of this feature; see the
      design memory for the full list and the "still open" follow-up note below.
    - **Step 10b (9–16-byte narrow-PK, `SQLITE_128BIT_ROWID`-only, genuine on-disk format change,
      not started).**
      A 9–16-byte `rowid_t` cannot fit any existing integer serial type, so this stage repurposes
      record serial type 11 — confirmed reserved and completely unused anywhere in this codebase or
      by mainline SQLite (unlike type 10, which already has a real meaning: the
      `sqlite3_vtab_nochange()` marker) — as a new, self-describing, fixed 16-byte field holding the
      raw `rowid_t` verbatim (no order-preserving transform needed, since this field is provably
      never used as a sort key — `sqlite3VdbeIdxKeyCompare()` already ignores it). No new page-1
      format-version gating is needed: a >8-byte narrow-PK table can only already exist in a file
      with the wide-rowid magic header set (per step 2's own width-cap validation), so type 11's
      usage automatically inherits that existing gate.

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
- **Broader `SQLITE_128BIT_ROWID` foundation gaps, unrelated to narrow-PK.** Step 10a's first-ever
  complete `veryquick.test` run under `SQLITE_128BIT_ROWID` (every prior 128-bit run in this
  project's history was cut off partway through) surfaced a large cluster of pre-existing failures
  confirmed (via `git stash` A/B testing) to be completely unrelated to narrow-PK or any step of
  this feature — spanning `VACUUM`, the `decimal` extension, generated columns, incremental vacuum,
  page overflow/size handling, sorting, file-format detection, disk-full handling, several FTS3/
  FTS4 corruption-fuzz suites, and `PRAGMA integrity_check` itself (the `rowidToI64()`-vs-
  `rowidTruncateToI64()` issue noted under step 10a). None investigated beyond confirming they
  predate this feature entirely — likely gaps in the wide-rowid foundation (step 1) itself. Worth a
  dedicated audit pass before considering `SQLITE_128BIT_ROWID` production-ready for anything.
- Also still open: the `rowidTruncateToI64()` gap from step 5 for the ~19 non-`P4_TABLE`-gated
  `OP_Rowid` call sites not touched by steps 6–10a (mostly `where.c`/`window.c`'s ephemeral-b-tree
  uses, believed unaffected in practice but not independently verified), and a recommended
  defensive audit of `select.c`/`vacuum.c`/`pragma.c` (beyond what step 10a already touched) for
  the same "assumes plain-64-bit-integer rowid" bug class found in `delete.c` (step 8) and
  `insert.c`/`expr.c` (step 10a).

## 5. Attribution

This fork's direction, goals, and design decisions — including every constraint, tradeoff, and
safety mechanism described in §2 — are [Ambeco](https://github.com/Ambeco)'s. Claude (Anthropic)
fleshed out the technical details, wrote all of the code, and ran all verification, under
Ambeco's direct guidance and review at each step.
