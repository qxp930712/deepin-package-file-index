#include "dpkg_parser.h"
#include "desktop_parser.h"

#include <sqlite3.h>
#include <getopt.h>
#include <ctime>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>
#include <errno.h>
#include <iostream>
#include <algorithm>
#include <unordered_map>

#include <unordered_set>
static const char *SCHEMA[] = {
    "PRAGMA journal_mode = WAL;",
    "PRAGMA synchronous = FULL;",
    "PRAGMA page_size = 4096;",
    "PRAGMA temp_store = MEMORY;",
    "",
    "CREATE TABLE IF NOT EXISTS packages ("
    "    pkg_id   INTEGER PRIMARY KEY,"
    "    name     TEXT NOT NULL,"
    "    version  TEXT NOT NULL,"
    "    arch     TEXT,"
    "    source   TEXT"
    ");",
    "CREATE INDEX IF NOT EXISTS idx_pkg_name ON packages(name);",
    "",
    "CREATE TABLE IF NOT EXISTS file_package ("
    "    file_path TEXT NOT NULL,"
    "    pkg_id    INTEGER NOT NULL REFERENCES packages(pkg_id),"
    "    PRIMARY KEY (file_path)"
    ");",
    "",
    "CREATE TABLE IF NOT EXISTS package_files ("
    "    pkg_id    INTEGER NOT NULL,"
    "    file_path TEXT NOT NULL,"
    "    PRIMARY KEY (pkg_id, file_path)"
    ");",
    "CREATE INDEX IF NOT EXISTS idx_pkg_files_pkg ON package_files(pkg_id);",
    "",
    "CREATE TABLE IF NOT EXISTS desktop_package ("
    "    desktop_path TEXT NOT NULL PRIMARY KEY,"
    "    pkg_id       INTEGER NOT NULL REFERENCES packages(pkg_id),"
    "    app_name     TEXT,"
    "    exec         TEXT"
    ");",
    "",
    "CREATE TABLE IF NOT EXISTS meta ("
    "    key   TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");",
    nullptr,
};

static void print_usage(const char *prog)
{
    std::cerr << "Usage: " << prog << " [OPTIONS]\n"
              << "\nOptions:\n"
              << "  --incremental    Incremental update (default: full rebuild)\n"
              << "  -o, --output     Output index path\n"
              << "                   (default: /var/cache/deepin/package-file-index/installed.idx)\n"
              << "  --stat           Print statistics only, do not build\n"
              << "  -h, --help        Show this help\n";
}

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << err << std::endl;
        sqlite3_free(err);
        return false;
    }
    return true;
}

static bool mkdir_p(const std::string &path)
{
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        if (mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
    }
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

static std::string dirname_of(const std::string &path)
{
    size_t slash = path.rfind('/');
    return (slash != std::string::npos) ? path.substr(0, slash) : ".";
}

int build_index(const std::string &output_path, bool incremental)
{
    std::string tmp_path = output_path + ".tmp";

    if (!mkdir_p(dirname_of(output_path))) {
        std::cerr << "Cannot create directory: " << dirname_of(output_path) << std::endl;
        return 1;
    }

    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(tmp_path.c_str(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot create " << tmp_path << ": " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }

    for (int i = 0; SCHEMA[i] != nullptr; i++) {
        if (SCHEMA[i][0] == '\0') continue;
        if (!exec_sql(db, SCHEMA[i])) {
            sqlite3_close(db);
            return 1;
        }
    }

    std::unordered_map<std::string, int64_t> existing_pkg_ids;
    if (incremental) {
        sqlite3_stmt *attach_stmt;
        if (sqlite3_prepare_v2(db, "ATTACH DATABASE ? AS old_db", -1,
                              &attach_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(attach_stmt, 1, output_path.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(attach_stmt) == SQLITE_DONE) {
                sqlite3_exec(db,
                    "INSERT OR IGNORE INTO packages SELECT * FROM old_db.packages;",
                    nullptr, nullptr, nullptr);
                sqlite3_exec(db,
                    "INSERT OR IGNORE INTO file_package SELECT * FROM old_db.file_package;",
                    nullptr, nullptr, nullptr);
                sqlite3_exec(db,
                    "INSERT OR IGNORE INTO package_files SELECT * FROM old_db.package_files;",
                    nullptr, nullptr, nullptr);
                sqlite3_exec(db,
                    "INSERT OR IGNORE INTO desktop_package SELECT * FROM old_db.desktop_package;",
                    nullptr, nullptr, nullptr);
            }
            sqlite3_finalize(attach_stmt);
            sqlite3_exec(db, "DETACH DATABASE old_db;", nullptr, nullptr, nullptr);

            sqlite3_stmt *sel;
            if (sqlite3_prepare_v2(db, "SELECT pkg_id, name FROM packages",
                                  -1, &sel, nullptr) == SQLITE_OK) {
                while (sqlite3_step(sel) == SQLITE_ROW)
                    existing_pkg_ids[(const char *)sqlite3_column_text(sel, 1)]
                        = sqlite3_column_int64(sel, 0);
                sqlite3_finalize(sel);
            }
        }
    }

    std::vector<PkgInfo> packages = parse_dpkg_status("/var/lib/dpkg/status");
    std::cout << "Parsed " << packages.size() << " installed packages" << std::endl;

    std::unordered_map<std::string, std::string> file_to_pkg_name;

    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    sqlite3_stmt *ins_pkg = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO packages (pkg_id, name, version, arch, source) "
        "VALUES (?1, ?2, ?3, ?4, ?5)", -1, &ins_pkg, nullptr);

    sqlite3_stmt *ins_file = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO file_package (file_path, pkg_id) VALUES (?1, ?2)",
        -1, &ins_file, nullptr);

    sqlite3_stmt *ins_pkg_file = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO package_files (pkg_id, file_path) VALUES (?1, ?2)",
        -1, &ins_pkg_file, nullptr);

    int64_t next_pkg_id = 1;
    std::unordered_map<std::string, int64_t> name_to_pkg_id = existing_pkg_ids;
    for (const auto &pkg : existing_pkg_ids)
        if (pkg.second >= next_pkg_id) next_pkg_id = pkg.second + 1;

    for (size_t i = 0; i < packages.size(); i++) {
        const PkgInfo &pkg = packages[i];

        PkgInfo mutable_pkg = pkg;
        read_pkg_list_file("/var/lib/dpkg/info", mutable_pkg);

        int64_t pkg_id;
        auto it = existing_pkg_ids.find(pkg.name);
        if (it != existing_pkg_ids.end())
            pkg_id = it->second;
        else
            pkg_id = next_pkg_id++;
        name_to_pkg_id[pkg.name] = pkg_id;

        sqlite3_bind_int64(ins_pkg, 1, pkg_id);
        sqlite3_bind_text(ins_pkg, 2, pkg.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins_pkg, 3, pkg.version.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins_pkg, 4, pkg.arch.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins_pkg, 5, pkg.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins_pkg);
        sqlite3_reset(ins_pkg);

        for (const auto &file : mutable_pkg.files) {
            sqlite3_bind_text(ins_file, 1, file.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins_file, 2, pkg_id);
            sqlite3_step(ins_file);
            sqlite3_reset(ins_file);

            sqlite3_bind_int64(ins_pkg_file, 1, pkg_id);
            sqlite3_bind_text(ins_pkg_file, 2, file.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(ins_pkg_file);
            sqlite3_reset(ins_pkg_file);

            file_to_pkg_name[file] = pkg.name;
        }
    }

    // In incremental mode, remove packages that are no longer installed
    if (incremental && !existing_pkg_ids.empty()) {
        std::unordered_set<std::string> current_names;
        for (const auto &pkg : packages)
            current_names.insert(pkg.name);

        std::vector<int64_t> removed_ids;
        for (const auto &ep : existing_pkg_ids) {
            if (current_names.find(ep.first) == current_names.end())
                removed_ids.push_back(ep.second);
        }

        if (!removed_ids.empty()) {
            std::cout << "Removing " << removed_ids.size() << " uninstalled packages" << std::endl;

            for (int64_t rid : removed_ids) {
                sqlite3_stmt *del = nullptr;
                sqlite3_prepare_v2(db,
                    "DELETE FROM file_package WHERE pkg_id = ?1",
                    -1, &del, nullptr);
                sqlite3_bind_int64(del, 1, rid);
                sqlite3_step(del);
                sqlite3_finalize(del);

                sqlite3_prepare_v2(db,
                    "DELETE FROM package_files WHERE pkg_id = ?1",
                    -1, &del, nullptr);
                sqlite3_bind_int64(del, 1, rid);
                sqlite3_step(del);
                sqlite3_finalize(del);

                sqlite3_prepare_v2(db,
                    "DELETE FROM desktop_package WHERE pkg_id = ?1",
                    -1, &del, nullptr);
                sqlite3_bind_int64(del, 1, rid);
                sqlite3_step(del);
                sqlite3_finalize(del);

                sqlite3_prepare_v2(db,
                    "DELETE FROM packages WHERE pkg_id = ?1",
                    -1, &del, nullptr);
                sqlite3_bind_int64(del, 1, rid);
                sqlite3_step(del);
                sqlite3_finalize(del);
            }
        }
    }
    sqlite3_stmt *ins_desktop = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO desktop_package (desktop_path, pkg_id, app_name, exec) "
        "VALUES (?1, ?2, ?3, ?4)", -1, &ins_desktop, nullptr);

    std::vector<DesktopEntry> desktops = scan_desktop_files("/usr/share/applications");
    std::cout << "Found " << desktops.size() << " desktop files" << std::endl;

    int desktop_matched = 0;
    for (const auto &de : desktops) {
        auto it = file_to_pkg_name.find(de.path);
        if (it == file_to_pkg_name.end()) continue;

        auto pit = name_to_pkg_id.find(it->second);
        if (pit == name_to_pkg_id.end()) continue;

        sqlite3_bind_text(ins_desktop, 1, de.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins_desktop, 2, pit->second);
        sqlite3_bind_text(ins_desktop, 3, de.app_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins_desktop, 4, de.exec.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins_desktop);
        sqlite3_reset(ins_desktop);
        desktop_matched++;
    }
    std::cout << "Matched " << desktop_matched << " desktop files to packages" << std::endl;

    char time_buf[64];
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    sqlite3_stmt *ins_meta = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?1, ?2)",
                       -1, &ins_meta, nullptr);

    auto set_meta = [&](const char *key, const std::string &val) {
        sqlite3_bind_text(ins_meta, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins_meta, 2, val.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins_meta);
        sqlite3_reset(ins_meta);
    };

    set_meta("build_time", time_buf);
    set_meta("package_count", std::to_string(packages.size()));
    set_meta("index_version", "1");
    set_meta("file_count", std::to_string(file_to_pkg_name.size()));

    sqlite3_finalize(ins_meta);
    sqlite3_finalize(ins_desktop);
    sqlite3_finalize(ins_pkg_file);
    sqlite3_finalize(ins_file);
    sqlite3_finalize(ins_pkg);

    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);

    sqlite3_exec(db, "PRAGMA journal_mode = DELETE;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr);

    { FILE *fp = fopen(tmp_path.c_str(), "rb"); if (fp) { fsync(fileno(fp)); fclose(fp); } }

    sqlite3_close(db);

    if (rename(tmp_path.c_str(), output_path.c_str()) != 0) {
        std::cerr << "Failed to rename " << tmp_path << " to " << output_path << std::endl;
        return 1;
    }

    std::cout << "Index written to " << output_path << std::endl;
    std::cout << "  Packages: " << packages.size() << std::endl;
    std::cout << "  Files:    " << file_to_pkg_name.size() << std::endl;
    std::cout << "  Desktops: " << desktop_matched << std::endl;
    std::cout << "  Built at: " << time_buf << std::endl;

    return 0;
}

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"incremental", no_argument,       nullptr, 'i'},
        {"output",      required_argument, nullptr, 'o'},
        {"stat",        no_argument,       nullptr, 's'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    bool incremental = false;
    bool stat_only = false;
    std::string output_path = "/var/cache/deepin/package-file-index/installed.idx";

    int opt;
    while ((opt = getopt_long(argc, argv, "o:h", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'i': incremental = true; break;
        case 'o': output_path = optarg; break;
        case 's': stat_only = true; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (stat_only) {
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(output_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            std::cerr << "Cannot open index: " << output_path << std::endl;
            return 1;
        }
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, "SELECT key, value FROM meta", -1, &stmt, nullptr) == SQLITE_OK) {
            std::cout << "Index: " << output_path << std::endl;
            while (sqlite3_step(stmt) == SQLITE_ROW)
                std::cout << "  " << sqlite3_column_text(stmt, 0)
                          << " = " << sqlite3_column_text(stmt, 1) << std::endl;
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
        return 0;
    }

    return build_index(output_path, incremental);
}
