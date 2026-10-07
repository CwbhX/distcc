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

/* Test harness for the mirror-mode helpers, driven by test/testdistcc.py.
 *
 *   h_mirror sha256 FILE        print the SHA-256 of FILE
 *   h_mirror digest FILE        same, through the digest cache
 *   h_mirror dotd FILE          print the prerequisites of a .d file
 *   h_mirror ident FILE         print the DSTA line for FILE
 *   h_mirror dsta LINE          parse a DSTA line and print it back
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "mirror.h"

const char *rs_program_name = __FILE__;

int main(int argc, char *argv[])
{
    char hex[DCC_SHA256_HEX_LEN + 1];
    int ret, i;

    rs_trace_set_level(RS_LOG_WARNING);
    rs_add_logger(rs_logger_file, RS_LOG_DEBUG, NULL, STDERR_FILENO);

    if (argc != 3) {
        fprintf(stderr, "usage: h_mirror sha256|digest|dotd|ident|dsta ARG\n");
        return 1;
    }

    if (strcmp(argv[1], "sha256") == 0) {
        if ((ret = dcc_sha256_file_hex(argv[2], hex)))
            return ret;
        printf("%s\n", hex);
    } else if (strcmp(argv[1], "digest") == 0) {
        struct stat st;
        if (stat(argv[2], &st) == -1)
            return EXIT_IO_ERROR;
        if ((ret = dcc_mirror_digest(argv[2], &st, hex)))
            return ret;
        printf("%s\n", hex);
    } else if (strcmp(argv[1], "dotd") == 0) {
        char *text, **paths;
        int n;
        if ((ret = dcc_load_file_string(argv[2], &text)))
            return ret;
        if ((ret = dcc_mirror_parse_dotd(text, strlen(text), &paths, &n)))
            return ret;
        for (i = 0; i < n; i++)
            printf("%s\n", paths[i]);
        dcc_mirror_free_paths(paths, n);
        free(text);
    } else if (strcmp(argv[1], "ident") == 0) {
        struct dcc_mirror_ident ident;
        char *line;
        if ((ret = dcc_mirror_ident_of(argv[2],
                                       dcc_mirror_is_installed_path(argv[2]),
                                       &ident)))
            return ret;
        if ((ret = dcc_mirror_format_dsta_line(argv[2], &ident, &line)))
            return ret;
        fputs(line, stdout);
        free(line);
    } else if (strcmp(argv[1], "dsta") == 0) {
        struct dcc_mirror_ident ident;
        char *copy = strdup(argv[2]), *path, *line;
        if ((ret = dcc_mirror_parse_dsta_line(copy, &path, &ident))) {
            printf("invalid\n");
            return 0;
        }
        if ((ret = dcc_mirror_format_dsta_line(path, &ident, &line)))
            return ret;
        fputs(line, stdout);
        free(line);
        free(copy);
    } else {
        fprintf(stderr, "h_mirror: unknown command %s\n", argv[1]);
        return 1;
    }
    return 0;
}
