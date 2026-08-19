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

/* Sign-extend a 64-bit signed integer into a two's-complement
** sqlite3_uint128. Converting a negative i64 to a wider unsigned type
** is well-defined by C as reduction modulo 2^128, which for a two's-
** complement representation is exactly sign extension. */
#define int128FromI64(x)        ((sqlite3_uint128)(i64)(x))

/* Unsigned-magnitude conversions to/from double, used to build the
** signed versions below. r must be in [0, 2^128) for the FromDouble
** direction. */
#define int128ToDoubleUnsigned(x)      ((double)(x))
static SQLITE_INLINE sqlite3_uint128 int128FromDoubleUnsigned(double r){
  return (sqlite3_uint128)r;
}

/* Full 128x128 unsigned multiply, truncated (wrapped mod 2^128) to the
** low 128 bits of the product. __uint128_t multiplication already
** wraps this way, so this is just an alias. */
#define int128MultiplyLow(a,b)   ((a)*(b))

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

/* Full 128x128 unsigned multiply, truncated (wrapped mod 2^128) to the
** low 128 bits of the product. Schoolbook multiply of two 64-bit-limb
** values: only the low 64 bits of the cross terms (a0*b1 + a1*b0) can
** affect the low 128 bits of the result, since anything they carry
** into bit 128 or beyond is discarded by the truncation anyway. */
static SQLITE_INLINE sqlite3_uint128 int128MultiplyLow(
  sqlite3_uint128 a,
  sqlite3_uint128 b
){
  sqlite3_uint128 r = int128Multiply(a.quads[0], b.quads[0]);
  r.quads[1] += a.quads[0]*b.quads[1] + a.quads[1]*b.quads[0];
  return r;
}

/* Sign-extend a 64-bit signed integer into a two's-complement
** sqlite3_uint128. */
static SQLITE_INLINE sqlite3_uint128 int128FromI64(i64 x){
  sqlite3_uint128 r;
  r.quads[0] = (u64)x;
  r.quads[1] = (x<0) ? ((u64)-1) : 0;
  return r;
}

/* Unsigned-magnitude conversions to/from double, used to build the
** signed versions below. r must be in [0, 2^64*2^64) for the
** FromDouble direction; 2^64 (18446744073709551616.0) is exactly
** representable as a double. */
static SQLITE_INLINE double int128ToDoubleUnsigned(sqlite3_uint128 x){
  return ((double)x.quads[1])*18446744073709551616.0 + (double)x.quads[0];
}
static SQLITE_INLINE sqlite3_uint128 int128FromDoubleUnsigned(double r){
  sqlite3_uint128 res;
  double hi = r/18446744073709551616.0;
  res.quads[1] = (u64)hi;
  res.quads[0] = (u64)(r - ((double)res.quads[1])*18446744073709551616.0);
  return res;
}

#endif /* SQLITE_USE_UINT128 */

/* The following two helpers are common to both representations above,
** built entirely from the per-representation primitives already
** defined. They convert between a signed (two's-complement)
** sqlite3_uint128 and the nearest double, mirroring how i64<->double
** conversion works for sqlite3IntFloatCompare() (vdbeaux.c). */
static SQLITE_INLINE double int128ToDoubleSigned(sqlite3_uint128 x){
  if( int128IsNegative(x) ) return -int128ToDoubleUnsigned(int128Negate(x));
  return int128ToDoubleUnsigned(x);
}
static SQLITE_INLINE sqlite3_uint128 int128FromDoubleSigned(double r){
  if( r<0.0 ) return int128Negate(int128FromDoubleUnsigned(-r));
  return int128FromDoubleUnsigned(r);
}

/* Unsigned 128-bit division: dividend/divisor -> *pQuot, *pRem. divisor
** must be nonzero (undefined -- infinite loop avoided but result is
** meaningless -- if zero; callers must check first, same as SQLite's
** existing 64-bit division opcodes do). Plain bit-at-a-time restoring
** long division: not fast, but this is only reached for values that
** overflow 64 bits in the first place, which is rare, and it is built
** entirely out of the portable primitives above so it needs no
** per-representation implementation. */
static SQLITE_INLINE void int128DivModU(
  sqlite3_uint128 dividend,
  sqlite3_uint128 divisor,
  sqlite3_uint128 *pQuot,
  sqlite3_uint128 *pRem
){
  sqlite3_uint128 quot = int128FromU64(0);
  sqlite3_uint128 rem = int128FromU64(0);
  int i;
  assert( !int128IsZero(divisor) );
  for(i=127; i>=0; i--){
    int bit = (int)(int128ToU64(int128ShiftRight(dividend, i)) & 1);
    rem = int128ShiftLeft(rem, 1);
    if( bit ) rem = int128Increment(rem);
    if( int128Compare(rem, divisor)>=0 ){
      rem = int128Sub(rem, divisor);
      quot = int128Increment(int128ShiftLeft(quot, 1));
    }else{
      quot = int128ShiftLeft(quot, 1);
    }
  }
  *pQuot = quot;
  *pRem = rem;
}

/* Signed 128-bit division, truncating toward zero (same convention as
** C's / and % on signed integers, and the same convention SQLite's
** OP_Divide/OP_Remainder use for i64). divisor must be nonzero.
**
** Note: dividend==SMALLEST_INT128 (-2^127) && divisor==-1 is the one
** input pair whose mathematical quotient (2^127) doesn't fit back into
** a signed 128-bit result; this function does not special-case it (it
** returns the wrapped/truncated bit pattern), matching how
** int128DivModU()/negation behave everywhere else in this header --
** callers needing overflow detection there should check for it
** explicitly, the same way OP_Divide already does for the i64 case. */
static SQLITE_INLINE void int128DivMod(
  sqlite3_uint128 dividend,
  sqlite3_uint128 divisor,
  sqlite3_uint128 *pQuot,
  sqlite3_uint128 *pRem
){
  int dn = int128IsNegative(dividend), vn = int128IsNegative(divisor);
  sqlite3_uint128 ad = dn ? int128Negate(dividend) : dividend;
  sqlite3_uint128 av = vn ? int128Negate(divisor) : divisor;
  sqlite3_uint128 q, r;
  int128DivModU(ad, av, &q, &r);
  *pQuot = (dn!=vn) ? int128Negate(q) : q;
  *pRem = dn ? int128Negate(r) : r;
}

/* Attempt to add/subtract/multiply the signed 128-bit value iB against
** the other signed 128-bit integer at *pA and store the result in *pA.
** Return 0 on success, or 1 (leaving *pA unchanged) on signed overflow.
** Mirrors sqlite3AddInt64()/sqlite3SubInt64()/sqlite3MulInt64() (util.c)
** at 128 bits, for OP_Add/OP_Subtract/OP_Multiply (Phase 6d) to fall
** back to floating-point on overflow the same way the i64 path does.
*/
static SQLITE_INLINE int int128AddOverflow(sqlite3_uint128 *pA, sqlite3_uint128 iB){
  sqlite3_uint128 iA = *pA;
  sqlite3_uint128 r = int128Add(iA, iB);
  int an = int128IsNegative(iA), bn = int128IsNegative(iB);
  if( an==bn && int128IsNegative(r)!=an ) return 1;
  *pA = r;
  return 0;
}
static SQLITE_INLINE int int128SubOverflow(sqlite3_uint128 *pA, sqlite3_uint128 iB){
  sqlite3_uint128 smallest = int128ShiftLeft(int128FromU64(1), 127); /* -2^127 */
  if( int128Compare(iB, smallest)==0 ){
    if( !int128IsNegative(*pA) ) return 1;
    *pA = int128Sub(*pA, iB);
    return 0;
  }
  return int128AddOverflow(pA, int128Negate(iB));
}
static SQLITE_INLINE int int128MulOverflow(sqlite3_uint128 *pA, sqlite3_uint128 iB){
  sqlite3_uint128 iA = *pA;
  sqlite3_uint128 smallest = int128ShiftLeft(int128FromU64(1), 127);  /* -2^127 */
  sqlite3_uint128 largest = int128Decrement(smallest);                /* 2^127-1, i.e. ~smallest */
  sqlite3_uint128 q, r;
  if( int128IsZero(iA) || int128IsZero(iB) ){
    *pA = int128FromU64(0);
    return 0;
  }
  if( !int128IsNegative(iB) ){
    int128DivMod(largest, iB, &q, &r);
    if( int128CompareSigned(iA, q)>0 ) return 1;
    int128DivMod(smallest, iB, &q, &r);
    if( int128CompareSigned(iA, q)<0 ) return 1;
  }else if( !int128IsNegative(iA) ){
    int128DivMod(smallest, iA, &q, &r);
    if( int128CompareSigned(iB, q)<0 ) return 1;
  }else{
    /* both iA and iB negative */
    if( int128Compare(iB, smallest)==0 ) return 1;
    if( int128Compare(iA, smallest)==0 ) return 1;
    int128DivMod(largest, int128Negate(iB), &q, &r);
    if( int128CompareSigned(int128Negate(iA), q)>0 ) return 1;
  }
  *pA = int128MultiplyLow(iA, iB);
  return 0;
}

#endif /* SQLITE_INT128_H */
