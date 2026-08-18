/*
** 2026 August 17
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This header defines sqlite3_uint128, the 128-bit unsigned integer type
** used to implement rowid_t when SQLITE_128BIT_ROWID is defined (see the
** rowid_t typedef in sqliteInt.h), plus the small set of arithmetic
** primitives rowid_t handling needs: construct from/decompose to a u64,
** add, increment, shift, and compare. Everything here is two's-complement
** at the bit level, so the same representation and the same unsigned
** primitives serve both sqlite's signed rowid values (see rowidFromI64()/
** rowidToI64() in sqliteInt.h, which sign-extend/truncate against this
** type) and genuinely unsigned 128-bit values such as a parsed UUID
** (see ext/misc/uuid.c's uuid_int()).
**
** Two implementations are provided:
**
**   SQLITE_USE_UINT128 defined   - sqlite3_uint128 is the compiler's
**                                   native __uint128_t and these routines
**                                   are thin wrappers around ordinary
**                                   operators. This is the fast path,
**                                   available on the same GCC/Clang
**                                   64-bit targets that already enable
**                                   SQLITE_USE_UINT128 for
**                                   sqlite3Multiply128() in util.c.
**
**   SQLITE_USE_UINT128 undefined - sqlite3_uint128 is a portable struct
**                                   of two u64 "quads" (lo, hi), and each
**                                   routine below is implemented in terms
**                                   of ordinary 64-bit arithmetic with
**                                   manual carry propagation. This path
**                                   is what MSVC and 32-bit targets use.
**
** Callers should not reach into sqlite3_uint128's representation (even
** though, for the struct form, the lo/hi fields are named and reachable)
** except through these routines or the rowid128FromU64()/rowid128ToU64()
** pair -- that keeps callers portable across both representations.
*/
#ifndef SQLITE_INT128_H
#define SQLITE_INT128_H

#ifdef SQLITE_USE_UINT128

typedef __uint128_t sqlite3_uint128;

#define rowid128FromU64(x)        ((sqlite3_uint128)(x))
#define rowid128ToU64(x)          ((u64)(x))
#define rowid128IsZero(x)         ((x)==0)
#define rowid128Add(a,b)          ((a)+(b))
#define rowid128Sub(a,b)          ((a)-(b))
#define rowid128Increment(x)      ((x)+1)
#define rowid128Decrement(x)      ((x)-1)
#define rowid128ShiftLeft(x,n)    ((x)<<(n))
#define rowid128ShiftRight(x,n)   ((x)>>(n))    /* logical (unsigned) */
#define rowid128Negate(x)         (~(x)+1)
#define rowid128IsNegative(x)     (((sqlite3_uint128)(x))>>127)

static SQLITE_INLINE int rowid128Compare(sqlite3_uint128 a, sqlite3_uint128 b){
  return a<b ? -1 : a>b ? 1 : 0;
}
static SQLITE_INLINE int rowid128CompareSigned(sqlite3_uint128 a, sqlite3_uint128 b){
  int an = rowid128IsNegative(a), bn = rowid128IsNegative(b);
  if( an!=bn ) return an ? -1 : 1;
  return rowid128Compare(a,b);
}
static SQLITE_INLINE sqlite3_uint128 rowid128Multiply(u64 a, u64 b){
  return (sqlite3_uint128)a * (sqlite3_uint128)b;
}

#else /* !SQLITE_USE_UINT128 */

typedef union sqlite3_uint128 sqlite3_uint128;
union sqlite3_uint128 {
  u64 quads[2];        /* quads[0] = low 64 bits, quads[1] = high 64 bits */
#if !defined(SQLITE_INT128_NO_ANON_STRUCT)
  struct {
    u64 lo;             /* Low-order 64 bits (aliases quads[0]) */
    u64 hi;             /* High-order 64 bits (aliases quads[1]) */
  };
#endif
};

static SQLITE_INLINE sqlite3_uint128 rowid128FromU64(u64 x){
  sqlite3_uint128 r;
  r.quads[0] = x;
  r.quads[1] = 0;
  return r;
}
static SQLITE_INLINE u64 rowid128ToU64(sqlite3_uint128 x){
  return x.quads[0];
}
static SQLITE_INLINE int rowid128IsZero(sqlite3_uint128 x){
  return x.quads[0]==0 && x.quads[1]==0;
}
static SQLITE_INLINE sqlite3_uint128 rowid128Add(sqlite3_uint128 a, sqlite3_uint128 b){
  sqlite3_uint128 r;
  r.quads[0] = a.quads[0] + b.quads[0];
  r.quads[1] = a.quads[1] + b.quads[1] + (r.quads[0]<a.quads[0]);
  return r;
}
static SQLITE_INLINE sqlite3_uint128 rowid128Negate(sqlite3_uint128 x){
  sqlite3_uint128 r;
  r.quads[0] = ~x.quads[0] + 1;
  r.quads[1] = ~x.quads[1] + (r.quads[0]==0);
  return r;
}
static SQLITE_INLINE sqlite3_uint128 rowid128Sub(sqlite3_uint128 a, sqlite3_uint128 b){
  return rowid128Add(a, rowid128Negate(b));
}
static SQLITE_INLINE sqlite3_uint128 rowid128Increment(sqlite3_uint128 x){
  return rowid128Add(x, rowid128FromU64(1));
}
static SQLITE_INLINE sqlite3_uint128 rowid128Decrement(sqlite3_uint128 x){
  return rowid128Sub(x, rowid128FromU64(1));
}
static SQLITE_INLINE sqlite3_uint128 rowid128ShiftLeft(sqlite3_uint128 x, int n){
  sqlite3_uint128 r;
  assert( n>=0 && n<128 );
  if( n==0 ){
    r = x;
  }else if( n<64 ){
    r.quads[1] = (x.quads[1]<<n) | (x.quads[0]>>(64-n));
    r.quads[0] = x.quads[0]<<n;
  }else{
    r.quads[1] = x.quads[0]<<(n-64);
    r.quads[0] = 0;
  }
  return r;
}
/* Logical (unsigned) right shift */
static SQLITE_INLINE sqlite3_uint128 rowid128ShiftRight(sqlite3_uint128 x, int n){
  sqlite3_uint128 r;
  assert( n>=0 && n<128 );
  if( n==0 ){
    r = x;
  }else if( n<64 ){
    r.quads[0] = (x.quads[0]>>n) | (x.quads[1]<<(64-n));
    r.quads[1] = x.quads[1]>>n;
  }else{
    r.quads[0] = x.quads[1]>>(n-64);
    r.quads[1] = 0;
  }
  return r;
}
static SQLITE_INLINE int rowid128IsNegative(sqlite3_uint128 x){
  return (x.quads[1]>>63)&1;
}
static SQLITE_INLINE int rowid128Compare(sqlite3_uint128 a, sqlite3_uint128 b){
  if( a.quads[1]!=b.quads[1] ) return a.quads[1]<b.quads[1] ? -1 : 1;
  if( a.quads[0]!=b.quads[0] ) return a.quads[0]<b.quads[0] ? -1 : 1;
  return 0;
}
static SQLITE_INLINE int rowid128CompareSigned(sqlite3_uint128 a, sqlite3_uint128 b){
  int an = rowid128IsNegative(a), bn = rowid128IsNegative(b);
  if( an!=bn ) return an ? -1 : 1;
  return rowid128Compare(a,b);
}

/* sqlite3Multiply128() (util.c) already computes a full 64x64->128 bit
** unsigned product; this just packs its two halves into a sqlite3_uint128.
*/
u64 sqlite3Multiply128(u64 a, u64 b, u64 *pLo);
static SQLITE_INLINE sqlite3_uint128 rowid128Multiply(u64 a, u64 b){
  sqlite3_uint128 r;
  r.quads[1] = sqlite3Multiply128(a, b, &r.quads[0]);
  return r;
}

#endif /* SQLITE_USE_UINT128 */

#endif /* SQLITE_INT128_H */
