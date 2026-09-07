# Rate limits and retention for robots

`rcl_logging_journal` has no knobs for rotation, compression or rate limiting on purpose: journald already has them, system wide, in `journald.conf(5)`.
This tutorial explains the settings that matter for a robot, the pitfalls, and the drop-in shipped in [config/ros2-journald.conf](../../config/ros2-journald.conf).

## Where configuration lives

Never edit `/etc/systemd/journald.conf` itself. Put a drop-in in `/etc/systemd/journald.conf.d/`:

```bash
sudo mkdir -p /etc/systemd/journald.conf.d
sudo cp config/ros2-journald.conf /etc/systemd/journald.conf.d/
sudo systemctl restart systemd-journald
# show the effective configuration, including drop-ins
systemd-analyze cat-config systemd/journald.conf
```

The restart is safe; journald keeps the existing files and nodes reconnect transparently (libsystemd reopens the socket per send).

## 1. Rate limiting: the one surprise

journald drops messages from a **service** (cgroup) that sends more than `RateLimitBurst` messages within `RateLimitIntervalSec`. Defaults are `10000` per `30s`, and the burst is scaled by the free space on the journal file system (down to 1x at 1 MiB free, up to 6x at 64 GiB free).

A ROS 2 node at DEBUG level with a 100 Hz timer and a few log lines per cycle reaches 10000 messages in about 30 s. When that happens the journal contains:

```text
systemd-journald[412]: Suppressed 2381 messages from user@1000.service
```

and those records are gone. Check for it:

```bash
journalctl -t systemd-journald -g Suppressed --since today
```

Options, from most to least targeted:

1. **Per unit** (nodes launched as systemd services): in the unit file

   ```ini
   [Service]
   LogRateLimitIntervalSec=30s
   LogRateLimitBurst=100000
   # or disable for this unit only
   LogRateLimitBurst=0
   ```

2. **Globally** in the drop-in:

   ```ini
   [Journal]
   RateLimitIntervalSec=30s
   RateLimitBurst=100000
   # RateLimitIntervalSec=0 or RateLimitBurst=0 disables rate limiting entirely
   ```

3. **Reduce the volume at the source**: `--ros-args --log-level <node>:=info`, or throttled macros (`RCLCPP_INFO_THROTTLE`).

Note that all processes launched from one interactive session share one cgroup (`user@1000.service` or `session-N.scope`), so the limit is shared by every node started from that terminal. Running nodes as systemd units gives each its own budget.

The benchmark harness (`scripts/benchmark/run_matrix.sh`, B3) measures at which rate suppression starts on your hardware.

## 2. Retention: size and time

```ini
[Journal]
Storage=persistent      # keep across reboots under /var/log/journal
SystemMaxUse=2G         # total cap for persistent journals
SystemKeepFree=1G       # never eat into the last 1 GiB of the disk
SystemMaxFileSize=128M  # rotate at this size (smaller = finer vacuum granularity)
SystemMaxFiles=100      # keep at most this many archived files
MaxRetentionSec=1month  # drop anything older (0 = size based only)
MaxFileSec=1day         # rotate at least daily even if small
```

`RuntimeMaxUse=` / `RuntimeKeepFree=` / `RuntimeMaxFileSize=` are the same knobs for the volatile journal in `/run/log/journal` (RAM).

Manual operations:

```bash
journalctl --disk-usage
sudo journalctl --rotate
sudo journalctl --vacuum-size=500M      # applies to archived files only
sudo journalctl --vacuum-time=7d
```

Sizing rule of thumb: measure bytes per record with the harness (B4) on your own message mix, multiply by your rate, and give `SystemMaxUse` enough room for the number of days you need to look back. Because rotated journals are still queryable, there is no reason to keep retention short for the sake of readability.

## 3. Compression

```ini
[Journal]
Compress=yes        # default; compress DATA objects above the threshold
# Compress=512      # systemd >= 245: threshold in bytes (default 512)
```

Modern systemd uses zstd. Short ROS lines stay uncompressed (fast path), large ones (backtraces, parameter dumps, `-o verbose` style messages) are compressed. Combined with field deduplication (`ROS2_NODE_NAME=talker`, `PRIORITY=6`, `SYSLOG_IDENTIFIER=talker` are stored once, not once per line) the journal is usually smaller than the equivalent text log for typical ROS traffic. Measure it with B4 rather than assuming.

## 4. Durability

```ini
[Journal]
SyncIntervalSec=5m
```

journald writes records to its mmap'd files immediately (they are visible to `journalctl` right away) but calls `fsync` on this interval, **except** for `CRIT`, `ALERT` and `EMERG` records which are synced at once. `rcl_logging_journal` maps ROS `FATAL` to `CRIT`, so FATAL lines survive a power cut without lowering `SyncIntervalSec` for everything. If you need every ERROR durable too, lower the interval and accept the extra I/O, or raise those messages to FATAL.

## 5. Forwarding and stdout

```ini
[Journal]
ForwardToSyslog=no      # set yes if rsyslog/imuxsock should also receive native records
ForwardToConsole=no
MaxLevelStore=debug     # highest level to store; lower it to drop DEBUG at the daemon
MaxLevelSyslog=debug
```

`MaxLevelStore=` is a coarse global filter; prefer ROS log levels per node.

## 6. Journal namespaces (advanced)

Since systemd 245 a second journald instance can own an isolated set of files with its own limits, useful to keep a very chatty robot stack away from the OS journal:

```bash
# /etc/systemd/journald@ros2.conf
[Journal]
Storage=persistent
SystemMaxUse=4G
RateLimitBurst=0
```

```ini
# in the unit files of the ROS 2 nodes
[Service]
LogNamespace=ros2
```

```bash
journalctl --namespace=ros2 -f ROS2_NODE_NAME=talker
```

Namespaces apply to systemd units only; the process must be started by systemd with `LogNamespace=`, so this is for production deployments rather than interactive use.

## 7. Checklist for a robot image

- [ ] `Storage=persistent` (or `mkdir /var/log/journal`)
- [ ] `SystemMaxUse=` sized from measured bytes/record and the desired look-back
- [ ] rate limit raised per unit or globally, verified with `journalctl -t systemd-journald -g Suppressed`
- [ ] nodes run as systemd units (own cgroup, own rate budget, `LogNamespace=` possible)
- [ ] `journalctl --verify` in the health check
- [ ] optional: `systemd-journal-upload` to a fleet collector

## Reference

- `journald.conf(5)`: https://www.freedesktop.org/software/systemd/man/latest/journald.conf.html
- `systemd.exec(5)` `LogRateLimitIntervalSec=`, `LogNamespace=`: https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html
- https://systemd.io/JOURNAL_FILE_FORMAT/
