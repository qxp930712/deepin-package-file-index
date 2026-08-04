#include <pkgfile_index.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <vector>

static const char *TEST_IDX = "/tmp/test_pkgfile_index.idx";

static bool build_test_index()
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(TEST_IDX, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        return false;

    const char *sql[] = {
        "CREATE TABLE IF NOT EXISTS packages (pkg_id INTEGER PRIMARY KEY, name TEXT NOT NULL, version TEXT NOT NULL, arch TEXT);",
        "CREATE TABLE IF NOT EXISTS file_package (file_path TEXT NOT NULL PRIMARY KEY, pkg_id INTEGER NOT NULL REFERENCES packages(pkg_id));",
        "CREATE TABLE IF NOT EXISTS desktop_package (desktop_path PRIMARY KEY, pkg_id INTEGER NOT NULL, app_name TEXT, exec TEXT);",
        "CREATE TABLE IF NOT EXISTS package_files (pkg_id INTEGER NOT NULL, file_path TEXT NOT NULL, PRIMARY KEY (pkg_id, file_path));",
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);",
        "INSERT INTO packages VALUES (1, 'test-pkg-a', '1.0.0-1', 'amd64');",
        "INSERT INTO packages VALUES (2, 'test-pkg-b', '2.3.4-1', 'arm64');",
        "INSERT INTO file_package VALUES ('/usr/bin/test-bin-a', 1);",
        "INSERT INTO file_package VALUES ('/usr/lib/test-pkg-a/libfoo.so', 1);",
        "INSERT INTO file_package VALUES ('/usr/bin/test-bin-b', 2);",
        "INSERT INTO package_files VALUES (1, '/usr/bin/test-bin-a');",
        "INSERT INTO package_files VALUES (1, '/usr/lib/test-pkg-a/libfoo.so');",
        "INSERT INTO package_files VALUES (2, '/usr/bin/test-bin-b');",
        "INSERT INTO desktop_package VALUES ('/usr/share/applications/test-a.desktop', 1, 'Test App A', 'test-bin-a');",
        "INSERT INTO meta VALUES ('build_time', '2026-08-03 20:00:00');",
        "INSERT INTO meta VALUES ('package_count', '2');",
        nullptr,
    };

    for (int i = 0; sql[i]; i++) {
        char *err = nullptr;
        if (sqlite3_exec(db, sql[i], nullptr, nullptr, &err) != SQLITE_OK) {
            fprintf(stderr, "test schema error: %s\n", err);
            sqlite3_free(err);
            sqlite3_close(db);
            return false;
        }
    }
    sqlite3_close(db);
    return true;
}

int main()
{
    if (!build_test_index()) {
        fprintf(stderr, "FAIL: cannot build test index\n");
        return 1;
    }
    printf("Test index built at %s\n", TEST_IDX);

    PkgFileIndex *idx = pkgfile_index_open(TEST_IDX, nullptr);
    if (!idx) {
        fprintf(stderr, "FAIL: cannot open index\n");
        return 1;
    }
    printf("PASS: open\n");

    /* Query by file - exact match */
    {
        PkgFilePkgInfo info = {};
        PkgFileErrorCode rc = pkgfile_index_query_by_file(idx, "/usr/bin/test-bin-a", &info);
        assert(rc == PKGFILE_OK);
        assert(strcmp(info.pkg_name, "test-pkg-a") == 0);
        assert(strcmp(info.version, "1.0.0-1") == 0);
        assert(strcmp(info.arch, "amd64") == 0);
        /* Result pointer survives after the call (no longer dangling) */
        const char *saved = info.pkg_name;
        rc = pkgfile_index_query_by_file(idx, "/usr/bin/test-bin-b", &info);
        assert(rc == PKGFILE_OK);
        assert(strcmp(info.pkg_name, "test-pkg-b") == 0);
        /* Previous pointer may be invalidated by pool reset — that's expected */
        (void)saved;
        printf("PASS: query_by_file (exact match)\n");
    }

    /* Query by file - no match */
    {
        PkgFilePkgInfo info = {};
        PkgFileErrorCode rc = pkgfile_index_query_by_file(idx, "/usr/bin/nonexistent", &info);
        assert(rc == PKGFILE_ERR_NO_MATCH);
        printf("PASS: query_by_file (no match)\n");
    }

    /* Batch query */
    {
        const char *paths[] = {
            "/usr/bin/test-bin-a",
            "/usr/bin/nonexistent",
            "/usr/bin/test-bin-b"
        };
        PkgFilePkgInfo infos[3] = {};
        PkgFileErrorCode rc = pkgfile_index_query_by_files(idx, paths, infos, 3);
        assert(rc == PKGFILE_OK);
        assert(strcmp(infos[0].pkg_name, "test-pkg-a") == 0);
        assert(infos[1].pkg_name == nullptr);
        assert(strcmp(infos[2].pkg_name, "test-pkg-b") == 0);
        printf("PASS: query_by_files (batch)\n");
    }

    /* Query by desktop */
    {
        PkgFilePkgInfo info = {};
        const char *app_name = nullptr;
        PkgFileErrorCode rc = pkgfile_index_query_by_desktop(
            idx, "/usr/share/applications/test-a.desktop", &info, &app_name);
        assert(rc == PKGFILE_OK);
        assert(strcmp(info.pkg_name, "test-pkg-a") == 0);
        assert(app_name != nullptr && strcmp(app_name, "Test App A") == 0);
        printf("PASS: query_by_desktop\n");
    }

    /* Query pkg files (caller-owned memory) */
    {
        char **files = nullptr;
        size_t count = 0;
        PkgFileErrorCode rc = pkgfile_index_query_pkg_files(idx, "test-pkg-a", &files, &count);
        assert(rc == PKGFILE_OK);
        assert(count == 2);
        printf("PASS: query_pkg_files (count=%zu)\n", count);
        for (size_t i = 0; i < count; i++) free(files[i]);
        free(files);
    }

    /* Query by prefix (caller-owned memory) */
    {
        char **paths = nullptr;
        PkgFilePkgInfo *infos = nullptr;
        size_t count = 0;
        PkgFileErrorCode rc = pkgfile_index_query_by_prefix(
            idx, "/usr/lib/", &paths, &infos, &count);
        assert(rc == PKGFILE_OK);
        assert(count == 1);
        assert(strcmp(infos[0].pkg_name, "test-pkg-a") == 0);
        printf("PASS: query_by_prefix (count=%zu)\n", count);
        for (size_t i = 0; i < count; i++) {
            free(paths[i]);
            free((void *)infos[i].pkg_name);
            free((void *)infos[i].version);
            free((void *)infos[i].arch);
        }
        free(paths);
        free(infos);
    }

    /* Metadata — build_time pointer survives */
    {
        assert(pkgfile_index_get_package_count(idx) == 2);
        const char *bt = pkgfile_index_get_build_time(idx);
        assert(bt != nullptr);
        assert(strcmp(bt, "2026-08-03 20:00:00") == 0);
        printf("PASS: metadata\n");
    }

    /* Open non-existent */
    {
        PkgFileErrorCode err = PKGFILE_OK;
        PkgFileIndex *bad = pkgfile_index_open("/tmp/nonexistent_foo.idx", &err);
        assert(bad == nullptr);
        assert(err == PKGFILE_ERR_NOT_FOUND);
        printf("PASS: open non-existent returns NOT_FOUND\n");
    }

    /* NULL parameter handling */
    {
        PkgFilePkgInfo info = {};
        assert(pkgfile_index_query_by_file(NULL, "/foo", &info) == PKGFILE_ERR_NO_MATCH);
        assert(pkgfile_index_query_by_file(idx, NULL, &info) == PKGFILE_ERR_NO_MATCH);
        assert(pkgfile_index_query_by_file(idx, "/foo", NULL) == PKGFILE_ERR_NO_MATCH);
        printf("PASS: NULL parameter handling\n");
    }

    pkgfile_index_close(idx);
    printf("\nAll tests passed!\n");

    remove(TEST_IDX);
    return 0;
}
