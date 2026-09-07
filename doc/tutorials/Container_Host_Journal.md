# ROS 2 in a container, logs in the host journal

Robots commonly run ROS 2 nodes in containers. Containers have no init system and no `systemd-journald`, but the **host** does.
This tutorial shows how to let containerized nodes log straight into the host journal with `rcl_logging_journal`, so that `journalctl` on the host (or in a maintenance shell) sees every node of every container, with trusted metadata, and rotation/retention handled once, on the host.

```text
+-------------------------------+            +-----------------------------------+
| container (any base image)    |            | host (systemd)                    |
|                               |  bind      |                                   |
|  ros2 run ... talker          |  mount     |  systemd-journald                 |
|    rcl_logging_journal -------+------------+--> /run/systemd/journal/socket    |
|                               |            |        |                          |
|  no journald, no rsyslogd     |            |        v                          |
+-------------------------------+            |  /var/log/journal/<machine-id>/   |
                                             |        ^                          |
                                             |  journalctl -f ROS2_NODE_NAME=... |
                                             +-----------------------------------+
```

## 1. Bind the native socket

`rcl_logging_journal` talks to `/run/systemd/journal/socket` (the **native** protocol socket), not `/dev/log` (the syslog socket used by `rcl_logging_syslog`). Bind just that path:

```bash
docker run -it --rm \
  -v /run/systemd/journal/socket:/run/systemd/journal/socket \
  -e RCL_LOGGING_IMPLEMENTATION=rcl_logging_journal \
  my_ros2_image \
  ros2 run demo_nodes_cpp talker
```

On the **host**:

```bash
journalctl -f ROS2_NODE_NAME=talker
```

```text
Sep 07 10:15:01 robot-07 talker[3141]: [INFO] [1757236501.620827927] [talker]: Publishing: 'Hello World: 1'
Sep 07 10:15:02 robot-07 talker[3141]: [INFO] [1757236502.620758249] [talker]: Publishing: 'Hello World: 2'
```

The image only needs `libsystemd0` (present in every Debian/Ubuntu base image) at runtime; no daemon, no configuration file, nothing to start.

### Compose

```yaml
services:
  talker:
    image: my_ros2_image
    command: ros2 run demo_nodes_cpp talker
    environment:
      RCL_LOGGING_IMPLEMENTATION: rcl_logging_journal
      RCL_LOGGING_JOURNAL_EXTRA_FIELDS: "ROBOT_ID=amr-07;CONTAINER=talker"
    volumes:
      - /run/systemd/journal/socket:/run/systemd/journal/socket
```

### Podman

```bash
podman run -it --rm \
  -v /run/systemd/journal/socket:/run/systemd/journal/socket \
  -e RCL_LOGGING_IMPLEMENTATION=rcl_logging_journal \
  my_ros2_image ros2 run demo_nodes_cpp talker
```

## 2. What the host sees

Trusted fields are filled by the **host** journald from the sender's kernel credentials, so they describe the container process as the host sees it:

```bash
journalctl ROS2_NODE_NAME=talker -o verbose -n 1
```

```text
    _PID=3141                      # host pid namespace
    _UID=1000                      # host uid (with user namespaces: the mapped uid)
    _COMM=talker
    _EXE=/opt/ros/rolling/lib/demo_nodes_cpp/talker     # path inside the container's rootfs
    _CMDLINE=/opt/ros/rolling/lib/demo_nodes_cpp/talker
    _SYSTEMD_CGROUP=/system.slice/docker-8f3c...scope    # ties the record to the container
    _HOSTNAME=robot-07             # host name, not the container id
    MESSAGE=[INFO] [1757236501.620827927] [talker]: Publishing: 'Hello World: 1'
    PRIORITY=6
    ROS2_NODE_NAME=talker
    ROS2_SEVERITY=INFO
    SYSLOG_IDENTIFIER=talker
    ROS2_DISTRO=rolling
```

Use `_SYSTEMD_CGROUP` (or `CONTAINER_NAME`/`CONTAINER_ID` when Docker's own journald log driver is also in use) to tell containers apart, or simply add a container field yourself:

```bash
-e RCL_LOGGING_JOURNAL_EXTRA_FIELDS="CONTAINER=talker"
journalctl CONTAINER=talker
```

## 3. Permissions

- The socket is world writable (`srw-rw-rw-`), so any uid inside the container can send. No `--privileged`, no extra capabilities, no group mapping needed for **writing**.
- **Reading** the journal from inside the container is a different matter: it requires the journal files (`/var/log/journal`) and membership in `systemd-journal`. Prefer reading on the host; if you must read inside, bind `/var/log/journal:/var/log/journal:ro` and `/etc/machine-id:/etc/machine-id:ro` and run as a uid in the `systemd-journal` group of the host.
- Rootless Podman/Docker: the bind mount works the same; `_UID` shows the host uid the container user maps to.

## 4. When the socket is not there

If the container is started **without** the bind mount, `rcl_logging_journal` fails at initialization by default with a clear message:

```text
systemd-journald native socket '/run/systemd/journal/socket' is not available (No such file or directory).
Is systemd-journald running? In a container, bind mount the host socket with
'-v /run/systemd/journal/socket:/run/systemd/journal/socket' or set RCL_LOGGING_JOURNAL_STRICT=0 to continue without journald.
```

For images that must run both with and without a host journal (CI, developer laptops without systemd), set `RCL_LOGGING_JOURNAL_STRICT=0`: initialization succeeds, the backend becomes a no-op and the stdout / rosout outputs of rcl keep working.

## 5. Alternative: a journald inside the container

For hermetic tests (this is what the repository's CI does) you can run journald as a plain process inside the container:

```bash
apt-get install -y systemd
[ -s /etc/machine-id ] || systemd-machine-id-setup
mkdir -p /run/systemd/journal
/usr/lib/systemd/systemd-journald &
```

Logs then live in the container's `/run/log/journal` (volatile) and `journalctl` works inside the container. This is convenient for CI but not recommended for robots: the host journal is the one with persistent storage, rotation policy and fleet tooling.

## 6. Container logs from stdout too?

Docker and Podman also offer a `journald` **log driver** that captures the container's stdout/stderr. That path goes through the container runtime, is line oriented, and has no ROS fields. `rcl_logging_journal` complements it: keep `--log-driver journald` for anything printed, and use the native socket for structured ROS records. Disable rcl's stdout output (`--ros-args --disable-stdout-logs`) if you do not want both.

## Reference

- https://systemd.io/JOURNAL_NATIVE_PROTOCOL/
- https://docs.docker.com/engine/logging/drivers/journald/
