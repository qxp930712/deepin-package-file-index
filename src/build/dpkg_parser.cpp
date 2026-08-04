#include "dpkg_parser.h"
#include "common.h"
#include <fstream>
#include <sstream>

static PkgInfo parse_stanza(const std::vector<std::string> &lines, size_t &pos)
{
    PkgInfo pkg;
    std::unordered_map<std::string, std::string> fields;
    std::string last_key;

    while (pos < lines.size()) {
        const std::string &line = lines[pos];
        if (line.empty()) { pos++; break; }

        if (line[0] == ' ' || line[0] == '\t') {
            if (!last_key.empty())
                fields[last_key] += "\n" + trim(line);
        } else {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                last_key = line.substr(0, colon);
                fields[last_key] = trim(line.substr(colon + 1));
            }
        }
        pos++;
    }

    auto sit = fields.find("Status");
    if (sit == fields.end()) return pkg;
    if (sit->second != "install ok installed") return pkg;

    auto nit = fields.find("Package");
    if (nit != fields.end()) pkg.name = nit->second;
    auto vit = fields.find("Version");
    if (vit != fields.end()) pkg.version = vit->second;
    auto ait = fields.find("Architecture");
    if (ait != fields.end()) pkg.arch = ait->second;
    auto srcit = fields.find("Source");
    if (srcit != fields.end()) pkg.source = srcit->second;

    return pkg;
}

std::vector<PkgInfo> parse_dpkg_status(const std::string &status_path)
{
    std::vector<PkgInfo> result;
    std::ifstream f(status_path);
    if (!f.is_open()) return result;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
        lines.push_back(line);

    size_t pos = 0;
    while (pos < lines.size()) {
        PkgInfo pkg = parse_stanza(lines, pos);
        if (!pkg.name.empty())
            result.push_back(std::move(pkg));
    }
    return result;
}

bool read_pkg_list_file(const std::string &info_dir, PkgInfo &pkg)
{
    std::string path = info_dir + "/" + pkg.name + ".list";
    std::ifstream f(path);
    if (!f.is_open() && !pkg.arch.empty()) {
        path = info_dir + "/" + pkg.name + ":" + pkg.arch + ".list";
        f.open(path);
    }
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (!t.empty())
            pkg.files.push_back(t);
    }
    return true;
}
