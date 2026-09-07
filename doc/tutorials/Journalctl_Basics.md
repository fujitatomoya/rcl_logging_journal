# journalctl basics for ROS 2 logs

This tutorial shows how to read, filter, export and manage the ROS 2 log records written by `rcl_logging_journal`.
Everything here uses stock `journalctl`; nothing else needs to be installed.

## Prerequisites

- `rcl_logging_journal` built and selected (see [README](../../README.md)):

  ```bash
  export RCL_LOGGING_IMPLEMENTATION=rcl_logging_journal
  ros2 run demo_nodes_cpp talker &
  ros2 run demo_nodes_py listener &
  ```

- Permission to read the system journal. Either run `journalctl` as root, or be a member of the `systemd-journal` group (Ubuntu also grants `adm`):

  ```bash
  sudo usermod -aG systemd-journal $USER   # log out and in again
  ```

## The record

`rcl_logging_journal` writes each ROS log line as one journal entry with these indexed fields (see [design.md](../design.md) 4.5):

| field | example | meaning |
| :---- | :------ | :------ |
| `MESSAGE` | `[INFO] [1724869818.620827927] [talker]: Publishing: 'Hello World: 1'` | formatted message from rcl |
| `PRIORITY` | `6` | syslog priority (DEBUG=7, INFO=6, WARN=4, ERROR=3, FATAL=2) |
| `ROS2_SEVERITY` | `INFO` | original ROS wording |
| `ROS2_NODE_NAME` | `talker` | rcutils logger name (node name for node loggers, `talker.child` for sub loggers) |
| `SYSLOG_IDENTIFIER` | `talker` | executable name unless overridden, used by `journalctl -t` |
| `ROS2_DISTRO` | `rolling` | `$ROS_DISTRO` of the process |
| `_PID`, `_UID`, `_COMM`, `_EXE`, `_CMDLINE`, `_BOOT_ID`, `_HOSTNAME` | | trusted fields added by journald from kernel credentials |

Show every field of a record:

```bash
journalctl ROS2_NODE_NAME=talker -o verbose -n 1
```

## Filtering

Any `FIELD=value` argument is an exact match on an indexed field. Several values of the **same** field are OR-ed, different fields are AND-ed.

```bash
# follow one node live (like tail -f on a per node file)
journalctl -f ROS2_NODE_NAME=talker

# two nodes at once
journalctl -f ROS2_NODE_NAME=talker ROS2_NODE_NAME=listener

# by executable (journalctl -t is a shortcut for SYSLOG_IDENTIFIER=)
journalctl -t talker

# by process id (trusted, cannot be spoofed by the client)
journalctl _PID=31348

# WARN and above from any ROS 2 node
journalctl -p warning ROS2_DISTRO=rolling

# ERROR and FATAL of one node, using the ROS wording
journalctl ROS2_NODE_NAME=talker ROS2_SEVERITY=ERROR ROS2_SEVERITY=FATAL

# add a plain text search on top of the field matches
journalctl ROS2_NODE_NAME=talker -g "Hello World: 4"
```

`-p` accepts the syslog names: `debug`, `info`, `warning` (WARN), `err` (ERROR), `crit` (FATAL), or a range such as `-p err..crit`.

## Time windows and boots

```bash
# last 10 minutes
journalctl ROS2_NODE_NAME=talker --since "10 min ago"

# a window
journalctl ROS2_DISTRO=rolling --since "2026-09-07 09:00" --until "2026-09-07 09:30"

# the current boot only
journalctl -b ROS2_NODE_NAME=talker

# the previous boot: what happened right before the robot rebooted?
journalctl -b -1 -p warning ROS2_DISTRO=rolling

# list boots known to the journal
journalctl --list-boots
```

`-b -1` is the killer feature after a crash or a watchdog reboot: the log of the previous life of the robot is still there and still filterable, without hunting for rotated files.

## Output formats

```bash
# ROS style, one line per record with the journald timestamp
journalctl ROS2_NODE_NAME=talker -o short-precise

# only the MESSAGE (what the console handler would have printed)
journalctl ROS2_NODE_NAME=talker -o cat

# JSON, one object per line, for offline analysis
journalctl ROS2_NODE_NAME=talker -o json > talker.jsonl

# pretty JSON
journalctl ROS2_NODE_NAME=talker -o json-pretty -n 1

# binary export format for re-import with systemd-journal-remote
journalctl ROS2_NODE_NAME=talker -o export > talker.export
```

Example JSON record:

```json
{"MESSAGE":"[INFO] [1724869818.620827927] [talker]: Publishing: 'Hello World: 1'",
 "PRIORITY":"6","ROS2_SEVERITY":"INFO","ROS2_NODE_NAME":"talker",
 "SYSLOG_IDENTIFIER":"talker","ROS2_DISTRO":"rolling",
 "_PID":"31348","_UID":"1000","_COMM":"talker","_TRANSPORT":"journal",
 "_BOOT_ID":"376ab38eebb14eedb2304ad5aa7f5f0e","_HOSTNAME":"robot-07", "...":"..."}
```

## Listing what is there

```bash
# all values a field has taken: which nodes have logged?
journalctl -F ROS2_NODE_NAME

# all executables
journalctl -F SYSLOG_IDENTIFIER

# all field names used by ROS 2 records
journalctl ROS2_DISTRO=rolling -o json -n 1 | tr ',' '\n' | cut -d: -f1
```

## Storage, rotation and vacuum

```bash
# how much disk the journal uses right now
journalctl --disk-usage

# force rotation now
sudo journalctl --rotate

# drop archived files older than 7 days / above 500 MiB total / keep 10 files
sudo journalctl --vacuum-time=7d
sudo journalctl --vacuum-size=500M
sudo journalctl --vacuum-files=10

# verify file integrity
journalctl --verify
```

Vacuuming only removes **archived** (rotated) files, so `--rotate` first if you want to reclaim space immediately.

### Persistent vs volatile

- `Storage=auto` (default): persistent under `/var/log/journal` **if that directory exists**, otherwise volatile in `/run/log/journal` (RAM, lost at reboot).
- Make it persistent once:

  ```bash
  sudo mkdir -p /var/log/journal
  sudo systemd-tmpfiles --create --prefix /var/log/journal
  sudo systemctl restart systemd-journald
  ```

  or set `Storage=persistent` in a drop-in, see [Rate_Limits_And_Retention.md](./Rate_Limits_And_Retention.md).

## Fleet fields

With `RCL_LOGGING_JOURNAL_EXTRA_FIELDS` every record carries your own static fields:

```bash
export RCL_LOGGING_JOURNAL_EXTRA_FIELDS="ROBOT_ID=amr-07;FLEET=tokyo;SITE=warehouse-3"
ros2 launch my_robot bringup.launch.py
```

```bash
journalctl FLEET=tokyo -p warning --since today
journalctl -F ROBOT_ID
```

After shipping journals to a central host with `systemd-journal-remote`, the same queries work across the whole fleet.

## Cheat sheet

| I want to ... | command |
| :--- | :--- |
| tail one node | `journalctl -f ROS2_NODE_NAME=<node>` |
| tail everything ROS 2 | `journalctl -f ROS2_DISTRO=$ROS_DISTRO` |
| errors since boot | `journalctl -b -p err ROS2_DISTRO=$ROS_DISTRO` |
| previous boot | `journalctl -b -1 ROS2_NODE_NAME=<node>` |
| export JSON | `journalctl ROS2_NODE_NAME=<node> -o json > out.jsonl` |
| who logged? | `journalctl -F ROS2_NODE_NAME` |
| all fields of a record | `journalctl ROS2_NODE_NAME=<node> -o verbose -n 1` |
| disk usage | `journalctl --disk-usage` |
| reclaim space | `sudo journalctl --rotate && sudo journalctl --vacuum-size=500M` |

## Reference

- `journalctl(1)`: https://www.freedesktop.org/software/systemd/man/latest/journalctl.html
- `systemd.journal-fields(7)`: https://www.freedesktop.org/software/systemd/man/latest/systemd.journal-fields.html
