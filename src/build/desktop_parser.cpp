#include "desktop_parser.h"
#include "common.h"
#include <fstream>
#include <unordered_set>
#include <dirent.h>
#include <sys/stat.h>

static bool parse_desktop_file(const std::string &path, DesktopEntry &entry)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;

    bool in_desktop_entry = false;
    bool hidden = false;
    std::string line;

    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        if (t == "[Desktop Entry]" || t == "[Desktop Entry ]") {
            in_desktop_entry = true;
            continue;
        }
        if (t[0] == '[' && t.back() == ']')
            break;
        if (!in_desktop_entry) continue;

        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;

        std::string key = t.substr(0, eq);
        std::string val = trim(t.substr(eq + 1));

        if (key == "Name") {
            entry.app_name = val;
        } else if (key == "Exec") {
            entry.exec = val;
        } else if (key == "Icon") {
            entry.icon = val;
        } else if (key == "NoDisplay") {
            if (val == "true") hidden = true;
        }
    }

    if (hidden) return false;
    entry.path = path;
    return !entry.path.empty();
}

std::vector<DesktopEntry> scan_desktop_files(const std::string &dir)
{
    std::vector<DesktopEntry> result;
    DIR *d = opendir(dir.c_str());
    if (!d) return result;

    std::vector<std::string> subdirs = {""};

    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            subdirs.push_back(name);
    }
    closedir(d);

    for (const auto &sub : subdirs) {
        std::string scan_dir = sub.empty() ? dir : dir + "/" + sub;
        DIR *sd = opendir(scan_dir.c_str());
        if (!sd) continue;

        while ((ent = readdir(sd)) != nullptr) {
            std::string name = ent->d_name;
            if (name.size() < 9 || name.substr(name.size() - 8) != ".desktop")
                continue;

            DesktopEntry entry;
            if (parse_desktop_file(scan_dir + "/" + name, entry))
                result.push_back(std::move(entry));
        }
        closedir(sd);
    }

    return result;
}
