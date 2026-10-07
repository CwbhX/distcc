/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * Copyright 2026 Clement Hathaway
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

/* SHA-256 as specified in FIPS 180-4. */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "sha256.h"

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void dcc_sha256_block(struct dcc_sha256 *ctx, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t) p[4*i] << 24) | ((uint32_t) p[4*i+1] << 16)
            | ((uint32_t) p[4*i+2] << 8) | (uint32_t) p[4*i+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i-15], 7) ^ ROTR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROTR(w[i-2], 17) ^ ROTR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
    d = ctx->state[3]; e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25))
            + ((e & f) ^ (~e & g)) + k[i] + w[i];
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22))
            + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void dcc_sha256_init(struct dcc_sha256 *ctx)
{
    static const uint32_t init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(ctx->state, init, sizeof init);
    ctx->total = 0;
    ctx->buf_len = 0;
}

void dcc_sha256_update(struct dcc_sha256 *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;

    ctx->total += len;
    if (ctx->buf_len) {
        size_t take = 64 - ctx->buf_len;
        if (take > len)
            take = len;
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len < 64)
            return;
        dcc_sha256_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (len >= 64) {
        dcc_sha256_block(ctx, p);
        p += 64;
        len -= 64;
    }
    memcpy(ctx->buf, p, len);
    ctx->buf_len = len;
}

void dcc_sha256_final(struct dcc_sha256 *ctx, uint8_t out[DCC_SHA256_LEN])
{
    uint64_t bits = ctx->total * 8;
    uint8_t pad = 0x80;
    uint8_t len_be[8];
    int i;

    dcc_sha256_update(ctx, &pad, 1);
    pad = 0;
    while (ctx->buf_len != 56)
        dcc_sha256_update(ctx, &pad, 1);
    for (i = 0; i < 8; i++)
        len_be[i] = (uint8_t) (bits >> (56 - 8 * i));
    dcc_sha256_update(ctx, len_be, 8);
    for (i = 0; i < 8; i++) {
        out[4*i] = (uint8_t) (ctx->state[i] >> 24);
        out[4*i+1] = (uint8_t) (ctx->state[i] >> 16);
        out[4*i+2] = (uint8_t) (ctx->state[i] >> 8);
        out[4*i+3] = (uint8_t) ctx->state[i];
    }
}

int dcc_sha256_file_hex(const char *fname, char hex[DCC_SHA256_HEX_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    struct dcc_sha256 ctx;
    uint8_t digest[DCC_SHA256_LEN];
    char buf[65536];
    ssize_t n;
    int fd, i;

    if ((fd = open(fname, O_RDONLY)) == -1) {
        rs_trace("open %s failed: %s", fname, strerror(errno));
        return EXIT_IO_ERROR;
    }
    dcc_sha256_init(&ctx);
    while ((n = read(fd, buf, sizeof buf)) != 0) {
        if (n == -1) {
            if (errno == EINTR)
                continue;
            rs_log_error("read %s failed: %s", fname, strerror(errno));
            close(fd);
            return EXIT_IO_ERROR;
        }
        dcc_sha256_update(&ctx, buf, (size_t) n);
    }
    close(fd);
    dcc_sha256_final(&ctx, digest);
    for (i = 0; i < DCC_SHA256_LEN; i++) {
        hex[2*i] = digits[digest[i] >> 4];
        hex[2*i+1] = digits[digest[i] & 15];
    }
    hex[DCC_SHA256_HEX_LEN] = '\0';
    return 0;
}
