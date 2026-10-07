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

/* SHA-256 (FIPS 180-4), used by mirror mode to compare file contents. */

#ifndef DISTCC_SHA256_H
#define DISTCC_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define DCC_SHA256_LEN 32
#define DCC_SHA256_HEX_LEN 64

struct dcc_sha256 {
    uint32_t state[8];
    uint64_t total;
    uint8_t buf[64];
    size_t buf_len;
};

void dcc_sha256_init(struct dcc_sha256 *ctx);
void dcc_sha256_update(struct dcc_sha256 *ctx, const void *data, size_t len);
void dcc_sha256_final(struct dcc_sha256 *ctx, uint8_t out[DCC_SHA256_LEN]);

/* Hash the contents of @p fname into a NUL-terminated lowercase hex string.
 * Returns 0 on success, or an EXIT_ code. */
int dcc_sha256_file_hex(const char *fname, char hex[DCC_SHA256_HEX_LEN + 1]);

#endif /* DISTCC_SHA256_H */
