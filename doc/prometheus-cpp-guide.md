# Building and Using prometheus-cpp from Source on Ubuntu (Restricted Network)

**prometheus-cpp is the leading C++ client library for Prometheus monitoring, and integrating it into a CMake project requires building three components — core, pull, and push — each exposed as clean CMake imported targets.** The canonical repository lives at `https://github.com/jupp0r/prometheus-cpp`. The latest stable release is **v1.3.0** (November 2024), which requires a **C++14**-compatible compiler and **CMake ≥ 3.14**. This guide is tailored for environments where **direct access to GitHub is forbidden** on the build machine and **`git clone --recurse-submodules` is not available** even on the fetching machine. All repositories must be cloned individually and assembled by hand.

---

## 1. Understanding the Repository Structure

prometheus-cpp relies on three third-party repositories that are normally pulled in via git submodules under its `3rdparty/` directory. Since `recurse-submodules` is unavailable, you need to clone each one separately and place them in the correct locations yourself.

| Submodule path | Upstream repository | Required for | Needed for production? |
|----------------|-------------------|-------------|----------------------|
| `3rdparty/civetweb` | `https://github.com/civetweb/civetweb` | `ENABLE_PULL=ON` (HTTP exposer) | **Yes** — bundled HTTP server |
| `3rdparty/googletest` | `https://github.com/google/googletest` | `ENABLE_TESTING=ON` | No — unit tests only |
| `3rdparty/benchmark` | `https://github.com/google/benchmark` | `ENABLE_TESTING=ON` | No — benchmarks only |

For a production build with `ENABLE_TESTING=OFF`, only **prometheus-cpp** and **civetweb** are required. The googletest and benchmark repos can be skipped entirely.

---

## 2. Determining the Correct Submodule Versions

Each prometheus-cpp release pins specific commits of its submodules. Using the wrong version of civetweb can cause build failures or subtle runtime issues. To find the exact pinned commits, examine the `.gitmodules` file and the git tree of the tag you want.

On any machine where you have a clone of prometheus-cpp (even a shallow one without submodules):

```bash
git clone https://github.com/jupp0r/prometheus-cpp.git
cd prometheus-cpp
git checkout v1.3.0

# Show the submodule URLs
cat .gitmodules

# Show the exact commits pinned for each submodule at this tag
git ls-tree HEAD 3rdparty/
```

The `git ls-tree` output will look something like this:

```
160000 commit <CIVETWEB_COMMIT_SHA>     3rdparty/civetweb
160000 commit <GOOGLETEST_COMMIT_SHA>   3rdparty/googletest
160000 commit <BENCHMARK_COMMIT_SHA>    3rdparty/benchmark
```

Record the `<CIVETWEB_COMMIT_SHA>` — you will need it when cloning civetweb at the correct version. (If you are skipping tests, you can ignore the other two.)

> **Why this matters**: prometheus-cpp's CMake build compiles civetweb from source using files at a specific path. If the civetweb API changed between versions, the build will break. Always use the pinned commit, not just the latest civetweb release.

---

## 3. Cloning Each Repository Separately

On the **fetching machine** (the one that can reach GitHub), clone each required repository individually. No `--recurse-submodules` is used anywhere.

### Minimal set (pull-only, no tests)

```bash
# 1. Clone prometheus-cpp (no submodules — they will be empty, that's expected)
git clone https://github.com/jupp0r/prometheus-cpp.git
cd prometheus-cpp
git checkout v1.3.0
cd ..

# 2. Clone civetweb at the exact pinned commit
git clone https://github.com/civetweb/civetweb.git
cd civetweb
git checkout <CIVETWEB_COMMIT_SHA>    # Use the SHA from step 2
cd ..
```

### Full set (if you also want to run tests)

```bash
# 3. Clone googletest at the pinned commit
git clone https://github.com/google/googletest.git
cd googletest
git checkout <GOOGLETEST_COMMIT_SHA>
cd ..

# 4. Clone google benchmark at the pinned commit
git clone https://github.com/google/benchmark.git
cd benchmark
git checkout <BENCHMARK_COMMIT_SHA>
cd ..
```

---

## 4. Assembling the Directory Structure

prometheus-cpp's CMake build expects the third-party sources at hardcoded paths under `3rdparty/`. The `civetweb-3rdparty-config.cmake` file resolves to `${PROJECT_SOURCE_DIR}/3rdparty/civetweb/` — so the contents must be placed there exactly.

```bash
# Remove the empty submodule stub directories
rm -rf prometheus-cpp/3rdparty/civetweb
rm -rf prometheus-cpp/3rdparty/googletest    # only if you cloned it
rm -rf prometheus-cpp/3rdparty/benchmark     # only if you cloned it

# Move the separately cloned repos into the expected locations
mv civetweb prometheus-cpp/3rdparty/civetweb

# Only if building with tests:
# mv googletest prometheus-cpp/3rdparty/googletest
# mv benchmark  prometheus-cpp/3rdparty/benchmark
```

### Verifying the assembly

The most critical file is `civetweb.c` — if this doesn't exist at the expected path, the pull component will fail to build.

```bash
# These must all exist:
ls prometheus-cpp/3rdparty/civetweb/include/civetweb.h
ls prometheus-cpp/3rdparty/civetweb/src/civetweb.c
```

### Creating the transfer archive

Now create a self-contained archive to transfer to the build machine:

```bash
tar czf prometheus-cpp-v1.3.0-assembled.tar.gz prometheus-cpp/
```

Transfer `prometheus-cpp-v1.3.0-assembled.tar.gz` to the build machine via whatever channel your environment allows (internal artifact store, USB, SCP to a bastion host, etc.).

---

## 5. Prerequisites on the Build Machine

prometheus-cpp has a small dependency footprint. The core library needs only a C++14 compiler and pthreads. The **pull** component uses the bundled civetweb from the archive. The **push** component (Pushgateway client) requires libcurl, and optional gzip compression requires zlib — neither is bundled.

Install everything you might need (adjust the repository source to your internal APT mirror if applicable):

```bash
sudo apt update
sudo apt install build-essential cmake libcurl4-openssl-dev zlib1g-dev
```

If you only plan to use the **pull model** (HTTP endpoint scraped by Prometheus) without compression:

```bash
sudo apt install build-essential cmake
```

Verify your toolchain meets the minimums:

```bash
g++ --version    # GCC 7+ recommended (C++14 support required)
cmake --version  # Must be 3.14 or newer
```

Ubuntu 20.04+ ships CMake 3.16+ and GCC 9+, so both requirements are met out of the box.

---

## 6. Extracting and Verifying the Source on the Build Machine

```bash
tar xzf prometheus-cpp-v1.3.0-assembled.tar.gz
cd prometheus-cpp

# Verify the assembly is intact
ls 3rdparty/civetweb/src/civetweb.c        # must exist
ls 3rdparty/civetweb/include/civetweb.h     # must exist
```

If `3rdparty/civetweb/` is empty or missing these files, the archive was assembled incorrectly. Go back to Section 4 and redo the assembly.

---

## 7. Building from Source — All CMake Options Explained

Create a build directory and run CMake. Every meaningful option is listed below so you can tailor the build to your exact needs.

### Full-featured build

```bash
mkdir _build && cd _build

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_PULL=ON \
  -DENABLE_PUSH=ON \
  -DENABLE_COMPRESSION=ON \
  -DENABLE_TESTING=OFF \
  -DUSE_THIRDPARTY_LIBRARIES=ON \
  -DGENERATE_PKGCONFIG=ON

cmake --build . --parallel $(nproc)
```

### Minimal build (pull-only, no curl or zlib needed)

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DENABLE_PUSH=OFF \
  -DENABLE_COMPRESSION=OFF \
  -DENABLE_TESTING=OFF

cmake --build . --parallel $(nproc)
```

### Complete CMake options reference

| Option | Default | What it controls |
|--------|---------|-----------------|
| `BUILD_SHARED_LIBS` | `OFF` | Build `.so` shared libraries instead of `.a` static archives |
| `ENABLE_PULL` | `ON` | Build `prometheus-cpp::pull` (HTTP exposer via civetweb) |
| `ENABLE_PUSH` | `ON` | Build `prometheus-cpp::push` (Pushgateway client; **requires libcurl**) |
| `ENABLE_COMPRESSION` | `ON` | Enable gzip response compression (**requires zlib**) |
| `ENABLE_TESTING` | `ON` | Build unit tests (requires googletest submodule) |
| `USE_THIRDPARTY_LIBRARIES` | `ON` | Use bundled submodules for civetweb and googletest |
| `THIRDPARTY_CIVETWEB_WITH_SSL` | `OFF` | Enable TLS/SSL in the bundled civetweb (requires libssl-dev) |
| `OVERRIDE_CXX_STANDARD_FLAGS` | `ON` | Override compiler standard flags set externally |
| `GENERATE_PKGCONFIG` | `ON` (Unix) | Install `.pc` files for pkg-config consumers |

**Critical**: setting `ENABLE_PUSH=ON` without libcurl installed causes `Could NOT find CURL`. Similarly, `ENABLE_COMPRESSION=ON` without zlib produces `Could NOT find ZLIB`. Either install the packages from your internal mirror or disable the features.

> **Restricted network tip**: always set `ENABLE_TESTING=OFF`. Tests require googletest, and the test runner itself may attempt network fetches at configure time, which will hang or fail in an isolated environment.

---

## 8. Installing to a Prefix

After building, install the headers, libraries, and CMake config files:

```bash
# System-wide install to /usr/local (requires sudo)
sudo cmake --install .

# Or to a custom prefix (no sudo needed)
cmake --install . --prefix $HOME/prometheus-cpp-install
```

If you set `-DCMAKE_INSTALL_PREFIX` during configuration, a bare `cmake --install .` uses that prefix. The installed file layout:

```
<prefix>/
├── include/prometheus/             # All public headers
│   ├── counter.h
│   ├── gauge.h
│   ├── histogram.h
│   ├── summary.h
│   ├── info.h
│   ├── registry.h
│   ├── exposer.h                   # Pull component
│   ├── gateway.h                   # Push component
│   └── ...
├── lib/
│   ├── libprometheus-cpp-core.a
│   ├── libprometheus-cpp-pull.a
│   ├── libprometheus-cpp-push.a    # Only if ENABLE_PUSH=ON
│   ├── cmake/prometheus-cpp/       # CMake config files
│   │   ├── prometheus-cpp-config.cmake
│   │   └── prometheus-cpp-targets.cmake
│   └── pkgconfig/                  # pkg-config files
│       ├── prometheus-cpp-core.pc
│       └── prometheus-cpp-pull.pc
```

---

## 9. Importing into a Downstream CMake Project

The installed CMake config files make integration straightforward. The package exports **three namespaced targets** that automatically handle include paths, compile definitions, and transitive dependencies:

- **`prometheus-cpp::core`** — Registry, all metric types (Counter, Gauge, Histogram, Summary, Info), and serialization
- **`prometheus-cpp::pull`** — The `Exposer` HTTP server; transitively links `core` and civetweb
- **`prometheus-cpp::push`** — The `Gateway` Pushgateway client; transitively links `core` and libcurl

Because `prometheus-cpp::pull` already depends on `prometheus-cpp::core`, linking against `pull` alone is sufficient for most use cases. An explicit link to `core` is only needed if you use core without pull or push.

### Downstream CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_metrics_app LANGUAGES CXX)

find_package(prometheus-cpp CONFIG REQUIRED)

add_executable(my_metrics_app main.cpp)
target_link_libraries(my_metrics_app PRIVATE prometheus-cpp::pull)
```

If prometheus-cpp is installed to a non-standard prefix, tell CMake where to find it:

```bash
cmake -B build \
  -Dprometheus-cpp_DIR=$HOME/prometheus-cpp-install/lib/cmake/prometheus-cpp \
  -S .
cmake --build build
```

Alternatively, you can set `CMAKE_PREFIX_PATH` to point to the install root, which is often cleaner when you have multiple locally-installed libraries:

```bash
cmake -B build \
  -DCMAKE_PREFIX_PATH=$HOME/prometheus-cpp-install \
  -S .
```

The config file also sets variables you can query: `PROMETHEUS_CPP_ENABLE_PULL`, `PROMETHEUS_CPP_ENABLE_PUSH`, and `PROMETHEUS_CPP_USE_COMPRESSION`. These let you conditionally compile features:

```cmake
find_package(prometheus-cpp CONFIG REQUIRED)

if(PROMETHEUS_CPP_ENABLE_PUSH)
  add_executable(push_client push_client.cpp)
  target_link_libraries(push_client PRIVATE prometheus-cpp::push)
endif()
```

---

## 10. Vendoring as a Subdirectory (No Install Step)

In a restricted environment, you may prefer to **vendor the entire assembled prometheus-cpp source tree** directly into your project rather than installing it system-wide. This eliminates the need for `find_package` and keeps the build fully self-contained — no network access is needed at configure or build time.

### Project layout

```
my_project/
├── CMakeLists.txt
├── src/
│   └── main.cpp
└── third_party/
    └── prometheus-cpp/          # Extracted from the assembled archive
        ├── 3rdparty/
        │   └── civetweb/        # Manually placed, must be populated
        ├── core/
        ├── pull/
        ├── push/
        └── CMakeLists.txt
```

Copy or extract the assembled archive into `third_party/`:

```bash
cd my_project
mkdir -p third_party
tar xzf /path/to/prometheus-cpp-v1.3.0-assembled.tar.gz -C third_party/
```

### Top-level CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_project LANGUAGES CXX)

# Set prometheus-cpp options BEFORE add_subdirectory so they take effect
set(ENABLE_PUSH OFF CACHE BOOL "" FORCE)
set(ENABLE_COMPRESSION OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/prometheus-cpp)

add_executable(my_app src/main.cpp)
target_link_libraries(my_app PRIVATE prometheus-cpp::pull)
```

The critical rule here: **set options with `FORCE` before `add_subdirectory`**. Without `FORCE`, the subdirectory's own `option()` calls take precedence and the defaults (`ENABLE_PUSH=ON`, `ENABLE_TESTING=ON`) will kick in — triggering missing-dependency errors.

---

## 11. Complete Minimal Working Example

A full, self-contained example that creates a Counter, increments it in a loop, and exposes it at `http://127.0.0.1:8080/metrics` for Prometheus to scrape.

### Project structure

```
my_metrics_app/
├── CMakeLists.txt
└── main.cpp
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_metrics_app LANGUAGES CXX)

find_package(prometheus-cpp CONFIG REQUIRED)

add_executable(my_metrics_app main.cpp)
target_link_libraries(my_metrics_app PRIVATE prometheus-cpp::pull)
```

### main.cpp

```cpp
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>

#include <chrono>
#include <memory>
#include <thread>

int main() {
    // Create an HTTP server that listens on port 8080.
    // Prometheus scrapes this endpoint at /metrics.
    prometheus::Exposer exposer{"127.0.0.1:8080"};

    // Create a metrics registry. The Exposer holds a weak_ptr to it,
    // so you must keep this shared_ptr alive.
    auto registry = std::make_shared<prometheus::Registry>();

    // Build a counter family (a group of counters sharing a name
    // but distinguished by label values).
    auto& request_counter = prometheus::BuildCounter()
                                .Name("http_requests_total")
                                .Help("Total number of HTTP requests")
                                .Register(*registry);

    // Add specific label-dimension counters.
    auto& get_counter  = request_counter.Add({{"method", "GET"},  {"path", "/api"}});
    auto& post_counter = request_counter.Add({{"method", "POST"}, {"path", "/api"}});

    // Register the registry with the exposer so /metrics serves these metrics.
    exposer.RegisterCollectable(registry);

    // Simulate work — increment counters in a loop.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        get_counter.Increment();
        if (std::rand() % 3 == 0) {
            post_counter.Increment();
        }
    }

    return 0;
}
```

### Build and run

```bash
cd my_metrics_app
cmake -B build
cmake --build build
./build/my_metrics_app
```

In another terminal, verify metrics are exposed:

```bash
curl http://127.0.0.1:8080/metrics
```

You should see output in Prometheus text exposition format:

```
# HELP http_requests_total Total number of HTTP requests
# TYPE http_requests_total counter
http_requests_total{method="GET",path="/api"} 5
http_requests_total{method="POST",path="/api"} 2
```

### Using Gauge and Histogram alongside Counter

The API pattern is identical across metric types:

```cpp
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>

// ...inside main():
auto registry = std::make_shared<prometheus::Registry>();

// Counter — monotonically increasing
auto& req_family = prometheus::BuildCounter()
    .Name("requests_total").Help("Total requests").Register(*registry);
auto& req = req_family.Add({{"endpoint", "/health"}});
req.Increment();

// Gauge — goes up and down
auto& temp_family = prometheus::BuildGauge()
    .Name("temperature_celsius").Help("Current temperature").Register(*registry);
auto& temp = temp_family.Add({{"location", "server_room"}});
temp.Set(22.5);

// Histogram — observations bucketed by value
auto& lat_family = prometheus::BuildHistogram()
    .Name("request_duration_seconds").Help("Latency histogram").Register(*registry);
auto& lat = lat_family.Add(
    {{"method", "GET"}},
    prometheus::Histogram::BucketBoundaries{0.01, 0.05, 0.1, 0.5, 1.0, 5.0}
);
lat.Observe(0.042);
```

---

## 12. Troubleshooting Common Issues

**"CIVETWEB_INCLUDE_DIR not found"** — The `3rdparty/civetweb/` directory is empty or was not populated correctly during assembly. Verify that `3rdparty/civetweb/include/civetweb.h` and `3rdparty/civetweb/src/civetweb.c` exist. This is the most common error when assembling repos manually — double-check that you moved the civetweb repo contents into the correct path (not a nested directory like `3rdparty/civetweb/civetweb-1.16/`).

**"Could NOT find CURL"** — You have `ENABLE_PUSH=ON` (the default) but libcurl is not installed. Fix: install `libcurl4-openssl-dev` from your internal mirror, or set `-DENABLE_PUSH=OFF`.

**"Could NOT find ZLIB"** — You have `ENABLE_COMPRESSION=ON` (the default) but zlib is not installed. Fix: install `zlib1g-dev` from your internal mirror, or set `-DENABLE_COMPRESSION=OFF`.

**"undefined reference to prometheus::Exposer"** — You linked against `prometheus-cpp::core` but not `prometheus-cpp::pull`. The `Exposer` class lives in the pull library. Fix: change `target_link_libraries` to use `prometheus-cpp::pull` (which transitively pulls in `core`).

**Compilation errors about C++14 features** — v1.2.0+ requires C++14. If your project forces `-std=c++11`, you will get compilation failures. Fix: set `CMAKE_CXX_STANDARD` to 14 or higher in your project.

**CMake configure hangs or times out** — In restricted environments, leaving `ENABLE_TESTING=ON` can cause CMake to attempt network fetches for test dependencies. Always set `-DENABLE_TESTING=OFF`.

**`find_package` cannot locate prometheus-cpp** — If installed to a custom prefix, you must either set `-Dprometheus-cpp_DIR=<prefix>/lib/cmake/prometheus-cpp` or `-DCMAKE_PREFIX_PATH=<prefix>` when configuring your downstream project.

**Civetweb version mismatch causing build errors** — If you used a civetweb version different from the one pinned in prometheus-cpp's submodule reference, API incompatibilities can cause compilation or linking errors. Always use `git ls-tree HEAD 3rdparty/` against the prometheus-cpp tag to find the exact commit SHA (see Section 2).

**Nested directory after extraction** — A common assembly mistake when using GitHub release tarballs instead of git clones: after extracting a tarball, the contents are inside a directory like `civetweb-1.16/`. You need the *contents* of that directory at `3rdparty/civetweb/`, not `3rdparty/civetweb/civetweb-1.16/`. If using tarballs instead of git clones, extract and move accordingly:
```bash
tar xzf civetweb-v1.16.tar.gz
mv civetweb-1.16 prometheus-cpp/3rdparty/civetweb
```
