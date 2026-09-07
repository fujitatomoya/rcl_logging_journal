// Copyright 2026 Tomoya Fujita <tomoya.fujita825@gmail.com>.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// bench_backend: drives any rcl_logging backend directly through the
// rcl_logging_interface symbols of its shared library (dlopen), with no ROS
// graph, rosout or stdout involved. This isolates the backend cost that a
// ROS 2 executor pays per log call (benchmark B1) and provides the load
// generator for B2..B5 in run_matrix.sh.
//
// Usage:
//   bench_backend --backend rcl_logging_journal [--count N] [--size BYTES]
//                 [--severity DEBUG|INFO|WARN|ERROR|FATAL] [--rate MSG_PER_SEC]
//                 [--burst N --gap-ms MS] [--logger-name NAME] [--warmup N]
//
// --rate paces single calls; --burst/--gap-ms issues N back to back calls,
// then sleeps MS milliseconds, which is what a ROS 2 node with a periodic
// callback that logs a few lines looks like. Without either option the calls
// are issued as fast as possible (sustained, throughput bound).
//
// Output: one JSON object on stdout with latency percentiles (ns), throughput
// and the CPU time consumed by this process.

#include <dlfcn.h>
#include <sys/resource.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rcl_logging_interface/rcl_logging_interface.h"
#include "rcutils/allocator.h"
#include "rcutils/logging.h"

namespace
{

using initialize_fn = rcl_logging_ret_t (*)(const char *, const char *, rcutils_allocator_t);
using shutdown_fn = rcl_logging_ret_t (*)(void);
using log_fn = void (*)(int, const char *, const char *);
using set_level_fn = rcl_logging_ret_t (*)(const char *, int);

struct Options
{
  std::string backend = "rcl_logging_journal";
  std::string logger_name = "bench_backend";
  std::uint64_t count = 100000;
  std::uint64_t warmup = 1000;
  std::size_t size = 256;
  int severity = RCUTILS_LOG_SEVERITY_INFO;
  std::string severity_name = "INFO";
  double rate = 0.0;  // messages per second, 0 = as fast as possible
  std::uint64_t burst = 0;  // calls per burst, 0 = no burst pacing
  double gap_ms = 0.0;      // pause between bursts
};

int parse_severity(const std::string & name)
{
  if (name == "DEBUG") {return RCUTILS_LOG_SEVERITY_DEBUG;}
  if (name == "INFO") {return RCUTILS_LOG_SEVERITY_INFO;}
  if (name == "WARN") {return RCUTILS_LOG_SEVERITY_WARN;}
  if (name == "ERROR") {return RCUTILS_LOG_SEVERITY_ERROR;}
  if (name == "FATAL") {return RCUTILS_LOG_SEVERITY_FATAL;}
  std::fprintf(stderr, "unknown severity '%s'\n", name.c_str());
  std::exit(2);
}

Options parse_args(int argc, char ** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "missing value for %s\n", arg.c_str());
          std::exit(2);
        }
        return argv[++i];
      };
    if (arg == "--backend") {
      options.backend = value();
    } else if (arg == "--count") {
      options.count = std::stoull(value());
    } else if (arg == "--warmup") {
      options.warmup = std::stoull(value());
    } else if (arg == "--size") {
      options.size = std::stoul(value());
    } else if (arg == "--severity") {
      options.severity_name = value();
      options.severity = parse_severity(options.severity_name);
    } else if (arg == "--rate") {
      options.rate = std::stod(value());
    } else if (arg == "--burst") {
      options.burst = std::stoull(value());
    } else if (arg == "--gap-ms") {
      options.gap_ms = std::stod(value());
    } else if (arg == "--logger-name") {
      options.logger_name = value();
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
        "usage: %s --backend NAME [--count N] [--size BYTES] [--severity LEVEL] "
        "[--rate MSG_PER_SEC] [--burst N --gap-ms MS] [--logger-name NAME] [--warmup N]\n",
        argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
      std::exit(2);
    }
  }
  return options;
}

std::uint64_t now_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

double cpu_seconds(const struct timeval & tv)
{
  return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

std::uint64_t percentile(const std::vector<std::uint64_t> & sorted, double p)
{
  if (sorted.empty()) {
    return 0;
  }
  const double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
  return sorted[static_cast<std::size_t>(rank)];
}

template<typename T>
T resolve(void * handle, const char * symbol)
{
  void * ptr = dlsym(handle, symbol);
  if (ptr == nullptr) {
    std::fprintf(stderr, "failed to resolve %s: %s\n", symbol, dlerror());
    std::exit(1);
  }
  return reinterpret_cast<T>(ptr);
}

}  // namespace

int main(int argc, char ** argv)
{
  const Options options = parse_args(argc, argv);

  // Load the backend the same way rcl_logging_implementation does.
  const std::string library = "lib" + options.backend + ".so";
  void * handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    std::fprintf(stderr, "failed to load %s: %s\n", library.c_str(), dlerror());
    return 1;
  }
  auto initialize = resolve<initialize_fn>(handle, "rcl_logging_external_initialize");
  auto shutdown = resolve<shutdown_fn>(handle, "rcl_logging_external_shutdown");
  auto log = resolve<log_fn>(handle, "rcl_logging_external_log");
  auto set_level = resolve<set_level_fn>(handle, "rcl_logging_external_set_logger_level");

  if (initialize(nullptr, nullptr, rcutils_get_default_allocator()) != RCL_LOGGING_RET_OK) {
    std::fprintf(stderr, "%s: initialize failed\n", options.backend.c_str());
    return 1;
  }
  if (set_level(nullptr, RCUTILS_LOG_SEVERITY_DEBUG) != RCL_LOGGING_RET_OK) {
    std::fprintf(stderr, "%s: set_logger_level failed\n", options.backend.c_str());
    return 1;
  }

  // Message shaped like rcl output; the counter keeps every record unique so
  // journald's field deduplication cannot collapse MESSAGE payloads.
  auto make_message = [&options](const std::string & logger) {
      std::string message(options.size, 'x');
      const std::string prefix = "[" + options.severity_name + "] [1700000000.000000000] [" +
        logger + "]: ";
      if (prefix.size() < message.size()) {
        std::memcpy(&message[0], prefix.data(), prefix.size());
      }
      return std::make_pair(message, prefix.size());
    };
  auto stamp = [](std::string & message, std::size_t prefix_size, std::uint64_t i) {
      // write the counter right after the prefix, zero padded to 12 digits
      char digits[24];
      const int written = std::snprintf(digits, sizeof(digits), "%012" PRIu64, i);
      const std::size_t offset = std::min(prefix_size, message.size());
      const std::size_t n = std::min(static_cast<std::size_t>(written), message.size() - offset);
      std::memcpy(&message[offset], digits, n);
    };

  // Warm up under a distinct logger name (in the field and in the text) so
  // that counting records of `logger_name` in any sink yields exactly `count`
  // (run_matrix.sh B3).
  const std::string warmup_logger = options.logger_name + ".warmup";
  auto [warmup_message, warmup_prefix_size] = make_message(warmup_logger);
  for (std::uint64_t i = 0; i < options.warmup; ++i) {
    stamp(warmup_message, warmup_prefix_size, i);
    log(options.severity, warmup_logger.c_str(), warmup_message.c_str());
  }

  auto [message, prefix_size] = make_message(options.logger_name);

  std::vector<std::uint64_t> latencies;
  latencies.reserve(options.count);

  struct rusage usage_before;
  getrusage(RUSAGE_SELF, &usage_before);

  const double period_ns = options.rate > 0.0 ? 1e9 / options.rate : 0.0;
  const std::uint64_t start = now_ns();
  std::uint64_t next_deadline = start;
  for (std::uint64_t i = 0; i < options.count; ++i) {
    if (options.burst != 0 && i != 0 && i % options.burst == 0 && options.gap_ms > 0.0) {
      std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<std::int64_t>(options.gap_ms * 1e6)));
    }
    if (period_ns > 0.0) {
      // pace without drift: sleep until the scheduled slot
      next_deadline = start + static_cast<std::uint64_t>(period_ns * static_cast<double>(i));
      std::uint64_t now = now_ns();
      if (now < next_deadline) {
        const std::uint64_t remaining = next_deadline - now;
        if (remaining > 200000) {  // > 200us: sleep, else spin for accuracy
          std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - 100000));
        }
        while (now_ns() < next_deadline) {
        }
      }
    }
    stamp(message, prefix_size, options.warmup + i);
    const std::uint64_t t0 = now_ns();
    log(options.severity, options.logger_name.c_str(), message.c_str());
    latencies.push_back(now_ns() - t0);
  }
  const std::uint64_t end = now_ns();

  struct rusage usage_after;
  getrusage(RUSAGE_SELF, &usage_after);

  if (shutdown() != RCL_LOGGING_RET_OK) {
    std::fprintf(stderr, "%s: shutdown failed\n", options.backend.c_str());
    return 1;
  }
  dlclose(handle);

  std::sort(latencies.begin(), latencies.end());
  double sum = 0.0;
  for (std::uint64_t v : latencies) {
    sum += static_cast<double>(v);
  }
  const double wall_sec = static_cast<double>(end - start) / 1e9;

  std::string mode = "sustained";
  if (options.rate > 0.0) {
    mode = "paced";
  } else if (options.burst != 0) {
    mode = "burst " + std::to_string(options.burst) + " / " +
      std::to_string(static_cast<int>(options.gap_ms)) + " ms";
  }
  std::printf(
    "{\"backend\":\"%s\",\"mode\":\"%s\",\"count\":%" PRIu64 ",\"size\":%zu,"
    "\"severity\":\"%s\",\"rate\":%.0f,"
    "\"p50_ns\":%" PRIu64 ",\"p95_ns\":%" PRIu64 ",\"p99_ns\":%" PRIu64 ","
    "\"p999_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64 ","
    "\"mean_ns\":%.1f,\"wall_sec\":%.6f,\"calls_per_sec\":%.1f,"
    "\"cpu_user_sec\":%.6f,\"cpu_sys_sec\":%.6f}\n",
    options.backend.c_str(),
    mode.c_str(),
    options.count,
    options.size,
    options.severity_name.c_str(),
    options.rate,
    percentile(latencies, 50.0),
    percentile(latencies, 95.0),
    percentile(latencies, 99.0),
    percentile(latencies, 99.9),
    latencies.empty() ? static_cast<std::uint64_t>(0) : latencies.back(),
    latencies.empty() ? 0.0 : sum / static_cast<double>(latencies.size()),
    wall_sec,
    wall_sec > 0.0 ? static_cast<double>(options.count) / wall_sec : 0.0,
    cpu_seconds(usage_after.ru_utime) - cpu_seconds(usage_before.ru_utime),
    cpu_seconds(usage_after.ru_stime) - cpu_seconds(usage_before.ru_stime));
  return 0;
}
