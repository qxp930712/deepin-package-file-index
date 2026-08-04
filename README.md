# deepin-package-file-index

为 DDE 桌面各组件提供通用的「文件路径 → 所属 deb 包」查询能力。

## 背景

DDE 桌面中多个组件（安全中心、启动器、窗管、崩溃收集器等）需要根据文件路径反查所属的 deb 包名，例如：

- 安全中心：根据 `/proc/<pid>/exe` 路径识别进程所属包，用于进程防杀、流量监控
- 启动器：判断应用是否由 deb 包管理（vs linglong/flatpak）
- 崩溃收集器：根据崩溃进程路径定位包名和版本，生成崩溃报告

当前各组件各自通过 `dpkg -S`、私有 SQLite、互调 D-Bus 等方式实现，存在重复造轮子、性能差、耦合深等问题。

## 方案

**D-Bus 系统服务 + 纯 C 实现**，消费者通过 D-Bus 接口查询，无需链接任何库。

```
数据源 (dpkg/status + *.list + *.desktop)
  ↓ deepin-pkgfile-index-build (apt hook 触发)
索引文件 (/var/cache/deepin/package-file-index/installed.idx)
  ↓ mmap 只读加载
deepin-pkgfile-indexd (D-Bus system service)
  ↓ D-Bus method call
消费者 (deepin-defender / dde-launcher / dde-wm / 崩溃收集器 / ...)
```

## 项目结构

```
deepin-package-file-index/
├── CMakeLists.txt                         # 顶层构建
├── data/
│   ├── com.deepin.pkgfileindex.service    # D-Bus system service (激活配置)
│   ├── com.deepin.pkgfileindex.conf       # D-Bus 策略
│   └── deepin-pkgfile-indexd.service      # systemd service
├── src/
│   ├── lib/                               # 内部查询库 (仅 daemon 使用)
│   │   ├── CMakeLists.txt
│   │   ├── pkgfile_index.h               # 内部 C API
│   │   ├── pkgfile_index.c               # 实现 (mmap + prepared statement)
│   │   └── pkgfile_index_priv.h          # 内部结构
│   ├── daemon/                            # D-Bus 守护进程 (纯 C + libdbus)
│   │   ├── CMakeLists.txt
│   │   └── main.c
│   └── build/                             # CLI 索引构建工具
│       ├── CMakeLists.txt
│       ├── main.cpp
│       ├── common.h / common.cpp         # 公共工具函数
│       ├── dpkg_parser.h / dpkg_parser.cpp
│       └── desktop_parser.h / desktop_parser.cpp
├── tests/
│   ├── CMakeLists.txt
│   └── test_query.cpp                     # 单元测试
├── etc/
│   └── apt/apt.conf.d/
│       └── 80deepin-pkgfile-index         # apt hook
└── debian/                                # 打包配置
    ├── control                            # 2 个二进制包
    ├── rules
    ├── changelog
    ├── source/format
    ├── deepin-pkgfile-indexd.install
    ├── deepin-pkgfile-indexd.postinst
    ├── deepin-pkgfile-index-build.install
    ├── deepin-pkgfile-index-build.postinst
    └── deepin-pkgfile-index-build.postrm
```

## 编译

### 依赖

- CMake >= 3.10
- GCC >= 8 (C11 / C++14)
- libsqlite3-dev
- libdbus-1-dev, pkg-config

### 编译命令

```bash
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)
```

### 运行测试

```bash
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
./tests/test_query
```

### 安装

```bash
make install
```

## Debian 包

| 包名 | 说明 |
|------|------|
| `deepin-pkgfile-indexd` | D-Bus 守护进程 + D-Bus 服务配置 + systemd service |
| `deepin-pkgfile-index-build` | CLI 构建工具 + apt hook |

## D-Bus 接口

**服务名**: `com.deepin.pkgfileindex`
**对象路径**: `/com/deepin/pkgfileindex`
**接口**: `com.deepin.pkgfileindex`
**总线**: system bus

### 方法

#### QueryByFile

文件路径 → 包名（最常用）

```
in:  s  file_path
out: s  package_name
     s  version
     s  arch
```

#### QueryByDesktop

Desktop 文件 → 包名 + 应用名

```
in:  s  desktop_path
out: s  package_name
     s  version
     s  arch
     s  app_name
```

#### QueryPackageFiles

包名 → 该包所有文件（替代 dpkg -L）

```
in:  s  package_name
out: as files
```

#### QueryByPrefix

前缀匹配（替代 dpkg -S）

```
in:  s  prefix
out: as paths
     as package_names
     as versions
```

#### GetMeta

索引元数据

```
in:  -
out: s  build_time
     u  package_count
     u  file_count
```

## 索引构建工具

```bash
# 全量构建
sudo deepin-pkgfile-index-build

# 增量更新（仅处理变化的包）
sudo deepin-pkgfile-index-build --incremental

# 指定输出路径
sudo deepin-pkgfile-index-build -o /tmp/test.idx

# 查看当前索引统计信息
deepin-pkgfile-index-build --stat
```

### 更新触发机制

- **apt hook**（`/etc/apt/apt.conf.d/80deepin-pkgfile-index`）：每次 `apt install/remove/upgrade` 后自动增量更新
- **inotify**：daemon 监听索引文件变化，自动重载
- **postinst**：首次安装 daemon 时触发全量构建

## 接入示例

以 Qt/C++ 项目为例，通过 D-Bus 查询文件所属包：

### CMakeLists.txt

```cmake
find_package(Qt5DBus REQUIRED)
target_link_libraries(myapp Qt5::DBus)
```

### 代码

```cpp
#include <QtDBus/QtDBus>

static QString queryPkgName(const QString &filePath)
{
    QDBusInterface iface(“com.deepin.pkgfileindex”,
        “/com/deepin/pkgfileindex”,
        “com.deepin.pkgfileindex”,
        QDBusConnection::systemBus());

    QDBusReply<QString> pkg = iface.call(“QueryByFile”, filePath);
    return pkg.isValid() ? pkg.value() : QString();
}

static QString queryPkgNameByDesktop(const QString &desktopPath)
{
    QDBusInterface iface(
com.deepin.pkgfileindex”,
        “/com/deepin/pkgfileindex”,
        “com.deepin.pkgfileindex”,
        QDBusConnection::systemBus());

    QDBusMessage reply = iface.call(“QueryByDesktop”, desktopPath);
    if (reply.type() == QDBusMessage::ReplyMessage)
        return reply.arguments().at(0).toString();
    return QString();
}

static QStringList queryPkgFiles(const QString &pkgName)
{
    QDBusInterface iface(“com.deepin.pkgfileindex”,
        “/com/deepin/pkgfileindex”,
        “com.deepin.pkgfileindex”,
        QDBusConnection::systemBus());

    QDBusReply<QStringList> files = iface.call(“QueryPackageFiles”, pkgName);
    return files.isValid() ? files.value() : QStringList();
}
```

### 命令行调试

```bash
# 使用 dbus-send
dbus-send --system --print-reply \
  --dest=com.deepin.pkgfileindex \
  /com/deepin/pkgfileindex \
  com.deepin.pkgfileindex.QueryByFile \
  string:/usr/bin/dde-desktop

# 查看索引元数据
dbus-send --system --print-reply \
  --dest=com.deepin.pkgfileindex \
  /com/deepin/pkgfileindex \
  com.deepin.pkgfileindex.GetMeta

# 查看 Introspection 数据
dbus-send --system --print-reply \
  --dest=com.deepin.pkgfileindex \
  /com/deepin/pkgfileindex \
  org.freedesktop.DBus.Introspectable.Introspect
```

## 索引格式

索引使用 SQLite，存储在 `/var/cache/deepin/package-file-index/installed.idx`，包含以下表：

| 表名 | 用途 |
|------|------|
| `packages` | 包信息（name, version, arch, source） |
| `file_package` | 核心映射：文件路径 → 包 ID（主查询表） |
| `desktop_package` | desktop 文件 → 包 ID + 应用名 |
| `package_files` | 包 ID → 所有文件（替代 `dpkg -L`） |
| `meta` | 索引元数据（构建时间、版本、包数量） |

Daemon 以只读模式打开索引并启用 mmap，多进程共享 page cache，零拷贝查询。
