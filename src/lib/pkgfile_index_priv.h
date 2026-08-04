#ifndef PKGFILE_INDEX_PRIV_H
#define PKGFILE_INDEX_PRIV_H

#include <sqlite3.h>
#include <stddef.h>

#define PKGFILE_INDEX_VERSION 1
#define PKGFILE_DEFAULT_PATH "/var/cache/deepin/package-file-index/installed.idx"

/* Grow-only string pool for query result pointers.
 * Pointers are valid until the next pool_reset or pool_free. */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} StringPool;

struct PkgFileIndex {
    sqlite3     *db;
    char         err_msg[256];
    sqlite3_stmt *stmt_query_file;
    sqlite3_stmt *stmt_query_desktop;
    sqlite3_stmt *stmt_query_pkg_id;
    sqlite3_stmt *stmt_query_pkg_files;
    sqlite3_stmt *stmt_query_prefix;
    sqlite3_stmt *stmt_meta;
    StringPool   result_pool;
};

#endif
