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
| `BLOB(W)`, W≤max | `NOT NULL`, `CHECK(typeof(col)='blob')`, `CHECK(length(col)=W)` |
| `TEXT(W)`, W≤max | `NOT NULL`, `CHECK(typeof(col)='text')`, `CHECK(length(CAST(col AS BLOB))=W)`, and effective collation must resolve to `BINARY` (the default, unless overridden) |
| `REAL` | `NOT NULL`, `CHECK(typeof(col)='real')` |

Where max is 8 bytes in a default build and 16 bytes under `SQLITE_128BIT_ROWID` — a default
build's rowid is still a plain 64-bit integer (only `SQLITE_128BIT_ROWID` widens it to 128 bits),
so only widths that actually fit are ever accepted. `REAL` is always exactly 8 bytes and qualifies
under either build.

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
3. **Value↔rowid conversion primitives (not started).** The `BLOB`/`TEXT`/`REAL`-to-`rowid_t`
   encoding and its inverse, including the bit-canonicalization transforms from §2, as standalone,
   unit-testable functions ahead of any VDBE integration.
4. **INSERT codegen (not started).** Wire step 3's conversion into `insert.c` so a qualifying table
   actually gets compact table-b-tree storage on write.
5. **SELECT read-back codegen (not started).** Reverse the step 3 transform wherever a rowid-aliased
   column is read back — likely the most spread-out step, since `INTEGER PRIMARY KEY` reads are
   currently free (the rowid *is* the value) via several short-circuit sites that will need the new
   kinds to additionally repackage the correct `BLOB`/`TEXT`/`REAL` `MEM` type.
6. **WHERE-clause/seek integration (not started).** Apply the step 3 transform to comparison values
   in the query planner's fast rowid-seek paths (`where.c`/`wherecode.c`).
7. **UPDATE codegen (not started).** Analogous to step 4, for `UPDATE` statements touching the PK
   column.
8. **Foreign-key verification (not started).** Confirm `fkey.c` correctly handles a parent table
   using one of the new rowid kinds — this is the feature's original motivating use case (a UUID
   primary key referenced by a foreign key).
9. **Remaining edge cases (not started).** Reject `AUTOINCREMENT` combined with a non-`INTEGER` PK
   kind; confirm `WITHOUT ROWID` tables and composite primary keys stay excluded; verify
   `PRAGMA table_info` and similar introspection keep reporting the column's original declared
   type.

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

## 5. Attribution

This fork's direction, goals, and design decisions — including every constraint, tradeoff, and
safety mechanism described in §2 — are [Ambeco](https://github.com/Ambeco)'s. Claude (Anthropic)
fleshed out the technical details, wrote all of the code, and ran all verification, under
Ambeco's direct guidance and review at each step.
