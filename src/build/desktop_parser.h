#ifndef DESKTOP_PARSER_H
#define DESKTOP_PARSER_H

#include <string>
#include <vector>

struct DesktopEntry {
    std::string path;      /* absolute path */
    std::string app_name;  /* Name= field */
    std::string exec;      /* Exec= field */
    std::string icon;      /* Icon= field */
};

/* Scan /usr/share/applications/*.desktop, return all valid entries */
std::vector<DesktopEntry> scan_desktop_files(const std::string &dir);

#endif
