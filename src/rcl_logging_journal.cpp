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
// Hot path design (see doc/design.md, section 4.7):
//   rcl_logging_external_log() copies the record into a preallocated ring
//   buffer (1 MiB unless RCL_LOGGING_JOURNAL_BUFFER_SIZE says otherwise) under
//   a mutex and returns. One sender thread drains the ring and
//   performs the sd_journal_sendv() (one sendmsg() and a journald wake up per
//   record, several microseconds) off the caller's thread. This is the same
//   idea as rcl_logging_spdlog's buffered file sink: the caller pays a memcpy,
//   the I/O happens later. FATAL records are the exception: the caller waits
//   until the record has been handed to journald, which fsyncs CRIT and above
//   immediately, so a FATAL followed by a crash is on disk.
//
// Record schema (see doc/design.md, section 4.5):
//   MESSAGE=<msg>                  formatted message as received from rcl
//   PRIORITY=<0..7>                syslog priority mapped from RCUTILS severity
//   ROS2_NODE_NAME=<logger name>   omitted when rcl passes no logger name
//   SYSLOG_IDENTIFIER=<id>         executable name unless overridden
//   ROS2_DISTRO=<$ROS_DISTRO>      omitted when ROS_DISTRO is not set
//   <RCL_LOGGING_JOURNAL_EXTRA_FIELDS...>
// journald adds the trusted _PID, _UID, _COMM, _EXE, _BOOT_ID, ... fields.
//
// Severity filtering is not done here: rcl already filters on the logger
// level before calling the backend, and journald can filter with
// MaxLevelStore= in journald.conf(5).

// Without this, <systemd/sd-journal.h> turns sd_journal_sendv() into a macro
// that stamps CODE_FILE/CODE_LINE/CODE_FUNC of *this* file on every record,
// which would be misleading (the location of the ROS log call is not
// available through rcl_logging_interface).
#define SD_JOURNAL_SUPPRESS_LOCATION

#include <pthread.h>
#include <sys/uio.h>
#include <syslog.h>
#include <systemd/sd-journal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "rcpputils/env.hpp"
#include "rcpputils/scope_exit.hpp"

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging.h"
#include "rcutils/logging_macros.h"
#include "rcutils/process.h"

#include "rcl_logging_interface/rcl_logging_interface.h"

namespace
{

constexpr const char * kLoggerName = "rcl_logging_journal";

// Environment variables (see README.md "Configuration").
constexpr const char * kEnvIdentifier = "RCL_LOGGING_JOURNAL_IDENTIFIER";
constexpr const char * kEnvExtraFields = "RCL_LOGGING_JOURNAL_EXTRA_FIELDS";
constexpr const char * kEnvStrict = "RCL_LOGGING_JOURNAL_STRICT";
constexpr const char * kEnvSocketPath = "RCL_LOGGING_JOURNAL_SOCKET_PATH";
constexpr const char * kEnvBufferSize = "RCL_LOGGING_JOURNAL_BUFFER_SIZE";
constexpr const char * kEnvRosDistro = "ROS_DISTRO";

// Path libsystemd connects to inside sd_journal_sendv(). It is not
// configurable in libsystemd; the env override only affects our probe.
constexpr const char * kDefaultSocketPath = "/run/systemd/journal/socket";

// Field names this backend emits itself; they are rejected in EXTRA_FIELDS.
constexpr const char * kReservedFieldNames[] = {
  "MESSAGE", "PRIORITY", "SYSLOG_IDENTIFIER", "ROS2_NODE_NAME", "ROS2_DISTRO",
};

// journald limits (src/libsystemd/sd-journal/journal-file.h: 64 chars).
constexpr std::size_t kMaxFieldNameLength = 64;
// Upper bound so the per-record iovec array can live on the stack.
constexpr std::size_t kMaxExtraFields = 32;
// Per-record fields: MESSAGE, PRIORITY, ROS2_NODE_NAME.
constexpr std::size_t kMaxDynamicFields = 3;
// Constant fields besides extra fields: SYSLOG_IDENTIFIER, ROS2_DISTRO.
constexpr std::size_t kMaxConstantFields = 2 + kMaxExtraFields;

constexpr char kMessagePrefix[] = "MESSAGE=";
constexpr std::size_t kMessagePrefixLen = sizeof(kMessagePrefix) - 1;
constexpr char kNodeNamePrefix[] = "ROS2_NODE_NAME=";
constexpr std::size_t kNodeNamePrefixLen = sizeof(kNodeNamePrefix) - 1;

// Ring buffer between the logging threads and the sender thread (see
// doc/design.md, section 4.7). The default 1 MiB holds roughly 8000 typical
// records; pages are only touched as they get used. RCL_LOGGING_JOURNAL_BUFFER_SIZE
// overrides the capacity within [kMinRingCapacity, kMaxRingCapacity].
constexpr std::size_t kDefaultRingCapacity = std::size_t{1} << 20;
constexpr std::size_t kMinRingCapacity = std::size_t{4} << 10;
constexpr std::size_t kMaxRingCapacity = std::size_t{1} << 30;
// Records larger than capacity / kRingSyncDivisor bypass the ring and are sent
// synchronously (after draining the ring so ordering is preserved). Covers
// backtraces and dumps: 256 KiB with the default capacity.
constexpr std::size_t kRingSyncDivisor = 4;
// Stack scratch for the synchronous path; larger values fall back to the heap.
constexpr std::size_t kSyncStackBuffer = 4096;

// "PRIORITY=n" for n in 0..7, selected by index, no formatting on the hot path.
constexpr const char * kPriorityFields[8] = {
  "PRIORITY=0", "PRIORITY=1", "PRIORITY=2", "PRIORITY=3",
  "PRIORITY=4", "PRIORITY=5", "PRIORITY=6", "PRIORITY=7",
};
constexpr std::size_t kPriorityFieldLen = sizeof("PRIORITY=0") - 1;

// design.md 4.3: DEBUG->7, INFO->6, WARN->4, ERROR->3, FATAL->2, other->6.
int severity_to_priority(int severity)
{
  switch (severity) {
    case RCUTILS_LOG_SEVERITY_DEBUG:
      return LOG_DEBUG;
    case RCUTILS_LOG_SEVERITY_INFO:
      return LOG_INFO;
    case RCUTILS_LOG_SEVERITY_WARN:
      return LOG_WARNING;
    case RCUTILS_LOG_SEVERITY_ERROR:
      return LOG_ERR;
    case RCUTILS_LOG_SEVERITY_FATAL:
      return LOG_CRIT;
    default:
      return LOG_INFO;
  }
}

// Constant fields shared by every record, built once at initialization.
struct ConstantFields
{
  std::string identifier_field;             // "SYSLOG_IDENTIFIER=<id>"
  std::string distro_field;                 // "ROS2_DISTRO=<distro>" or empty
  std::vector<std::string> extra_fields;    // "KEY=VALUE" ...
  std::size_t iov_count = 0;
  struct iovec iov[kMaxConstantFields];

  void add(const std::string & field)
  {
    // Callers guarantee the capacity: 2 + kMaxExtraFields.
    iov[iov_count].iov_base = const_cast<char *>(field.data());
    iov[iov_count].iov_len = field.size();
    ++iov_count;
  }
};

// Sends one record synchronously. `name_field`/`message_field` are complete
// "KEY=VALUE" buffers. Returns the sd_journal_sendv() result.
int send_fields(
  const ConstantFields & constants,
  int priority,
  const char * name_field, std::size_t name_field_len,
  const char * message_field, std::size_t message_field_len)
{
  struct iovec iov[kMaxDynamicFields + kMaxConstantFields];
  std::size_t n = 0;
  iov[n].iov_base = const_cast<char *>(message_field);
  iov[n++].iov_len = message_field_len;
  iov[n].iov_base = const_cast<char *>(kPriorityFields[priority]);
  iov[n++].iov_len = kPriorityFieldLen;
  if (name_field_len != 0) {
    iov[n].iov_base = const_cast<char *>(name_field);
    iov[n++].iov_len = name_field_len;
  }
  std::memcpy(&iov[n], constants.iov, constants.iov_count * sizeof(struct iovec));
  n += constants.iov_count;
  // sd_journal_sendv() sends one AF_UNIX datagram and transparently falls
  // back to a sealed memfd when the record exceeds the datagram size.
  return sd_journal_sendv(iov, static_cast<int>(n));
}

// Composes the KEY=VALUE buffers on the stack (heap for big values) and sends.
int send_direct(
  const ConstantFields & constants,
  int priority,
  const char * name, std::size_t name_len,
  const char * msg, std::size_t msg_len)
{
  const std::size_t message_field_len = kMessagePrefixLen + msg_len;
  const std::size_t name_field_len = (name_len != 0) ? kNodeNamePrefixLen + name_len : 0;
  char stack[kSyncStackBuffer];
  std::unique_ptr<char[]> heap;
  char * buffer = stack;
  if (message_field_len + name_field_len > sizeof(stack)) {
    heap.reset(new char[message_field_len + name_field_len]);
    buffer = heap.get();
  }
  std::memcpy(buffer, kMessagePrefix, kMessagePrefixLen);
  std::memcpy(buffer + kMessagePrefixLen, msg, msg_len);
  char * name_field = buffer + message_field_len;
  if (name_field_len != 0) {
    std::memcpy(name_field, kNodeNamePrefix, kNodeNamePrefixLen);
    std::memcpy(name_field + kNodeNamePrefixLen, name, name_len);
  }
  return send_fields(
    constants, priority, name_field, name_field_len, buffer, message_field_len);
}

// Single producer-side mutex, single consumer thread, preallocated byte ring.
//
// Layout of one slot (8 byte aligned):
//   RecordHeader { total_len, kind, priority, name_field_len }
//   "ROS2_NODE_NAME=<name>"   name_field_len bytes (0 when no logger name)
//   "MESSAGE=<msg>"           total_len - sizeof(header) - name_field_len bytes
// A kind == kPad slot fills the gap at the end of the buffer when a record
// does not fit contiguously; the consumer skips it and wraps to offset 0.
class Sender
{
public:
  // `capacity` is a multiple of 8 within [kMinRingCapacity, kMaxRingCapacity];
  // parse_buffer_size() guarantees that.
  Sender(const ConstantFields & constants, std::size_t capacity)
  : constants_(constants),
    capacity_(capacity),
    sync_threshold_(capacity / kRingSyncDivisor),
    buffer_(new char[capacity]),
    worker_(&Sender::run, this)
  {
  }

  ~Sender()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_data_.notify_one();
    cv_space_.notify_all();
    cv_sent_.notify_all();
    if (forked_child_.load(std::memory_order_relaxed)) {
      // The worker thread does not exist in a forked child; do not join it.
      worker_.detach();
    } else {
      // Drains everything still queued before returning.
      worker_.join();
    }
  }

  // Copies the record into the ring and returns. Blocks only when the ring is
  // full (journald slower than the producers) or when wait_until_sent is set.
  void enqueue(
    int priority,
    const char * name, std::size_t name_len,
    const char * msg, std::size_t msg_len,
    bool wait_until_sent)
  {
    if (forked_child_.load(std::memory_order_relaxed)) {
      // No worker thread on this side of fork(): send synchronously.
      if (send_direct(constants_, priority, name, name_len, msg, msg_len) < 0) {
        failures_.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }

    const std::size_t name_field_len = (name_len != 0) ? kNodeNamePrefixLen + name_len : 0;
    const std::size_t message_field_len = kMessagePrefixLen + msg_len;
    const std::size_t total_len = sizeof(RecordHeader) + name_field_len + message_field_len;
    const std::size_t advance = round_up(total_len);
    if (advance > sync_threshold_ || name_field_len > UINT16_MAX) {
      // Too big for the ring: drain what is queued so ordering is kept, then
      // send from this thread (libsystemd uses a sealed memfd for big records).
      flush();
      if (send_direct(constants_, priority, name, name_len, msg, msg_len) < 0) {
        failures_.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }

    std::uint64_t seq = 0;
    bool wake_consumer = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!reserve(advance)) {
        if (stop_) {
          return;
        }
        ++space_waiters_;
        cv_space_.wait(lock);
        --space_waiters_;
      }
      RecordHeader header;
      header.total_len = static_cast<std::uint32_t>(total_len);
      header.kind = kRecord;
      header.priority = static_cast<std::uint8_t>(priority);
      header.name_field_len = static_cast<std::uint16_t>(name_field_len);
      char * slot = buffer_.get() + tail_;
      std::memcpy(slot, &header, sizeof(header));
      char * cursor = slot + sizeof(header);
      if (name_field_len != 0) {
        std::memcpy(cursor, kNodeNamePrefix, kNodeNamePrefixLen);
        std::memcpy(cursor + kNodeNamePrefixLen, name, name_len);
        cursor += name_field_len;
      }
      std::memcpy(cursor, kMessagePrefix, kMessagePrefixLen);
      std::memcpy(cursor + kMessagePrefixLen, msg, msg_len);
      tail_ += advance;
      if (tail_ == capacity_) {
        tail_ = 0;
      }
      used_ += advance;
      seq = ++enqueued_;
      wake_consumer = consumer_waiting_;
    }
    if (wake_consumer) {
      cv_data_.notify_one();
    }
    if (wait_until_sent) {
      wait_for(seq);
    }
  }

  // Blocks until every record enqueued so far has been handed to journald.
  void flush()
  {
    std::uint64_t seq = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seq = enqueued_;
    }
    wait_for(seq);
  }

  std::uint64_t failures() const
  {
    return failures_.load(std::memory_order_relaxed);
  }

  static void on_fork_child()
  {
    forked_child_.store(true, std::memory_order_relaxed);
  }

private:
  static constexpr std::uint8_t kRecord = 0;
  static constexpr std::uint8_t kPad = 1;

  struct RecordHeader
  {
    std::uint32_t total_len;       // header + name field + message field
    std::uint8_t kind;             // kRecord or kPad (total_len = pad length)
    std::uint8_t priority;         // 0..7
    std::uint16_t name_field_len;  // 0 when there is no logger name
  };
  static_assert(sizeof(RecordHeader) == 8, "ring slots are 8 byte aligned");

  static std::size_t round_up(std::size_t n)
  {
    return (n + 7u) & ~static_cast<std::size_t>(7u);
  }

  // Makes room for `advance` bytes at tail_, inserting a pad slot when the
  // record would not be contiguous. Caller holds mutex_.
  bool reserve(std::size_t advance)
  {
    if (tail_ + advance <= capacity_) {
      return capacity_ - used_ >= advance;
    }
    const std::size_t pad = capacity_ - tail_;  // >= 8, multiple of 8
    if (capacity_ - used_ < pad + advance) {
      return false;
    }
    RecordHeader header;
    header.total_len = static_cast<std::uint32_t>(pad);
    header.kind = kPad;
    header.priority = 0;
    header.name_field_len = 0;
    std::memcpy(buffer_.get() + tail_, &header, sizeof(header));
    used_ += pad;
    tail_ = 0;
    return true;
  }

  void wait_for(std::uint64_t seq)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    ++flush_waiters_;
    while (sent_ < seq && !stop_) {
      cv_sent_.wait(lock);
    }
    --flush_waiters_;
  }

  void run()
  {
    pthread_setname_np(pthread_self(), "rcl_journal");
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      while (used_ == 0 && !stop_) {
        consumer_waiting_ = true;
        cv_data_.wait(lock);
        consumer_waiting_ = false;
      }
      if (used_ == 0) {
        break;  // stop_ requested and everything drained
      }
      RecordHeader header;
      std::memcpy(&header, buffer_.get() + head_, sizeof(header));
      if (header.kind == kPad) {
        used_ -= header.total_len;
        head_ = 0;
        continue;
      }
      const std::size_t advance = round_up(header.total_len);
      const char * name_field = buffer_.get() + head_ + sizeof(header);
      const char * message_field = name_field + header.name_field_len;
      const std::size_t message_field_len =
        header.total_len - sizeof(header) - header.name_field_len;
      lock.unlock();

      // Producers never write into [head_, head_ + advance) until head_ moves,
      // so the record can be sent straight from the ring without a copy.
      if (send_fields(
          constants_, header.priority,
          name_field, header.name_field_len,
          message_field, message_field_len) < 0)
      {
        failures_.fetch_add(1, std::memory_order_relaxed);
      }

      lock.lock();
      head_ += advance;
      if (head_ == capacity_) {
        head_ = 0;
      }
      used_ -= advance;
      ++sent_;
      if (space_waiters_ != 0) {
        cv_space_.notify_all();
      }
      if (flush_waiters_ != 0) {
        cv_sent_.notify_all();
      }
    }
  }

  const ConstantFields & constants_;
  const std::size_t capacity_;        // ring size in bytes, multiple of 8
  const std::size_t sync_threshold_;  // capacity_ / kRingSyncDivisor
  std::unique_ptr<char[]> buffer_;

  std::mutex mutex_;
  std::condition_variable cv_data_;   // ring became non-empty
  std::condition_variable cv_space_;  // ring has room again
  std::condition_variable cv_sent_;   // sent_ advanced
  std::size_t head_ = 0;   // consumer read offset
  std::size_t tail_ = 0;   // producer write offset
  std::size_t used_ = 0;   // bytes committed, including pad slots
  std::uint64_t enqueued_ = 0;
  std::uint64_t sent_ = 0;
  unsigned space_waiters_ = 0;
  unsigned flush_waiters_ = 0;
  bool consumer_waiting_ = false;
  bool stop_ = false;

  std::atomic<std::uint64_t> failures_{0};
  static std::atomic<bool> forked_child_;

  std::thread worker_;  // last member: started after everything above exists
};

std::atomic<bool> Sender::forked_child_{false};

struct JournalState
{
  // false when journald is absent and RCL_LOGGING_JOURNAL_STRICT=0
  // turned the backend into a no-op.
  bool enabled = true;
  ConstantFields constants;
  std::unique_ptr<Sender> sender;  // null when !enabled
};

// Written by initialize()/shutdown() only, read by log(). Concurrent
// initialize/shutdown against log() is undefined, as for every other
// rcl_logging backend.
std::unique_ptr<JournalState> g_state;
std::once_flag g_atfork_once;

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

// Parses RCL_LOGGING_JOURNAL_BUFFER_SIZE: a decimal byte count with an optional
// K, M or G suffix (powers of 1024), e.g. "262144", "256K", "4M". Empty selects
// the default. The result is rounded up to a multiple of 8 (slot alignment)
// and must be within [kMinRingCapacity, kMaxRingCapacity]. Returns true and
// sets `capacity` on success; false with error set otherwise.
bool parse_buffer_size(const std::string & raw, std::size_t & capacity)
{
  if (raw.empty()) {
    capacity = kDefaultRingCapacity;
    return true;
  }
  const auto invalid = [&raw]() {
      RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "invalid value '%s' for %s, expected a byte count between %zu and %zu "
        "with an optional K, M or G suffix (e.g. 256K or 4M)",
        raw.c_str(), kEnvBufferSize, kMinRingCapacity, kMaxRingCapacity);
      return false;
    };
  std::size_t pos = 0;
  std::uint64_t value = 0;
  while (pos < raw.size() && raw[pos] >= '0' && raw[pos] <= '9') {
    const std::uint64_t digit = static_cast<std::uint64_t>(raw[pos] - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return invalid();
    }
    value = value * 10 + digit;
    ++pos;
  }
  if (pos == 0) {
    return invalid();  // no digits at all (also rejects a leading sign or space)
  }
  if (pos < raw.size()) {
    unsigned shift = 0;
    const char suffix = raw[pos];
    if (suffix == 'k' || suffix == 'K') {
      shift = 10;
    } else if (suffix == 'm' || suffix == 'M') {
      shift = 20;
    } else if (suffix == 'g' || suffix == 'G') {
      shift = 30;
    } else {
      return invalid();
    }
    if (pos + 1 != raw.size()) {
      return invalid();  // trailing garbage such as "1MB"
    }
    if (value > (UINT64_MAX >> shift)) {
      return invalid();
    }
    value <<= shift;
  }
  if (value < kMinRingCapacity || value > kMaxRingCapacity) {
    return invalid();
  }
  // Slots are 8 byte aligned and the pad slot logic relies on the capacity
  // being a multiple of 8. kMaxRingCapacity is one, so this stays in range.
  capacity = (static_cast<std::size_t>(value) + 7u) & ~static_cast<std::size_t>(7u);
  return true;
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

// Parses "KEY=VALUE;KEY2=VALUE2" into constants.extra_fields.
bool parse_extra_fields(const std::string & raw, ConstantFields & constants)
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
    if (constants.extra_fields.size() >= kMaxExtraFields) {
      RCUTILS_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "%s has more than %zu entries", kEnvExtraFields, kMaxExtraFields);
      return false;
    }
    constants.extra_fields.push_back(token);
  }
  return true;
}

}  // namespace

// Humble ships rcl_logging_interface 2.x, whose initialize() has no
// file_name_prefix parameter (added in 3.0.0 / Jazzy). The identifier is
// therefore the env override or the executable name.
rcl_logging_ret_t rcl_logging_external_initialize(
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
  ConstantFields & constants = state->constants;

  // 1. SYSLOG_IDENTIFIER: env override -> executable name.
  std::string identifier;
  if (!get_env(kEnvIdentifier, identifier)) {
    return RCL_LOGGING_RET_ERROR;
  }
  if (identifier.empty()) {
    char * basec = rcutils_get_executable_name(allocator);
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
  constants.identifier_field = std::string("SYSLOG_IDENTIFIER=") + identifier;
  constants.add(constants.identifier_field);

  // 2. ROS2_DISTRO from $ROS_DISTRO, if any.
  std::string distro;
  if (!get_env(kEnvRosDistro, distro)) {
    return RCL_LOGGING_RET_ERROR;
  }
  if (!distro.empty()) {
    constants.distro_field = std::string("ROS2_DISTRO=") + distro;
    constants.add(constants.distro_field);
  }

  // 3. Static extra fields.
  std::string extra_fields_raw;
  if (!get_env(kEnvExtraFields, extra_fields_raw)) {
    return RCL_LOGGING_RET_ERROR;
  }
  if (!parse_extra_fields(extra_fields_raw, constants)) {
    return RCL_LOGGING_RET_INVALID_ARGUMENT;
  }
  for (const std::string & field : constants.extra_fields) {
    constants.add(field);
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

  // 5. Ring buffer capacity between the producers and the sender thread.
  std::string buffer_size_raw;
  if (!get_env(kEnvBufferSize, buffer_size_raw)) {
    return RCL_LOGGING_RET_ERROR;
  }
  std::size_t ring_capacity = kDefaultRingCapacity;
  if (!parse_buffer_size(buffer_size_raw, ring_capacity)) {
    return RCL_LOGGING_RET_INVALID_ARGUMENT;
  }

  // 6. Probe the journald native socket. libsystemd manages the client
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

  // 7. Start the sender thread. A forked child has no copy of it, so make
  // the child fall back to synchronous sends.
  if (state->enabled) {
    std::call_once(
      g_atfork_once, []() {
        pthread_atfork(nullptr, nullptr, &Sender::on_fork_child);
      });
    state->sender = std::make_unique<Sender>(state->constants, ring_capacity);
  }

  RCUTILS_LOG_DEBUG_NAMED(
    kLoggerName,
    "journald logging backend initialized: %s, %zu extra field(s), strict=%d, "
    "buffer=%zu bytes, enabled=%d",
    constants.identifier_field.c_str(), constants.extra_fields.size(),
    strict ? 1 : 0, ring_capacity, state->enabled ? 1 : 0);

  g_state = std::move(state);
  return RCL_LOGGING_RET_OK;
}

rcl_logging_ret_t rcl_logging_external_shutdown()
{
  if (g_state == nullptr) {
    return RCL_LOGGING_RET_OK;
  }
  std::uint64_t failures = 0;
  if (g_state->sender != nullptr) {
    // Destroying the sender drains the ring and joins the thread; count the
    // failures afterwards so records sent during the drain are included.
    Sender * sender = g_state->sender.get();
    std::unique_ptr<Sender> owned = std::move(g_state->sender);
    sender->flush();
    failures = sender->failures();
    owned.reset();
  }
  g_state.reset();
  if (failures != 0) {
    RCUTILS_LOG_WARN_NAMED(
      kLoggerName,
      "%" PRIu64 " log record(s) could not be delivered to systemd-journald",
      failures);
  }
  return RCL_LOGGING_RET_OK;
}

void rcl_logging_external_log(int severity, const char * name, const char * msg)
{
  const JournalState * state = g_state.get();
  if (state == nullptr || !state->enabled || msg == nullptr) {
    return;
  }
  const int priority = severity_to_priority(severity);
  const std::size_t name_len = (name != nullptr) ? std::strlen(name) : 0;
  // FATAL (LOG_CRIT) waits until the record is in journald's hands: journald
  // fsyncs CRIT and above at once, so a FATAL followed by a crash is on disk.
  state->sender->enqueue(
    priority, name, name_len, msg, std::strlen(msg), priority <= LOG_CRIT);
}

rcl_logging_ret_t rcl_logging_external_set_logger_level(const char * name, int level)
{
  // Intentionally a no-op. rcl filters on the rcutils logger level before the
  // backend is called, so a second threshold here only costs time; storage
  // side filtering belongs to journald (MaxLevelStore= in journald.conf).
  (void) name;
  (void) level;
  return RCL_LOGGING_RET_OK;
}
