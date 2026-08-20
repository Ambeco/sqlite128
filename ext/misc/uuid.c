/*
** 2019-10-23
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This SQLite extension implements functions that handling RFC-4122 UUIDs
** Four SQL functions are implemented:
**
**     uuid()        - generate a version 4 UUID as a string
**     uuid_str(X)   - convert a UUID X into a well-formed UUID string
**     uuid_blob(X)  - convert a UUID X into a 16-byte blob
**     uuid_int(X)   - convert a UUID X into its canonical 128-bit integer
**                     representation, as a 16-byte big-endian blob
**
** The output from uuid() and uuid_str(X) are always well-formed RFC-4122
** UUID strings in this format:
**
**        xxxxxxxx-xxxx-Mxxx-Nxxx-xxxxxxxxxxxx
**
** All of the 'x', 'M', and 'N' values are lower-case hexadecimal digits.
** The M digit indicates the "version".  For uuid()-generated UUIDs, the
** version is always "4" (a random UUID).  The upper three bits of N digit
** are the "variant".  This library only supports variant 1 (indicated
** by values of N between '8' and 'b') as those are overwhelming the most
** common.  Other variants are for legacy compatibility only.
**
** The output of uuid_blob(X) is always a 16-byte blob.  The UUID input
** string is converted in network byte order (big-endian) in accordance
** with RFC-4122 specifications for variant-1 UUIDs.  Note that network
** byte order is *always* used, even if the input self-identifies as a
** variant-2 UUID.
**
** The input X to the uuid_str() and uuid_blob() functions can be either
** a string or a BLOB.  If it is a BLOB it must be exactly 16 bytes in
** length or else a NULL is returned.  If the input is a string it must
** consist of 32 hexadecimal digits, upper or lower case, optionally
** surrounded by {...} and with optional "-" characters interposed in the
** middle.  The flexibility of input is inspired by the PostgreSQL
** implementation of UUID functions that accept in all of the following
** formats:
**
**     A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11
**     {a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11}
**     a0eebc999c0b4ef8bb6d6bb9bd380a11
**     a0ee-bc99-9c0b-4ef8-bb6d-6bb9-bd38-0a11
**     {a0eebc99-9c0b4ef8-bb6d6bb9-bd380a11}
**
** If any of the above inputs are passed into uuid_str(), the output will
** always be in the canonical RFC-4122 format:
**
**     a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11
**
** If the X input string has too few or too many digits or contains
** stray characters other than {, }, or -, then NULL is returned.
**
** uuid_int(X) accepts the same inputs as uuid_str()/uuid_blob() (a
** UUID string in any of the accepted forms above, or an exact 16-byte
** blob) and returns the UUID's canonical 128-bit-integer representation,
** interpreting the same 16 bytes uuid_blob() returns as an unsigned
** big-endian ("network" byte order) 128-bit magnitude. Network byte
** order isn't an arbitrary choice here -- comparing two UUIDs byte-by-
** byte (memcmp order, which is also SQLite's default BLOB collation)
** gives exactly the same result as comparing the two 128-bit magnitudes
** numerically, so this representation is both the UUID's defined byte
** layout *and* directly usable wherever an ordered/sortable 128-bit
** integer is needed -- notably as a wide rowid value in a build with
** SQLITE_128BIT_ROWID enabled.
**
** In a build with SQLITE_128BIT_ROWID defined, uuid_int(X) returns a
** genuine SQLITE_INTEGER-typed 128-bit value (via sqlite3_result_int128()
** -- Phase 7), usable directly in arithmetic, comparison, INSERT/SELECT,
** WHERE clauses, and so on, the same as any other integer. In a default
** build, where the engine has no 128-bit integer type to return, it
** falls back to producing the same 16-byte big-endian BLOB uuid_blob()
** does (its pre-Phase-7 behavior) -- since a UUID's magnitude routinely
** exceeds 2^63, a plain 64-bit sqlite3_result_int64() would silently
** truncate it, which would be worse than being explicit that a default
** build cannot represent this value as a native integer at all.
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <ctype.h>

#if !defined(SQLITE_ASCII) && !defined(SQLITE_EBCDIC)
# define SQLITE_ASCII 1
#endif

/*
** Translate a single byte of Hex into an integer.
** This routine only works if h really is a valid hexadecimal
** character:  0..9a..fA..F
*/
static unsigned char sqlite3UuidHexToInt(int h){
  assert( (h>='0' && h<='9') ||  (h>='a' && h<='f') ||  (h>='A' && h<='F') );
#ifdef SQLITE_ASCII
  h += 9*(1&(h>>6));
#endif
#ifdef SQLITE_EBCDIC
  h += 9*(1&~(h>>4));
#endif
  return (unsigned char)(h & 0xf);
}

/*
** Convert a 16-byte BLOB into a well-formed RFC-4122 UUID.  The output
** buffer zStr should be at least 37 bytes in length.   The output will
** be zero-terminated.
*/
static void sqlite3UuidBlobToStr(
  const unsigned char *aBlob,  /* Input blob */
  unsigned char *zStr          /* Write the answer here */
){
  static const char zDigits[] = "0123456789abcdef";
  int i, k;
  unsigned char x;
  k = 0;
  for(i=0, k=0x550; i<16; i++, k=k>>1){
    if( k&1 ){
      zStr[0] = '-';
      zStr++;
    }
    x = aBlob[i];
    zStr[0] = zDigits[x>>4];
    zStr[1] = zDigits[x&0xf];
    zStr += 2;
  }
  *zStr = 0;
}

/*
** Attempt to parse a zero-terminated input string zStr into a binary
** UUID.  Return 0 on success, or non-zero if the input string is not
** parsable.
*/
static int sqlite3UuidStrToBlob(
  const unsigned char *zStr,   /* Input string */
  unsigned char *aBlob         /* Write results here */
){
  int i;
  if( zStr==0 ) return 1;
  if( zStr[0]=='{' ) zStr++;
  for(i=0; i<16; i++){
    if( zStr[0]=='-' ) zStr++;
    if( isxdigit(zStr[0]) && isxdigit(zStr[1]) ){
      aBlob[i] = (sqlite3UuidHexToInt(zStr[0])<<4)
                      + sqlite3UuidHexToInt(zStr[1]);
      zStr += 2;
    }else{
      return 1;
    }
  }
  if( zStr[0]=='}' ) zStr++;
  return zStr[0]!=0;
}

/*
** Render sqlite3_value pIn as a 16-byte UUID blob.  Return a pointer
** to the blob, or NULL if the input is not well-formed.
*/
static const unsigned char *sqlite3UuidInputToBlob(
  sqlite3_value *pIn,     /* Input text */
  unsigned char *pBuf     /* output buffer */
){
  switch( sqlite3_value_type(pIn) ){
    case SQLITE_TEXT: {
      const unsigned char *z = sqlite3_value_text(pIn);
      if( sqlite3UuidStrToBlob(z, pBuf) ) return 0;
      return pBuf;
    }
    case SQLITE_BLOB: {
      int n = sqlite3_value_bytes(pIn);
      return n==16 ? sqlite3_value_blob(pIn) : 0;
    }
    default: {
      return 0;
    }
  }
}

/* Implementation of uuid() */
static void sqlite3UuidFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  unsigned char aBlob[16];
  unsigned char zStr[37];
  (void)argc;
  (void)argv;
  sqlite3_randomness(16, aBlob);
  aBlob[6] = (aBlob[6]&0x0f) + 0x40;
  aBlob[8] = (aBlob[8]&0x3f) + 0x80;
  sqlite3UuidBlobToStr(aBlob, zStr);
  sqlite3_result_text(context, (char*)zStr, 36, SQLITE_TRANSIENT);
}

/* Implementation of uuid_str() */
static void sqlite3UuidStrFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  unsigned char aBlob[16];
  unsigned char zStr[37];
  const unsigned char *pBlob;
  (void)argc;
  pBlob = sqlite3UuidInputToBlob(argv[0], aBlob);
  if( pBlob==0 ) return;
  sqlite3UuidBlobToStr(pBlob, zStr);
  sqlite3_result_text(context, (char*)zStr, 36, SQLITE_TRANSIENT);
}

/* Implementation of uuid_blob() */
static void sqlite3UuidBlobFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  unsigned char aBlob[16];
  const unsigned char *pBlob;
  (void)argc;
  pBlob = sqlite3UuidInputToBlob(argv[0], aBlob);
  if( pBlob==0 ) return;
  sqlite3_result_blob(context, pBlob, 16, SQLITE_TRANSIENT);
}

/* Implementation of uuid_int() -- see the block comment near the top of
** this file for the SQLITE_128BIT_ROWID-vs-default-build distinction.
*/
static void sqlite3UuidIntFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  unsigned char aBlob[16];
  const unsigned char *pBlob;
  (void)argc;
  pBlob = sqlite3UuidInputToBlob(argv[0], aBlob);
  if( pBlob==0 ) return;
#ifdef SQLITE_128BIT_ROWID
  {
    /* Big-endian 16 bytes -> (hi,lo) limb pair for sqlite3_result_int128().
    ** aBlob[0..7] is the high (sign-bearing, for the two's-complement
    ** convention sqlite3_result_int128() otherwise uses) half; a UUID's
    ** magnitude is always treated as unsigned/non-negative here, same as
    ** uuid_blob()'s byte layout, so the top bit of aBlob[0] simply
    ** becomes the sign bit of hiValue -- a UUID with that bit set (over
    ** half of all UUIDs, including every RFC-4122 variant-1 UUID this
    ** library generates) reports as numerically negative when read back
    ** as a signed 128-bit integer. That mirrors how a 16-hex-digit hex
    ** integer literal with its top bit set already becomes a negative
    ** i64 today (see sqlite3DecOrHexToI64()) -- this extension has no
    ** separate unsigned 128-bit SQL type to offer instead, and byte-order
    ** comparison still behaves correctly (see the file-level comment)
    ** regardless of how the sign bit reads back to the caller. */
    sqlite3_int64 hi;
    sqlite3_uint64 lo;
    int i;
    sqlite3_uint64 hiU = 0, loU = 0;
    for(i=0; i<8; i++)  hiU = (hiU<<8) | pBlob[i];
    for(i=8; i<16; i++) loU = (loU<<8) | pBlob[i];
    hi = (sqlite3_int64)hiU;
    lo = loU;
    sqlite3_result_int128(context, hi, lo);
  }
#else
  sqlite3_result_blob(context, pBlob, 16, SQLITE_TRANSIENT);
#endif
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_uuid_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = sqlite3_create_function(db, "uuid", 0, SQLITE_UTF8|SQLITE_INNOCUOUS, 0,
                               sqlite3UuidFunc, 0, 0);
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "uuid_str", 1, 
                       SQLITE_UTF8|SQLITE_INNOCUOUS|SQLITE_DETERMINISTIC,
                       0, sqlite3UuidStrFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "uuid_blob", 1,
                       SQLITE_UTF8|SQLITE_INNOCUOUS|SQLITE_DETERMINISTIC,
                       0, sqlite3UuidBlobFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "uuid_int", 1,
                       SQLITE_UTF8|SQLITE_INNOCUOUS|SQLITE_DETERMINISTIC,
                       0, sqlite3UuidIntFunc, 0, 0);
  }
  return rc;
}
