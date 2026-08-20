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
** This header defines sqlite3_uint128, a general-purpose 128-bit integer
** type, plus a small set of arithmetic primitives on it: construct
** from/decompose to a u64, add, increment, shift, and compare. It is not
** specific to rowids -- rowid_t (see sqliteInt.h) is defined in terms of
** it when SQLITE_128BIT_ROWID is enabled, but sqlite3_uint128 is also
** used directly wherever a genuinely unsigned 128-bit value is needed,
** such as a parsed UUID (see ext/misc/uuid.c's uuid_int()).
**
** Everything here is two's-complement at the bit level, so the same
** representation and the same unsigned primitives serve both signed
** values (rowid_t's rowidFromI64()/rowidToI64() helpers in sqliteInt.h
** sign-extend/truncate against this type) and genuinely unsigned ones.
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
** except through these routines or the int128FromU64()/int128ToU64()
** pair -- that keeps callers portable across both representations.
*/
#ifndef SQLITE_INT128_H
#define SQLITE_INT128_H

#ifdef SQLITE_USE_UINT128

typedef __uint128_t sqlite3_uint128;

#define int128FromU64(x)        ((sqlite3_uint128)(x))
#define int128ToU64(x)          ((u64)(x))
#define int128IsZero(x)         ((x)==0)
#define int128Add(a,b)          ((a)+(b))
#define int128Sub(a,b)          ((a)-(b))
#define int128Increment(x)      ((x)+1)
#define int128Decrement(x)      ((x)-1)
#define int128ShiftLeft(x,n)    ((x)<<(n))
#define int128ShiftRight(x,n)   ((x)>>(n))    /* logical (unsigned) */
#define int128Negate(x)         (~(x)+1)
#define int128IsNegative(x)     (((sqlite3_uint128)(x))>>127)

static SQLITE_INLINE int int128Compare(sqlite3_uint128 a, sqlite3_uint128 b){
  return a<b ? -1 : a>b ? 1 : 0;
}
static SQLITE_INLINE int int128CompareSigned(sqlite3_uint128 a, sqlite3_uint128 b){
  int an = int128IsNegative(a), bn = int128IsNegative(b);
  if( an!=bn ) return an ? -1 : 1;
  return int128Compare(a,b);
}
static SQLITE_INLINE sqlite3_uint128 int128Multiply(u64 a, u64 b){
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

static SQLITE_INLINE sqlite3_uint128 int128FromU64(u64 x){
  sqlite3_uint128 r;
  r.quads[0] = x;
  r.quads[1] = 0;
  return r;
}
static SQLITE_INLINE u64 int128ToU64(sqlite3_uint128 x){
  return x.quads[0];
}
static SQLITE_INLINE int int128IsZero(sqlite3_uint128 x){
  return x.quads[0]==0 && x.quads[1]==0;
}
static SQLITE_INLINE sqlite3_uint128 int128Add(sqlite3_uint128 a, sqlite3_uint128 b){
  sqlite3_uint128 r;
  r.quads[0] = a.quads[0] + b.quads[0];
  r.quads[1] = a.quads[1] + b.quads[1] + (r.quads[0]<a.quads[0]);
  return r;
}
static SQLITE_INLINE sqlite3_uint128 int128Negate(sqlite3_uint128 x){
  sqlite3_uint128 r;
  r.quads[0] = ~x.quads[0] + 1;
  r.quads[1] = ~x.quads[1] + (r.quads[0]==0);
  return r;
}
static SQLITE_INLINE sqlite3_uint128 int128Sub(sqlite3_uint128 a, sqlite3_uint128 b){
  return int128Add(a, int128Negate(b));
}
static SQLITE_INLINE sqlite3_uint128 int128Increment(sqlite3_uint128 x){
  return int128Add(x, int128FromU64(1));
}
static SQLITE_INLINE sqlite3_uint128 int128Decrement(sqlite3_uint128 x){
  return int128Sub(x, int128FromU64(1));
}
static SQLITE_INLINE sqlite3_uint128 int128ShiftLeft(sqlite3_uint128 x, int n){
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
static SQLITE_INLINE sqlite3_uint128 int128ShiftRight(sqlite3_uint128 x, int n){
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
static SQLITE_INLINE int int128IsNegative(sqlite3_uint128 x){
  return (x.quads[1]>>63)&1;
}
static SQLITE_INLINE int int128Compare(sqlite3_uint128 a, sqlite3_uint128 b){
  if( a.quads[1]!=b.quads[1] ) return a.quads[1]<b.quads[1] ? -1 : 1;
  if( a.quads[0]!=b.quads[0] ) return a.quads[0]<b.quads[0] ? -1 : 1;
  return 0;
}
static SQLITE_INLINE int int128CompareSigned(sqlite3_uint128 a, sqlite3_uint128 b){
  int an = int128IsNegative(a), bn = int128IsNegative(b);
  if( an!=bn ) return an ? -1 : 1;
  return int128Compare(a,b);
}

/* sqlite3Multiply128() (util.c) already computes a full 64x64->128 bit
** unsigned product; this just packs its two halves into a sqlite3_uint128.
*/
u64 sqlite3Multiply128(u64 a, u64 b, u64 *pLo);
static SQLITE_INLINE sqlite3_uint128 int128Multiply(u64 a, u64 b){
  sqlite3_uint128 r;
  r.quads[1] = sqlite3Multiply128(a, b, &r.quads[0]);
  return r;
}

#endif /* SQLITE_USE_UINT128 */

#endif /* SQLITE_INT128_H */
