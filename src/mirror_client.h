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

/* Mirrored-tree mode, client side (mirror_client.c). */

#ifndef DISTCC_MIRROR_CLIENT_H
#define DISTCC_MIRROR_CLIENT_H

#include "mirror.h"

struct dcc_mirror_job {
    char *cwd;                          /* CDIR, after DISTCC_MIRROR_PATHMAP */
    struct dcc_mirror_checklist checks; /* NCHK */
    char cver[DCC_SHA256_HEX_LEN + 1];  /* CVER: compiler binary digest */
    char **env;                         /* ENVS: NAME=VALUE */
    int n_env;
    struct dcc_mirror_rules rules;      /* RULE */
    char **argv;                        /* what was sent, for the check */
};

void dcc_mirror_job_init(struct dcc_mirror_job *job);
void dcc_mirror_job_free(struct dcc_mirror_job *job);

int dcc_mirror_build_checklist(char **argv, const char *input_fname,
                               struct dcc_mirror_checklist *cl);
int dcc_mirror_prepare(char **argv, const char *input_fname,
                       struct dcc_mirror_job *job);
int dcc_mirror_send_request(int fd, char **argv, struct dcc_mirror_job *job);
int dcc_mirror_verify(struct dcc_mirror_job *job, char **argv,
                      char *dsta, size_t dsta_len,
                      const char *dotd_text, size_t dotd_len,
                      char **first_mismatch);
int dcc_mirror_retrieve_results(int fd, int *status,
                                const char *output_fname,
                                const char *deps_fname,
                                const char *server_stderr_fname,
                                struct dcc_mirror_job *job,
                                int *mirr, int *verify_failed);

#endif /* DISTCC_MIRROR_CLIENT_H */
