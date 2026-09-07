#!/bin/bash

#####################################################################
# rcl_logging_journal: rcl logging implementation via systemd-journald.
#
# This script builds and tests it with full source with dev docker images.
# see more details for https://github.com/fujitatomoya/ros2_devenv_builder
#
# Since rcl_logging_journal is built against rcl_logging_interface, the
# mainline could break the build if that changes the interface. Besides,
# for rolling it requires to build rcl from the source because rolling can
# change the rcl API and ABI.
#
# To avoid updating and modifying the files under `.github/workflows`,
# this scripts should be adjusted workflow process accordingly.
# And `.github/workflows` just calls this script in the workflow pipeline.
# This allows us to maintain the workflow process easier for contributors.
#
# Containers have no running systemd-journald, so this script starts one in
# the foreground (background process) exactly like rcl_logging_syslog CI
# starts `rsyslogd -n -iNONE`.
#
#####################################################################

########################
# Function Definitions #
########################

function mark {
    export $1=`pwd`;
}

function exit_trap() {
    # shellcheck disable=SC2317  # Don't warn about unreachable commands in this function
    if [ $? != 0 ]; then
        echo "Command [$BASH_COMMAND] is failed"
        exit 1
    fi
}

function install_prerequisites () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: update and install dependent packages."
    apt update && apt upgrade -y
    # git is needed by vcs, libsystemd-dev provides sd-journal.h and the
    # pkg-config module, systemd provides systemd-journald and journalctl.
    apt install -y git libsystemd-dev systemd
    cd $there
}

function setup_colcon_env () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: set up colcon build environment."
    # create colcon temporary workspace
    mkdir -p ${COLCON_WORKSPACE}/src
    cd ${COLCON_WORKSPACE}
    # fetch all source mainline source code to build with rcl_logging_journal.
    vcs import --input https://raw.githubusercontent.com/ros2/ros2/${ROS_DISTRO}/ros2.repos src
    # move rcl_logging_journal directory to colcon workspace
    cp -rf $there ${COLCON_WORKSPACE}/src
}

function setup_journald () {
    # this is required basically only for container environment.
    # for security reason, container does not have system service or daemon processes.
    # just for the test with journald, it starts systemd-journald as a plain process.
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: setup and start systemd-journald."
    # journald refuses to start without a machine id.
    if [ ! -s /etc/machine-id ]; then
        systemd-machine-id-setup
    fi
    # runtime directory for the native protocol sockets.
    mkdir -p /run/systemd/journal
    # start journald in the background; it binds /run/systemd/journal/socket,
    # /dev/log and /run/systemd/journal/stdout itself when not socket activated.
    /usr/lib/systemd/systemd-journald &
    # give it a moment to create the sockets.
    for i in $(seq 1 50); do
        if [ -S /run/systemd/journal/socket ]; then
            break
        fi
        sleep 0.1
    done
    test -S /run/systemd/journal/socket
    journalctl --version
}

function build_colcon_package () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: build rcl and rcl_logging_journal packages."
    cd ${COLCON_WORKSPACE}
    # Lyrical and later load the backend dynamically via rcl_logging_implementation,
    # older distributions statically link it into rcl (see README.md).
    colcon build --symlink-install --cmake-clean-cache --packages-up-to rcl_logging_journal
}

function test_colcon_package () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: test rcl_logging_journal packages."
    # source local workspace projects
    cd ${COLCON_WORKSPACE}
    source ./install/local_setup.bash
    # initiate the colcon test
    colcon test --event-handlers console_direct+ --packages-select rcl_logging_journal
    colcon test-result --verbose
}

########
# Main #
########

export DEBIAN_FRONTEND=noninteractive
export COLCON_WORKSPACE=/tmp/colcon_ws

# mark the working space root directory, so that we can come back anytime with `cd $there`
mark there

# set the trap on error
trap exit_trap ERR

# call install functions in sequence
install_prerequisites
setup_colcon_env
setup_journald
build_colcon_package
test_colcon_package

exit 0
