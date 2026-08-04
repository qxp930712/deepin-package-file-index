#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/inotify.h>

#include <dbus/dbus.h>

#include "../lib/pkgfile_index.h"

#define SERVICE_NAME "com.deepin.pkgfileindex"
#define OBJECT_PATH  "/com/deepin/pkgfileindex"
#define IFACE_NAME  "com.deepin.pkgfileindex"
#define INDEX_PATH  "/var/cache/deepin/package-file-index/installed.idx"
#define INDEX_DIR   "/var/cache/deepin/package-file-index"
#define POLL_MS     100

static PkgFileIndex *g_index = NULL;
static DBusConnection *g_conn = NULL;
static int g_inotify_fd = -1;
static int g_inotify_wd = -1;
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static const char *INTROSPECT_XML =
"<!DOCTYPE node>\n"
"<node name=\"/com/deepin/pkgfileindex\">\n"
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\">\n"
"      <arg name=\"data\" type=\"s\" direction=\"out\"/>\n"
"    </method>\n"
"  </interface>\n"
"  <interface name=\"com.deepin.pkgfileindex\">\n"
"    <method name=\"QueryByFile\">\n"
"      <arg name=\"file_path\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"package_name\" type=\"s\" direction=\"out\"/>\n"
"      <arg name=\"version\" type=\"s\" direction=\"out\"/>\n"
"      <arg name=\"arch\" type=\"s\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"QueryByDesktop\">\n"
"      <arg name=\"desktop_path\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"package_name\" type=\"s\" direction=\"out\"/>\n"
"      <arg name=\"version\" type=\"s\" direction=\"out\"/>\n"
"      <arg name=\"arch\" type=\"s\" direction=\"out\"/>\n"
"      <arg name=\"app_name\" type=\"s\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"QueryPackageFiles\">\n"
"      <arg name=\"package_name\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"files\" type=\"as\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"QueryByPrefix\">\n"
"      <arg name=\"prefix\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"paths\" type=\"as\" direction=\"out\"/>\n"
"      <arg name=\"package_names\" type=\"as\" direction=\"out\"/>\n"
"      <arg name=\"versions\" type=\"as\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"GetMeta\">\n"
"      <arg name=\"build_time\" type=\"s\" direction=\"out\"/>\n"
"      <arg name=\"package_count\" type=\"u\" direction=\"out\"/>\n"
"      <arg name=\"file_count\" type=\"u\" direction=\"out\"/>\n"
"    </method>\n"
"  </interface>\n"
"</node>\n";

static void send_error(DBusConnection *conn, DBusMessage *msg,
                        const char *name, const char *text)
{
    DBusMessage *reply = dbus_message_new_error(msg, name, text);
    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
    }
}

static void load_index(void)
{
    if (g_index) {
        pkgfile_index_close(g_index);
        g_index = NULL;
    }
    g_index = pkgfile_index_open(INDEX_PATH, NULL);
    if (g_index) {
        fprintf(stderr, "pkgfile-indexd: index loaded, %u packages\n",
                pkgfile_index_get_package_count(g_index));
    } else {
        fprintf(stderr, "pkgfile-indexd: warning: index not available\n");
    }
}

static void inotify_setup(void)
{
    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify_fd < 0) return;

    g_inotify_wd = inotify_add_watch(g_inotify_fd, INDEX_DIR,
                                      IN_CLOSE_WRITE | IN_MOVED_TO);
    if (g_inotify_wd < 0) {
        close(g_inotify_fd);
        g_inotify_fd = -1;
    }
}

static void inotify_drain(void)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t len = read(g_inotify_fd, buf, sizeof(buf));
        if (len <= 0) break;
    }
    load_index();
}

static DBusHandlerResult handle_introspect(DBusConnection *conn, DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    const char *data = INTROSPECT_XML;
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &data, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_query_by_file(DBusConnection *conn, DBusMessage *msg)
{
    const char *file_path = NULL;
    if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_STRING, &file_path, DBUS_TYPE_INVALID))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    const char *pkg = "", *ver = "", *arch = "";
    if (g_index) {
        PkgFilePkgInfo info = {};
        if (pkgfile_index_query_by_file(g_index, file_path, &info) == PKGFILE_OK) {
            pkg  = info.pkg_name ? info.pkg_name : "";
            ver  = info.version   ? info.version   : "";
            arch = info.arch      ? info.arch      : "";
        }
    }

    dbus_message_append_args(reply,
        DBUS_TYPE_STRING, &pkg,
        DBUS_TYPE_STRING, &ver,
        DBUS_TYPE_STRING, &arch,
        DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_query_by_desktop(DBusConnection *conn, DBusMessage *msg)
{
    const char *desktop_path = NULL;
    if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_STRING, &desktop_path, DBUS_TYPE_INVALID))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    const char *pkg = "", *ver = "", *arch = "", *app_name = "";
    if (g_index) {
        PkgFilePkgInfo info = {};
        const char *aname = NULL;
        if (pkgfile_index_query_by_desktop(g_index, desktop_path, &info, &aname) == PKGFILE_OK) {
            pkg      = info.pkg_name ? info.pkg_name : "";
            ver      = info.version   ? info.version   : "";
            arch     = info.arch      ? info.arch      : "";
            app_name = aname         ? aname          : "";
        }
    }

    dbus_message_append_args(reply,
        DBUS_TYPE_STRING, &pkg,
        DBUS_TYPE_STRING, &ver,
        DBUS_TYPE_STRING, &arch,
        DBUS_TYPE_STRING, &app_name,
        DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_query_pkg_files(DBusConnection *conn, DBusMessage *msg)
{
    const char *pkg_name = NULL;
    if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_STRING, &pkg_name, DBUS_TYPE_INVALID))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    char **files = NULL;
    int count = 0;
    if (g_index) {
        size_t n = 0;
        if (pkgfile_index_query_pkg_files(g_index, pkg_name, &files, &n) == PKGFILE_OK)
            count = (int)n;
    }

    dbus_message_append_args(reply,
        DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &files, count,
        DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);

    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_query_by_prefix(DBusConnection *conn, DBusMessage *msg)
{
    const char *prefix = NULL;
    if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_STRING, &prefix, DBUS_TYPE_INVALID))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    char **paths = NULL;
    char **pkg_names = NULL;
    char **versions = NULL;
    int count = 0;

    if (g_index) {
        PkgFilePkgInfo *infos = NULL;
        size_t n = 0;
        if (pkgfile_index_query_by_prefix(g_index, prefix, &paths, &infos, &n) == PKGFILE_OK) {
            count = (int)n;
            pkg_names = malloc(n * sizeof(char *));
            versions   = malloc(n * sizeof(char *));
            for (size_t i = 0; i < n; i++) {
                pkg_names[i] = (char *)(infos[i].pkg_name ? infos[i].pkg_name : "");
                versions[i]   = (char *)(infos[i].version   ? infos[i].version   : "");
                free((void *)infos[i].arch);
            }
            free(infos);
        }
    }

    dbus_message_append_args(reply,
        DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &paths,      count,
        DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &pkg_names,  count,
        DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &versions,   count,
        DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    free(pkg_names);
    free(versions);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_get_meta(DBusConnection *conn, DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    const char *build_time = "";
    uint32_t pkg_count = 0, file_count = 0;
    if (g_index) {
        const char *bt = pkgfile_index_get_build_time(g_index);
        build_time = bt ? bt : "";
        pkg_count  = pkgfile_index_get_package_count(g_index);
        file_count = pkgfile_index_get_file_count(g_index);
    }

    dbus_message_append_args(reply,
        DBUS_TYPE_STRING, &build_time,
        DBUS_TYPE_UINT32, &pkg_count,
        DBUS_TYPE_UINT32, &file_count,
        DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult message_handler(DBusConnection *conn,
                                          DBusMessage *msg, void *data)
{
    (void)data;
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    if (strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0 &&
        strcmp(member, "Introspect") == 0)
        return handle_introspect(conn, msg);

    if (strcmp(iface, IFACE_NAME) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(member, "QueryByFile") == 0)
        return handle_query_by_file(conn, msg);
    if (strcmp(member, "QueryByDesktop") == 0)
        return handle_query_by_desktop(conn, msg);
    if (strcmp(member, "QueryPackageFiles") == 0)
        return handle_query_pkg_files(conn, msg);
    if (strcmp(member, "QueryByPrefix") == 0)
        return handle_query_by_prefix(conn, msg);
    if (strcmp(member, "GetMeta") == 0)
        return handle_get_meta(conn, msg);

    send_error(conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable vtable = {
    .unregister_function = NULL,
    .message_function    = message_handler,
};

int main(void)
{
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    g_conn = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);
    if (!g_conn) {
        fprintf(stderr, "pkgfile-indexd: cannot connect to system bus\n");
        return 1;
    }

    int ret = dbus_bus_request_name(g_conn, SERVICE_NAME,
                                     DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "pkgfile-indexd: cannot acquire %s (ret=%d)\n", SERVICE_NAME, ret);
        return 1;
    }

    if (!dbus_connection_register_object_path(g_conn, OBJECT_PATH, &vtable, NULL)) {
        fprintf(stderr, "pkgfile-indexd: cannot register object path\n");
        return 1;
    }

    load_index();
    inotify_setup();

    fprintf(stderr, "pkgfile-indexd: service started on %s\n", SERVICE_NAME);

    /* Main loop: poll-based dispatch */
    while (g_running) {
        /* Dispatch pending D-Bus messages (non-blocking) */
        while (dbus_connection_dispatch(g_conn) == DBUS_DISPATCH_DATA_REMAINS)
            ;
        dbus_connection_read_write(g_conn, 0);

        /* Sleep or watch inotify */
        if (g_inotify_fd >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(g_inotify_fd, &rfds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = POLL_MS * 1000 };
            if (select(g_inotify_fd + 1, &rfds, NULL, NULL, &tv) > 0)
                inotify_drain();
        } else {
            usleep((useconds_t)POLL_MS * 1000);
        }
    }

    if (g_inotify_fd >= 0) close(g_inotify_fd);
    if (g_index) pkgfile_index_close(g_index);
    if (g_conn) dbus_connection_unref(g_conn);

    fprintf(stderr, "pkgfile-indexd: stopped\n");
    return 0;
}
