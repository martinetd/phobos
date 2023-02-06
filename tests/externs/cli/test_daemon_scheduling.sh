#!/bin/bash

#
#  All rights reserved (c) 2014-2022 CEA/DAM.
#
#  This file is part of Phobos.
#
#  Phobos is free software: you can redistribute it and/or modify it under
#  the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation, either version 2.1 of the License, or
#  (at your option) any later version.
#
#  Phobos is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
#

test_dir=$(dirname $(readlink -e $0))
. $test_dir/../../test_env.sh
. $test_dir/../../setup_db.sh
. $test_dir/../../test_launch_daemon.sh
. $test_dir/../../utils.sh
. $test_dir/../../utils_generation.sh

set -xe

export PHOBOS_LRS_lock_file="$test_dir/phobosd.lock"
EVENT_FILE="$test_dir/test_daemon_scheduling_events"

function invoke_daemon_debug()
{
    # XXX grep --line-buffered allows each line to be written in EVENT_FILE as
    # they are read in the logs. Otherwise, the logs would not be always
    # flushed.
    stdbuf -eL -oL $phobosd -vv -i 2>&1 | stdbuf -i0 -oL tr -d '\000' \
        | grep --line-buffered "sync: " > "$EVENT_FILE"
}

function setup()
{
    setup_test_dirs
    rm -f "$PHOBOS_LRS_lock_file"
    setup_dummy_files 1

    setup_tables
    invoke_daemon_debug &
}

function cleanup()
{
    cleanup_dummy_files
    cleanup_test_dirs
    rm -f "$EVENT_FILE"

    kill $(pgrep phobosd)
    drop_tables
}

function now()
{
    date +%s%N
}

function get_timestamp()
{
    echo "$1" | awk '{print $1, $2}' | xargs -I{} date -d "{}" +%s%N
}

function newer()
{
    local t=$1

    while read line; do
        if [ $t -le $(get_timestamp "$line") ]; then
            echo "$line"
        fi
    done
}

function get_daemon_event()
{
    # get new events
    local event="$1: "

    grep "$event" "$EVENT_FILE" | newer $last_read
}

function wait_for_daemon()
{
    local Nretry=10
    local N=0

    while ! $phobos ping; do
        local N=$((N+1))

        if [ $N -ge $Nretry ]; then
            exit 1
        fi

        sleep 0.1
    done
}

function test_sync_after_put_get()
{
    local prefix=$(generate_prefix_id)

    local oid=${prefix}_id
    local dir="$DIR_TEST_OUT"
    local file="${FILES[0]}"

    # XXX last_read has to be updated outside of get_daemon_event due to the
    # way sub shells and functions work
    last_read=$(now)
    $phobos dir add "$dir"
    $phobos dir format --fs posix --unlock "$dir"

    $phobos put --family dir "$file" $oid

    local res=$(get_daemon_event "sync")
    last_read=$(now)

    echo "$res" | grep "medium=$dir" || error "$dir should have been flushed"
    echo "$res" | grep "rc=0" || error "Synchronisation of $file failed"

    # FIXME For some reason, last_read can be smaller than the timestamps in
    # EVENT_FILE. This sleep seems to prevent this issue. Using the timestamps
    # may not be so much robust.
    sleep 0.1
    $phobos get $oid "$TEST_DIR_OUT/$(basename $file)"

    local res=$(get_daemon_event "sync")
    last_read=$(now)

    if [ ! -z "$res" ]; then
        error "No synchronisation should occur after a get operation"
    fi
}

function check_fair_share_min_max()
{
    local values
    local min
    local max

    values=$($valg_phobos sched fair_share --type LTO5)
    min=$(echo "$values" | grep min)
    max=$(echo "$values" | grep max)

    echo "$min" | grep "$1" ||
        error "Invalid min in '$min'. Expected '$1'"
    echo "$max" | grep "$2" ||
        error "Invalid max in '$max'. Expected '$2'"
}

function test_conf_set()
{
    $valg_phobos sched fair_share --type LTO5 --min "0,1,2" --max "1,2,3"
    check_fair_share_min_max "0,1,2" "1,2,3"

    $valg_phobos sched fair_share --type LTO5 --min "1,2,3" --max "0,1,2"
    check_fair_share_min_max "1,2,3" "0,1,2"
}

function test_fair_share_conf_at_startup()
{
    export PHOBOS_IO_SCHED_TAPE_fair_share_lto5_min="12,12,12"
    export PHOBOS_IO_SCHED_TAPE_fair_share_lto5_max="13,13,13"

    trap "waive_daemon; drop_tables" EXIT
    setup_tables
    invoke_daemon

    check_fair_share_min_max "12,12,12" "13,13,13"
    unset PHOBOS_IO_SCHED_TAPE_fair_share_lto5_min
    unset PHOBOS_IO_SCHED_TAPE_fair_share_lto5_max

    trap drop_tables EXIT
    waive_daemon
    trap "" EXIT
    drop_tables
}

# This test must be executed before the setup because it starts and stops the
# daemon.
test_fair_share_conf_at_startup

last_read=$(now)
trap cleanup EXIT
setup
wait_for_daemon

test_sync_after_put_get
test_conf_set
