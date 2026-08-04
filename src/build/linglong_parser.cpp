#include "linglong_parser.h"
#include "desktop_parser.h"
#include "common.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <algorithm>

/* Recursively walk a directory, call callback for each file */
static void walk_dir(const std::string &dir, std::vector<std::string> &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d) return;

    std::vector<std::string> subdirs;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode))
                subdirs.push_back(name);
            else if (S_ISREG(st.st_mode))
                out.push_back(full);
        }
    }
    closedir(d);

    for (const auto &sub : subdirs)
        walk_dir(dir + "/" + sub, out);
}

/* Parse a .desktop file to extract the Name= value (simplified) */
static std::string parse_desktop_name(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open()) return "";

    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq);
        if (key == "Name") {
            std::string val = trim(t.substr(eq + 1));
            /* Skip localized keys that were already skipped by the parser,
             * but if the first Name= is found, return it */
            return val;
        }
    }
    return "";
}

std::vector<LinglongPkg> scan_linglong_packages(
    const std::string &apps_dir,
    std::function<void(const std::string &, const std::string &, const std::string &)> on_desktop)
{
    std::vector<LinglongPkg> result;

    DIR *d = opendir(apps_dir.c_str());
    if (!d) return result;

    std::vector<std::string> pkg_dirs;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = apps_dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            pkg_dirs.push_back(name);
    }
    closedir(d);

    std::sort(pkg_dirs.begin(), pkg_dirs.end());

    for (const auto &id : pkg_dirs) {
        LinglongPkg pkg;
        pkg.id = id;

        std::string base = apps_dir + "/" + id;

        /* Walk files/ */
        walk_dir(base + "/files", pkg.files);

        /* Walk entries/ (e.g. entries/applications/*.desktop) */
        walk_dir(base + "/entries", pkg.files);

        /* Parse desktop files for app_name */
        std::string desktop_dir = base + "/entries/applications";
        DIR *dd = opendir(desktop_dir.c_str());
        if (dd) {
            struct dirent *de;
            while ((de = readdir(dd)) != nullptr) {
                std::string name = de->d_name;
                if (name.size() >= 9 && name.substr(name.size() - 8) == ".desktop") {
                    std::string dpath = desktop_dir + "/" + name;
                    std::string app_name = parse_desktop_name(dpath);
                    if (!app_name.empty()) {
                        pkg.app_name = app_name;
                        if (on_desktop)
                            on_desktop(id, dpath, app_name);
                    }
                }
            }
            closedir(dd);
        }

        if (!pkg.files.empty())
            result.push_back(std::move(pkg));
    }

    return result;
}
