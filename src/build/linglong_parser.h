#ifndef LINGLONG_PARSER_H
#define LINGLONG_PARSER_H

#include <string>
#include <vector>
#include <functional>

struct LinglongPkg {
    std::string id;          // bundle id, e.g. "com.qq.qqmusic"
    std::string app_name;    // from .desktop Name= field
    std::vector<std::string> files;
};

/* Scan /opt/apps/ for installed linglong packages.
 * For each package, walks entries/ and files/ to collect all files.
 * Calls on_desktop(id, desktop_path, app_name) for each .desktop found. */
std::vector<LinglongPkg> scan_linglong_packages(
    const std::string &apps_dir,
    std::function<void(const std::string &id, const std::string &desktop_path, const std::string &app_name)> on_desktop);

#endif
