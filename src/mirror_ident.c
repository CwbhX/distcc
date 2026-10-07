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

/* Mirror mode: directory identity, the set of things a job must describe,
 * and compiler identity.
 *
 * Comparing the files a compile read is not enough: if a header exists on
 * the client in a directory searched earlier than where the helper found
 * it, the client's own compile would have read a different file.  So the
 * directories that take part in the include search are compared too.
 *
 * A directory's identity is the SHA-256 of its sorted entries ("<kind>
 * <name>"), where the kind of a symlink is that of its target.  Entries the
 * sync deliberately leaves out are ignored on both sides: the top-level
 * excludes of each synced root, and in build trees every file that is not
 * a source, header or PCH.
 *
 * Synced directories must be identical.  Installed directories (CLT,
 * Homebrew) may differ, because the helper need not have every package
 * the client has, as long as no differing entry is named like a component
 * of any path the compile read: an include spelling is always a suffix of
 * the path it resolved to, so a header that would shadow it has a first
 * component among those names. */

#include <config.h>

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "mirror.h"

/* ----- string sets ----- */

static unsigned long long strhash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 1099511628211ULL;
    }
    return h;
}

int dcc_strset_add(struct dcc_strset *s, const char *str)
{
    size_t i;

    if ((s->used + 1) * 2 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 1024, j;
        char **n = calloc(ncap, sizeof *n);
        if (!n)
            return EXIT_OUT_OF_MEMORY;
        for (j = 0; j < s->cap; j++) {
            if (s->items[j]) {
                i = strhash(s->items[j]) & (ncap - 1);
                while (n[i])
                    i = (i + 1) & (ncap - 1);
                n[i] = s->items[j];
            }
        }
        free(s->items);
        s->items = n;
        s->cap = ncap;
    }
    i = strhash(str) & (s->cap - 1);
    while (s->items[i]) {
        if (strcmp(s->items[i], str) == 0)
            return 0;
        i = (i + 1) & (s->cap - 1);
    }
    if (!(s->items[i] = strdup(str)))
        return EXIT_OUT_OF_MEMORY;
    s->used++;
    return 0;
}

int dcc_strset_has(const struct dcc_strset *s, const char *str)
{
    size_t i;

    if (!s->cap)
        return 0;
    i = strhash(str) & (s->cap - 1);
    while (s->items[i]) {
        if (strcmp(s->items[i], str) == 0)
            return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

void dcc_strset_free(struct dcc_strset *s)
{
    size_t i;

    for (i = 0; i < s->cap; i++)
        free(s->items[i]);
    free(s->items);
    memset(s, 0, sizeof *s);
}


/* ----- rules ----- */

static int split_colon(const char *list, char ***out, int *n)
{
    char *copy, *p, *tok;

    *out = NULL;
    *n = 0;
    if (!list || !*list)
        return 0;
    if (!(copy = strdup(list)))
        return EXIT_OUT_OF_MEMORY;
    for (p = copy; (tok = strsep(&p, ":")) != NULL; ) {
        char **nv;
        size_t len = strlen(tok);
        while (len > 1 && tok[len - 1] == '/')
            tok[--len] = '\0';
        if (!len)
            continue;
        if (!(nv = realloc(*out, (*n + 1) * sizeof *nv))
            || !(nv[*n] = strdup(tok))) {
            free(copy);
            return EXIT_OUT_OF_MEMORY;
        }
        *out = nv;
        (*n)++;
    }
    free(copy);
    return 0;
}

static int add_str(char ***v, int *n, const char *s)
{
    char **nv = realloc(*v, (*n + 1) * sizeof *nv);
    if (!nv || !(nv[*n] = strdup(s)))
        return EXIT_OUT_OF_MEMORY;
    *v = nv;
    (*n)++;
    return 0;
}

/**
 * Client side: rules from DISTCC_MIRROR_ROOTS, DISTCC_MIRROR_EXCLUDE and
 * the logical sides of DISTCC_MIRROR_PATHMAP (the build trees).
 **/
int dcc_mirror_rules_from_env(struct dcc_mirror_rules *r, const char *cwd)
{
    const char *map = getenv("DISTCC_MIRROR_PATHMAP");
    int ret;

    memset(r, 0, sizeof *r);
    if ((ret = split_colon(getenv("DISTCC_MIRROR_ROOTS"), &r->roots,
                           &r->n_roots))
        || (ret = split_colon(getenv("DISTCC_MIRROR_EXCLUDE"), &r->excludes,
                              &r->n_excludes)))
        return ret;
    if (map && *map) {
        char **pairs;
        int n, i;
        if ((ret = split_colon(map, &pairs, &n)))
            return ret;
        for (i = 0; i < n && !ret; i++) {
            char *eq = strchr(pairs[i], '=');
            if (eq && eq[1] == '/' && pairs[i][0] == '/') {
                int dummy = r->n_maps;
                ret = add_str(&r->build_roots, &r->n_build_roots, eq + 1);
                *eq = '\0';
                if (!ret)
                    ret = add_str(&r->map_phys, &dummy, pairs[i]);
                if (!ret)
                    ret = add_str(&r->map_logical, &r->n_maps, eq + 1);
            }
        }
        for (i = 0; i < n; i++)
            free(pairs[i]);
        free(pairs);
        if (ret)
            return ret;
    }
    if (!(r->cwd = strdup(cwd)))
        return EXIT_OUT_OF_MEMORY;
    return 0;
}

int dcc_mirror_rules_add(struct dcc_mirror_rules *r, const char *rule)
{
    if (rule[0] == 'i' && rule[1] == ':' && rule[2] == '/')
        return add_str(&r->installed, &r->n_installed, rule + 2);
    if (rule[0] == 'p' && rule[1] == ':' && rule[2] == '/') {
        /* "p:/physical=/logical" */
        char *copy = strdup(rule + 2), *eq;
        int ret, dummy = r->n_maps;
        if (!copy)
            return EXIT_OUT_OF_MEMORY;
        if (!(eq = strchr(copy, '=')) || eq[1] != '/') {
            free(copy);
            return EXIT_PROTOCOL_ERROR;
        }
        *eq = '\0';
        ret = add_str(&r->map_phys, &dummy, copy);
        if (!ret)
            ret = add_str(&r->map_logical, &r->n_maps, eq + 1);
        free(copy);
        return ret;
    }
    if (rule[0] == 'r' && rule[1] == ':' && rule[2] == '/')
        return add_str(&r->roots, &r->n_roots, rule + 2);
    if (rule[0] == 'x' && rule[1] == ':' && rule[2])
        return add_str(&r->excludes, &r->n_excludes, rule + 2);
    if (rule[0] == 'b' && rule[1] == ':' && rule[2] == '/')
        return add_str(&r->build_roots, &r->n_build_roots, rule + 2);
    rs_log_warning("mirror: unknown rule %s", rule);
    return EXIT_PROTOCOL_ERROR;
}

/* All rules as "r:/x", "x:glob", "b:/y" strings, for the request. */
int dcc_mirror_rules_list(const struct dcc_mirror_rules *r, char ***out,
                          int *n)
{
    struct { char prefix; char **v; int n; } kinds[4];
    int k, i, ret = 0;

    kinds[0].prefix = 'r'; kinds[0].v = r->roots; kinds[0].n = r->n_roots;
    kinds[1].prefix = 'x'; kinds[1].v = r->excludes; kinds[1].n = r->n_excludes;
    kinds[2].prefix = 'b'; kinds[2].v = r->build_roots;
    kinds[2].n = r->n_build_roots;
    kinds[3].prefix = 'i';
    dcc_mirror_installed_prefixes(&kinds[3].v, &kinds[3].n);
    *out = NULL;
    *n = 0;
    for (k = 0; k < 4 && !ret; k++) {
        for (i = 0; i < kinds[k].n && !ret; i++) {
            char *s;
            if (asprintf(&s, "%c:%s", kinds[k].prefix, kinds[k].v[i]) < 0)
                return EXIT_OUT_OF_MEMORY;
            ret = add_str(out, n, s);
            free(s);
        }
    }
    for (i = 0; i < r->n_maps && !ret; i++) {
        char *s;
        if (asprintf(&s, "p:%s=%s", r->map_phys[i], r->map_logical[i]) < 0)
            return EXIT_OUT_OF_MEMORY;
        ret = add_str(out, n, s);
        free(s);
    }
    return ret;
}

static void free_strs(char **v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

void dcc_mirror_rules_free(struct dcc_mirror_rules *r)
{
    free_strs(r->roots, r->n_roots);
    free_strs(r->excludes, r->n_excludes);
    free_strs(r->build_roots, r->n_build_roots);
    free_strs(r->installed, r->n_installed);
    free_strs(r->map_phys, r->n_maps);
    free_strs(r->map_logical, r->n_maps);
    free(r->cwd);
    memset(r, 0, sizeof *r);
}

/* A short fingerprint of the rules, so that cached listing hashes made
 * under other rules are not reused. */
static void rules_key(const struct dcc_mirror_rules *r, char out[17])
{
    unsigned long long h = 1469598103934665603ULL;
    int i;

#define MIX(str) do { const char *_p = (str); \
        while (*_p) { h ^= (unsigned char) *_p++; h *= 1099511628211ULL; } \
        h ^= 0xff; h *= 1099511628211ULL; } while (0)
    for (i = 0; i < r->n_roots; i++) { MIX("r"); MIX(r->roots[i]); }
    for (i = 0; i < r->n_excludes; i++) { MIX("x"); MIX(r->excludes[i]); }
    for (i = 0; i < r->n_build_roots; i++) { MIX("b"); MIX(r->build_roots[i]); }
#undef MIX
    snprintf(out, 17, "%016llx", h);
}

/* @p path made absolute against the logical cwd, not normalized. */
static char *logical_abs(const struct dcc_mirror_rules *r, const char *path)
{
    char *s;
    if (path[0] == '/')
        return strdup(path);
    if (strcmp(path, ".") == 0)
        return strdup(r->cwd);
    if (asprintf(&s, "%s/%s", r->cwd, path) < 0)
        return NULL;
    return s;
}

char *dcc_mirror_normalize(const struct dcc_mirror_rules *r, const char *path);

static int is_under(const char *path, const char *root)
{
    size_t len = strlen(root);
    return strncmp(path, root, len) == 0
        && (path[len] == '/' || path[len] == '\0');
}


/* ----- directory identity ----- */

static const char *const build_suffixes[] = {
    ".h", ".hh", ".hpp", ".hxx", ".h++", ".inc", ".inl", ".ipp", ".tpp",
    ".tcc", ".def", ".c", ".cc", ".cpp", ".cxx", ".c++", ".m", ".mm",
    ".pch", ".gch", NULL
};

/**
 * Is @p name a file the sync copies out of a build tree?  The same test
 * decides which build-tree files count in a directory's identity.
 **/
int dcc_mirror_is_build_file(const char *name)
{
    size_t len = strlen(name);
    int i;

    if (strncmp(name, "cmake_pch.", 10) == 0)
        return 1;
    for (i = 0; build_suffixes[i]; i++) {
        size_t sl = strlen(build_suffixes[i]);
        if (len > sl && strcmp(name + len - sl, build_suffixes[i]) == 0)
            return 1;
    }
    return 0;
}

static int cmp_strp(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

void dcc_mirror_free_names(char **names, int n)
{
    free_strs(names, n);
}

/**
 * Read a directory's counted entries as sorted "<kind><name>" strings,
 * kind being 'f' (file), 'd' (directory), 'o' (other) or 'x' (dangling
 * symlink).
 **/
static int read_names(const char *path, const char *abs,
                      const struct dcc_mirror_rules *r,
                      char ***names_ret, int *n_ret)
{
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    int n = 0, cap = 0, i, is_root = 0, in_build = 0;

    for (i = 0; i < r->n_roots; i++)
        if (strcmp(abs, r->roots[i]) == 0)
            is_root = 1;
    for (i = 0; i < r->n_build_roots; i++)
        if (is_under(abs, r->build_roots[i]))
            in_build = 1;

    if (!(d = opendir(path)))
        return EXIT_IO_ERROR;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        char kind;
        char *e;
        int skip = 0;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0
            || strcmp(name, ".DS_Store") == 0)
            continue;
        if (is_root)
            for (i = 0; i < r->n_excludes && !skip; i++)
                if (fnmatch(r->excludes[i], name, 0) == 0)
                    skip = 1;
        if (skip)
            continue;

        if (de->d_type == DT_DIR)
            kind = 'd';
        else if (de->d_type == DT_REG)
            kind = 'f';
        else {
            struct stat st;
            char *full;
            if (asprintf(&full, "%s/%s", path, name) < 0) {
                closedir(d);
                free_strs(names, n);
                return EXIT_OUT_OF_MEMORY;
            }
            if (stat(full, &st) == -1)
                kind = 'x';
            else if (S_ISDIR(st.st_mode))
                kind = 'd';
            else if (S_ISREG(st.st_mode))
                kind = 'f';
            else
                kind = 'o';
            free(full);
        }
        if (in_build && kind != 'd' && !dcc_mirror_is_build_file(name))
            continue;

        if (n == cap) {
            int ncap = cap ? cap * 2 : 64;
            char **nn = realloc(names, ncap * sizeof *nn);
            if (!nn) {
                closedir(d);
                free_strs(names, n);
                return EXIT_OUT_OF_MEMORY;
            }
            names = nn;
            cap = ncap;
        }
        if (asprintf(&e, "%c%s", kind, name) < 0) {
            closedir(d);
            free_strs(names, n);
            return EXIT_OUT_OF_MEMORY;
        }
        names[n++] = e;
    }
    closedir(d);
    if (n)
        qsort(names, (size_t) n, sizeof *names, cmp_strp);
    *names_ret = names;
    *n_ret = n;
    return 0;
}

static void hash_names(char **names, int n, char hex[DCC_SHA256_HEX_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    struct dcc_sha256 ctx;
    uint8_t dg[DCC_SHA256_LEN];
    int i;

    dcc_sha256_init(&ctx);
    for (i = 0; i < n; i++)
        dcc_sha256_update(&ctx, names[i], strlen(names[i]) + 1);
    dcc_sha256_final(&ctx, dg);
    for (i = 0; i < DCC_SHA256_LEN; i++) {
        hex[2*i] = digits[dg[i] >> 4];
        hex[2*i+1] = digits[dg[i] & 15];
    }
    hex[DCC_SHA256_HEX_LEN] = '\0';
}

/**
 * Describe directory @p path: absent, not a directory, or a directory with
 * the hash of its counted entries in ident->digest.  If @p names_ret is
 * given, also return the entries.
 **/
int dcc_mirror_dir_ident(const char *path, const struct dcc_mirror_rules *r,
                         struct dcc_mirror_ident *ident,
                         char ***names_ret, int *n_names)
{
    struct stat st;
    char key_prefix[17];
    char *abs = NULL, *key = NULL;
    char **names = NULL;
    int n = 0, ret;

    memset(ident, 0, sizeof *ident);
    if (names_ret) {
        *names_ret = NULL;
        *n_names = 0;
    }
    if (stat(path, &st) == -1) {
        if (errno == ENOENT || errno == ENOTDIR) {
            ident->present = DCC_MIRROR_ABSENT;
            return 0;
        }
        return EXIT_IO_ERROR;
    }
    if (!S_ISDIR(st.st_mode)) {
        ident->present = S_ISREG(st.st_mode) ? DCC_MIRROR_FILE
            : DCC_MIRROR_OTHER;
        return 0;
    }
    ident->present = DCC_MIRROR_DIR;
    /* Classify (root, build tree) and key the cache by the normalized
     * name: ccache's base_dir makes paths like ../../../../Users/x. */
    if (!(abs = dcc_mirror_normalize(r, path)))
        return EXIT_OUT_OF_MEMORY;

    rules_key(r, key_prefix);
    if (asprintf(&key, "D%s:%s", key_prefix, abs) < 0) {
        free(abs);
        return EXIT_OUT_OF_MEMORY;
    }
    if (!names_ret && dcc_mirror_cache_get(key, &st, ident->digest) == 0) {
        free(abs);
        free(key);
        return 0;
    }
    if ((ret = read_names(path, abs, r, &names, &n))) {
        free(abs);
        free(key);
        return ret;
    }
    hash_names(names, n, ident->digest);
    dcc_mirror_cache_put(key, &st, ident->digest);
    if (names_ret) {
        *names_ret = names;
        *n_names = n;
    } else {
        free_strs(names, n);
    }
    free(abs);
    free(key);
    return 0;
}


/* ----- what a job must describe ----- */

/* Search-path options and whether their operand is a directory. */
static const char *const search_options[] = {
    "-I", "-isystem", "-iquote", "-idirafter", "-F", "-iframework",
    "-iframeworkwithsysroot", "-isystem-after", "--include-directory",
    NULL
};

/**
 * The directories argv adds to the include search, in order.
 **/
int dcc_mirror_search_dirs(char **argv, char ***dirs_ret, int *n_ret)
{
    char **dirs = NULL;
    int n = 0, i, j, ret;

    for (i = 0; argv[i]; i++) {
        const char *a = argv[i];
        const char *val = NULL;
        if (strcmp(a, "-Xclang") == 0 || strcmp(a, "-Xpreprocessor") == 0) {
            i++;
            continue;
        }
        for (j = 0; search_options[j]; j++) {
            size_t len = strlen(search_options[j]);
            if (strcmp(a, search_options[j]) == 0) {
                val = argv[i + 1];
                if (val)
                    i++;
                break;
            }
            if (strncmp(a, search_options[j], len) == 0 && a[len]
                && (len == 2 || a[len] == '=')) {
                val = a + len + (a[len] == '=');
                break;
            }
        }
        if (val && *val && (ret = add_str(&dirs, &n, val))) {
            free_strs(dirs, n);
            return ret;
        }
    }
    *dirs_ret = dirs;
    *n_ret = n;
    return 0;
}

static char *dirname_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return strdup(".");
    if (slash == path)
        return strdup("/");
    return strndup(path, (size_t) (slash - path));
}

/* Add every component of @p path to @p comps, and for "X.framework" also
 * "X" (framework includes are spelled without the suffix). */
static int add_components(struct dcc_strset *comps, const char *path)
{
    char *copy = strdup(path), *p, *tok;
    int ret = 0;

    if (!copy)
        return EXIT_OUT_OF_MEMORY;
    for (p = copy; !ret && (tok = strsep(&p, "/")) != NULL; ) {
        size_t len = strlen(tok);
        if (!len)
            continue;
        ret = dcc_strset_add(comps, tok);
        if (!ret && len > 10 && strcmp(tok + len - 10, ".framework") == 0) {
            tok[len - 10] = '\0';
            ret = dcc_strset_add(comps, tok);
        }
    }
    free(copy);
    return ret;
}

/**
 * From argv and the files a job read (the .d prerequisites and the check
 * list), the files and directories the daemon must describe and the
 * component names used to judge differences in installed directories.
 **/
int dcc_mirror_required(char **argv, char **files, int n_files,
                        struct dcc_strset *file_set,
                        struct dcc_strset *dir_set,
                        struct dcc_strset *comps)
{
    char **dirs;
    int n_dirs, i, ret;

    if ((ret = dcc_strset_add(dir_set, ".")))
        return ret;
    if ((ret = dcc_mirror_search_dirs(argv, &dirs, &n_dirs)))
        return ret;
    for (i = 0; i < n_dirs && !ret; i++)
        ret = dcc_strset_add(dir_set, dirs[i]);
    free_strs(dirs, n_dirs);
    for (i = 0; i < n_files && !ret; i++) {
        char *d;
        if ((ret = dcc_strset_add(file_set, files[i])))
            break;
        if ((ret = add_components(comps, files[i])))
            break;
        if (!(d = dirname_of(files[i])))
            return EXIT_OUT_OF_MEMORY;
        ret = dcc_strset_add(dir_set, d);
        free(d);
    }
    return ret;
}


/* ----- search order ----- */

/* @p path made absolute against the logical cwd and normalized lexically
 * ("//", "/./" and "x/.." collapsed).  Returns a malloc'd string. */
char *dcc_mirror_normalize(const struct dcc_mirror_rules *r, const char *path)
{
    char *abs = logical_abs(r, path), *out, *p, *tok, *save;
    size_t len;

    if (!abs)
        return NULL;
    len = strlen(abs);
    if (!(out = malloc(len + 2))) {
        free(abs);
        return NULL;
    }
    out[0] = '\0';
    for (p = abs; (tok = strsep(&p, "/")) != NULL; ) {
        if (!*tok || strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            save = strrchr(out, '/');
            if (save)
                *save = '\0';
            continue;
        }
        strcat(out, "/");
        strcat(out, tok);
    }
    if (!out[0])
        strcpy(out, "/");
    free(abs);
    /* A physical prefix of the path map names the logical directory. */
    {
        int i;
        for (i = 0; i < r->n_maps; i++) {
            if (is_under(out, r->map_phys[i])) {
                char *m;
                if (asprintf(&m, "%s%s", r->map_logical[i],
                             out + strlen(r->map_phys[i])) < 0) {
                    free(out);
                    return NULL;
                }
                free(out);
                return m;
            }
        }
    }
    return out;
}

static int is_under_any(const char *path, char **dirs, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (is_under(path, dirs[i]))
            return 1;
    return 0;
}

/**
 * Daemon side: the file the compiler really opened for @p path.  The
 * compile runs with the overlay written by dcc_mirror_write_overlay(),
 * under which clang resolves a path inside a mirror root or a mapped
 * physical prefix lexically (".." removed, the physical prefix replaced by
 * its logical name), and any other path through the file system.
 **/
char *dcc_mirror_daemon_path(const struct dcc_mirror_rules *r,
                              char **roots, int n_roots, const char *path)
{
    char *norm = dcc_mirror_normalize(r, path);

    if (norm && (is_under_any(norm, roots, n_roots)
                 || is_under_any(norm, r->map_logical, r->n_maps)))
        return norm;
    free(norm);
    return strdup(path);
}

/* Quote @p s for the YAML overlay; refuses quotes and newlines. */
static int yaml_ok(const char *s)
{
    return !strpbrk(s, "'\"\n\\");
}

/**
 * Write the clang VFS overlay for a mirrored compile: each physical prefix
 * of the path map is remapped to its logical directory, and each mirror
 * root to itself, so that paths recorded on the client (a PCH stores its
 * inputs under the client's physical cwd, e.g.
 * /Volumes/X/build/../../src/a.h) open the helper's copy.  Names are kept
 * as spelled, so the .d is the client's own.
 **/
int dcc_mirror_write_overlay(const struct dcc_mirror_rules *r,
                             char **roots, int n_roots, const char *fname)
{
    FILE *f;
    int i, first = 1;

    for (i = 0; i < r->n_maps; i++)
        if (!yaml_ok(r->map_phys[i]) || !yaml_ok(r->map_logical[i]))
            return EXIT_PROTOCOL_ERROR;
    for (i = 0; i < n_roots; i++)
        if (!yaml_ok(roots[i]))
            return EXIT_DISTCC_FAILED;
    if (!(f = fopen(fname, "w")))
        return EXIT_IO_ERROR;
    fprintf(f, "{ 'version': 0, 'redirecting-with': 'fallthrough',\n"
            "  'use-external-names': false,\n  'roots': [\n");
    for (i = 0; i < r->n_maps; i++) {
        fprintf(f, "%s    { 'type': 'directory-remap', 'name': '%s',"
                " 'external-contents': '%s' }", first ? "" : ",\n",
                r->map_phys[i], r->map_logical[i]);
        first = 0;
    }
    for (i = 0; i < n_roots; i++) {
        fprintf(f, "%s    { 'type': 'directory-remap', 'name': '%s',"
                " 'external-contents': '%s' }", first ? "" : ",\n",
                roots[i], roots[i]);
        first = 0;
    }
    fprintf(f, "\n  ] }\n");
    return fclose(f) == 0 ? 0 : EXIT_IO_ERROR;
}

/**
 * The search directories of argv in the order the preprocessor uses them
 * for <...> includes (quoted-only -iquote directories first, since they
 * come before everything for "..." includes): -iquote, -I and -F, -isystem
 * and -iframework, then -idirafter.  Normalized.
 **/
int dcc_mirror_search_order(char **argv, const struct dcc_mirror_rules *r,
                            char ***dirs_ret, int *n_ret)
{
    static const char *const order[][4] = {
        { "-iquote", NULL },
        { "-I", "--include-directory", "-F", NULL },
        { "-isystem", "-iframework", "-iframeworkwithsysroot", NULL },
        { "-idirafter", "-isystem-after", NULL },
    };
    char **dirs = NULL;
    int n = 0, pass, i, j, ret;

    for (pass = 0; pass < 4; pass++) {
        for (i = 0; argv[i]; i++) {
            const char *a = argv[i], *val = NULL;
            if (strcmp(a, "-Xclang") == 0 || strcmp(a, "-Xpreprocessor") == 0) {
                i++;
                continue;
            }
            for (j = 0; order[pass][j]; j++) {
                const char *opt = order[pass][j];
                size_t len = strlen(opt);
                if (strcmp(a, opt) == 0) {
                    val = argv[i + 1];
                    if (val)
                        i++;
                    break;
                }
                if (strncmp(a, opt, len) == 0 && a[len]
                    && (len == 2 || a[len] == '=')) {
                    val = a + len + (a[len] == '=');
                    break;
                }
            }
            if (val && *val) {
                char *norm = dcc_mirror_normalize(r, val);
                if (!norm || (ret = add_str(&dirs, &n, norm))) {
                    free(norm);
                    free_strs(dirs, n);
                    return norm ? ret : EXIT_OUT_OF_MEMORY;
                }
                free(norm);
            }
        }
    }
    *dirs_ret = dirs;
    *n_ret = n;
    return 0;
}

/* ----- component positions: name -> latest search position ----- */

void dcc_strint_free(struct dcc_strint *m)
{
    size_t i;
    for (i = 0; i < m->cap; i++)
        free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

/* Set m[key] = max(m[key], val). */
static int strint_max(struct dcc_strint *m, const char *key, int val)
{
    size_t i;

    if ((m->used + 1) * 2 > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 1024, j;
        char **nk = calloc(ncap, sizeof *nk);
        int *nv = calloc(ncap, sizeof *nv);
        if (!nk || !nv) {
            free(nk);
            free(nv);
            return EXIT_OUT_OF_MEMORY;
        }
        for (j = 0; j < m->cap; j++) {
            if (m->keys[j]) {
                i = strhash(m->keys[j]) & (ncap - 1);
                while (nk[i])
                    i = (i + 1) & (ncap - 1);
                nk[i] = m->keys[j];
                nv[i] = m->vals[j];
            }
        }
        free(m->keys);
        free(m->vals);
        m->keys = nk;
        m->vals = nv;
        m->cap = ncap;
    }
    i = strhash(key) & (m->cap - 1);
    while (m->keys[i]) {
        if (strcmp(m->keys[i], key) == 0) {
            if (val > m->vals[i])
                m->vals[i] = val;
            return 0;
        }
        i = (i + 1) & (m->cap - 1);
    }
    if (!(m->keys[i] = strdup(key)))
        return EXIT_OUT_OF_MEMORY;
    m->vals[i] = val;
    m->used++;
    return 0;
}

/* m[key], or -1. */
int dcc_strint_get(const struct dcc_strint *m, const char *key)
{
    size_t i;

    if (!m->cap)
        return -1;
    i = strhash(key) & (m->cap - 1);
    while (m->keys[i]) {
        if (strcmp(m->keys[i], key) == 0)
            return m->vals[i];
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

static int add_component_positions(struct dcc_strint *m, const char *path,
                                   int pos)
{
    char *copy = strdup(path), *p, *tok;
    int ret = 0;

    if (!copy)
        return EXIT_OUT_OF_MEMORY;
    for (p = copy; !ret && (tok = strsep(&p, "/")) != NULL; ) {
        size_t len = strlen(tok);
        if (!len)
            continue;
        ret = strint_max(m, tok, pos);
        if (!ret && len > 10 && strcmp(tok + len - 10, ".framework") == 0) {
            tok[len - 10] = '\0';
            ret = strint_max(m, tok, pos);
        }
    }
    free(copy);
    return ret;
}

/**
 * For every path component of the files a job read, the latest position in
 * the search order (@p dirs, normalized) through which a file containing it
 * could have been found: the last search directory that is a prefix of the
 * file, or @p n_dirs if none is (system directories come after all of
 * them).  A directory at position d can only shadow a file found later, so
 * a name that differs there matters only if its position is greater than d.
 * Both the path as written and its normalized form count.
 **/
int dcc_mirror_component_positions(char **files, int n_files,
                                   char **dirs, int n_dirs,
                                   const struct dcc_mirror_rules *r,
                                   struct dcc_strint *m)
{
    int f, k, ret = 0;

    for (f = 0; f < n_files && !ret; f++) {
        char *norm = dcc_mirror_normalize(r, files[f]);
        int pos = n_dirs;
        if (!norm)
            return EXIT_OUT_OF_MEMORY;
        for (k = n_dirs - 1; k >= 0; k--) {
            if (strcmp(dirs[k], "/") == 0 || is_under(norm, dirs[k])) {
                pos = k;
                break;
            }
        }
        ret = add_component_positions(m, files[f], pos);
        if (!ret)
            ret = add_component_positions(m, norm, pos);
        free(norm);
    }
    return ret;
}


/* ----- compiler identity ----- */

/* Find @p name on PATH. */
static char *path_lookup(const char *name)
{
    const char *path = getenv("PATH");
    char *copy, *p, *dir, *full = NULL;

    if (strchr(name, '/'))
        return strdup(name);
    if (!path || !(copy = strdup(path)))
        return NULL;
    for (p = copy; (dir = strsep(&p, ":")) != NULL; ) {
        if (asprintf(&full, "%s/%s", *dir ? dir : ".", name) < 0) {
            full = NULL;
            break;
        }
        if (access(full, X_OK) == 0)
            break;
        free(full);
        full = NULL;
    }
    free(copy);
    return full;
}

/**
 * The compiler binary that @p argv0 really runs.  On macOS the tools in
 * /usr/bin are stubs that run the active developer directory's copy
 * (DEVELOPER_DIR, else the xcode-select link), so resolve through that.
 **/
static char *resolve_compiler(const char *argv0)
{
    char *found = path_lookup(argv0);
    char real[MAXPATHLEN + 1];

#ifdef __APPLE__
    if (found && strncmp(found, "/usr/bin/", 9) == 0) {
        const char *base = found + 9;
        const char *dev = getenv("DEVELOPER_DIR");
        char link[MAXPATHLEN + 1];
        const char *subdirs[] = {
            "/usr/bin/", "/Toolchains/XcodeDefault.xctoolchain/usr/bin/", NULL
        };
        int i;
        if (!dev || !*dev) {
            ssize_t len = readlink("/var/db/xcode_select_link", link,
                                   sizeof link - 1);
            if (len > 0) {
                link[len] = '\0';
                dev = link;
            } else {
                dev = "/Library/Developer/CommandLineTools";
            }
        }
        for (i = 0; subdirs[i]; i++) {
            char *cand;
            if (asprintf(&cand, "%s%s%s", dev, subdirs[i], base) < 0)
                break;
            if (access(cand, X_OK) == 0 && realpath(cand, real)) {
                free(cand);
                free(found);
                return strdup(real);
            }
            free(cand);
        }
    }
#endif
    if (found && realpath(found, real)) {
        free(found);
        return strdup(real);
    }
    return found;
}

/**
 * SHA-256 of the compiler binary @p argv0 runs, via the digest cache.
 **/
int dcc_mirror_compiler_ident(const char *argv0,
                              char hex[DCC_SHA256_HEX_LEN + 1])
{
    char *bin = resolve_compiler(argv0);
    struct stat st;
    int ret;

    hex[0] = '\0';
    if (!bin)
        return EXIT_COMPILER_MISSING;
    if (stat(bin, &st) == -1) {
        free(bin);
        return EXIT_COMPILER_MISSING;
    }
    ret = dcc_mirror_digest(bin, &st, hex);
    free(bin);
    return ret;
}


/* ----- environment that changes what the compiler reads ----- */

const char *const dcc_mirror_env_names[] = {
    "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
    "OBJCPLUS_INCLUDE_PATH", "SDKROOT", "DEVELOPER_DIR",
    "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET",
    "CCC_OVERRIDE_OPTIONS", "COMPILER_PATH", "GCC_EXEC_PREFIX",
    "SOURCE_DATE_EPOCH", "ZERO_AR_DATE", NULL
};
