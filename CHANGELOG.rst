^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package rcl_logging_journal
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.1.0 (unreleased)
------------------
* Initial implementation of the rcl_logging_interface on top of the
  systemd-journald native protocol (sd_journal_sendv).
* Structured record schema: MESSAGE, PRIORITY, SYSLOG_IDENTIFIER,
  ROS2_NODE_NAME, ROS2_DISTRO and user defined extra fields.
* Asynchronous hot path: records are copied into a preallocated ring buffer and
  sent to journald by one sender thread; FATAL stays synchronous; no severity
  filtering in the backend (rcl and journald MaxLevelStore= do that).
* Environment configuration: RCL_LOGGING_JOURNAL_IDENTIFIER,
  RCL_LOGGING_JOURNAL_EXTRA_FIELDS, RCL_LOGGING_JOURNAL_STRICT,
  RCL_LOGGING_JOURNAL_SOCKET_PATH, RCL_LOGGING_JOURNAL_BUFFER_SIZE (ring
  buffer capacity, 1 MiB by default).
* gtest suite reading records back through the sd_journal API and journalctl.
* GitHub workflows per distribution with a standalone journald in the container,
  Mergify backports, codespell, stale and ABI checks.
* Design document, journalctl / container / retention tutorials, overview deck.
* Benchmark harness comparing the spdlog and journald backends.
* Contributors: Tomoya Fujita
