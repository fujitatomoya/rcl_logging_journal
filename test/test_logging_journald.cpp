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

// These tests write through rcl_logging_journal and read the records back
// with the sd_journal API of the very same libsystemd, so they need a running
// systemd-journald and read access to its journal files (root, or membership
// in the systemd-journal / adm group). See README.md "Test".

#include <systemd/sd-journal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "rcl_logging_interface/rcl_logging_interface.h"

#include "rcpputils/env.hpp"

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging.h"
#include "rcutils/process.h"

namespace
{

constexpr int logger_levels[] =
{
  RCUTILS_LOG_SEVERITY_UNSET,
  RCUTILS_LOG_SEVERITY_DEBUG,
  RCUTILS_LOG_SEVERITY_INFO,
  RCUTILS_LOG_SEVERITY_WARN,
  RCUTILS_LOG_SEVERITY_ERROR,
  RCUTILS_LOG_SEVERITY_FATAL,
};

// Expected journald PRIORITY for each RCUTILS severity (design.md 4.3).
std::string expected_priority(int severity)
{
  switch (severity) {
    case RCUTILS_LOG_SEVERITY_DEBUG: return "7";
    case RCUTILS_LOG_SEVERITY_INFO: return "6";
    case RCUTILS_LOG_SEVERITY_WARN: return "4";
    case RCUTILS_LOG_SEVERITY_ERROR: return "3";
    case RCUTILS_LOG_SEVERITY_FATAL: return "2";
    default: return "6";  // UNSET and unknown values
  }
}

std::string random_token()
{
  std::random_device rd;
  std::uniform_int_distribution<uint32_t> dist;
  std::stringstream ss;
  ss << std::hex << rcutils_get_pid() << "_" << dist(rd) << dist(rd);
  return ss.str();
}

// A journal entry as KEY -> VALUE (binary values are kept verbatim).
using JournalEntry = std::map<std::string, std::string>;

// Reads back every entry that matches all `matches` ("KEY=VALUE"), retrying
// until at least `expected_count` entries are visible or `timeout` passes.
// journald processes datagrams asynchronously, so a short wait is normal.
std::vector<JournalEntry> read_journal(
  const std::vector<std::string> & matches,
  std::size_t expected_count,
  std::chrono::milliseconds timeout = std::chrono::seconds(15))
{
  std::vector<JournalEntry> entries;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    entries.clear();
    sd_journal * journal = nullptr;
    int ret = sd_journal_open(&journal, 0);
    if (ret < 0) {
      throw std::runtime_error(
              std::string("sd_journal_open failed: ") + std::strerror(-ret));
    }
    for (const std::string & match : matches) {
      ret = sd_journal_add_match(journal, match.c_str(), 0);
      if (ret < 0) {
        sd_journal_close(journal);
        throw std::runtime_error(
                std::string("sd_journal_add_match failed: ") + std::strerror(-ret));
      }
    }
    ret = sd_journal_seek_head(journal);
    if (ret < 0) {
      sd_journal_close(journal);
      throw std::runtime_error(
              std::string("sd_journal_seek_head failed: ") + std::strerror(-ret));
    }
    while (sd_journal_next(journal) > 0) {
      JournalEntry entry;
      const void * data = nullptr;
      size_t length = 0;
      SD_JOURNAL_FOREACH_DATA(journal, data, length) {
        const char * field = static_cast<const char *>(data);
        const char * eq = static_cast<const char *>(std::memchr(field, '=', length));
        if (eq == nullptr) {
          continue;
        }
        const std::size_t key_len = static_cast<std::size_t>(eq - field);
        entry.emplace(
          std::string(field, key_len),
          std::string(eq + 1, length - key_len - 1));
      }
      entries.push_back(std::move(entry));
    }
    sd_journal_close(journal);

    if (entries.size() >= expected_count || std::chrono::steady_clock::now() >= deadline) {
      return entries;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

std::string executable_name()
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  char * exe = rcutils_get_executable_name(allocator);
  if (exe == nullptr) {
    throw std::runtime_error("Failed to determine executable name");
  }
  std::string result(exe);
  allocator.deallocate(exe, allocator.state);
  return result;
}

// Resets an environment variable when leaving scope.
class RestoreEnvVar final
{
public:
  explicit RestoreEnvVar(const std::string & name)
  : name_(name),
    value_(rcpputils::get_env_var(name.c_str()))
  {
  }

  ~RestoreEnvVar()
  {
    if (!rcpputils::set_env_var(name_.c_str(), value_.empty() ? nullptr : value_.c_str())) {
      std::cerr << "Failed to restore value of environment variable: " << name_ << std::endl;
    }
  }

private:
  const std::string name_;
  const std::string value_;
};

}  // namespace

class AllocatorTest : public ::testing::Test
{
public:
  AllocatorTest()
  : allocator(rcutils_get_default_allocator()),
    bad_allocator(get_bad_allocator()),
    invalid_allocator(rcutils_get_zero_initialized_allocator())
  {
  }

  rcutils_allocator_t allocator;
  rcutils_allocator_t bad_allocator;
  rcutils_allocator_t invalid_allocator;

private:
  static rcutils_allocator_t get_bad_allocator()
  {
    rcutils_allocator_t bad_allocator = rcutils_get_default_allocator();
    bad_allocator.allocate = AllocatorTest::bad_malloc;
    bad_allocator.reallocate = AllocatorTest::bad_realloc;
    return bad_allocator;
  }

  static void * bad_malloc(size_t, void *)
  {
    return nullptr;
  }

  static void * bad_realloc(void *, size_t, void *)
  {
    return nullptr;
  }
};

class LoggingTest : public ::testing::Test
{
public:
  void SetUp() override
  {
    allocator = rcutils_get_default_allocator();
    token = random_token();
    // Unique logger name per test so the read back never sees other runs.
    logger_name = "rcl_logging_journal_test_" + token;
    node_match = "ROS2_NODE_NAME=" + logger_name;
  }

  void TearDown() override
  {
    // Every test must leave the backend shut down for the next one.
    EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());
    rcutils_reset_error();
  }

  rcutils_allocator_t allocator;
  std::string token;
  std::string logger_name;
  std::string node_match;
};

TEST_F(AllocatorTest, init_invalid)
{
  // The executable name is the only allocation; make sure it is exercised.
  RestoreEnvVar identifier_var("RCL_LOGGING_JOURNAL_IDENTIFIER");
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_IDENTIFIER", nullptr));

  EXPECT_EQ(
    RCL_LOGGING_RET_ERROR,
    rcl_logging_external_initialize(nullptr, nullptr, bad_allocator));
  EXPECT_TRUE(rcutils_error_is_set());
  rcutils_reset_error();
  EXPECT_EQ(
    RCL_LOGGING_RET_INVALID_ARGUMENT,
    rcl_logging_external_initialize(nullptr, nullptr, invalid_allocator));
  rcutils_reset_error();
}

TEST_F(AllocatorTest, init_valid)
{
  // Config files are not supported and pass through with a warning.
  EXPECT_EQ(
    RCL_LOGGING_RET_OK,
    rcl_logging_external_initialize(nullptr, "config_file", allocator));
  // Initializing twice is fine.
  EXPECT_EQ(
    RCL_LOGGING_RET_OK,
    rcl_logging_external_initialize(nullptr, nullptr, allocator));
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());
  // Shutting down twice is fine as well.
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());
  rcutils_reset_error();
}

TEST_F(LoggingTest, record_fields)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  const std::string message = "record_fields " + token;
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), message.c_str());

  std::vector<JournalEntry> entries = read_journal({node_match}, 1);
  ASSERT_EQ(1u, entries.size());
  const JournalEntry & entry = entries.front();

  // Fields written by the backend.
  EXPECT_EQ(message, entry.at("MESSAGE"));
  EXPECT_EQ("6", entry.at("PRIORITY"));
  EXPECT_EQ(logger_name, entry.at("ROS2_NODE_NAME"));
  EXPECT_EQ(executable_name(), entry.at("SYSLOG_IDENTIFIER"));
  // Severity is carried by PRIORITY only (journalctl -p); no duplicate field.
  EXPECT_EQ(0u, entry.count("ROS2_SEVERITY"));
  const std::string ros_distro = rcpputils::get_env_var("ROS_DISTRO");
  if (ros_distro.empty()) {
    EXPECT_EQ(0u, entry.count("ROS2_DISTRO"));
  } else {
    EXPECT_EQ(ros_distro, entry.at("ROS2_DISTRO"));
  }
  // Location fields must not point into the backend itself.
  EXPECT_EQ(0u, entry.count("CODE_FILE"));
  EXPECT_EQ(0u, entry.count("CODE_LINE"));
  EXPECT_EQ(0u, entry.count("CODE_FUNC"));

  // Trusted fields added by journald from the sender's credentials.
  EXPECT_EQ(std::to_string(rcutils_get_pid()), entry.at("_PID"));
  EXPECT_EQ("journal", entry.at("_TRANSPORT"));
  EXPECT_EQ(1u, entry.count("_BOOT_ID"));
  EXPECT_EQ(1u, entry.count("_COMM"));
}

TEST_F(LoggingTest, severity_mapping)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  for (int severity : logger_levels) {
    std::stringstream ss;
    ss << "severity_mapping " << token << " " << severity;
    rcl_logging_external_log(severity, logger_name.c_str(), ss.str().c_str());
  }

  const std::size_t expected = sizeof(logger_levels) / sizeof(logger_levels[0]);
  std::vector<JournalEntry> entries = read_journal({node_match}, expected);
  ASSERT_EQ(expected, entries.size());

  for (int severity : logger_levels) {
    std::stringstream ss;
    ss << "severity_mapping " << token << " " << severity;
    bool found = false;
    for (const JournalEntry & entry : entries) {
      if (entry.at("MESSAGE") != ss.str()) {
        continue;
      }
      found = true;
      EXPECT_EQ(expected_priority(severity), entry.at("PRIORITY")) << "severity " << severity;
    }
    EXPECT_TRUE(found) << "missing record for severity " << severity;
  }
}

TEST_F(LoggingTest, full_cycle)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));
  // Make sure we can call initialize more than once
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  // The backend does not filter: rcl filters on the logger level before the
  // backend is called and journald filters with MaxLevelStore=. So
  // set_logger_level() is accepted but every record reaches the journal.
  std::map<std::string, std::string> expected_messages;  // MESSAGE -> PRIORITY
  for (int level : logger_levels) {
    EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_set_logger_level(nullptr, level));

    for (int severity : logger_levels) {
      std::stringstream ss;
      ss << "full_cycle " << token << " severity " << severity << " at level " << level;
      rcl_logging_external_log(severity, logger_name.c_str(), ss.str().c_str());
      expected_messages.emplace(ss.str(), expected_priority(severity));
    }
  }
  // shutdown drains the ring before returning
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());

  std::vector<JournalEntry> entries = read_journal({node_match}, expected_messages.size());
  std::map<std::string, std::string> actual_messages;
  for (const JournalEntry & entry : entries) {
    actual_messages.emplace(entry.at("MESSAGE"), entry.at("PRIORITY"));
  }
  EXPECT_EQ(expected_messages, actual_messages);
}

TEST_F(LoggingTest, shutdown_drains_queue)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  // More bytes than the 1 MiB ring holds, so producers wrap and block on the
  // sender thread; every record must still be in the journal after shutdown,
  // in order.
  constexpr int count = 20000;
  std::string payload(200, 'p');
  for (int i = 0; i < count; ++i) {
    std::stringstream ss;
    ss << "drain " << token << " " << i << " " << payload;
    rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), ss.str().c_str());
  }
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());

  std::vector<JournalEntry> entries = read_journal({node_match}, count, std::chrono::seconds(60));
  ASSERT_EQ(static_cast<std::size_t>(count), entries.size());
  for (int i = 0; i < count; ++i) {
    std::stringstream ss;
    ss << "drain " << token << " " << i << " ";
    EXPECT_EQ(0u, entries[static_cast<std::size_t>(i)].at("MESSAGE").rfind(ss.str(), 0))
      << "record " << i << " out of order";
  }
}

TEST_F(LoggingTest, concurrent_producers_keep_order)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  constexpr int threads = 4;
  constexpr int per_thread = 2000;
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back(
      [this, t]() {
        for (int i = 0; i < per_thread; ++i) {
          std::stringstream ss;
          ss << "concurrent " << token << " thread " << t << " seq " << i;
          rcl_logging_external_log(
            RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), ss.str().c_str());
        }
      });
  }
  for (std::thread & worker : workers) {
    worker.join();
  }
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());

  std::vector<JournalEntry> entries =
    read_journal({node_match}, threads * per_thread, std::chrono::seconds(60));
  ASSERT_EQ(static_cast<std::size_t>(threads * per_thread), entries.size());
  // Records of one thread must appear in the order that thread logged them.
  std::vector<int> next_seq(threads, 0);
  const std::string prefix = "concurrent " + token + " thread ";
  for (const JournalEntry & entry : entries) {
    const std::string & message = entry.at("MESSAGE");
    ASSERT_EQ(0u, message.rfind(prefix, 0)) << message;
    std::istringstream fields(message.substr(prefix.size()));
    int t = -1;
    int seq = -1;
    std::string seq_word;
    fields >> t >> seq_word >> seq;
    ASSERT_EQ("seq", seq_word) << message;
    ASSERT_GE(t, 0);
    ASSERT_LT(t, threads);
    EXPECT_EQ(next_seq[static_cast<std::size_t>(t)], seq) << "thread " << t;
    next_seq[static_cast<std::size_t>(t)] = seq + 1;
  }
}

TEST_F(LoggingTest, fatal_is_synchronous)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  // Queue a burst, then a FATAL: when the FATAL call returns, it and every
  // record before it must already have been handed to journald. Verify with a
  // single, non retrying read (timeout 0).
  for (int i = 0; i < 500; ++i) {
    std::stringstream ss;
    ss << "before fatal " << token << " " << i;
    rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), ss.str().c_str());
  }
  const std::string fatal = "fatal " + token;
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_FATAL, logger_name.c_str(), fatal.c_str());

  // journald processes the datagrams asynchronously, allow it a moment but do
  // not wait for our own sender thread: it must already be done.
  std::vector<JournalEntry> entries = read_journal({node_match}, 501, std::chrono::seconds(5));
  ASSERT_EQ(501u, entries.size());
  EXPECT_EQ(fatal, entries.back().at("MESSAGE"));
  EXPECT_EQ("2", entries.back().at("PRIORITY"));
}

TEST_F(LoggingTest, no_logger_name)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  // rcl logs some records without a logger name; they must still be stored,
  // just without ROS2_NODE_NAME. Match on the unique MESSAGE instead.
  const std::string message_null = "no_logger_name null " + token;
  const std::string message_empty = "no_logger_name empty " + token;
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_WARN, nullptr, message_null.c_str());
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_WARN, "", message_empty.c_str());

  for (const std::string & message : {message_null, message_empty}) {
    std::vector<JournalEntry> entries = read_journal(
      {"MESSAGE=" + message, "_PID=" + std::to_string(rcutils_get_pid())}, 1);
    ASSERT_EQ(1u, entries.size()) << message;
    EXPECT_EQ(0u, entries.front().count("ROS2_NODE_NAME"));
    EXPECT_EQ("4", entries.front().at("PRIORITY"));
    EXPECT_EQ(executable_name(), entries.front().at("SYSLOG_IDENTIFIER"));
  }
}

TEST_F(LoggingTest, file_name_prefix_as_identifier)
{
  RestoreEnvVar identifier_var("RCL_LOGGING_JOURNAL_IDENTIFIER");
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_IDENTIFIER", nullptr));

  // --log-file-name prefix is used as SYSLOG_IDENTIFIER when no env override.
  const std::string prefix = "prefix_" + token;
  ASSERT_EQ(
    RCL_LOGGING_RET_OK,
    rcl_logging_external_initialize(prefix.c_str(), nullptr, allocator));
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), "prefix test");

  std::vector<JournalEntry> entries = read_journal({node_match}, 1);
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(prefix, entries.front().at("SYSLOG_IDENTIFIER"));
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());

  // An empty prefix falls back to the executable name.
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize("", nullptr, allocator));
  const std::string message = "empty prefix " + token;
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), message.c_str());
  entries = read_journal({node_match, "MESSAGE=" + message}, 1);
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(executable_name(), entries.front().at("SYSLOG_IDENTIFIER"));
}

TEST_F(LoggingTest, identifier_override)
{
  RestoreEnvVar identifier_var("RCL_LOGGING_JOURNAL_IDENTIFIER");
  const std::string identifier = "custom_ident_" + token;
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_IDENTIFIER", identifier.c_str()));

  // The env override wins over the file name prefix.
  ASSERT_EQ(
    RCL_LOGGING_RET_OK,
    rcl_logging_external_initialize("ignored_prefix", nullptr, allocator));
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_ERROR, logger_name.c_str(), "identifier test");

  // journalctl -t <identifier> maps to this match.
  std::vector<JournalEntry> entries = read_journal({"SYSLOG_IDENTIFIER=" + identifier}, 1);
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(logger_name, entries.front().at("ROS2_NODE_NAME"));
  EXPECT_EQ("3", entries.front().at("PRIORITY"));
}

TEST_F(LoggingTest, extra_fields)
{
  RestoreEnvVar extra_var("RCL_LOGGING_JOURNAL_EXTRA_FIELDS");
  const std::string robot_id = "amr-" + token;
  const std::string extra = "ROBOT_ID=" + robot_id + ";FLEET=tokyo;EMPTY_VALUE=;";
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_EXTRA_FIELDS", extra.c_str()));

  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), "extra fields");

  // Extra fields are indexed like any other field: match on ROBOT_ID.
  std::vector<JournalEntry> entries = read_journal({"ROBOT_ID=" + robot_id}, 1);
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(logger_name, entries.front().at("ROS2_NODE_NAME"));
  EXPECT_EQ("tokyo", entries.front().at("FLEET"));
  ASSERT_EQ(1u, entries.front().count("EMPTY_VALUE"));
  EXPECT_EQ("", entries.front().at("EMPTY_VALUE"));
}

TEST_F(LoggingTest, extra_fields_invalid)
{
  RestoreEnvVar extra_var("RCL_LOGGING_JOURNAL_EXTRA_FIELDS");

  const char * invalid_values[] = {
    "NOEQUALS",             // not KEY=VALUE
    "lowercase=1",          // journald requires [A-Z0-9_]
    "_TRUSTED=1",           // leading underscore is reserved for journald
    "=1",                   // empty key
    "MESSAGE=spoof",        // set by the backend itself
    "ROS2_NODE_NAME=spoof",  // set by the backend itself
    "BAD-CHAR=1",           // '-' not allowed
  };
  for (const char * value : invalid_values) {
    ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_EXTRA_FIELDS", value));
    EXPECT_EQ(
      RCL_LOGGING_RET_INVALID_ARGUMENT,
      rcl_logging_external_initialize(nullptr, nullptr, allocator)) << value;
    EXPECT_TRUE(rcutils_error_is_set()) << value;
    rcutils_reset_error();
    EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());
  }

  // Too many entries.
  std::stringstream too_many;
  for (int i = 0; i < 33; ++i) {
    too_many << "FIELD_" << i << "=" << i << ";";
  }
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_EXTRA_FIELDS", too_many.str().c_str()));
  EXPECT_EQ(
    RCL_LOGGING_RET_INVALID_ARGUMENT,
    rcl_logging_external_initialize(nullptr, nullptr, allocator));
  rcutils_reset_error();
}

TEST_F(LoggingTest, strict_mode)
{
  RestoreEnvVar strict_var("RCL_LOGGING_JOURNAL_STRICT");
  RestoreEnvVar socket_var("RCL_LOGGING_JOURNAL_SOCKET_PATH");
  // Point the availability probe at a path that cannot exist.
  const std::string missing = "/nonexistent/rcl_logging_journal_" + token + "/socket";
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_SOCKET_PATH", missing.c_str()));

  // Default is strict: initialization fails with an actionable error.
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_STRICT", nullptr));
  EXPECT_EQ(
    RCL_LOGGING_RET_ERROR,
    rcl_logging_external_initialize(nullptr, nullptr, allocator));
  ASSERT_TRUE(rcutils_error_is_set());
  EXPECT_NE(nullptr, std::strstr(rcutils_get_error_string().str, missing.c_str()));
  rcutils_reset_error();
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());

  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_STRICT", "1"));
  EXPECT_EQ(
    RCL_LOGGING_RET_ERROR,
    rcl_logging_external_initialize(nullptr, nullptr, allocator));
  rcutils_reset_error();
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());

  // Non-strict: initialization succeeds and logging becomes a no-op.
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_STRICT", "0"));
  EXPECT_EQ(
    RCL_LOGGING_RET_OK,
    rcl_logging_external_initialize(nullptr, nullptr, allocator));
  const std::string message = "strict_mode must not be stored " + token;
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_FATAL, logger_name.c_str(), message.c_str());
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_set_logger_level(nullptr, 0));
  EXPECT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_shutdown());
  std::vector<JournalEntry> entries = read_journal({node_match}, 1, std::chrono::seconds(1));
  EXPECT_EQ(0u, entries.size());

  // Garbage is rejected.
  ASSERT_TRUE(rcpputils::set_env_var("RCL_LOGGING_JOURNAL_STRICT", "maybe"));
  EXPECT_EQ(
    RCL_LOGGING_RET_INVALID_ARGUMENT,
    rcl_logging_external_initialize(nullptr, nullptr, allocator));
  rcutils_reset_error();
}

TEST_F(LoggingTest, large_message)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  auto make_message = [this](const char * tag, std::size_t target) {
      std::string message = std::string(tag) + " " + token + " ";
      while (message.size() < target) {
        message += "0123456789abcdef";
      }
      return message;
    };
  // 100 KiB goes through the ring but above the AF_UNIX datagram limit, so
  // libsystemd takes the sealed memfd path and journald compresses it.
  const std::string ring_message = make_message("large_ring", 100 * 1024);
  // 300 KiB is above the ring threshold: drained, then sent synchronously.
  const std::string sync_message = make_message("large_sync", 300 * 1024);
  const std::string after = "large_after " + token;
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), ring_message.c_str());
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), sync_message.c_str());
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, logger_name.c_str(), after.c_str());

  std::vector<JournalEntry> entries = read_journal({node_match}, 3);
  ASSERT_EQ(3u, entries.size());
  EXPECT_EQ(ring_message.size(), entries[0].at("MESSAGE").size());
  EXPECT_EQ(ring_message, entries[0].at("MESSAGE"));
  EXPECT_EQ(sync_message.size(), entries[1].at("MESSAGE").size());
  EXPECT_EQ(sync_message, entries[1].at("MESSAGE"));
  EXPECT_EQ(after, entries[2].at("MESSAGE"));
}

TEST_F(LoggingTest, long_logger_name)
{
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  // Hierarchical logger names can exceed the stack scratch buffer.
  std::string long_name = logger_name;
  while (long_name.size() < 2048) {
    long_name += ".child";
  }
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_INFO, long_name.c_str(), "long name");

  std::vector<JournalEntry> entries = read_journal({"ROS2_NODE_NAME=" + long_name}, 1);
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(long_name, entries.front().at("ROS2_NODE_NAME"));
}

TEST_F(LoggingTest, journalctl_smoke)
{
  // Guard the real user experience: the record must be reachable through the
  // journalctl command line as documented in the README.
  if (std::system("journalctl --version > /dev/null 2>&1") != 0) {
    GTEST_SKIP() << "journalctl is not available";
  }
  ASSERT_EQ(RCL_LOGGING_RET_OK, rcl_logging_external_initialize(nullptr, nullptr, allocator));

  const std::string message = "journalctl_smoke " + token;
  rcl_logging_external_log(RCUTILS_LOG_SEVERITY_WARN, logger_name.c_str(), message.c_str());
  // Wait until the record is visible through the library first.
  ASSERT_EQ(1u, read_journal({node_match}, 1).size());

  const std::string command = "journalctl -q " + node_match + " -o json 2>&1";
  FILE * pipe = popen(command.c_str(), "r");
  ASSERT_NE(nullptr, pipe);
  std::string output;
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  const int status = pclose(pipe);
  EXPECT_EQ(0, status) << output;
  EXPECT_NE(std::string::npos, output.find("\"MESSAGE\":\"" + message + "\"")) << output;
  EXPECT_NE(std::string::npos, output.find("\"PRIORITY\":\"4\"")) << output;
}
