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
| `BLOB(W)`, W≤16 | `NOT NULL`, `CHECK(length(col)=W)` |
| `TEXT(W)`, W≤16 | `NOT NULL`, `CHECK(length(CAST(col AS BLOB))=W)`, and effective collation must resolve to `BINARY` (the default, unless overridden) |
| `REAL`/`FLOAT`/`DOUBLE`* | `NOT NULL`, `CHECK(typeof(col)='real')` |

This mirrors, deliberately, how `INTEGER PRIMARY KEY` itself already works in mainline SQLite: it's
spelling-sensitive (`INT PRIMARY KEY` does *not* get rowid-aliasing, only literally `INTEGER`
does), and it disallows `NULL` because a rowid can never be null. The `CHECK` constraints are the
explicit, schema-visible way a developer opts into the tradeoffs — this fork deliberately does
**not** auto-coerce or auto-enforce these types under the hood; the constraints are how SQLite's
existing engine (which already always enforces `CHECK`) does the enforcement, not a new bespoke
mechanism.

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
  not an implicit type-conversion feature. Existing conversion functions (`uuid_str()`,
  `uuid_blob()`, `uuid_int()` — see `ext/misc/uuid.c`) remain the explicit way to move between
  representations.
- Width is capped at 16 bytes (the size of a 128-bit rowid).

## 3. Step-by-step plan and progress

This fork has been built as a sequence of small, individually-verified phases.

### Phase 1–4: wide-rowid foundation (**done**)
`rowid_t` typedef and core rowid abstraction; disk-I/O rowid handling and file-format width
detection; `sqlite3_uint128` arithmetic primitives (`sqliteInt128.h`); `SQLITE_128BIT_ROWID`
compiles; page-1 magic-header gate for wide-rowid files; whole-database wide-rowid codec with
convert-only-via-SQL migration.

### Phase 5: UUID extension groundwork (**done**)
`uuid_int(x)` added to `ext/misc/uuid.c`.

### Phase 6: widen `Mem`/SQL-value handling to 128 bits (**done**, 6a–6i)
Construction/access (6a) → record/column on-disk storage, serial type 11 (6b) → comparison (6c) →
128-bit multiply/divide/modulo primitives (6e) → arithmetic opcodes (6d) → affinity/numerify/cast
awareness (6f) → decimal-render/stringify support (6g) → decimal & hex SQL literal parsing plus a
new `OP_Int128` opcode (6h) → `sqlite3_value_type()` fix and a full `sqlite3_bind_int128`/
`sqlite3_column_int128`/`sqlite3_value_int128` C API (6i). A 128-bit `INTEGER` value is now fully
usable everywhere in the engine: arithmetic, comparison, casting, storage, and C API access.

### Phase 7: making int128 practically usable (**in progress**)
- **7a (done):** `sqlite3_result_int128()` C API, wired into the loadable-extension API surface
  (`sqlite3ext.h`/`loadext.c`) so extensions — not just directly-linked C code — can produce
  128-bit results. `uuid_int()` now returns a genuine `INTEGER`-typed 128-bit value in a
  `SQLITE_128BIT_ROWID` build, instead of a 16-byte `BLOB`.
- **Narrow-fixed-width-PK-as-rowid (planned, design complete, implementation not started):** the
  §1/§2 feature above. Full design is settled; remaining pre-implementation work is a concrete
  near-miss test-case table, exact error-message wording, and a from-scratch audit of every place
  in the codebase that currently assumes a rowid-aliased column is an `INTEGER` (there are many:
  `insert.c`, `update.c`, the query planner's seek paths, `fkey.c`, and more). Planned phases (A–H):
  schema-level detection/validation → value↔rowid conversion primitives → INSERT codegen → SELECT
  read-back codegen → WHERE-clause/seek integration → UPDATE codegen → foreign-key verification →
  remaining edge cases (`AUTOINCREMENT` rejection, `WITHOUT ROWID` exclusion, introspection).

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
