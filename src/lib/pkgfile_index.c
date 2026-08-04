#include "pkgfile_index.h"
#include "pkgfile_index_priv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------- String pool ---------- */

static void pool_reset(StringPool *p) { p->len = 0; }

static void pool_free(StringPool *p)
{
    free(p->data);
    p->data = NULL;
    p->len = 0;
    p->cap = 0;
}

static const char *pool_strdup(StringPool *p, const char *src)
{
    if (!src) return NULL;
    size_t slen = strlen(src) + 1;
    if (p->len + slen > p->cap) {
        size_t newcap = p->cap ? p->cap : 4096;
        while (newcap < p->len + slen) newcap *= 2;
        char *tmp = realloc(p->data, newcap);
        if (!tmp) return NULL;
        p->data = tmp;
        p->cap = newcap;
    }
    char *dst = p->data + p->len;
    memcpy(dst, src, slen);
    p->len += slen;
    return dst;
}

/* ---------- helpers ---------- */

static void set_err(PkgFileIndex *idx, const char *fmt, ...)
{
    if (!idx) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(idx->err_msg, sizeof(idx->err_msg), fmt, ap);
    va_end(ap);
}

static const char *default_path(void)
{
    const char *env = getenv("PKGFILE_INDEX_PATH");
    return env ? env : PKGFILE_DEFAULT_PATH;
}

static int prepare_stmt(PkgFileIndex *idx, const char *sql, sqlite3_stmt **out)
{
    int rc = sqlite3_prepare_v2(idx->db, sql, -1, out, NULL);
    if (rc != SQLITE_OK)
        set_err(idx, "prepare failed: %s", sqlite3_errmsg(idx->db));
    return rc == SQLITE_OK ? 0 : -1;
}

static void finalize_stmts(PkgFileIndex *idx)
{
#define F(S) do { if (S) { sqlite3_finalize(S); S = NULL; } } while(0)
    F(idx->stmt_query_file);
    F(idx->stmt_query_desktop);
    F(idx->stmt_query_pkg_id);
    F(idx->stmt_query_pkg_files);
    F(idx->stmt_query_prefix);
    F(idx->stmt_meta);
#undef F
}

/* ---------- public API ---------- */

PkgFileIndex *pkgfile_index_open(const char *index_path, PkgFileErrorCode *err)
{
    const char *path = index_path ? index_path : default_path();

    PkgFileIndex *idx = calloc(1, sizeof(PkgFileIndex));
    if (!idx) {
        if (err) *err = PKGFILE_ERR_NOMEM;
        return NULL;
    }

    /* Serialized mode — safe to call from different threads,
     * but result pointers are only valid until the next call. */
    int rc = sqlite3_open_v2(path, &idx->db,
                             SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (rc == SQLITE_CANTOPEN) {
            set_err(idx, "cannot open: %s", path);
            if (err) *err = PKGFILE_ERR_NOT_FOUND;
        } else {
            set_err(idx, "open error: %s", sqlite3_errmsg(idx->db));
            if (err) *err = PKGFILE_ERR_CORRUPTED;
        }
        sqlite3_close(idx->db);
        free(idx);
        return NULL;
    }

    /* Enable mmap for read performance */
    sqlite3_exec(idx->db, "PRAGMA mmap_size = 268435456;", NULL, NULL, NULL);

    if (prepare_stmt(idx,
        "SELECT p.name, p.version, p.arch "
        "FROM file_package fp JOIN packages p ON fp.pkg_id = p.pkg_id "
        "WHERE fp.file_path = ?1",
        &idx->stmt_query_file) < 0) {
        if (err) *err = PKGFILE_ERR_CORRUPTED;
        goto fail;
    }

    if (prepare_stmt(idx,
        "SELECT p.name, p.version, p.arch, dp.app_name "
        "FROM desktop_package dp JOIN packages p ON dp.pkg_id = p.pkg_id "
        "WHERE dp.desktop_path = ?1",
        &idx->stmt_query_desktop) < 0) {
        if (err) *err = PKGFILE_ERR_CORRUPTED;
        goto fail;
    }

    if (prepare_stmt(idx,
        "SELECT pkg_id FROM packages WHERE name = ?1",
        &idx->stmt_query_pkg_id) < 0) {
        if (err) *err = PKGFILE_ERR_CORRUPTED;
        goto fail;
    }

    if (prepare_stmt(idx,
        "SELECT file_path FROM package_files WHERE pkg_id = ?1",
        &idx->stmt_query_pkg_files) < 0) {
        if (err) *err = PKGFILE_ERR_CORRUPTED;
        goto fail;
    }

    if (prepare_stmt(idx,
        "SELECT fp.file_path, p.name, p.version, p.arch "
        "FROM file_package fp JOIN packages p ON fp.pkg_id = p.pkg_id "
        "WHERE fp.file_path >= ?1 AND fp.file_path < ?2",
        &idx->stmt_query_prefix) < 0) {
        if (err) *err = PKGFILE_ERR_CORRUPTED;
        goto fail;
    }

    if (prepare_stmt(idx,
        "SELECT value FROM meta WHERE key = ?1",
        &idx->stmt_meta) < 0) {
        if (err) *err = PKGFILE_ERR_CORRUPTED;
        goto fail;
    }

    if (err) *err = PKGFILE_OK;
    return idx;

fail:
    finalize_stmts(idx);
    sqlite3_close(idx->db);
    free(idx);
    return NULL;
}

void pkgfile_index_close(PkgFileIndex *idx)
{
    if (!idx) return;
    finalize_stmts(idx);
    pool_free(&idx->result_pool);
    sqlite3_close(idx->db);
    free(idx);
}

PkgFileErrorCode pkgfile_index_query_by_file(
    PkgFileIndex *idx, const char *file_path, PkgFilePkgInfo *out_info)
{
    if (!idx || !file_path || !out_info) return PKGFILE_ERR_NO_MATCH;

    pool_reset(&idx->result_pool);

    sqlite3_reset(idx->stmt_query_file);
    sqlite3_bind_text(idx->stmt_query_file, 1, file_path, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(idx->stmt_query_file) == SQLITE_ROW) {
        out_info->pkg_name = pool_strdup(&idx->result_pool,
            (const char *)sqlite3_column_text(idx->stmt_query_file, 0));
        out_info->version   = pool_strdup(&idx->result_pool,
            (const char *)sqlite3_column_text(idx->stmt_query_file, 1));
        out_info->arch      = pool_strdup(&idx->result_pool,
            (const char *)sqlite3_column_text(idx->stmt_query_file, 2));
        return PKGFILE_OK;
    }
    return PKGFILE_ERR_NO_MATCH;
}

PkgFileErrorCode pkgfile_index_query_by_files(
    PkgFileIndex *idx,
    const char **paths, PkgFilePkgInfo *out_infos,
    size_t count)
{
    if (!idx || !paths || !out_infos || count == 0) return PKGFILE_ERR_NO_MATCH;

    pool_reset(&idx->result_pool);

    for (size_t i = 0; i < count; i++) {
        out_infos[i].pkg_name = NULL;
        out_infos[i].version   = NULL;
        out_infos[i].arch      = NULL;
        if (!paths[i]) continue;

        sqlite3_reset(idx->stmt_query_file);
        sqlite3_bind_text(idx->stmt_query_file, 1, paths[i], -1, SQLITE_TRANSIENT);

        if (sqlite3_step(idx->stmt_query_file) == SQLITE_ROW) {
            out_infos[i].pkg_name = pool_strdup(&idx->result_pool,
                (const char *)sqlite3_column_text(idx->stmt_query_file, 0));
            out_infos[i].version   = pool_strdup(&idx->result_pool,
                (const char *)sqlite3_column_text(idx->stmt_query_file, 1));
            out_infos[i].arch      = pool_strdup(&idx->result_pool,
                (const char *)sqlite3_column_text(idx->stmt_query_file, 2));
        }
    }
    return PKGFILE_OK;
}

PkgFileErrorCode pkgfile_index_query_by_desktop(
    PkgFileIndex *idx, const char *desktop_path,
    PkgFilePkgInfo *out_info, const char **out_app_name)
{
    if (!idx || !desktop_path || !out_info) return PKGFILE_ERR_NO_MATCH;

    pool_reset(&idx->result_pool);

    sqlite3_reset(idx->stmt_query_desktop);
    sqlite3_bind_text(idx->stmt_query_desktop, 1, desktop_path, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(idx->stmt_query_desktop) == SQLITE_ROW) {
        out_info->pkg_name = pool_strdup(&idx->result_pool,
            (const char *)sqlite3_column_text(idx->stmt_query_desktop, 0));
        out_info->version   = pool_strdup(&idx->result_pool,
            (const char *)sqlite3_column_text(idx->stmt_query_desktop, 1));
        out_info->arch      = pool_strdup(&idx->result_pool,
            (const char *)sqlite3_column_text(idx->stmt_query_desktop, 2));
        if (out_app_name)
            *out_app_name = pool_strdup(&idx->result_pool,
                (const char *)sqlite3_column_text(idx->stmt_query_desktop, 3));
        return PKGFILE_OK;
    }
    return PKGFILE_ERR_NO_MATCH;
}

PkgFileErrorCode pkgfile_index_query_pkg_files(
    PkgFileIndex *idx, const char *pkg_name,
    char ***out_files, size_t *out_count)
{
    if (!idx || !pkg_name || !out_files || !out_count) return PKGFILE_ERR_NO_MATCH;
    *out_files = NULL;
    *out_count = 0;

    sqlite3_reset(idx->stmt_query_pkg_id);
    sqlite3_bind_text(idx->stmt_query_pkg_id, 1, pkg_name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(idx->stmt_query_pkg_id) != SQLITE_ROW)
        return PKGFILE_ERR_NO_MATCH;

    int64_t pkg_id = sqlite3_column_int64(idx->stmt_query_pkg_id, 0);

    sqlite3_reset(idx->stmt_query_pkg_files);
    sqlite3_bind_int64(idx->stmt_query_pkg_files, 1, pkg_id);

    size_t cap = 256;
    char **files = malloc(cap * sizeof(char *));
    if (!files) return PKGFILE_ERR_NOMEM;
    size_t n = 0;

    while (sqlite3_step(idx->stmt_query_pkg_files) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            char **tmp = realloc(files, cap * sizeof(char *));
            if (!tmp) {
                for (size_t j = 0; j < n; j++) free(files[j]);
                free(files);
                return PKGFILE_ERR_NOMEM;
            }
            files = tmp;
        }
        const char *fp = (const char *)sqlite3_column_text(idx->stmt_query_pkg_files, 0);
        files[n] = strdup(fp ? fp : "");
        if (!files[n]) {
            for (size_t j = 0; j < n; j++) free(files[j]);
            free(files);
            return PKGFILE_ERR_NOMEM;
        }
        n++;
    }

    *out_files = files;
    *out_count = n;
    return PKGFILE_OK;
}

PkgFileErrorCode pkgfile_index_query_by_prefix(
    PkgFileIndex *idx, const char *prefix,
    char ***out_paths, PkgFilePkgInfo **out_infos,
    size_t *out_count)
{
    if (!idx || !prefix || !out_paths || !out_infos || !out_count)
        return PKGFILE_ERR_NO_MATCH;
    *out_paths = NULL;
    *out_infos = NULL;
    *out_count = 0;

    size_t plen = strlen(prefix);
    if (plen == 0) return PKGFILE_ERR_NO_MATCH;

    char *upper = malloc(plen + 2);
    if (!upper) return PKGFILE_ERR_NOMEM;
    memcpy(upper, prefix, plen);
    unsigned char last = (unsigned char)upper[plen - 1];
    if (last < 0xFF) {
        upper[plen - 1] = (char)(last + 1);
        upper[plen] = '\0';
    } else {
        upper[plen]     = '\x01';
        upper[plen + 1] = '\0';
    }

    sqlite3_reset(idx->stmt_query_prefix);
    sqlite3_bind_text(idx->stmt_query_prefix, 1, prefix, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(idx->stmt_query_prefix, 2, upper, -1, SQLITE_TRANSIENT);

    size_t cap = 256;
    char **paths = malloc(cap * sizeof(char *));
    PkgFilePkgInfo *infos = malloc(cap * sizeof(PkgFilePkgInfo));
    if (!paths || !infos) {
        free(paths);
        free(infos);
        free(upper);
        return PKGFILE_ERR_NOMEM;
    }
    size_t n = 0;

    while (sqlite3_step(idx->stmt_query_prefix) == SQLITE_ROW) {
        if (n >= cap) {
            size_t newcap = cap * 2;
            char **np = realloc(paths, newcap * sizeof(char *));
            if (!np) goto prefix_oom;
            paths = np;

            PkgFilePkgInfo *ni = realloc(infos, newcap * sizeof(PkgFilePkgInfo));
            if (!ni) goto prefix_oom;
            infos = ni;
            cap = newcap;
        }

        const char *fp = (const char *)sqlite3_column_text(idx->stmt_query_prefix, 0);
        paths[n]         = strdup(fp ? fp : "");
        infos[n].pkg_name = strdup((const char *)sqlite3_column_text(idx->stmt_query_prefix, 1));
        infos[n].version   = strdup((const char *)sqlite3_column_text(idx->stmt_query_prefix, 2));
        infos[n].arch      = strdup((const char *)sqlite3_column_text(idx->stmt_query_prefix, 3));

        if (!paths[n] || !infos[n].pkg_name || !infos[n].version || !infos[n].arch) {
            free(paths[n]);
            free((void *)infos[n].pkg_name);
            free((void *)infos[n].version);
            free((void *)infos[n].arch);
            goto prefix_oom;
        }
        n++;
    }

    free(upper);
    *out_paths = paths;
    *out_infos = infos;
    *out_count = n;
    return n > 0 ? PKGFILE_OK : PKGFILE_ERR_NO_MATCH;

prefix_oom:
    for (size_t j = 0; j < n; j++) {
        free(paths[j]);
        free((void *)infos[j].pkg_name);
        free((void *)infos[j].version);
        free((void *)infos[j].arch);
    }
    free(paths);
    free(infos);
    free(upper);
    return PKGFILE_ERR_NOMEM;
}

const char *pkgfile_index_get_build_time(PkgFileIndex *idx)
{
    if (!idx) return NULL;
    pool_reset(&idx->result_pool);

    sqlite3_reset(idx->stmt_meta);
    sqlite3_bind_text(idx->stmt_meta, 1, "build_time", -1, SQLITE_STATIC);
    if (sqlite3_step(idx->stmt_meta) == SQLITE_ROW)
        return pool_strdup(&idx->result_pool,
            (const char *)sqlite3_column_text(idx->stmt_meta, 0));
    return NULL;
}

uint32_t pkgfile_index_get_package_count(PkgFileIndex *idx)
{
    if (!idx) return 0;
    sqlite3_reset(idx->stmt_meta);
    sqlite3_bind_text(idx->stmt_meta, 1, "package_count", -1, SQLITE_STATIC);
    uint32_t count = 0;
    if (sqlite3_step(idx->stmt_meta) == SQLITE_ROW)
        count = (uint32_t)sqlite3_column_int(idx->stmt_meta, 0);
    return count;
}


uint32_t pkgfile_index_get_file_count(PkgFileIndex *idx)
{
    if (!idx) return 0;
    sqlite3_reset(idx->stmt_meta);
    sqlite3_bind_text(idx->stmt_meta, 1, "file_count", -1, SQLITE_STATIC);
    uint32_t count = 0;
    if (sqlite3_step(idx->stmt_meta) == SQLITE_ROW)
        count = (uint32_t)sqlite3_column_int(idx->stmt_meta, 0);
    return count;
}
const char *pkgfile_index_last_error(PkgFileIndex *idx)
{
    if (!idx) return "null index";
    return idx->err_msg;
}
