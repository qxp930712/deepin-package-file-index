#ifndef PKGFILE_INDEX_H
#define PKGFILE_INDEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PkgFileIndex PkgFileIndex;

typedef enum {
    PKGFILE_OK = 0,
    PKGFILE_ERR_NOT_FOUND,
    PKGFILE_ERR_CORRUPTED,
    PKGFILE_ERR_VERSION,
    PKGFILE_ERR_NO_MATCH,
    PKGFILE_ERR_IO,
    PKGFILE_ERR_NOMEM,
} PkgFileErrorCode;

typedef struct {
    const char *pkg_name;
    const char *version;
    const char *arch;
} PkgFilePkgInfo;

PkgFileIndex *pkgfile_index_open(const char *index_path, PkgFileErrorCode *err);
void pkgfile_index_close(PkgFileIndex *idx);

PkgFileErrorCode pkgfile_index_query_by_file(
    PkgFileIndex *idx, const char *file_path, PkgFilePkgInfo *out_info);

PkgFileErrorCode pkgfile_index_query_by_files(
    PkgFileIndex *idx,
    const char **paths, PkgFilePkgInfo *out_infos,
    size_t count);

PkgFileErrorCode pkgfile_index_query_by_desktop(
    PkgFileIndex *idx, const char *desktop_path,
    PkgFilePkgInfo *out_info, const char **out_app_name);

PkgFileErrorCode pkgfile_index_query_pkg_files(
    PkgFileIndex *idx, const char *pkg_name,
    char ***out_files, size_t *out_count);

PkgFileErrorCode pkgfile_index_query_by_prefix(
    PkgFileIndex *idx, const char *prefix,
    char ***out_paths, PkgFilePkgInfo **out_infos,
    size_t *out_count);

const char *pkgfile_index_get_build_time(PkgFileIndex *idx);
uint32_t    pkgfile_index_get_package_count(PkgFileIndex *idx);
uint32_t    pkgfile_index_get_file_count(PkgFileIndex *idx);
const char *pkgfile_index_last_error(PkgFileIndex *idx);

#ifdef __cplusplus
}
#endif

#endif /* PKGFILE_INDEX_H */
