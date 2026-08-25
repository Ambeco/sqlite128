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
   narrower pattern. (Every remaining site was subsequently audited — see §5a.)
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
    - **Step 10b (9–16-byte narrow-PK, `SQLITE_128BIT_ROWID`-only, genuine on-disk format change) —
      done.**
      A 9–16-byte `rowid_t` cannot fit any existing integer serial type, so this stage repurposes
      record serial type 11 — confirmed reserved and completely unused anywhere in this codebase or
      by mainline SQLite (unlike type 10, which already has a real meaning: the
      `sqlite3_vtab_nochange()` marker) — as a new, self-describing, fixed 16-byte field holding the
      raw `rowid_t` verbatim (no order-preserving transform: `rowidToRaw16Bytes()`/
      `rowidFromRaw16Bytes()` in `sqliteInt.h`). No new page-1 format-version gating is needed: a
      >8-byte narrow-PK table can only already exist in a file with the wide-rowid magic header set
      (per step 2's own width-cap validation), so type 11's usage automatically inherits that
      existing gate. `build.c`'s two step-7/10a guards are now conditional on
      `pTab->pKeyWidth>NARROWPK_MAX_WIDTH` (16 under `SQLITE_128BIT_ROWID`, still 8 in a default
      build, where a >8-byte narrow-PK table can never qualify at all) instead of an unconditional
      `>8` refusal.

      **Read side.** `sqlite3SmallTypeSizes[11]` is now 16 (was 0), so every generic
      length-based record-header-walking caller (`sqlite3VdbeRecordUnpack()`, comparison paths,
      etc.) automatically stays in sync. `sqlite3VdbeSerialGet()`'s case 11 decodes the field as an
      ordinary 16-byte `MEM_Blob` (zero-copy, like the generic blob case) rather than reconstructing
      a `rowid_t` — this generic decode point has no `Table` to know which table the value belongs
      to, and (see below) this representation is exactly what generic Mem-vs-Mem comparison needs.
      `sqlite3VdbeIdxRowid()` gained a dedicated, `SQLITE_128BIT_ROWID`-gated type-11 read branch
      (bypassing the generic decode, reading the 16 bytes directly into a real `rowid_t` via
      `rowidFromRaw16Bytes()`) alongside its existing classic-integer-type accept list; a default
      build's accept-check is untouched, still rejecting type 11 as corruption (structurally
      unreachable there regardless). `vdbe.c`'s shared `OP_IdxRowid`/`OP_DeferredSeek` handler is now
      width-aware: for `pKeyWidth<=8` it keeps step 10a's `rowidFromIdxInt64(rowidTruncateToI64())`
      re-derivation; for `pKeyWidth>8`, `sqlite3VdbeIdxRowid()` already returned the correct, full
      `rowid_t` verbatim, used as-is.

      **Write side — the actual new opcode.** Ordinary `OP_MakeRecord` has no way to classify a
      single field as this fork's own type 11 (`Mem` has no 128-bit representation, deliberately —
      see the design memory). Both write-side record-construction sites (`insert.c`'s
      UNIQUE-constraint/index-key loop, and `delete.c`'s shared `sqlite3GenerateIndexKey()`, used by
      `DELETE`'s index cleanup, `pragma.c`'s `integrity_check`, and `build.c`'s `CREATE INDEX`
      bulk-populate / `where.c`'s automatic-index materialization) now, for a >8-byte-wide alias,
      build the record over the real indexed columns only, then call a new opcode,
      `OP_IdxAppendRowid` (`vdbe.c`), to splice a type-11 field onto the end: it recomputes the
      record's header-length varint for the +1-byte growth (correctly handling the rare case where
      the varint's own byte-length also grows, e.g. a header at exactly the 127/128-byte boundary),
      copies the existing serial-type list and data forward, and appends the 16 raw bytes. Two small
      existing opcodes gained a `P5` flag to feed it: `OP_Rowid` (cursor → raw-16-byte-blob, used by
      `sqlite3ExprCodeLoadIndexColumn()`'s search-key construction) and `OP_IntCopy` (natural-typed
      Mem → raw-16-byte-blob via the same `sqlite3VdbeMemToRowid()` dispatch its `P5=0` classic-int
      projection already uses, for `insert.c`'s storage path). `OP_IdxAppendRowid` itself has no
      `rowid_t`-width-specific logic (pure byte/varint surgery) so, unlike most of this fork's other
      additions, it isn't `SQLITE_128BIT_ROWID`-gated — it's simply never emitted in a default build.

      **Two bugs found and fixed while implementing this** (both go deeper than step 10b itself —
      see the design memory for the full debugging narrative):
      - **A step-10a regression, not a step-10b-only bug:** `insert.c`'s index-key loop and
        `sqlite3ExprCodeLoadIndexColumn()` (`expr.c`) both matched on `iField==XN_ROWID ||
        iField==pTab->iPKey` without distinguishing them — but an index can reference the narrow-PK
        column *explicitly*, as an ordinary sort-key column (e.g.
        `CREATE INDEX i ON t(narrowPkCol, othercol)`), at a *non-last* position, in *addition* to the
        implicit trailing `XN_ROWID` slot every rowid-table index still gets appended automatically.
        Both call sites applied the trailing-slot's special (classic-int-projection, or now
        raw-16-byte-blob) treatment to *that* explicit reference too, silently breaking range and
        comparison queries against such an index — reproduced directly: `id > x'...' AND id < x'...'`
        via an explicit `INDEXED BY` on a 2-column index leading with the narrow-PK column returned
        zero rows instead of the matching one, for both an 8-byte and a 16-byte narrow-PK. Fixed by
        splitting the check: only `iField==XN_ROWID`/`iTabCol==XN_ROWID` (the guaranteed-synthetic
        marker) gets the special on-disk-representation treatment; a real, explicit
        `iField==pTab->iPKey` reference now falls through to the ordinary column path (`OP_SCopy` /
        `sqlite3ExprCodeGetColumnOfTable()`), holding the plain natural-typed value like any other
        indexed column, exactly as it always should have.
      - **`sqlite3VdbeRecordCompareWithSkip()`'s (`vdbeaux.c`) hand-optimized "RHS is a blob" fast
        path** hard-codes `serial_type<12` as "sorts before any real blob" — a leftover from when
        types 10/11 were genuinely unused reserved slots (the comment there still says so). This
        unconditionally returned "less than" for a type-11 field being compared, regardless of
        actual content — reproduced directly: with the `insert.c`/`expr.c` fix alone,
        `PRAGMA integrity_check` reported "row missing from index" for every row of a 9-16-byte
        narrow-PK table with any secondary index, because `OP_Found`'s underlying comparison could
        never see a match. Fixed by adding an explicit `serial_type==11` case that does a genuine
        16-byte `memcmp`, ahead of the old `<12` fallback (still correct for type 10, still
        genuinely unused). This function's numeric-RHS branches needed no equivalent fix: they
        already treat any `serial_type>=10` other than exactly 10 as "sorts after," which happens to
        already be correct for a type-11 field (it's never compared against a numeric RHS in
        practice regardless, since the only legitimate RHS at that position is another
        raw-16-byte-blob, but the existing code's behavior for it is harmless either way).

      **`pragma.c`'s `integrity_check`** needed one more fix: its self-consistency check (comparing
      `OP_IdxRowid`'s raw read of an index entry's trailing field against
      `sqlite3ExprCodeLoadIndexColumn()`'s freshly-built expectation for the current table row) used
      a bare, untagged `OP_IdxRowid` — which, by a coincidence of `rowidFromI64()`/
      `rowidTruncateToI64()` being exact inverses, already happened to match for `pKeyWidth<=8`, but
      not for `pKeyWidth>8` (where the untagged path's `rowidTruncateToI64()` extracts the wrong
      low-order half, matching nothing). `OP_IdxRowid` gained its own `P5` flag (parallel to
      `OP_Rowid`'s) to produce the matching raw-16-byte-blob when tagged; `pragma.c` now tags it only
      for a qualifying `pKeyWidth>8` table, leaving the already-correct `pKeyWidth<=8`/classic-PK
      behavior untouched. `upsert.c`'s `OP_IdxRowid`→`OP_SeekRowid` chain and `wherecode.c`'s
      `codeDeferredSeek()` needed only their leftover `assert(pKeyWidth<=8)` removed — both were
      already width-generic, `OP_IdxRowid`'s width-aware fix and `OP_SeekRowid`'s
      `sqlite3VdbeMemToRowid()` dispatch cover any width automatically.

      **Verified** with dedicated repros for all of the above under `SQLITE_128BIT_ROWID` (covering-
      index reads, `DELETE`, `UPDATE`, `UPSERT` via a secondary unique index, `ANALYZE`,
      `integrity_check`, and composite indexes with the narrow-PK column in both a leading and a
      non-leading explicit position, alongside the automatic trailing slot), plus the full
      established regression suite (27 files) and a broader general-comparison sanity set (17 more
      files: `select*`, `where*`, `in*`, `collate*`, `insert*`, `blob.test` — given the change to a
      very hot, shared comparison function) — all clean under both a default build and
      `SQLITE_128BIT_ROWID`, with identical per-file counts in both configs.
11. **Fix the two confirmed §5a bugs found by the dedicated `SQLITE_128BIT_ROWID` audit — done.**
    `VACUUM`/`INSERT ... SELECT` (via `insert.c`'s `xferOptimization()`, including `vdbe.c`'s
    `OP_RowCell` opcode it relies on) and `ALTER TABLE ... DROP COLUMN` (`alter.c`'s row-rewrite)
    both silently corrupted narrow-PK primary keys; `PRAGMA foreign_key_check` (`pragma.c`) also
    misreported the violating row's identifier for a narrow-PK child table. All three fixed with the
    same `P4_TABLE`-tagging pattern used throughout this project; full technical writeup in §5a.
    Deliberately scoped separately from the numbered narrow-PK design steps above (these are bug
    fixes to already-built functionality, not new functionality), but numbered here to keep the
    step-by-step history complete and in order.

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
- **Known bugs.** See §5 — a dedicated audit of `SQLITE_128BIT_ROWID` (both the narrow-PK feature's
  own remaining gaps and pre-existing gaps in the underlying wide-rowid foundation it depends on)
  was carried out and is tracked there rather than in this list.

## 5. Known bugs

Confirmed, reproducible bugs under `SQLITE_128BIT_ROWID`, found via a dedicated audit pass (not
part of the numbered step-by-step plan in §3, and not speculative follow-up work like §4 — these are
concrete, verified defects). Kept separate and explicit so they aren't lost track of. None of these
affect a default (non-`SQLITE_128BIT_ROWID`) build. §5a (this fork's own narrow-PK bugs) is now fully
fixed; §5b (pre-existing foundation bugs, unrelated to narrow-PK) remains open.

### 5a. Narrow-PK feature bugs (this fork's own code) — all fixed (step 11)

Step 5 flagged that `rowidTruncateToI64()` — used, unchanged, at every `OP_Rowid` call site that
doesn't carry the `P4_TABLE` tag — takes the LOW 64 bits of a `rowid_t`, which is the wrong half for
a narrow-PK table's HIGH-aligned embedding (see `rowidFromNarrowBytes()`'s doc comment). Steps 6–10a
fixed every such site actually reachable by ordinary `SELECT`/`INSERT`/`UPDATE`/`DELETE`/`UPSERT`
statement execution. A dedicated audit (2026-08-24) went through every remaining `OP_Rowid` emission
site in the codebase (grep for `OP_Rowid,` across `src/*.c`, cross-checked against what's already
`P4_TABLE`-tagged) and empirically tested each one still in doubt, finding three confirmed bugs, all
in table-maintenance code paths outside ordinary DML. All three were fixed immediately afterward
(step 11, 2026-08-24):

- **`VACUUM` silently corrupted every row of a narrow-PK table.** `vacuum.c` rebuilds each table via
  `insert.c`'s `xferOptimization()` fast-copy path, which read the source table's rowid with a bare
  `OP_Rowid` (`insert.c`, `pDest->iPKey>=0` branch) and used it directly as the new row's key. For a
  narrow-PK table under `SQLITE_128BIT_ROWID`, the low 64 bits this read were *always zero*
  (the real bytes live entirely in the high half), so every row collapsed onto the same rowid —
  reproduced directly: a 3-row `BLOB(8)` PK table, after `VACUUM`, had all three rows reporting the
  identical (wrong) key. The same `xferOptimization()` path is also used by plain
  `INSERT INTO dst SELECT * FROM src` between two identically-shaped tables — reproduced there too,
  as a `UNIQUE constraint failed` (the collapsed keys collide) rather than silent data loss, same
  underlying defect. **Fix:** tag that `OP_Rowid` with `P4_TABLE` (keyed off `pSrc`, the cursor it
  actually reads — `xferOptimization()`'s own eligibility checks already guarantee `pSrc` and `pDest`
  agree on PK column, affinity, and width) so it decodes into `pSrc`'s natural column type instead of
  handing back raw `rowid_t` bits. That alone wasn't sufficient for `VACUUM`'s specific fast path,
  though: `VACUUM` (and any xfer where `SQLITE_ENABLE_PREUPDATE_HOOK` is off) routes the row through
  `OP_RowCell`, a second, independent opcode that read the rowid register as a raw `aMem[].u.i`
  integer rather than going through the same `sqlite3VdbeMemToRowid()` dispatch `OP_Insert` and
  `OP_NotExists` already use — silently discarding the natural-typed value the fixed `OP_Rowid` had
  just produced. Fixed by switching `OP_RowCell` (`vdbe.c`) to `sqlite3VdbeMemToRowid()` as well; for
  the classic `INTEGER PRIMARY KEY` case (the overwhelming majority of `OP_RowCell` uses) that
  dispatches straight through to the previous behavior, unchanged.
- **`ALTER TABLE ... DROP COLUMN` corrupted narrow-PK values while rewriting the table.** The
  drop-column row-rewrite in `alter.c` read each row's rowid with a bare `OP_Rowid` and re-inserted
  the row (minus the dropped column) under that same, truncated key. Reproduced directly: dropping
  an unrelated column from a `BLOB(8)`-PK table left one row with a corrupted key and effectively
  duplicated the row that key collided into, rather than a clean rewrite. **Fix:** the same
  `P4_TABLE` tag on that `OP_Rowid` emission, letting the downstream `OP_Insert` (which already
  dispatches via `sqlite3VdbeMemToRowid()` unconditionally) re-encode it correctly.

One further, lower-severity issue found the same way, also fixed:

- **`PRAGMA foreign_key_check` misreported the offending row for a narrow-PK *child* table.** The
  reported "rowid" of the violating row (`pragma.c`, the `OP_Rowid` feeding the check's result-row
  columns) was read raw, so it showed the truncated/wrong value instead of identifying the actual
  row — a diagnostic-output-only bug (nothing was corrupted on disk), reproduced directly: the check
  reported `0` as the child row identifier when the true key was `x'0100000000000000'`. **Fix:** same
  `P4_TABLE` tag, decoding the value into its natural type before it's reported as a query result.

Everything else checked was confirmed safe, either by direct testing or by the raw value never
crossing into another table's real b-tree key (only ever compared against another equally-raw value
from the *same* internal bookkeeping structure, so a consistently-wrong value is harmless):
`where.c`'s bloom-filter construction (`OP_FilterAdd` — performance-only even in the worst case, never
a correctness issue); `wherecode.c`'s `IN`-operator rowid-list optimization (tested directly: a
`WHERE k IN (SELECT k FROM t WHERE ...)` query against a narrow-PK column returned correct results);
`window.c`'s five sites (all operate on window functions' own ephemeral frame-buffer cursor, never
the real table cursor; tested directly with a windowed aggregate over a narrow-PK-ordered frame);
`update.c`'s two sites (virtual-table `UPDATE` codegen only — narrow-PK is a real-table-only feature
by design, unreachable for a virtual table).

All three fixes were verified with dedicated 8-byte and 16-byte `BLOB` PK repros under both build
configs, git-stash A/B'd against the pre-fix baseline to confirm each was a genuine, reproducible
defect (the baseline run visibly corrupted a 16-byte key into a garbled 17-byte value on `VACUUM`),
and the established regression file set (27 files, incl. all `vacuum*.test`, `alter*dropcol*.test`,
`alter.test`, `altertab.test`) was run clean (0 errors) under both build configs afterward.

### 5b. Pre-existing `SQLITE_128BIT_ROWID` foundation bugs (unrelated to narrow-PK)

Found incidentally while testing the narrow-PK feature (steps 6, 7, 8, 10a), each confirmed via
`git stash` A/B testing to reproduce identically with none of this feature's code present — i.e.
these are gaps in the wide-rowid foundation (step 1) itself, or in wholly unrelated subsystems,
predating and unrelated to narrow-PK-as-rowid. None investigated beyond confirming they predate this
feature; each would need its own root-cause pass.

- `PRAGMA integrity_check` reports a false "Rowid N out of order" for *any* narrow-PK table (not just
  ones with a secondary index) — `btree.c`'s `checkTreePage()` uses the assert-on-overflow
  `rowidToI64()` instead of the safe `rowidTruncateToI64()` on table b-tree cell keys, which can't
  represent a narrow-PK rowid_t's high-aligned embedding. (This one specifically involves narrow-PK
  data, but the defect is in generic, pre-existing integrity-check code, not in this fork's own
  narrow-PK code paths — hence listed here rather than in §5a.)
- `test/boundary1.test`, `test/boundary3.test`, `test/boundary4.test` — huge plain-`INTEGER` rowid
  values.
- `ext/fts5/test/fts5contentless2.test`, `fts5origintext4.test` — FTS5's internal rowid bookkeeping.
- `test/amatch1.test`, `test/altercorrupt.test` — corruption-recovery paths.
- Several `ext/recover/*` tests, `ext/rtree/rtreefuzz001.test` — recovery/R-tree fuzz corpora.
- `test/vacuum6.test`, `test/decimal.test`, `test/resetdb.test`, `test/gencol1.test`, one sub-case of
  `test/pragma.test`, `test/dbfuzz001.test`, `test/filefmt.test`, `test/sort5.test`,
  `test/incrvacuum.test`, `test/pagesize.test`, `test/ovfl.test`, plus clusters of `corruptL`/
  `corruptN`/`dbpage`/`diskfull`/`fts3corrupt4`/`fts4aa`/`fts3fuzz001`/`fts3query` cases — surfaced by
  step 10a's `veryquick.test` run, the first one in this project's history to ever run to completion
  under `SQLITE_128BIT_ROWID` (every earlier attempt was cut off partway through before reaching
  these test files).

Worth a dedicated audit pass of its own before considering `SQLITE_128BIT_ROWID` production-ready for
anything beyond this fork's own narrow-PK feature.

## 6. Attribution

This fork's direction, goals, and design decisions — including every constraint, tradeoff, and
safety mechanism described in §2 — are [Ambeco](https://github.com/Ambeco)'s. Claude (Anthropic)
fleshed out the technical details, wrote all of the code, and ran all verification, under
Ambeco's direct guidance and review at each step.
