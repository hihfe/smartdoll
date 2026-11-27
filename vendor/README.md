# vendor/：底座力学件（无外网机器的引入形）

tech-stack.md 选型的 header-only / 动态库在本机无系统包、无外网（github 被墙）时的落点。
想改系统级安装：`sudo apt install liburing-dev libconcurrentqueue-dev` 后可不再依赖 sysroot。

| 件 | 版本 | 来源 | 落点 |
|---|---|---|---|
| BS::thread_pool | v3.5.0 (MIT) | raw.githubusercontent.com/bshoshany/thread-pool v3.5.0 tag | `BS_thread_pool.hpp` |
| liburing | 2.5-1build1 (MIT) | Ubuntu noble 包 `liburing-dev` + `liburing2`，apt-get download + dpkg -x | `sysroot/usr/{include,lib}` |
| moodycamel::ConcurrentQueue | 1.0.4 (BSD-2/CC0) | Ubuntu noble 包 `libconcurrentqueue-dev`，同上 | `sysroot/usr/include/concurrentqueue/` |

.deb 原包留在 `debs/` 备查。Boost.Heap / nlohmann/json / Catch2 用系统包，不进 vendor。
