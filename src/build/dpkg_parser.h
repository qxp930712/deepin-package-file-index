#ifndef DPKG_PARSER_H
#define DPKG_PARSER_H

#include <string>
#include <vector>
#include <unordered_map>

struct PkgInfo {
    std::string name;
    std::string version;
    std::string arch;
    std::string source;
    std::vector<std::string> files;  // from .list file
};

/* Parse /var/lib/dpkg/status, return all installed packages */
std::vector<PkgInfo> parse_dpkg_status(const std::string &status_path);

/* Read /var/lib/dpkg/info/<name>.list, populate pkg.files */
bool read_pkg_list_file(const std::string &info_dir, PkgInfo &pkg);

#endif
