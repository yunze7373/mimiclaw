/**
 * Ed25519 signature verification for ESP32.
 *
 * Extracted from TweetNaCl (public domain) by Daniel J. Bernstein,
 * Bernard van Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe,
 * Sjaak Smetsers.
 *
 * Only the Ed25519 open/verify path is included.
 * SHA-512 provided by mbedtls (available in ESP-IDF).
 *
 * Reference: https://tweetnacl.cr.yp.to/20140427/tweetnacl.c
 * License: Public Domain
 */

#include "crypto/ed25519_verify.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/sha512.h"

/* ── Types ────────────────────────────────────────────────────── */

typedef unsigned char u8;
typedef unsigned long long u64;
typedef long long i64;
typedef i64 gf[16];

/* ── Constants ────────────────────────────────────────────────── */

static const gf
  gf0 = {0},
  gf1 = {1},
  D = {0x78a3, 0x1359, 0x4dca, 0x75eb,
       0xd8ab, 0x4141, 0x0a4d, 0x0070,
       0xe898, 0x7779, 0x4079, 0x8cc7,
       0xfe73, 0x2b6f, 0x6cee, 0x5203},
  D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6,
        0xb156, 0x8283, 0x149a, 0x00e0,
        0xd130, 0xeef3, 0x80f2, 0x198e,
        0xfce7, 0x56df, 0xd9dc, 0x2406},
  X = {0xd51a, 0x8f25, 0x2d60, 0xc956,
       0xa7b2, 0x9525, 0xc760, 0x692c,
       0xdc5c, 0xfdd6, 0xe231, 0xc0a4,
       0x53fe, 0xcd6e, 0x36d3, 0x2169},
  Y = {0x6658, 0x6666, 0x6666, 0x6666,
       0x6666, 0x6666, 0x6666, 0x6666,
       0x6666, 0x6666, 0x6666, 0x6666,
       0x6666, 0x6666, 0x6666, 0x6666},
  I_ = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee,
        0xe478, 0xad2f, 0x1806, 0x2f43,
        0xd7a7, 0x3dfb, 0x0099, 0x2b4d,
        0xdf0b, 0x4fc1, 0x2480, 0x2b83};

static const u64 L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x10
};

/* ── Utility ──────────────────────────────────────────────────── */

#define FOR(i,n) for (i64 i = 0; i < (n); ++i)

static int vn(const u8 *x, const u8 *y, int n)
{
    unsigned int d = 0;
    FOR(i, n) d |= x[i] ^ y[i];
    return (1 & ((d - 1) >> 8)) - 1;
}

/* ── Field arithmetic (GF(2^255-19)) ──────────────────────────── */

static void set25519(gf r, const gf a)
{
    FOR(i, 16) r[i] = a[i];
}

static void car25519(gf o)
{
    i64 c;
    FOR(i, 16) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    i64 c = ~(b - 1);
    FOR(i, 16) {
        i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(u8 *o, const gf n)
{
    int b;
    gf m, t;
    FOR(i, 16) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    FOR(i, 16) {
        o[2 * i] = t[i] & 0xff;
        o[2 * i + 1] = t[i] >> 8;
    }
}

static int neq25519(const gf a, const gf b)
{
    u8 c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return vn(c, d, 32);
}

static u8 par25519(const gf a)
{
    u8 d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const u8 *n)
{
    FOR(i, 16) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { FOR(i, 16) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { FOR(i, 16) o[i] = a[i] - b[i]; }

static void M(gf o, const gf a, const gf b)
{
    i64 t[31];
    FOR(i, 31) t[i] = 0;
    FOR(i, 16) FOR(j, 16) t[i + j] += a[i] * b[j];
    FOR(i, 15) t[i] += 38 * t[i + 16];
    FOR(i, 16) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i)
{
    gf c;
    FOR(a, 16) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    FOR(a, 16) o[a] = c[a];
}

static void pow2523(gf o, const gf i)
{
    gf c;
    FOR(a, 16) c[a] = i[a];
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    FOR(a, 16) o[a] = c[a];
}

/* ── Extended point operations ────────────────────────────────── */

static void add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], u8 b)
{
    FOR(i, 4) sel25519(p[i], q[i], b);
}

static void pack(u8 *r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void scalarmult(gf p[4], gf q[4], const u8 *s)
{
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        u8 b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const u8 *s)
{
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

/* ── Scalar modular reduction ─────────────────────────────────── */

static void modL(u8 *r, i64 x[64])
{
    i64 carry;
    for (i64 i = 63; i >= 32; --i) {
        carry = 0;
        for (i64 j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    carry = 0;
    FOR(j, 32) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    FOR(j, 32) x[j] -= carry * L[j];
    FOR(i, 32) {
        x[i + 1] += x[i] >> 8;
        r[i] = x[i] & 255;
    }
}

static void reduce(u8 *r)
{
    i64 x[64];
    FOR(i, 64) x[i] = (u64)r[i];
    FOR(i, 64) r[i] = 0;
    modL(r, x);
}

/* ── Point decompression ──────────────────────────────────────── */

static int unpackneg(gf r[4], const u8 p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I_);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

/* ── SHA-512 using mbedtls ────────────────────────────────────── */

static void crypto_hash(u8 *out, const u8 *m, size_t n)
{
    mbedtls_sha512(m, n, out, 0);
}

/* ── Public API ───────────────────────────────────────────────── */

bool ed25519_verify(const uint8_t signature[64],
                    const uint8_t *message, size_t message_len,
                    const uint8_t public_key[32])
{
    if (!signature || !message || !public_key) return false;

    /*
     * TweetNaCl's crypto_sign_open expects a "signed message" format:
     *   sm[0..31]   = R (first half of signature)
     *   sm[32..63]  = S (second half of signature)
     *   sm[64..n-1] = original message
     * Then it verifies: [S]B == R + [H(R||A||M)]A
     */
    size_t n = 64 + message_len;
    u8 *m = malloc(n);
    if (!m) return false;

    u8 *sm = malloc(n);
    if (!sm) { free(m); return false; }

    /* Build signed message: R || S || message */
    memcpy(sm, signature, 64);
    memcpy(sm + 64, message, message_len);

    /* === Inline crypto_sign_open logic === */
    u8 t[32], h[64];
    gf p[4], q[4];

    if (unpackneg(q, public_key)) {
        free(m);
        free(sm);
        return false;
    }

    /* m = copy of sm, but with pk replacing bytes 32..63 for hashing */
    memcpy(m, sm, n);
    memcpy(m + 32, public_key, 32);
    crypto_hash(h, m, n);
    reduce(h);

    scalarmult(p, q, h);
    scalarbase(q, sm + 32);
    add(p, q);
    pack(t, p);

    /* Compare R (first 32 bytes of sm) with computed point */
    int result = vn(sm, t, 32);

    free(m);
    free(sm);

    return (result == 0);
}
