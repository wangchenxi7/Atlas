#!/bin/bash

function kill_mcd {
    pkill -9 memcached > /dev/null 2>&1 
    sleep 5s
    memcached_pid=`pidof memcached`
    if [ ! -z $memcached_pid ]; then
        echo "memcached is still running, pid $memcached_pid"
        exit 1
    fi
}

kill_mcd

### set up memcached
echo "Setting parameters"

OS_DISTRO=$( awk -F= '/^NAME/{print $2}' /etc/os-release | sed -e 's/^"//' -e 's/"$//' )

if [[ $OS_DISTRO == "CentOS Linux" ]]
then
    sudo chmod 777 -R /sys/fs/cgroup/cpuset
    sudo chmod 777 -R /sys/fs/cgroup/memory
    sudo chmod 777 -R /sys/kernel/mm/swap/vma_ra_enabled
elif [[ $OS_DISTRO == "Ubuntu" ]]
then
    sudo chmod 777 -R /sys/fs/cgroup/cpuset
    sudo chmod 777 -R /sys/fs/cgroup/memory
    sudo chmod 777 -R /sys/kernel/mm/swap/vma_ra_enabled
    sudo chmod 777 -R /sys/kernel/mm/swap/readahead_win
    sudo chmod 777 -R /sys/kernel/debug/
fi

echo Y > /sys/kernel/debug/hermit/vaddr_swapout
echo Y > /sys/kernel/debug/hermit/batch_swapout
echo Y > /sys/kernel/debug/hermit/batch_io
echo Y > /sys/kernel/debug/hermit/batch_tlb
echo Y > /sys/kernel/debug/hermit/batch_account
echo Y > /sys/kernel/debug/hermit/bypass_swapcache
echo Y > /sys/kernel/debug/hermit/lazy_poll
echo Y > /sys/kernel/debug/hermit/speculative_io
echo N > /sys/kernel/debug/hermit/speculative_lock

echo N > /sys/kernel/debug/hermit/async_prefetch
echo N > /sys/kernel/debug/hermit/prefetch_direct_poll
echo N > /sys/kernel/debug/hermit/prefetch_direct_map
echo N > /sys/kernel/debug/hermit/prefetch_populate
echo N > /sys/kernel/debug/hermit/prefetch_always_ascend
echo N > /sys/kernel/debug/hermit/atl_card_prof
echo N > /sys/kernel/debug/hermit/atl_card_prof_print

echo 0 > /sys/kernel/mm/swap/readahead_win
echo 1 > /sys/kernel/debug/hermit/sthd_cnt
echo 1 > /sys/kernel/debug/hermit/populate_work_cnt
echo 1 > /sys/kernel/debug/hermit/min_sthd_cnt
echo 26 > /sys/kernel/debug/hermit/atl_card_prof_thres
echo 0 > /sys/kernel/debug/hermit/atl_card_prof_low_thres


### prepare to run memcached
exec_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/server

# create cgroup
mkdir /sys/fs/cgroup/memory/memcached

function run_server {
    local_size=$1
    local_ratio=$2

    pkill -9 memcached > /dev/null 2>&1 
    sleep 5s
    memcached_pid=`pidof memcached`
    if [ ! -z $memcached_pid ]; then
        echo "memcached is still running, pid $memcached_pid"
        exit 1
    fi

    echo "Running memcached, local memory size $local_size, local memory ratio $local_ratio"
    echo ${local_size} | tee /sys/fs/cgroup/memory/memcached/memory.limit_in_bytes
    echo "mcd to run is ${exec_dir}/memcached"
    sleep 3
    cgexec -g memory:memcached --sticky taskset -c 1-12 ${exec_dir}/memcached -m 102400 -p 11211 -t 12 -o hashpower=29,no_hashexpand,no_lru_crawler,no_lru_maintainer -c 32768 -b 32768 &
    sleep 5
}

run_server 100G 100
