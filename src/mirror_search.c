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

/* Mirror mode: the include search path, and the files that could shadow
 * what a compile read.
 *
 * The include search goes through, in order: the -iquote directories (for
 * "..." includes, after the includer's own directory), -I and -F with
 * CPATH, -isystem and -iframework with C_INCLUDE_PATH and its language
 * variants, the compiler's own implicit directories (asked from the
 * compiler with -E -v, cached per compiler and flags), and -idirafter.
 *
 * A file the compile read under search directory j, spelled S relative to
 * it, would have been shadowed by S in any earlier directory i < j, and,
 * for a "..." include, by S in the includer's directory.  So both sides
 * list every such candidate that exists there but was not read (a
 * candidate that was read is an #include_next chain, not a shadow).  The
 * client accepts the result only if each of its candidates also exists on
 * the helper with the same content: when the trees match, both see the
 * same harmless duplicates; a file only the client has is exactly what
 * would make its own compile read something else. */

#include <config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "mirror.h"
#include "mirror_search.h"

enum search_group {
    G_QUOTE = 0,
    G_ANGLE,        /* -I, -F, CPATH */
    G_SYSTEM,       /* -isystem, -iframework, C_INCLUDE_PATH, ... */
    G_IMPLICIT,     /* the compiler's own */
    G_AFTER,        /* -idirafter */
    G_COUNT
};

struct search_option {
    const char *name;
    int group;          /* enum search_group, or -1 to refuse */
    int framework;
    int joined;         /* also accepts "<name><dir>" */
    int equals;         /* also accepts "<name>=<dir>" */
};

/* Longest names first, so that a prefix never hides a longer option. */
static const struct search_option search_options[] = {
    { "--include-directory-after", G_AFTER, 0, 0, 1 },
    { "--include-directory", G_ANGLE, 0, 0, 1 },
    { "-iframeworkwithsysroot", -1, 1, 1, 0 },
    { "-iwithprefixbefore", -1, 0, 1, 0 },
    { "-index-header-map", -1, 0, 0, 0 },
    { "-isystem-after", -1, 0, 1, 0 },
    { "-iwithsysroot", -1, 0, 1, 0 },
    { "-iwithprefix", -1, 0, 1, 0 },
    { "-ivfsoverlay", -1, 0, 1, 0 },
    { "-cxx-isystem", -1, 0, 1, 0 },
    { "-iframework", G_SYSTEM, 1, 1, 0 },
    { "-idirafter", G_AFTER, 0, 1, 0 },
    { "--include-prefix", -1, 0, 1, 1 },
    { "-isystem", G_SYSTEM, 0, 1, 0 },
    { "-iprefix", -1, 0, 1, 0 },
    { "-iquote", G_QUOTE, 0, 1, 0 },
    { "-I-", -1, 0, 0, 0 },
    { "-I", G_ANGLE, 0, 1, 0 },
    { "-F", G_ANGLE, 1, 1, 0 },
    { NULL, 0, 0, 0, 0 }
};

/* Environment variables that add search directories, and where. */
static const struct { const char *name; int group; } search_env[] = {
    { "CPATH", G_ANGLE },
    { "C_INCLUDE_PATH", G_SYSTEM },
    { "CPLUS_INCLUDE_PATH", G_SYSTEM },
    { "OBJC_INCLUDE_PATH", G_SYSTEM },
    { "OBJCPLUS_INCLUDE_PATH", G_SYSTEM },
    { NULL, 0 }
};


/**
 * Is argv[i] a search-path option?  On a match, *opt is set, *val to its
 * operand (NULL if missing) and *consumed to the number of extra argv
 * entries it used.
 **/
static int match_search_option(char **argv, int i,
                               const struct search_option **opt,
                               const char **val, int *consumed)
{
    const char *a = argv[i];
    int k;

    for (k = 0; search_options[k].name; k++) {
        const struct search_option *o = &search_options[k];
        size_t len = strlen(o->name);
        if (strncmp(a, o->name, len) != 0)
            continue;
        if (a[len] == '\0') {
            *opt = o;
            *val = argv[i + 1];
            *consumed = argv[i + 1] ? 1 : 0;
            return 1;
        }
        if (o->equals && a[len] == '=') {
            *opt = o;
            *val = a + len + 1;
            *consumed = 0;
            return 1;
        }
        if (o->joined && !o->equals) {
            *opt = o;
            *val = a + len;
            *consumed = 0;
            return 1;
        }
    }
    return 0;
}


/**
 * Does argv change the include search in a way mirror mode does not
 * model?  Logs and returns nonzero if so.
 **/
int dcc_mirror_check_search_options(char **argv)
{
    int i;

    for (i = 0; argv[i]; i++) {
        const struct search_option *o;
        const char *val;
        int consumed;
        if (strcmp(argv[i], "-Xclang") == 0
            || strcmp(argv[i], "-Xpreprocessor") == 0) {
            i++;
            continue;
        }
        if (!match_search_option(argv, i, &o, &val, &consumed))
            continue;
        if (o->group < 0 || !val || !*val) {
            rs_log_info("mirror: %s is not supported", argv[i]);
            return EXIT_DISTCC_FAILED;
        }
        i += consumed;
    }
    return 0;
}


void dcc_search_list_free(struct dcc_search_list *sl)
{
    int i;

    for (i = 0; i < sl->n; i++) {
        free(sl->d[i].spelled);
        free(sl->d[i].norm);
    }
    free(sl->d);
    memset(sl, 0, sizeof *sl);
}

static int search_list_add(struct dcc_search_list *sl,
                           const struct dcc_mirror_rules *r,
                           const char *spelled, int framework)
{
    struct dcc_search_dir *n;
    char *norm;
    int i;

    if (!(norm = dcc_mirror_normalize(r, spelled)))
        return EXIT_OUT_OF_MEMORY;
    /* The compiler drops a directory already in the list. */
    for (i = 0; i < sl->n; i++) {
        if (strcmp(sl->d[i].norm, norm) == 0) {
            free(norm);
            return 0;
        }
    }
    if (!(n = realloc(sl->d, (sl->n + 1) * sizeof *n))) {
        free(norm);
        return EXIT_OUT_OF_MEMORY;
    }
    sl->d = n;
    n[sl->n].norm = norm;
    n[sl->n].framework = framework;
    if (!(n[sl->n].spelled = strdup(spelled))) {
        free(norm);
        return EXIT_OUT_OF_MEMORY;
    }
    sl->n++;
    return 0;
}

/* Add the directories of a colon-separated environment path; an empty
 * element means the cwd. */
static int add_env_path(struct dcc_search_list *sl,
                        const struct dcc_mirror_rules *r, const char *value)
{
    char *copy, *p, *tok;
    int ret = 0;

    if (!(copy = strdup(value)))
        return EXIT_OUT_OF_MEMORY;
    for (p = copy; !ret && (tok = strsep(&p, ":")) != NULL; )
        ret = search_list_add(sl, r, *tok ? tok : ".", 0);
    free(copy);
    return ret;
}


/* ----- the compiler's implicit directories ----- */

/* The language the compiler sees for @p input, or NULL. */
static const char *input_language(char **argv, const char *input)
{
    const char *x = NULL, *ext;
    int i;

    for (i = 0; argv[i]; i++) {
        if (strcmp(argv[i], "-x") == 0 && argv[i + 1])
            x = argv[++i];
        else if (strncmp(argv[i], "-x", 2) == 0 && argv[i][2])
            x = argv[i] + 2;
    }
    if (x && strcmp(x, "none") != 0)
        return x;
    if (!(ext = strrchr(input, '.')))
        return NULL;
    ext++;
    if (!strcmp(ext, "c") || !strcmp(ext, "i"))
        return "c";
    if (!strcmp(ext, "m"))
        return "objective-c";
    if (!strcmp(ext, "mm") || !strcmp(ext, "M"))
        return "objective-c++";
    if (!strcmp(ext, "cpp") || !strcmp(ext, "cc") || !strcmp(ext, "cxx")
        || !strcmp(ext, "c++") || !strcmp(ext, "C") || !strcmp(ext, "cp")
        || !strcmp(ext, "CPP") || !strcmp(ext, "ii"))
        return "c++";
    if (!strcmp(ext, "S"))
        return "assembler-with-cpp";
    return NULL;
}

/* Flags that change the implicit directories. */
static int implicit_flag(char **argv, int i, int *takes_arg)
{
    static const char *const with_arg[] = {
        "-isysroot", "--sysroot", "-target", "-arch", "-resource-dir",
        "-stdlib", "--gcc-toolchain", "-gcc-toolchain", NULL
    };
    static const char *const prefixes[] = {
        "--sysroot=", "--target=", "-resource-dir=", "-stdlib=",
        "--gcc-toolchain=", "-std=", "-nostdinc", "-nostdlibinc",
        "-nobuiltininc", "-mmacosx-version-min=", "-isysroot", NULL
    };
    int k;

    *takes_arg = 0;
    for (k = 0; with_arg[k]; k++) {
        if (strcmp(argv[i], with_arg[k]) == 0) {
            *takes_arg = argv[i + 1] != NULL;
            return 1;
        }
    }
    for (k = 0; prefixes[k]; k++)
        if (strncmp(argv[i], prefixes[k], strlen(prefixes[k])) == 0)
            return 1;
    return 0;
}

static char *implicit_cache_fname(void)
{
    char *dir, *f;
    if (dcc_get_top_dir(&dir) != 0)
        return NULL;
    if (asprintf(&f, "%s/mirror-implicit", dir) < 0)
        return NULL;
    return f;
}

/* Parse "D:path\tF:path..." into the list (implicit group). */
static int implicit_parse(struct dcc_search_list *sl,
                          const struct dcc_mirror_rules *r, char *entries)
{
    char *p, *tok;
    int ret = 0;

    for (p = entries; !ret && (tok = strsep(&p, "\t")) != NULL; ) {
        if ((tok[0] == 'D' || tok[0] == 'F') && tok[1] == ':' && tok[2] == '/')
            ret = search_list_add(sl, r, tok + 2, tok[0] == 'F');
    }
    return ret;
}

/**
 * Add the compiler's implicit include directories for this command.
 * Runs "<compiler> <flags> -x <lang> -E -v -" once per compiler binary and
 * set of relevant flags, and keeps the answer in $DISTCC_DIR.
 **/
static int add_implicit_dirs(struct dcc_search_list *sl, char **argv,
                             const char *input,
                             const struct dcc_mirror_rules *r)
{
    char cver[DCC_SHA256_HEX_LEN + 1], keyhex[DCC_SHA256_HEX_LEN + 1];
    struct dcc_sha256 ctx;
    uint8_t dg[DCC_SHA256_LEN];
    char *probe[64], *fname = NULL, *line = NULL, *out = NULL, *entries = NULL;
    const char *lang = input_language(argv, input);
    int n = 0, i, ret = 0, pipefd[2], status;
    size_t cap = 0, out_len = 0, out_cap = 0;
    ssize_t len;
    FILE *f;
    pid_t pid;
    static const char *const env_keys[] = { "SDKROOT", "DEVELOPER_DIR", NULL };

    if (!lang)
        return EXIT_DISTCC_FAILED;
    if ((ret = dcc_mirror_compiler_ident(argv[0], cver)))
        return ret;

    probe[n++] = argv[0];
    for (i = 1; argv[i] && n < 56; i++) {
        int takes;
        if (implicit_flag(argv, i, &takes)) {
            probe[n++] = argv[i];
            if (takes)
                probe[n++] = argv[++i];
        }
    }
    if (argv[i])
        return EXIT_DISTCC_FAILED;      /* too many flags to probe */
    probe[n++] = (char *) "-x";
    probe[n++] = (char *) lang;
    probe[n++] = (char *) "-E";
    probe[n++] = (char *) "-v";
    probe[n++] = (char *) "-";
    probe[n] = NULL;

    dcc_sha256_init(&ctx);
    dcc_sha256_update(&ctx, cver, strlen(cver) + 1);
    for (i = 0; i < n; i++)
        dcc_sha256_update(&ctx, probe[i], strlen(probe[i]) + 1);
    for (i = 0; env_keys[i]; i++) {
        const char *v = getenv(env_keys[i]);
        dcc_sha256_update(&ctx, env_keys[i], strlen(env_keys[i]) + 1);
        dcc_sha256_update(&ctx, v ? v : "", strlen(v ? v : "") + 1);
    }
    dcc_sha256_final(&ctx, dg);
    for (i = 0; i < DCC_SHA256_LEN; i++)
        sprintf(keyhex + 2 * i, "%02x", dg[i]);

    /* Cached? */
    fname = implicit_cache_fname();
    if (fname && (f = fopen(fname, "r"))) {
        while ((len = getline(&line, &cap, f)) > 0) {
            if (line[len - 1] != '\n')
                break;
            line[len - 1] = '\0';
            if (strncmp(line, keyhex, DCC_SHA256_HEX_LEN) == 0
                && line[DCC_SHA256_HEX_LEN] == ' ') {
                ret = implicit_parse(sl, r, line + DCC_SHA256_HEX_LEN + 1);
                fclose(f);
                free(line);
                free(fname);
                return ret;
            }
        }
        fclose(f);
    }

    /* Ask the compiler, without the environment's search paths so that
     * only its own directories are listed. */
    if (pipe(pipefd) == -1) {
        ret = EXIT_DISTCC_FAILED;
        goto out;
    }
    if ((pid = fork()) == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        ret = EXIT_DISTCC_FAILED;
        goto out;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        for (i = 0; search_env[i].name; i++)
            unsetenv(search_env[i].name);
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        execvp(probe[0], probe);
        _exit(127);
    }
    close(pipefd[1]);
    for (;;) {
        char buf[8192];
        ssize_t got = read(pipefd[0], buf, sizeof buf);
        if (got == -1 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        if (out_len + (size_t) got + 1 > out_cap) {
            size_t nc = out_cap ? out_cap * 2 : 16384;
            char *nb;
            while (nc < out_len + (size_t) got + 1)
                nc *= 2;
            if (!(nb = realloc(out, nc))) {
                ret = EXIT_OUT_OF_MEMORY;
                break;
            }
            out = nb;
            out_cap = nc;
        }
        memcpy(out + out_len, buf, (size_t) got);
        out_len += (size_t) got;
    }
    close(pipefd[0]);
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
        ;
    if (ret)
        goto out;
    if (!out || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        rs_log_info("mirror: could not ask %s for its search path", argv[0]);
        ret = EXIT_DISTCC_FAILED;
        goto out;
    }
    out[out_len] = '\0';
    {
        char *start = strstr(out, "#include <...> search starts here:\n");
        char *end = start ? strstr(start, "End of search list.") : NULL;
        char *p, *l;
        size_t elen = 0;
        if (!start || !end) {
            ret = EXIT_DISTCC_FAILED;
            goto out;
        }
        *end = '\0';
        start += strlen("#include <...> search starts here:\n");
        if (!(entries = calloc(strlen(start) * 2 + 16, 1))) {
            ret = EXIT_OUT_OF_MEMORY;
            goto out;
        }
        for (p = start; (l = strsep(&p, "\n")) != NULL; ) {
            char *suffix;
            int fw = 0;
            while (*l == ' ')
                l++;
            if (!*l)
                continue;
            if ((suffix = strstr(l, " (framework directory)"))) {
                *suffix = '\0';
                fw = 1;
            }
            if (l[0] != '/' || strchr(l, '\t'))
                continue;
            elen += (size_t) sprintf(entries + elen, "%s%c:%s",
                                     elen ? "\t" : "", fw ? 'F' : 'D', l);
        }
    }
    /* Record, then use. */
    if (fname && (f = fopen(fname, "a"))) {
        fprintf(f, "%s %s\n", keyhex, entries);
        fclose(f);
    }
    ret = implicit_parse(sl, r, entries);

  out:
    free(entries);
    free(out);
    free(line);
    free(fname);
    return ret;
}


/**
 * The include search path of a compile, in order.  With @p implicit, the
 * compiler's own directories are added (this may run the compiler once).
 * Fails if argv uses a search option mirror mode does not model.
 **/
int dcc_mirror_search_list(char **argv, const char *input,
                           const struct dcc_mirror_rules *r, int implicit,
                           struct dcc_search_list *sl)
{
    int group, i, k, ret = 0;

    memset(sl, 0, sizeof *sl);
    if ((ret = dcc_mirror_check_search_options(argv)))
        return ret;
    for (group = 0; group < G_COUNT && !ret; group++) {
        if (group == G_IMPLICIT) {
            if (implicit)
                ret = add_implicit_dirs(sl, argv, input, r);
            continue;
        }
        for (i = 0; argv[i] && !ret; i++) {
            const struct search_option *o;
            const char *val;
            int consumed;
            if (strcmp(argv[i], "-Xclang") == 0
                || strcmp(argv[i], "-Xpreprocessor") == 0) {
                i++;
                continue;
            }
            if (!match_search_option(argv, i, &o, &val, &consumed))
                continue;
            if (o->group == group)
                ret = search_list_add(sl, r, val, o->framework);
            i += consumed;
        }
        for (k = 0; search_env[k].name && !ret; k++) {
            const char *v = getenv(search_env[k].name);
            if (v && search_env[k].group == group)
                ret = add_env_path(sl, r, v);
        }
    }
    if (ret)
        dcc_search_list_free(sl);
    return ret;
}


/* ----- shadow candidates ----- */

/* Load a directory's entry names into @p set. */
static int load_names(const char *opened, struct dcc_strset *set)
{
    DIR *d;
    struct dirent *de;
    int ret = 0;

    if (!(d = opendir(opened)))
        return 0;
    while (!ret && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && (!de->d_name[1]
                                     || (de->d_name[1] == '.' && !de->d_name[2])))
            continue;
        ret = dcc_strset_add(set, de->d_name);
    }
    closedir(d);
    return ret;
}

/* name -> directories (of files read) that have an entry of that name. */
struct name_index {
    char **names;
    char ***dirs;
    int *n_dirs;
    size_t cap, used;
};

static unsigned long long nhash(const char *s, size_t len)
{
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned char) s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int index_slot(struct name_index *x, const char *name, size_t len,
                      int create, size_t *slot)
{
    size_t i;

    if (create && (x->used + 1) * 2 > x->cap) {
        size_t ncap = x->cap ? x->cap * 2 : 4096, j;
        char **nn = calloc(ncap, sizeof *nn);
        char ***nd = calloc(ncap, sizeof *nd);
        int *nc = calloc(ncap, sizeof *nc);
        if (!nn || !nd || !nc) {
            free(nn); free(nd); free(nc);
            return EXIT_OUT_OF_MEMORY;
        }
        for (j = 0; j < x->cap; j++) {
            if (!x->names[j])
                continue;
            i = nhash(x->names[j], strlen(x->names[j])) & (ncap - 1);
            while (nn[i])
                i = (i + 1) & (ncap - 1);
            nn[i] = x->names[j];
            nd[i] = x->dirs[j];
            nc[i] = x->n_dirs[j];
        }
        free(x->names); free(x->dirs); free(x->n_dirs);
        x->names = nn; x->dirs = nd; x->n_dirs = nc;
        x->cap = ncap;
    }
    if (!x->cap)
        return -1;
    i = nhash(name, len) & (x->cap - 1);
    while (x->names[i]) {
        if (strlen(x->names[i]) == len && memcmp(x->names[i], name, len) == 0) {
            *slot = i;
            return 0;
        }
        i = (i + 1) & (x->cap - 1);
    }
    if (!create)
        return -1;
    if (!(x->names[i] = strndup(name, len)))
        return EXIT_OUT_OF_MEMORY;
    x->used++;
    *slot = i;
    return 0;
}

static int index_add(struct name_index *x, const char *name, const char *dir)
{
    size_t slot;
    char **nd;
    int ret = index_slot(x, name, strlen(name), 1, &slot);

    if (ret)
        return ret;
    if (!(nd = realloc(x->dirs[slot], (x->n_dirs[slot] + 1) * sizeof *nd)))
        return EXIT_OUT_OF_MEMORY;
    x->dirs[slot] = nd;
    nd[x->n_dirs[slot]++] = (char *) dir;   /* owned by the includer set */
    return 0;
}

static void index_free(struct name_index *x)
{
    size_t i;
    for (i = 0; i < x->cap; i++) {
        free(x->names[i]);
        free(x->dirs[i]);
    }
    free(x->names); free(x->dirs); free(x->n_dirs);
    memset(x, 0, sizeof *x);
}

static char *resolve(dcc_mirror_resolve_fn fn, const void *ctx,
                     const char *path)
{
    return fn ? fn(ctx, path) : strdup(path);
}

static void devino_key(char *buf, size_t len, const struct stat *st)
{
    snprintf(buf, len, "%llu:%llu", (unsigned long long) st->st_dev,
             (unsigned long long) st->st_ino);
}

/* Is @p cand an existing file not in the read set?  Then add it. */
static int consider(const char *cand, dcc_mirror_resolve_fn fn,
                    const void *ctx, const struct dcc_strset *readset,
                    struct dcc_strset *out)
{
    char *opened = resolve(fn, ctx, cand), key[64];
    struct stat st;
    int ret = 0;

    if (!opened)
        return EXIT_OUT_OF_MEMORY;
    if (stat(opened, &st) == 0 && !S_ISDIR(st.st_mode)) {
        devino_key(key, sizeof key, &st);
        if (!dcc_strset_has(readset, key))
            ret = dcc_strset_add(out, cand);
    }
    free(opened);
    return ret;
}

/* The include spelling of @p norm relative to search directory @p d, or
 * NULL.  For a framework directory "X.framework/Headers/rest" (or
 * PrivateHeaders) is spelled "X/rest". */
static char *spelling(const struct dcc_search_dir *d, const char *norm)
{
    size_t len = strlen(d->norm);
    const char *rel, *fw, *rest;
    char *s;

    if (strcmp(d->norm, "/") == 0)
        rel = norm + 1;
    else if (strncmp(norm, d->norm, len) == 0 && norm[len] == '/')
        rel = norm + len + 1;
    else
        return NULL;
    if (!d->framework)
        return strdup(rel);
    if (!(fw = strstr(rel, ".framework/")) || strchr(rel, '/') < fw)
        return NULL;
    rest = fw + strlen(".framework/");
    if (strncmp(rest, "Headers/", 8) == 0)
        rest += 8;
    else if (strncmp(rest, "PrivateHeaders/", 15) == 0)
        rest += 15;
    else
        return NULL;
    if (asprintf(&s, "%.*s/%s", (int) (fw - rel), rel, rest) < 0)
        return NULL;
    return s;
}

/**
 * Every file that would shadow one the compile read and exists here
 * without having been read: the same spelling in an earlier search
 * directory, or (for spellings with a directory part) in the directory of
 * a file read.  @p fn maps a path to the one to open (NULL: as is).
 **/
int dcc_mirror_shadow_candidates(const struct dcc_search_list *sl,
                                 char **files, int n_files,
                                 const char *input,
                                 const struct dcc_mirror_rules *r,
                                 dcc_mirror_resolve_fn fn, const void *ctx,
                                 struct dcc_strset *out)
{
    struct dcc_strset readset = { NULL, 0, 0 }, includers = { NULL, 0, 0 };
    struct dcc_strset *search_names = NULL;
    struct name_index idx;
    char **norms = NULL, *input_norm = NULL, key[64];
    unsigned char *loaded = NULL;
    int f, j, i, ret = 0;
    size_t k;

    memset(&idx, 0, sizeof idx);
    if (!(norms = calloc((size_t) n_files + 1, sizeof *norms))
        || !(search_names = calloc((size_t) sl->n + 1, sizeof *search_names))
        || !(loaded = calloc((size_t) sl->n + 1, 1))
        || (input && !(input_norm = dcc_mirror_normalize(r, input)))) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }

    /* What was read, and the directories it was read from. */
    for (f = 0; f < n_files && !ret; f++) {
        char *opened = resolve(fn, ctx, files[f]), *slash;
        struct stat st;
        if (!opened || !(norms[f] = dcc_mirror_normalize(r, files[f]))) {
            free(opened);
            ret = EXIT_OUT_OF_MEMORY;
            break;
        }
        if (stat(opened, &st) == 0) {
            devino_key(key, sizeof key, &st);
            ret = dcc_strset_add(&readset, key);
        }
        free(opened);
        slash = strrchr(norms[f], '/');
        if (!ret && slash && slash != norms[f]) {
            char *dir = strndup(norms[f], (size_t) (slash - norms[f]));
            if (!dir)
                ret = EXIT_OUT_OF_MEMORY;
            else {
                ret = dcc_strset_add(&includers, dir);
                free(dir);
            }
        }
    }
    /* Which includer directories have which names. */
    for (k = 0; k < includers.cap && !ret; k++) {
        struct dcc_strset names = { NULL, 0, 0 };
        const char *u = includers.items[k];
        char *opened;
        size_t m;
        if (!u)
            continue;
        if (!(opened = resolve(fn, ctx, u))) {
            ret = EXIT_OUT_OF_MEMORY;
            break;
        }
        ret = load_names(opened, &names);
        free(opened);
        for (m = 0; m < names.cap && !ret; m++)
            if (names.items[m])
                ret = index_add(&idx, names.items[m], u);
        dcc_strset_free(&names);
    }

    for (f = 0; f < n_files && !ret; f++) {
        if (input_norm && strcmp(norms[f], input_norm) == 0)
            continue;   /* the main file is not looked up */
        for (j = 0; j < sl->n && !ret; j++) {
            char *spell = spelling(&sl->d[j], norms[f]);
            const char *slash;
            size_t first_len;
            if (!spell)
                continue;
            slash = strchr(spell, '/');
            first_len = slash ? (size_t) (slash - spell) : strlen(spell);

            /* The same spelling in an earlier search directory. */
            for (i = 0; i < j && !ret; i++) {
                const struct dcc_search_dir *d = &sl->d[i];
                char fwname[MAXPATHLEN], *cand = NULL;
                if (d->framework && !slash)
                    continue;
                if (!loaded[i]) {
                    char *opened = resolve(fn, ctx, d->spelled);
                    if (!opened) {
                        ret = EXIT_OUT_OF_MEMORY;
                        break;
                    }
                    ret = load_names(opened, &search_names[i]);
                    free(opened);
                    loaded[i] = 1;
                    if (ret)
                        break;
                }
                if (d->framework) {
                    snprintf(fwname, sizeof fwname, "%.*s.framework",
                             (int) first_len, spell);
                    if (!dcc_strset_has(&search_names[i], fwname))
                        continue;
                    if (asprintf(&cand, "%s/%s/Headers/%s", d->spelled,
                                 fwname, slash + 1) < 0)
                        ret = EXIT_OUT_OF_MEMORY;
                    else {
                        ret = consider(cand, fn, ctx, &readset, out);
                        free(cand);
                    }
                    if (!ret && asprintf(&cand, "%s/%s/PrivateHeaders/%s",
                                         d->spelled, fwname, slash + 1) >= 0) {
                        ret = consider(cand, fn, ctx, &readset, out);
                        free(cand);
                    }
                } else {
                    char first[MAXPATHLEN];
                    snprintf(first, sizeof first, "%.*s", (int) first_len,
                             spell);
                    if (!dcc_strset_has(&search_names[i], first))
                        continue;
                    if (asprintf(&cand, "%s/%s", d->spelled, spell) < 0)
                        ret = EXIT_OUT_OF_MEMORY;
                    else {
                        ret = consider(cand, fn, ctx, &readset, out);
                        free(cand);
                    }
                }
            }

            /* "a/b.h" in the directory of an includer.  (A spelling
             * without a directory part is covered by comparing the
             * includers' directory entries.) */
            if (slash && !sl->d[j].framework && !ret) {
                size_t slot;
                if (index_slot(&idx, spell, first_len, 0, &slot) == 0) {
                    int m;
                    for (m = 0; m < idx.n_dirs[slot] && !ret; m++) {
                        char *cand;
                        if (asprintf(&cand, "%s/%s", idx.dirs[slot][m],
                                     spell) < 0) {
                            ret = EXIT_OUT_OF_MEMORY;
                            break;
                        }
                        ret = consider(cand, fn, ctx, &readset, out);
                        free(cand);
                    }
                }
            }
            free(spell);
        }
    }

  out:
    for (f = 0; norms && f < n_files; f++)
        free(norms[f]);
    free(norms);
    for (i = 0; search_names && i < sl->n; i++)
        dcc_strset_free(&search_names[i]);
    free(search_names);
    free(loaded);
    free(input_norm);
    index_free(&idx);
    dcc_strset_free(&readset);
    dcc_strset_free(&includers);
    return ret;
}
