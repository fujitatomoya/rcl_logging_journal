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

// rcl_logging backend for systemd-journald.
//
// Every log record is handed to journald with the native journal protocol
// (sd_journal_sendv) as a set of KEY=VALUE fields. journald stores each field
// as an indexed, deduplicated object, so `journalctl ROS2_NODE_NAME=talker`
// is an index lookup rather than a text scan.
//
// Record schema (see doc/design.md, section 4.5):
//   MESSAGE=<msg>                  formatted message as received from rcl
//   PRIORITY=<0..7>                syslog priority mapped from RCUTILS severity
//   SYSLOG_IDENTIFIER=<id>         executable name unless overridden
//   ROS2_NODE_NAME=<logger name>   omitted when rcl passes no logger name
//   ROS2_SEVERITY=<DEBUG..FATAL>   original ROS wording
//   ROS2_DISTRO=<$ROS_DISTRO>      omitted when ROS_DISTRO is not set
//   <RCL_LOGGING_JOURNAL_EXTRA_FIELDS...>
// journald adds the trusted _PID, _UID, _COMM, _EXE, _BOOT_ID, ... fields.

// Without this, <systemd/sd-journal.h> turns sd_journal_sendv() into a macro
// that stamps CODE_FILE/CODE_LINE/CODE_FUNC of *this* file on every record,
// which would be misleading (the location of the ROS log call is not
// available through rcl_logging_interface).
#define SD_JOURNAL_SUPPRESS_LOCATION

#include <sys/uio.h>
#include <syslog.h>
#include <systemd/sd-journal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rcpputils/env.hpp"
#include "rcpputils/scope_exit.hpp"

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging.h"
#include "rcutils/logging_macros.h"
#include "rcutils/process.h"
#include "rcutils/strdup.h"

#include "rcl_logging_interface/rcl_logging_interface.h"

namespace
{

constexpr const char * kLoggerName = "rcl_logging_journal";

// Environment variables (see README.md "Configuration").
constexpr const char * kEnvIdentifier = "RCL_LOGGING_JOURNAL_IDENTIFIER";
constexpr const char * kEnvExtraFields = "RCL_LOGGING_JOURNAL_EXTRA_FIELDS";
constexpr const char * kEnvStrict = "RCL_LOGGING_JOURNAL_STRICT";
constexpr const char * kEnvSocketPath = "RCL_LOGGING_JOURNAL_SOCKET_PATH";
constexpr const char * kEnvRosDistro = "ROS_DISTRO";

// Path libsystemd connects to inside sd_journal_sendv(). It is not
// configurable in libsystemd; the env override only affects our probe.
constexpr const char * kDefaultSocketPath = "/run/systemd/journal/socket";

// Field names this backend emits itself; they are rejected in EXTRA_FIELDS.
constexpr const char * kReservedFieldNames[] = {
  "MESSAGE", "PRIORITY", "SYSLOG_IDENTIFIER",
  "ROS2_NODE_NAME", "ROS2_SEVERITY", "ROS2_DISTRO",
};

// journald limits (src/libsystemd/sd-journal/journal-file.h: 64 chars).
constexpr std::size_t kMaxFieldNameLength = 64;
// Upper bound so the per-call iovec array can live on the stack.
constexpr std::size_t kMaxExtraFields = 32;
// Per-call fields: MESSAGE, PRIORITY, ROS2_SEVERITY, ROS2_NODE_NAME.
constexpr std::size_t kMaxDynamicFields = 4;
// Constant fields besides extra fields: SYSLOG_IDENTIFIER, ROS2_DISTRO.
constexpr std::size_t kMaxConstantFields = 2 + kMaxExtraFields;
// Stack scratch for "MESSAGE=<msg>"; larger messages fall back to the heap.
constexpr std::size_t kMessageStackBuffer = 4096;
// Stack scratch for "ROS2_NODE_NAME=<name>".
constexpr std::size_t kNodeNameStackBuffer = 512;

struct SeverityInfo
{
  int priority;
  const char * priority_field;
  std::size_t priority_field_len;
  const char * severity_field;
  std::size_t severity_field_len;
};

#define RCL_LOGGING_JOURNAL_FIELD(str) str, sizeof(str) - 1

constexpr SeverityInfo kSeverityDebug = {
  LOG_DEBUG,
  RCL_LOGGING_JOURNAL_FIELD("PRIORITY=7"),
  RCL_LOGGING_JOURNAL_FIELD("ROS2_SEVERITY=DEBUG")};
constexpr SeverityInfo kSeverityInfo = {
  LOG_INFO,
  RCL_LOGGING_JOURNAL_FIELD("PRIORITY=6"),
  RCL_LOGGING_JOURNAL_FIELD("ROS2_SEVERITY=INFO")};
constexpr SeverityInfo kSeverityWarn = {
  LOG_WARNING,
  RCL_LOGGING_JOURNAL_FIELD("PRIORITY=4"),
  RCL_LOGGING_JOURNAL_FIELD("ROS2_SEVERITY=WARN")};
constexpr SeverityInfo kSeverityError = {
  LOG_ERR,
  RCL_LOGGING_JOURNAL_FIELD("PRIORITY=3"),
  RCL_LOGGING_JOURNAL_FIELD("ROS2_SEVERITY=ERROR")};
constexpr SeverityInfo kSeverityFatal = {
  LOG_CRIT,
  RCL_LOGGING_JOURNAL_FIELD("PRIORITY=2"),
  RCL_LOGGING_JOURNAL_FIELD("ROS2_SEVERITY=FATAL")};
// RCUTILS_LOG_SEVERITY_UNSET and unknown values are reported as INFO
// (design.md 4.3) but keep the original wording so they stay identifiable.
constexpr SeverityInfo kSeverityUnset = {
  LOG_INFO,
  RCL_LOGGING_JOURNAL_FIELD("PRIORITY=6"),
  RCL_LOGGING_JOURNAL_FIELD("ROS2_SEVERITY=UNSET")};

#undef RCL_LOGGING_JOURNAL_FIELD

const SeverityInfo & severity_info(int severity)
{
  switch (severity) {
    case RCUTILS_LOG_SEVERITY_DEBUG:
      return kSeverityDebug;
    case RCUTILS_LOG_SEVERITY_INFO:
      return kSeverityInfo;
    case RCUTILS_LOG_SEVERITY_WARN:
      return kSeverityWarn;
    case RCUTILS_LOG_SEVERITY_ERROR:
      return kSeverityError;
    case RCUTILS_LOG_SEVERITY_FATAL:
      return kSeverityFatal;
    default:
      return kSeverityUnset;
  }
}

// Threshold used by set_logger_level(): a record is emitted when its journald
// priority is numerically <= the threshold (lower number = more severe).
int level_to_priority_threshold(int level)
{
  if (level <= RCUTILS_LOG_SEVERITY_UNSET) {
    // "unset" means no filtering by the backend at all.
    return LOG_DEBUG;
  }
  return severity_info(level).priority;
}

struct JournalState
{
  // false when journald is absent and RCL_LOGGING_JOURNAL_STRICT=0
  // turned the backend into a no-op.
  bool enabled = true;
  // Owning storage for the constant fields referenced by constant_iov.
  std::string identifier_field;             // "SYSLOG_IDENTIFIER=<id>"
  std::string distro_field;                 // "ROS2_DISTRO=<distro>" or empty
  std::vector<std::string> extra_fields;    // "KEY=VALUE" ...
  // Pre-built iovecs copied into every record; see rcl_logging_external_log.
  std::size_t constant_iov_count = 0;
  struct iovec constant_iov[kMaxConstantFields];
};

// Written by initialize()/shutdown() only, read by log(). Concurrent
// initialize/shutdown against log() is undefined, as for every other
// rcl_logging backend.
std::unique_ptr<const JournalState> g_state;
std::atomic<int> g_priority_threshold{LOG_DEBUG};
std::atomic<std::uint64_t> g_send_failures{0};

bool get_env(const char * name, std::string & value)
{
  try {
    value = rcpputils::get_env_var(name);
  } catch (const std::runtime_error & error) {
    RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
      "failed to get env var '%s': %s", name, error.what());
    return false;
  }
  return true;
}

// Returns true and sets `strict` on success; false with error set otherwise.
bool parse_strict(const std::string & raw, bool & strict)
{
  if (raw.empty() || raw == "1" || raw == "true" || raw == "TRUE" || raw == "True") {
    strict = true;
    return true;
  }
  if (raw == "0" || raw == "false" || raw == "FALSE" || raw == "False") {
    strict = false;
    return true;
  }
  RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
    "invalid value '%s' for %s, expected 0/1/true/false", raw.c_str(), kEnvStrict);
  return false;
}

bool is_valid_field_name(const std::string & name)
{
  // Mirrors journald's journal_field_valid(): [A-Z0-9_]+, not starting with
  // '_' (reserved for trusted fields), at most 64 characters.
  if (name.empty() || name.size() > kMaxFieldNameLength || name[0] == '_') {
    return false;
  }
  for (char c : name) {
    const bool upper = (c >= 'A' && c <= 'Z');
    const bool digit = (c >= '0' && c <= '9');
    if (!upper && !digit && c != '_') {
      return false;
    }
  }
  return true;
}

bool is_reserved_field_name(const std::string & name)
{
  for (const char * reserved : kReservedFieldNames) {
    if (name == reserved) {
      return true;
    }
  }
  return false;
}

// Parses "KEY=VALUE;KEY2=VALUE2" into state.extra_fields.
bool parse_extra_fields(const std::string & raw, JournalState & state)
{
  std::size_t begin = 0;
  while (begin <= raw.size()) {
    std::size_t end = raw.find(';', begin);
    if (end == std::string::npos) {
      end = raw.size();
    }
    const std::string token = raw.substr(begin, end - begin);
    begin = end + 1;
    if (token.empty()) {
      // tolerate empty entries such as a trailing ';'
      continue;
    }
    const std::size_t eq = token.find('=');
    if (eq == std::string::npos) {
      RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "%s entry '%s' is not of the form KEY=VALUE", kEnvExtraFields, token.c_str());
      return false;
    }
    const std::string key = token.substr(0, eq);
    if (!is_valid_field_name(key)) {
      RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "%s entry '%s' has an invalid journal field name "
        "(use [A-Z0-9_], not starting with '_', at most %zu characters)",
        kEnvExtraFields, token.c_str(), kMaxFieldNameLength);
      return false;
    }
    if (is_reserved_field_name(key)) {
      RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "%s entry '%s' uses field name '%s' which is set by %s itself",
        kEnvExtraFields, token.c_str(), key.c_str(), kLoggerName);
      return false;
    }
    if (state.extra_fields.size() >= kMaxExtraFields) {
      RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "%s has more than %zu entries", kEnvExtraFields, kMaxExtraFields);
      return false;
    }
    state.extra_fields.push_back(token);
  }
  return true;
}

void add_constant_field(JournalState & state, const std::string & field)
{
  // Caller guarantees the capacity: 2 + kMaxExtraFields.
  struct iovec & iov = state.constant_iov[state.constant_iov_count++];
  iov.iov_base = const_cast<char *>(field.data());
  iov.iov_len = field.size();
}

// Builds "<prefix><value>" in `stack` when it fits, otherwise on the heap.
// Returns the pointer to use and writes the total length to `out_len`.
const char * compose_field(
  const char * prefix, std::size_t prefix_len,
  const char * value, std::size_t value_len,
  char * stack, std::size_t stack_len,
  std::unique_ptr<char[]> & heap, std::size_t & out_len)
{
  out_len = prefix_len + value_len;
  char * dst = stack;
  if (out_len > stack_len) {
    heap.reset(new char[out_len]);
    dst = heap.get();
  }
  std::memcpy(dst, prefix, prefix_len);
  std::memcpy(dst + prefix_len, value, value_len);
  return dst;
}

}  // namespace

rcl_logging_ret_t rcl_logging_external_initialize(
  const char * file_name_prefix,
  const char * config_file,
  rcutils_allocator_t allocator)
{
  RCUTILS_CHECK_ALLOCATOR(&allocator, return RCL_LOGGING_RET_INVALID_ARGUMENT);

  // It is possible for this to get called more than once in a process (some of
  // the tests do this implicitly by calling rclcpp::init more than once).
  // If the backend is already set up, don't do anything.
  if (g_state != nullptr) {
    return RCL_LOGGING_RET_OK;
  }

  const bool config_file_provided = (nullptr != config_file) && (config_file[0] != '\0');
  if (config_file_provided) {
    // journald is configured system wide in journald.conf(5); the backend
    // itself is configured through environment variables only.
    RCUTILS_LOG_WARN_NAMED(
      kLoggerName,
      "journald logging backend doesn't have client configuration files, "
      "ignoring '%s'; use environment variables or /etc/systemd/journald.conf instead",
      config_file);
  }

  auto state = std::make_unique<JournalState>();

  // 1. SYSLOG_IDENTIFIER: env override -> file name prefix -> executable name.
  std::string identifier;
  if (!get_env(kEnvIdentifier, identifier)) {
    return RCL_LOGGING_RET_ERROR;
  }
  if (identifier.empty()) {
    const bool file_name_provided =
      (nullptr != file_name_prefix) && (file_name_prefix[0] != '\0');
    char * basec = nullptr;
    if (file_name_provided) {
      basec = rcutils_strdup(file_name_prefix, allocator);
    } else {
      basec = rcutils_get_executable_name(allocator);
    }
    if (basec == nullptr) {
      RCUTILS_SET_ERROR_MSG("Failed to get the executable name for SYSLOG_IDENTIFIER");
      return RCL_LOGGING_RET_ERROR;
    }
    RCPPUTILS_SCOPE_EXIT(
    {
      allocator.deallocate(basec, allocator.state);
    });
    identifier = basec;
  }
  state->identifier_field = std::string("SYSLOG_IDENTIFIER=") + identifier;
  add_constant_field(*state, state->identifier_field);

  // 2. ROS2_DISTRO from $ROS_DISTRO, if any.
  std::string distro;
  if (!get_env(kEnvRosDistro, distro)) {
    return RCL_LOGGING_RET_ERROR;
  }
  if (!distro.empty()) {
    state->distro_field = std::string("ROS2_DISTRO=") + distro;
    add_constant_field(*state, state->distro_field);
  }

  // 3. Static extra fields.
  std::string extra_fields_raw;
  if (!get_env(kEnvExtraFields, extra_fields_raw)) {
    return RCL_LOGGING_RET_ERROR;
  }
  if (!parse_extra_fields(extra_fields_raw, *state)) {
    return RCL_LOGGING_RET_INVALID_ARGUMENT;
  }
  for (const std::string & field : state->extra_fields) {
    add_constant_field(*state, field);
  }

  // 4. Strict / non-strict handling of a missing journald.
  std::string strict_raw;
  if (!get_env(kEnvStrict, strict_raw)) {
    return RCL_LOGGING_RET_ERROR;
  }
  bool strict = true;
  if (!parse_strict(strict_raw, strict)) {
    return RCL_LOGGING_RET_INVALID_ARGUMENT;
  }

  // 5. Probe the journald native socket. libsystemd manages the client
  // socket internally, so there is no connection to hold here.
  std::string socket_path;
  if (!get_env(kEnvSocketPath, socket_path)) {
    return RCL_LOGGING_RET_ERROR;
  }
  if (socket_path.empty()) {
    socket_path = kDefaultSocketPath;
  }
  if (access(socket_path.c_str(), W_OK) != 0) {
    const int saved_errno = errno;
    if (strict) {
      RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "systemd-journald native socket '%s' is not available (%s). "
        "Is systemd-journald running? In a container, bind mount the host socket "
        "with '-v /run/systemd/journal/socket:/run/systemd/journal/socket' "
        "or set %s=0 to continue without journald.",
        socket_path.c_str(), std::strerror(saved_errno), kEnvStrict);
      return RCL_LOGGING_RET_ERROR;
    }
    RCUTILS_LOG_WARN_NAMED(
      kLoggerName,
      "systemd-journald native socket '%s' is not available (%s), "
      "%s=0 so the journald backend is disabled for this process",
      socket_path.c_str(), std::strerror(saved_errno), kEnvStrict);
    state->enabled = false;
  }

  RCUTILS_LOG_DEBUG_NAMED(
    kLoggerName,
    "journald logging backend initialized: %s, %zu extra field(s), strict=%d, enabled=%d",
    state->identifier_field.c_str(), state->extra_fields.size(),
    strict ? 1 : 0, state->enabled ? 1 : 0);

  g_priority_threshold.store(LOG_DEBUG, std::memory_order_relaxed);
  g_send_failures.store(0, std::memory_order_relaxed);
  g_state = std::move(state);
  return RCL_LOGGING_RET_OK;
}

rcl_logging_ret_t rcl_logging_external_shutdown()
{
  // Nothing to flush: every sd_journal_sendv() call is a complete datagram
  // that journald has already received once the call returns.
  const std::uint64_t failures = g_send_failures.exchange(0, std::memory_order_relaxed);
  if (failures != 0) {
    RCUTILS_LOG_WARN_NAMED(
      kLoggerName,
      "%" PRIu64 " log record(s) could not be delivered to systemd-journald",
      failures);
  }
  g_state.reset();
  g_priority_threshold.store(LOG_DEBUG, std::memory_order_relaxed);
  return RCL_LOGGING_RET_OK;
}

void rcl_logging_external_log(int severity, const char * name, const char * msg)
{
  const JournalState * state = g_state.get();
  if (state == nullptr || !state->enabled || msg == nullptr) {
    return;
  }

  const SeverityInfo & info = severity_info(severity);
  if (info.priority > g_priority_threshold.load(std::memory_order_relaxed)) {
    return;
  }

  // MESSAGE=<msg>
  static constexpr char kMessagePrefix[] = "MESSAGE=";
  char message_stack[kMessageStackBuffer];
  std::unique_ptr<char[]> message_heap;
  std::size_t message_len = 0;
  const char * message_field = compose_field(
    kMessagePrefix, sizeof(kMessagePrefix) - 1,
    msg, std::strlen(msg),
    message_stack, sizeof(message_stack), message_heap, message_len);

  struct iovec iov[kMaxDynamicFields + kMaxConstantFields];
  std::size_t n = 0;
  iov[n].iov_base = const_cast<char *>(message_field);
  iov[n++].iov_len = message_len;
  iov[n].iov_base = const_cast<char *>(info.priority_field);
  iov[n++].iov_len = info.priority_field_len;
  iov[n].iov_base = const_cast<char *>(info.severity_field);
  iov[n++].iov_len = info.severity_field_len;

  // ROS2_NODE_NAME=<name>, only when rcl passed a logger name.
  static constexpr char kNodeNamePrefix[] = "ROS2_NODE_NAME=";
  char node_name_stack[kNodeNameStackBuffer];
  std::unique_ptr<char[]> node_name_heap;
  if (name != nullptr && name[0] != '\0') {
    std::size_t node_name_len = 0;
    const char * node_name_field = compose_field(
      kNodeNamePrefix, sizeof(kNodeNamePrefix) - 1,
      name, std::strlen(name),
      node_name_stack, sizeof(node_name_stack), node_name_heap, node_name_len);
    iov[n].iov_base = const_cast<char *>(node_name_field);
    iov[n++].iov_len = node_name_len;
  }

  // SYSLOG_IDENTIFIER, ROS2_DISTRO and extra fields, pre-built at init.
  std::memcpy(
    &iov[n], state->constant_iov, state->constant_iov_count * sizeof(struct iovec));
  n += state->constant_iov_count;

  // sd_journal_sendv() sends one AF_UNIX datagram and transparently falls
  // back to a sealed memfd when the record exceeds the datagram size.
  const int ret = sd_journal_sendv(iov, static_cast<int>(n));
  if (ret < 0) {
    // Never block or spin on the hot path; count it and report at shutdown.
    g_send_failures.fetch_add(1, std::memory_order_relaxed);
  }
}

rcl_logging_ret_t rcl_logging_external_set_logger_level(const char * name, int level)
{
  // v1 keeps a single, process wide threshold like rcl_logging_syslog does via
  // setlogmask(); rcl only invokes this for the default logger today.
  (void) name;

  g_priority_threshold.store(level_to_priority_threshold(level), std::memory_order_relaxed);
  return RCL_LOGGING_RET_OK;
}
