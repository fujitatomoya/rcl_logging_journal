# Design: `rcl_logging_journal`

**Alternative ROS 2 logging backend using the systemd-journald native protocol**

- Status: Implemented (0.1.0), kept in sync with the code in this repository
- Author: Tomoya Fujita
- Inspired by: [fujitatomoya/rcl_logging_syslog](https://github.com/fujitatomoya/rcl_logging_syslog)
- License: Apache-2.0

---

## 1. Overview

`rcl_logging_journal` is an alternative logging backend implementation for [ROS 2](https://github.com/ros2), plugged in via [`rcl_logging_interface`](https://github.com/ros2/rcl_logging/tree/rolling/rcl_logging_interface), exactly like `rcl_logging_syslog` and the default `rcl_logging_spdlog`.

Instead of `SYSLOG(3)`, it uses the **journald native protocol** via `sd_journal_sendv()` (libsystemd) to write **structured, indexed log records** directly into `systemd-journald`.

The main objective is:

> **Enabling ROS 2 logging with the native Linux system journal: structured per-node filtering with `journalctl`, built-in rotation, vacuuming, binary storage with field deduplication and compression, with zero additional daemons or configuration.**

```
+----------------------+
|  ROS 2 Application   |
|  (rclcpp / rclpy)    |
+----------+-----------+
           | rcl / rcl_logging_interface
+----------v-----------+
| rcl_logging_journal  |   sd_journal_sendv()
+----------+-----------+
           | AF_UNIX SOCK_DGRAM  /run/systemd/journal/socket
           | (memfd + sealing fallback for large records)
+----------v-----------+
|   systemd-journald   |  append-only binary journal files
+----------+-----------+  /var/log/journal/<machine-id>/*.journal
           |
   +-------+--------+----------------------+
   |                |                      |
journalctl     systemd-journal-       (optional, out of scope
(filter/tail/   remote/upload,         for v1: FluentBit
 rotate/vacuum) journal-gatewayd       systemd input, rsyslog
                                       imjournal, Loki, ...)
```

### 1.1 Why journald and not (only) syslog?

`rcl_logging_syslog` proved the concept of routing ROS 2 logs into the system logging stack. However, the `SYSLOG(3)` API is intentionally minimal: it carries only *facility*, *priority*, and a *flat pre-formatted text message*. Everything downstream (per-node file separation, filtering, rotation) must be reconstructed by parsing text in `rsyslogd` templates.

journald's native protocol removes that limitation:

| | `rcl_logging_syslog` (SYSLOG(3)) | `rcl_logging_journal` (sd_journal_sendv) |
|---|---|---|
| Record model | flat text line | arbitrary `KEY=VALUE` fields, all indexed |
| Per-node filtering | rsyslog template parsing (`programname`) | native: `journalctl ROS2_NODE_NAME=talker` |
| Extra daemon required | `rsyslogd` (config in `/etc/rsyslog.d/`) | none, journald runs on every systemd host |
| Rotation / retention | logrotate / rsyslog config | built-in (`SystemMaxUse=`, `journalctl --vacuum-*`) |
| Storage | plain text files | append-only binary, field dedup, optional zstd/lz4/xz |
| Trusted metadata | none (client can spoof ident) | `_PID`, `_UID`, `_COMM`, `_EXE`, `_CMDLINE`, `_BOOT_ID`, `_MACHINE_ID`, `_HOSTNAME` added by journald from kernel-verified credentials |
| Query tooling | `grep`, `less` over rotated files | `journalctl` with time ranges, priorities, field matches, boot filtering, `-o json` export |
| Container to host | bind `/dev/log` | bind `/run/systemd/journal/socket` |

This directly addresses the well-known gap that ROS 2 ships no log *consumption* utility: the default spdlog backend writes files under `~/.ros/log` with no equivalent of `journalctl` to tail, filter, correlate across nodes, or manage retention.

### 1.2 What users get, concretely

```bash
# follow one node's log live
journalctl -f ROS2_NODE_NAME=talker

# all WARN and above from any ROS 2 node since the last boot
journalctl -b -p warning ROS2_DISTRO=rolling

# everything a specific process logged, with trusted PID/exe metadata
journalctl _PID=31348 -o verbose

# export a node's log as JSON for offline analysis
journalctl ROS2_NODE_NAME=talker -o json > talker.json

# retention management, no logrotate configuration needed
sudo journalctl --vacuum-time=7d
sudo journalctl --vacuum-size=500M
```

---

## 2. Research Summary: journald internals & performance vs syslog

This section records the public/technical information gathered to justify the design and to define what the benchmark harness (Section 8) must actually measure. Sources: systemd documentation ([Native Journal Protocol](https://systemd.io/JOURNAL_NATIVE_PROTOCOL/), [Journal File Format](https://systemd.io/JOURNAL_FILE_FORMAT/)), `sd_journal_send(3)`, `journald.conf(5)`, rsyslog documentation (`imjournal`, `imuxsock`), and Red Hat performance guidance.

### 2.1 Transport path comparison

**syslog path (rcl_logging_syslog on a systemd host):**
On modern systemd distributions, journald owns `/dev/log`. A `syslog(3)` call therefore traverses:

```
app: snprintf() text -> sendmsg(/dev/log)
  -> journald: parse RFC3164 text, extract PRI/ident/pid, attach trusted fields,
               write to journal, THEN forward a re-serialized copy
  -> /run/systemd/journal/syslog -> rsyslogd (imuxsock): parse text AGAIN,
               apply templates/rulesets -> write text file (+ optional TCP forward)
```

The message is serialized, parsed, and re-serialized multiple times, and two daemons handle every record. Red Hat's tuning guidance explicitly notes that the default layered journald+rsyslog setup is "not optimal" for data throughput and memory in performance-critical cases.

**journald native path (this implementation):**

```
app: KEY=VALUE iovec array -> sendmsg(/run/systemd/journal/socket)
  -> journald: no text parsing (fields arrive pre-structured),
               attach trusted fields, append to journal. Done.
```

One daemon, zero text re-parsing, one copy fewer. Per the [Native Journal Protocol](https://systemd.io/JOURNAL_NATIVE_PROTOCOL/) spec, the client first attempts a plain `AF_UNIX/SOCK_DGRAM` datagram; if the payload exceeds the datagram limit (`EMSGSIZE`), it retries by handing journald a **sealed memfd**, a zero-copy transfer of the record memory. libsystemd implements this fallback automatically inside `sd_journal_sendv()`. The spec also recommends clients bump `SO_SNDBUF` to delay blocking I/O, which libsystemd does.

### 2.2 Journal file format: what "binary" actually buys

Key properties from the [Journal File Format](https://systemd.io/JOURNAL_FILE_FORMAT/) specification:

- **Append-only, mmap-based** files. Writes append objects; reads are mmap + hash-table lookups. No daemon round-trip is needed to *read* (journalctl reads files directly).
- **Field-level deduplication.** Every `KEY=VALUE` payload is a `DATA` object addressed via a hash table. Identical payloads are stored **once** and referenced by every entry. For ROS 2 this is significant: `ROS2_NODE_NAME=talker`, `SYSLOG_IDENTIFIER=talker`, `PRIORITY=6`, `ROS2_DISTRO=...` repeat on every single log line; in the journal they cost one object plus a fixed-size reference per entry, whereas in a text log each line carries the full prefix bytes again.
- **Compression of large payloads.** `DATA` objects above a threshold (default 512 bytes, `Compress=` in `journald.conf`) are compressed with **zstd** (default in modern systemd), lz4, or xz, indicated by the `HEADER_INCOMPATIBLE_COMPRESSED_ZSTD/LZ4/XZ` header flags. Typical short ROS log lines stay uncompressed (fast path); large payloads (backtraces, parameter dumps) get compressed automatically.
- **Compact mode** (`HEADER_INCOMPATIBLE_COMPACT`, systemd >= 252) shrinks per-entry overhead on disk.
- **Indexed lookup.** Entries are cross-referenced from per-field entry arrays, so `journalctl ROS2_NODE_NAME=talker` is an index walk, not a scan/grep over rotated text files.
- **Built-in rotation and vacuum.** Size- and time-based (`SystemMaxUse=`, `SystemMaxFileSize=`, `MaxRetentionSec=`), plus online `journalctl --vacuum-*`. Rotated files remain fully queryable through the same `journalctl` command, unlike rotated/gzipped text logs.

### 2.3 Honest performance expectations (what to verify, not assume)

The intuition "binary format, therefore faster and smaller" needs qualification. The project commits to measuring rather than claiming:

1. **Client-side call latency** (`sd_journal_sendv` vs `syslog(3)` vs spdlog file write): expected to be in the same order of magnitude, all end in one `sendmsg()`/`write()`. `sd_journal_sendv` avoids client-side `snprintf` of a syslog header but serializes multiple fields. This is the number that matters for real-time-ish ROS executors, and it is Benchmark B1.
2. **End-to-end pipeline cost**: native journald eliminates the second daemon (rsyslogd) and double text parsing. System-wide CPU per log line is expected to drop vs the syslog/rsyslog chain. Benchmark B2.
3. **Sustained ingestion throughput**: rsyslog is famously optimized for raw ingestion rates ("rocket-fast"); journald is not always the throughput winner, and rsyslog's own `imjournal` docs describe *reading* the journal as "relatively performance-intense" compared to socket input. For very high message rates journald's write path may saturate earlier than a tuned rsyslog file sink. Benchmark B3 quantifies the crossover.
4. **Storage footprint**: field dedup + compact mode + zstd for large payloads is expected to beat plain text for typical ROS traffic (highly repetitive prefixes), but this depends on message entropy. Benchmark B4 compares bytes-on-disk for identical workloads across spdlog / syslog+rsyslog file sink / journald.
5. **fsync behavior**: journald syncs on a timer (`SyncIntervalSec=`, default 5 min) but **immediately for CRIT/ALERT/EMERG**, meaning ROS `FATAL` (mapped to `LOG_CRIT`) gets durability by default, while INFO-level bursts remain buffered and fast. This is a *feature* to document, and Benchmark B1 reports FATAL separately.

### 2.4 Known operational caveats (documented, not hidden)

- **Rate limiting.** journald applies per-service rate limits (`RateLimitIntervalSec=30s`, `RateLimitBurst=10000` by default; scaled by free disk). A chatty ROS node can hit this and see `Suppressed N messages` in the journal. The README and the retention tutorial show how to raise or disable it (globally in `journald.conf`, or per-unit via `LogRateLimitBurst=` when nodes run as systemd services).
- **Non-systemd environments.** Alpine-based containers, non-systemd distros, and bare containers have no journald. Behavior in that case is defined (Section 4.6): fail initialization with a clear error by default, or become a no-op with `RCL_LOGGING_JOURNAL_STRICT=0` since a fallback to stderr already exists in rcl's logging output flags.
- **Datagram size.** Very large single log lines take the memfd fallback path (slower but correct). Not a practical limit for ROS logs; the test suite exercises a 300 KiB record.
- **journal namespaces** (systemd >= 245, `systemd-journald@<ns>.service`, `journalctl --namespace=`) can isolate high-volume robot logs from the OS journal with independent rate/size limits. Documented as an advanced topic in the retention tutorial, not required for v1.

---

## 3. Requirements

### 3.1 Functional

- FR1: Implement the full `rcl_logging_interface` contract: `rcl_logging_external_initialize()`, `rcl_logging_external_log()`, `rcl_logging_external_set_logger_level()`, `rcl_logging_external_shutdown()`.
- FR2: Every log record carries, at minimum: `MESSAGE`, `PRIORITY` (mapped severity), `SYSLOG_IDENTIFIER` (executable name, for `journalctl -t` compatibility), and `ROS2_NODE_NAME` (the rcutils logger name) as an indexed custom field.
- FR3: Severity mapping between RCUTILS levels and syslog/journald priorities (see 4.3), including severity filtering inside the backend (records below the configured level are dropped before any socket I/O).
- FR4: Runtime dynamic loading via `RCL_LOGGING_IMPLEMENTATION=rcl_logging_journal` on Lyrical+; static linking path (rebuild `rcl`) for Kilted/Jazzy/Humble, identical to `rcl_logging_syslog`.
- FR5: Work inside containers by bind-mounting the host native socket (`-v /run/systemd/journal/socket:/run/systemd/journal/socket`), storing container logs in the **host** journal.
- FR6: Configuration via environment variables only (no config files owned by this package), consistent with `rcl_logging_syslog`'s `RCL_LOGGING_SYSLOG_FACILITY` approach.

### 3.2 Non-functional

- NFR1: No heap allocation on the hot logging path for typical records (stack scratch buffers, heap fallback only for messages above 4 KiB or logger names above 512 bytes) beyond what `sd_journal_sendv` itself performs; no locks besides what libsystemd requires (it is thread-safe).
- NFR2: No additional daemon, service file, or `/etc` configuration required for the default experience on a stock Ubuntu host.
- NFR3: Graceful, well-defined behavior when journald is absent (initialize returns `RCL_LOGGING_RET_ERROR` with a clear rcutils error message, or no-op in non-strict mode).
- NFR4: CI parity with `rcl_logging_syslog`: per-distro branches and workflows (humble/jazzy/kilted/lyrical/rolling + nightly).

### 3.3 Out of scope (v1)

- Forwarding/aggregation (FluentBit, Loki, systemd-journal-remote), covered as *tutorials* only, since journald makes them orthogonal to the backend.
- Windows/macOS: package is Linux-only; CMake guards accordingly.
- Journal namespace management.

---

## 4. Detailed Design

### 4.1 Repository layout (inherited from `rcl_logging_syslog`)

```
rcl_logging_journal/
├── .github/
│   ├── workflows/
│   │   ├── humble.yaml          # container: tomoyafujita/ros2dev:humble, build+test
│   │   ├── jazzy.yaml
│   │   ├── kilted.yaml
│   │   ├── lyrical.yaml
│   │   ├── rolling.yaml
│   │   ├── nightly.yaml         # scheduled matrix over all distro branches
│   │   ├── abi.yaml             # ABI check of librcl_logging_journal.so on PRs
│   │   ├── codespell.yaml
│   │   ├── stale.yml
│   │   └── mirror-rolling-to-main.yaml
│   ├── ISSUE_TEMPLATE/
│   └── dependabot.yml           # github-actions and pip group updates
├── .mergify/
│   └── config.yml               # backport rules: rolling -> lyrical/kilted/jazzy/humble
├── config/
│   └── ros2-journald.conf       # OPTIONAL drop-in example for /etc/systemd/journald.conf.d/
│                                #   (rate-limit & retention tuning; NOT required to run)
├── doc/
│   ├── design.md                # this document
│   ├── overview.md              # marp slide deck source
│   ├── overview.html            # rendered deck (raw.githack publishing flow)
│   ├── images/
│   │   └── architecture_overview.svg
│   ├── presentation/            # conference/meetup decks
│   └── tutorials/
│       ├── Journalctl_Basics.md         # per-node filtering, priorities, JSON export, vacuum
│       ├── Container_Host_Journal.md    # bind /run/systemd/journal/socket
│       └── Rate_Limits_And_Retention.md # journald.conf tuning for robots
├── scripts/
│   ├── github_workflows.sh      # CI entry point (full source build + standalone journald)
│   └── benchmark/               # Section 8 harness
│       ├── bench_backend.cpp    # drives rcl_logging_external_* directly (dlopen)
│       ├── CMakeLists.txt
│       ├── run_matrix.sh        # spdlog vs syslog vs journald matrix
│       └── report.py            # latency percentiles, CPU, bytes-on-disk -> markdown
├── src/
│   └── rcl_logging_journal.cpp
├── test/
│   ├── test_logging_journald.cpp
│   └── benchmark/
│       └── benchmark_logging_interface.cpp   # performance_test_fixture micro benchmark
├── CMakeLists.txt
├── package.xml
├── CHANGELOG.rst
├── CODEOWNERS
├── CONTRIBUTING.md
├── LICENSE                      # Apache-2.0
├── README.md
├── codespell.cfg
├── codespell_dictionary.txt
└── codespell_whitelist.txt
```

### 4.2 API mapping (`rcl_logging_interface` to libsystemd)

| rcl_logging_interface | journald backend behavior |
|---|---|
| `rcl_logging_external_initialize(file_name_prefix, config_file, allocator)` | Validate the allocator. Idempotent when already initialized. Warn if a `config_file` is given (journald has no client side configuration file). Resolve `SYSLOG_IDENTIFIER`: `RCL_LOGGING_JOURNAL_IDENTIFIER`, else `file_name_prefix` (`--log-file-name`), else the executable name. Cache `ROS2_DISTRO` from `$ROS_DISTRO`. Parse and validate `RCL_LOGGING_JOURNAL_EXTRA_FIELDS`. Probe journald availability with `access("/run/systemd/journal/socket", W_OK)`. Return `RCL_LOGGING_RET_ERROR` with an actionable message if unavailable in strict mode. No connection to hold: `sd_journal_sendv` manages its own socket. |
| `rcl_logging_external_log(severity, name, msg)` | Map severity (4.3); drop if above the current threshold. Compose `MESSAGE=` and `ROS2_NODE_NAME=` in stack buffers, append the pre-built constant iovecs, call `sd_journal_sendv()` once. `name` may be NULL/empty (rcl logs some records with no logger): omit `ROS2_NODE_NAME`, keep `SYSLOG_IDENTIFIER`. Failures are counted, never retried or blocked on. |
| `rcl_logging_external_set_logger_level(name, level)` | v1: single global threshold (matches `rcl_logging_syslog` behavior via `setlogmask` semantics). Stored in an atomic; `UNSET` disables backend filtering. A per-logger map is a possible v2 (rcl only invokes this for the default logger today). |
| `rcl_logging_external_shutdown()` | Report the number of undeliverable records (if any) through rcutils logging, reset state. Nothing to flush: every `sd_journal_sendv` is a completed datagram; durability is journald's job (and immediate for CRIT+). |

### 4.3 Severity mapping

| RCUTILS severity | journald `PRIORITY` | journalctl `-p` name | `ROS2_SEVERITY` |
|---|---|---|---|
| `DEBUG (10)` | 7 `LOG_DEBUG` | debug | `DEBUG` |
| `INFO (20)`  | 6 `LOG_INFO` | info | `INFO` |
| `WARN (30)`  | 4 `LOG_WARNING` | warning | `WARN` |
| `ERROR (40)` | 3 `LOG_ERR` | err | `ERROR` |
| `FATAL (50)` | 2 `LOG_CRIT` | crit | `FATAL` |
| `UNSET/other` | 6 `LOG_INFO` | info | `UNSET` |

Filtering compares journald priorities: a record passes when its priority is numerically less than or equal to the threshold derived from `set_logger_level()` (`UNSET` level means "everything").

Note: mapping FATAL to `LOG_CRIT` means journald **fsyncs immediately** on FATAL, free crash-durability for the records you care most about.

### 4.4 Configuration (environment variables)

Following the `RCL_LOGGING_SYSLOG_FACILITY` precedent: env-only, sane defaults:

| variable | default | purpose |
|---|---|---|
| `RCL_LOGGING_JOURNAL_IDENTIFIER` | executable short name (or `--log-file-name` prefix) | overrides `SYSLOG_IDENTIFIER` (useful when many nodes share one process, e.g. component containers) |
| `RCL_LOGGING_JOURNAL_EXTRA_FIELDS` | *(empty)* | `;`-separated static `KEY=VALUE` pairs attached to every record (e.g. `ROBOT_ID=amr-07;FLEET=tokyo`), fully indexed, enables fleet-level `journalctl` queries. Keys must follow journald rules (`[A-Z0-9_]`, no leading `_`, at most 64 characters), must not collide with fields set by the backend, at most 32 entries. Invalid input fails initialization with `RCL_LOGGING_RET_INVALID_ARGUMENT`. |
| `RCL_LOGGING_JOURNAL_STRICT` | `1` | `1`: fail init when journald socket is absent. `0`: init succeeds, backend becomes no-op (stderr/rosout continue to work via rcl's other output flags) |
| `RCL_LOGGING_JOURNAL_SOCKET_PATH` | `/run/systemd/journal/socket` | path probed at initialization to decide whether journald is available. Diagnostic/testing knob: libsystemd itself always sends to the default path. |

Deliberately **no** option that duplicates journald's own knobs (rotation, compression, rate limits); those belong in `journald.conf`, and the `config/ros2-journald.conf` drop-in documents recommended robot settings.

### 4.5 Record schema

Every record sent via one `sd_journal_sendv()` call:

```
MESSAGE=<msg as received from rcl (already includes rcutils-formatted content)>
PRIORITY=<mapped severity>
ROS2_SEVERITY=<DEBUG|INFO|WARN|ERROR|FATAL|UNSET>   # original ROS wording, indexed
ROS2_NODE_NAME=<logger name>            # primary filtering key (omitted if unset)
SYSLOG_IDENTIFIER=<identifier>          # journalctl -t compatibility
ROS2_DISTRO=<$ROS_DISTRO if set>
<EXTRA_FIELDS...>                       # from RCL_LOGGING_JOURNAL_EXTRA_FIELDS
```

journald adds trusted `_PID`, `_UID`, `_GID`, `_COMM`, `_EXE`, `_CMDLINE`, `_BOOT_ID`, `_MACHINE_ID`, `_HOSTNAME`, `_TRANSPORT=journal`, and `_SOURCE_REALTIME_TIMESTAMP` automatically.

`CODE_FILE` / `CODE_LINE` / `CODE_FUNC` are deliberately **not** emitted: `rcl_logging_interface` does not pass the call site, and libsystemd's default macro would otherwise stamp the location inside the backend itself (`SD_JOURNAL_SUPPRESS_LOCATION` is defined). Should the interface ever carry the location, these become the natural v2 fields.

Field-name rationale: `ROS2_*` prefix avoids collision with journald's reserved `_*` (trusted) namespace and with common `SYSLOG_*` fields; uppercase+underscore is the journal convention; every custom field is automatically indexed for matching.

### 4.6 Failure & fallback semantics

- `sd_journal_sendv()` failure at runtime (journald restarted, socket ENOENT): the record is lost, a counter is incremented, the count is reported once at shutdown. Never crash, never spin, never block. rcl already tolerates backend errors.
- No journald at init: controlled by `RCL_LOGGING_JOURNAL_STRICT` (4.4). Strict mode gives users an immediate, actionable error instead of silently lost logs.
- Not Linux / no libsystemd at build time: CMake hard-fails with a clear message (`find_package(PkgConfig)` + `pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)`).

### 4.7 Implementation notes (`src/rcl_logging_journal.cpp`)

- Constant fields (`SYSLOG_IDENTIFIER`, `ROS2_DISTRO`, extra fields) are composed once at initialization into owning `std::string`s plus a pre-built `struct iovec` array.
- `PRIORITY=n` and `ROS2_SEVERITY=x` are compile time string literals selected by a `switch` on the severity, no formatting at all on the hot path.
- `MESSAGE=` and `ROS2_NODE_NAME=` are composed with `memcpy` into stack buffers (4 KiB and 512 B) with a heap fallback for larger values.
- Per call work is therefore: threshold check, two `memcpy`s, one `memcpy` of the constant iovecs, one `sd_journal_sendv()` (one `sendmsg`).
- `sd_journal_sendv()` is used instead of the printf-style `sd_journal_send()` to avoid the `vasprintf` per field that the latter performs.

### 4.8 `package.xml` / `CMakeLists.txt`

```xml
<!-- package.xml (format 3) -->
<name>rcl_logging_journal</name>
<description>Implementation of rcl_logging API for a systemd-journald backend.</description>
<license>Apache License 2.0</license>
<buildtool_depend>ament_cmake_ros</buildtool_depend>
<buildtool_depend>pkg-config</buildtool_depend>
<depend>rcl_logging_interface</depend>
<depend>rcpputils</depend>
<depend>rcutils</depend>
<depend>libsystemd-dev</depend>   <!-- rosdep key: libsystemd-dev -->
<exec_depend>systemd</exec_depend>
<test_depend>ament_cmake_gtest</test_depend>
<test_depend>ament_lint_auto</test_depend>
<test_depend>ament_lint_common</test_depend>
<test_depend>performance_test_fixture</test_depend>
```

CMake essentials, mirroring `rcl_logging_syslog`'s structure:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
add_library(${PROJECT_NAME} src/rcl_logging_journal.cpp)
target_link_libraries(${PROJECT_NAME}
  PUBLIC rcl_logging_interface::rcl_logging_interface
  PRIVATE PkgConfig::SYSTEMD rcpputils::rcpputils rcutils::rcutils)
# exported as an rcl_logging implementation so both dynamic loading (Lyrical+)
# and RCL_LOGGING_IMPLEMENTATION static rebuild (<=Kilted) work
ament_export_dependencies(rcl_logging_interface)
ament_export_libraries(${PROJECT_NAME})
ament_export_targets(${PROJECT_NAME})
```

---

## 5. Testing

### 5.1 Unit / integration test (`test/test_logging_journald.cpp`)

Same philosophy as `rcl_logging_syslog`'s colcon test (which writes via the backend and verifies file existence/content under `/var/log/ros/...`), adapted to the journal:

1. `rcl_logging_external_initialize()` returns OK when journald is present (CI containers run `systemd-journald` standalone; see 6). Invalid and failing allocators are rejected.
2. Log a set of unique, PID and random token tagged messages at each severity through `rcl_logging_external_log()`, using a unique logger name per test.
3. Read back via the **`sd_journal` read API** (`sd_journal_open` + `sd_journal_add_match("ROS2_NODE_NAME=<token>")`), asserting: message content, `PRIORITY` mapping, `ROS2_SEVERITY`, presence of `ROS2_NODE_NAME`, `SYSLOG_IDENTIFIER`, `ROS2_DISTRO`, absence of `CODE_*`, and journald's trusted `_PID` / `_TRANSPORT=journal`.
4. Severity threshold test: for every (level, severity) pair, assert exactly the expected subset is stored.
5. Records without a logger name are stored without `ROS2_NODE_NAME`.
6. `SYSLOG_IDENTIFIER` precedence: env override, `--log-file-name` prefix, executable name.
7. Extra fields are stored and indexed; invalid extra fields fail initialization.
8. Strict-mode test: point `RCL_LOGGING_JOURNAL_SOCKET_PATH` at a nonexistent path, assert init failure with `STRICT=1` (error message names the path) and no-op success with `STRICT=0` (nothing stored).
9. Fallback path test: log one 300 KiB message to exercise the memfd path and journald compression.
10. Long hierarchical logger names (> 512 B) exercise the heap fallback.

Reading back with the same library we write with keeps the test hermetic (no dependency on `journalctl` output formatting), but one smoke test also shells out to `journalctl ROS2_NODE_NAME=<token> -o json` to guard the real user experience.

### 5.2 Lint

`ament_lint_common` (copyright, cpplint, uncrustify, lint_cmake, xmllint) + codespell (config inherited verbatim).

---

## 6. CI: GitHub Actions (inherited from `rcl_logging_syslog`)

Branch-per-distro model with matching workflows and Mergify backports:

| Branch | Workflow | Container | Notes |
|---|---|---|---|
| `rolling` (dev) | `rolling.yaml` | `tomoyafujita/ros2dev:rolling` | dynamic loading path |
| `lyrical` | `lyrical.yaml` | `tomoyafujita/ros2dev:lyrical` | dynamic loading path |
| `kilted` | `kilted.yaml` | `tomoyafujita/ros2dev:kilted` | static link |
| `jazzy` | `jazzy.yaml` | `tomoyafujita/ros2dev:jazzy` | static link |
| `humble` | `humble.yaml` | `tomoyafujita/ros2dev:humble` | static link |
| all (cron) | `nightly.yaml` | matrix over the above | build against ros2.repos of each distro |

The one journald-specific CI problem: **containers have no running journald**. Two supported strategies, both exercised:

- **Primary (hermetic):** `scripts/github_workflows.sh` installs `libsystemd-dev systemd`, ensures `/etc/machine-id`, creates `/run/systemd/journal` and starts `/usr/lib/systemd/systemd-journald` in the background before `colcon test` (it runs fine as a plain process and binds its sockets itself), analogous to how `rcl_logging_syslog` CI starts `rsyslogd -n -iNONE` in containers.
- **Secondary (documented, not CI):** bind the host socket. This is the recommended *user* pattern for robots (Section 7.2) and is covered by the container tutorial.

Plus: `dependabot.yml`, README badges per workflow, `.mergify/config.yml` backport rules, `abi.yaml` (ABI diff of `librcl_logging_journal.so` on PRs), `codespell.yaml`, `stale.yml`, all copied and renamed.

---

## 7. Documentation deliverables

### 7.1 README.md

Mirrors `rcl_logging_syslog` structure: badges, intro, **Motivation** (ROS 2 has no log query utility; journalctl is that utility), Demonstration, Supported ROS Distribution table (Rolling/Lyrical dynamic, Kilted/Jazzy/Humble static), `rcl_logging_implementation` section (verbatim inheritance), Installation (prereqs: `libsystemd-dev`; **no daemon setup needed on hosts**, the contrast with rsyslog setup is a selling point), Build Source (dynamic + static variants), Test, Configuration (env var table), Usage/Examples, Reference.

### 7.2 Tutorials (`doc/tutorials/`)

1. **`Journalctl_Basics.md`**: per-node filter, severity filter, time windows, `-b` boot correlation (invaluable after a robot crash/reboot), JSON export, `--vacuum-time/--vacuum-size`, persistent vs volatile storage (`Storage=persistent`, `mkdir /var/log/journal`).
2. **`Container_Host_Journal.md`**: the key container scenario, binding the *native* socket (not `/dev/log`), what trusted fields look like from a container, rootless/permission notes, and the standalone journald alternative.
3. **`Rate_Limits_And_Retention.md`**: `RateLimitIntervalSec/RateLimitBurst`, per-unit `LogRateLimitBurst=`, `SystemMaxUse`, `Compress=`, `SyncIntervalSec`, journal namespaces, and the shipped `config/ros2-journald.conf` drop-in with commented robot-oriented defaults.

### 7.3 Overview slide deck (`doc/overview.md` / `doc/overview.html`)

Same publishing flow as the syslog repo (marp markdown, HTML deck viewable via raw.githack). Outline:

1. ROS 2 logging subsystem & `rcl_logging_interface`
2. Pain: spdlog files with no query/rotation tooling
3. journald in 3 slides: native protocol, journal file format (dedup/compression/indexing), journalctl
4. `rcl_logging_journal` architecture diagram
5. Demo: per-node journalctl filtering; container to host journal
6. Performance: benchmark methodology + measured results (from Section 8)
7. Positioning vs `rcl_logging_syslog` (complementary: syslog backend when you want rsyslog pipelines/FluentBit; journald backend when you want local observability with zero config)

### 7.4 design docs

This `design.md` lives at `doc/design.md` and is kept current with implementation PRs.

---

## 8. Benchmark plan (`scripts/benchmark/`)

Goal: replace the "binary should be faster" assumption with reproducible numbers, published in README + deck.

**Harness:** `bench_backend` is a small C++ driver that `dlopen()`s any backend's shared library and calls the `rcl_logging_external_*` symbols directly (no ROS graph, no rosout, no stdout, isolating backend cost). `run_matrix.sh` runs the matrix and `report.py` renders markdown tables.

| ID | Question | Method | Metrics |
|---|---|---|---|
| B1 | Client call cost | 1 M calls/backend, msg sizes 64 B / 256 B / 4 KiB, severities INFO & FATAL | p50/p95/p99/p99.9 latency per `rcl_logging_external_log`, calls/sec |
| B2 | System cost per record | 100 k msgs; app CPU from `getrusage`, daemon CPU (journald, rsyslogd) from `/proc/<pid>/stat` deltas | total CPU-ms per 1 k records |
| B3 | Sustained throughput & loss | paced ramp 1 k to 200 k msg/s, 5 s each; count delivered records in the sink, journald `Suppressed N messages` | max lossless rate, suppressed count |
| B4 | Storage footprint | identical 1 M-record workload (70 % 64 B INFO, 25 % 256 B INFO, 5 % 4 KiB WARN); `journalctl --disk-usage` vs text file sizes | bytes on disk, bytes per record |
| B5 | Query performance | "get all WARN+ for logger X": `journalctl` match vs `grep` over text | wall time, first and second run |

Backends compared: `rcl_logging_spdlog` (baseline), `rcl_logging_syslog` (+rsyslogd file sink), `rcl_logging_journal` (this work). Environment recorded in `environment.txt` (CPU, governor, systemd version, effective `journald.conf`); `report.py` emits the markdown tables committed to `doc/`.

---

## 9. Milestones

| Phase | Deliverables | Status |
|---|---|---|
| M1 Skeleton | repo from syslog template, CMake/package.xml, CI with journald-in-container, minimal backend (MESSAGE/PRIORITY/IDENTIFIER) | done |
| M2 Feature complete | full field schema, env config, strict/fallback semantics, gtest suite incl. journal read-back, lint | done |
| M3 Distro coverage | lyrical/kilted/jazzy/humble branches + workflows + Mergify backports; static-link CI job for <= Kilted | workflows and Mergify rules in place; distro branches to be cut from `rolling` |
| M4 Docs & demo | README, three tutorials, overview deck, demo recording | docs done; recording pending |
| M5 Benchmarks | harness + published B1..B5 results; README performance section written from data | harness done; publish numbers from a reference robot host |
| M6 Release | rosdistro release ticket (needs `libsystemd-dev` rosdep key, already exists), announce on ROS Discourse | pending |

---

## 10. Risks & open questions

| Risk / question | Mitigation / decision |
|---|---|
| journald rate limiting silently drops bursts from chatty nodes | Documented prominently; drop-in config shipped; B3 quantifies limits; `sd_journal_sendv` errors are counted and reported at shutdown |
| Ingestion throughput ceiling below tuned rsyslog for extreme rates | Honest positioning: journald backend targets observability/usability; B3 publishes the crossover so users choose informed |
| Non-systemd targets (Alpine images, RTOS-adjacent) | strict/non-strict init modes; README states Linux+systemd requirement upfront |
| Logger name vs node name mismatch (logger names can be hierarchical / non-node) | Field named `ROS2_NODE_NAME` for discoverability but documented as "rcutils logger name"; a `ROS2_LOGGER_NAME` alias field can be added later without breaking queries |
| Per-logger severity: interface currently gives the backend limited use of `set_logger_level` | v1 global threshold (parity with syslog backend); revisit if `rcl_logging_interface` evolves |
| `sd_journal_sendv` per-call socket overhead vs batched writers | B1 measures; pre-assembled iovecs already implemented |
| Naming | **Decided: `rcl_logging_journal`** (not `rcl_logging_journald`). Named after the API/subsystem (sd-journal, "the journal"), consistent with `rcl_logging_syslog` (named after the syslog(3) API, not the rsyslogd daemon) and `rcl_logging_spdlog` (library name), and with other bindings (Python `systemd.journal`, Go `go-systemd/journal`). Search discoverability handled by "journald"/"systemd-journald" keywords in the package description, README, and repo topics |

---

## 11. References

- https://github.com/fujitatomoya/rcl_logging_syslog : structure, CI, docs flow inherited from this project
- https://github.com/ros2/rcl_logging : `rcl_logging_interface`, `rcl_logging_spdlog`, `rcl_logging_implementation`
- https://systemd.io/JOURNAL_NATIVE_PROTOCOL/ : datagram + sealed-memfd client protocol
- https://systemd.io/JOURNAL_FILE_FORMAT/ : append-only format, hash-table dedup, zstd/lz4/xz flags, compact mode
- `sd_journal_send(3)`, `sd_journal_print(3)`, `systemd.journal-fields(7)`, `journald.conf(5)`, `journalctl(1)`
- https://www.rsyslog.com/doc/configuration/modules/imjournal.html : journal-read performance characteristics, rate limiting
- https://access.redhat.com/articles/4058681 : RHEL guidance on journald+rsyslog layered performance
- https://docs.ros.org/en/rolling/ROS-Framework/nodes/About-Logging/About-Logging.html : ROS 2 logging & `rcl_logging_implementation`
