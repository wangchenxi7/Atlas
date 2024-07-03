# Hermit Kernel

## Prerequisites
Hardware:
* Mellanox ConnectX-3/4 (Infiniband)

I am not sure if our code works with RoCE on Ethernet.

Software:
* OS: Ubuntu 18.04 (glibc version < 2.31)
* gcc 7.5

I remembered I got some linkage errors and booting issues when compiling the kernel with Ubuntu 20.04 and gcc 9 because of the newer `ld/glibc`.

Driver:
We use the RDMA driver in kernel directly so no MLNX OFED driver is needed.
There is a `config-with-rdma-driver in the root directory of the kernel repo which works on our machines. Basically I enabled the following options related to INFINIBAND and MLX4 here:

```txt
CONFIG_INFINIBAND=y
CONFIG_INFINIBAND_USER_MAD=y
CONFIG_INFINIBAND_USER_ACCESS=y
CONFIG_INFINIBAND_USER_MEM=y
CONFIG_INFINIBAND_ON_DEMAND_PAGING=y
CONFIG_INFINIBAND_ADDR_TRANS=y
CONFIG_INFINIBAND_VIRT_DMA=y
CONFIG_INFINIBAND_MTHCA=y
# CONFIG_INFINIBAND_MTHCA_DEBUG is not set
CONFIG_INFINIBAND_QIB=y
CONFIG_INFINIBAND_CXGB4=m
# CONFIG_INFINIBAND_EFA is not set
CONFIG_MLX4_INFINIBAND=y

# ...

CONFIG_NET_VENDOR_MELLANOX=y
CONFIG_MLX4_EN=y
CONFIG_MLX4_EN_DCB=y
CONFIG_MLX4_CORE=y
CONFIG_MLX4_DEBUG=y
CONFIG_MLX4_CORE_GEN2=y
```

## Some hard-coded parameters in kernel
There are many hard-coded macros and paramters that work only for our machine:
* A lot of places in the code assumes hyper-threading (HT) enabled... So for now let's enable that before I fix them.
* [RMGRID_CPU_FREQ](https://github.com/ivanium/linux-5.14-rc5/blob/separate-swapout-codepath/include/linux/adc_macros.h#L9): should be the CPU base frequency in MHz
* [RMGRID_NR_PCORE](https://github.com/ivanium/linux-5.14-rc5/blob/separate-swapout-codepath/include/linux/adc_macros.h#L10): should be the number of physical cores.

There must be a lot of magic numbers I didn't even make them macros. I will keep update the list here.

## Build
We use the `build_kernel.sh` script to build and install kernel.
Here are the instructions:
1. copy the `config-with-rdma-driver` to `.config`. Modify it if needed.
2. `build_kernel.sh build` to build the kernel image and modules.
3. `build_kernel.sh install` to install both the kernel image and all kernel modules. `build_kernel.sh replace` will replace the kernel image but leave kernel modules unchanged.
4. Check the grub file at `/etc/default/grub`, select the default kernel, add boot options, etc.
5. Update grub with `sudo update-grub` on Ubuntu.
6. Reboot the machine and enter with newly compiled kernel.

## Necessary softwares
The kernel RDMA driver doesn't come with the user libraries and utility applications. So we install them manually via `apt` on our kernel:
```bash
sudo apt install -y infiniband-diags opensm libibverbs-dev librdmacm-dev perftest
sudo apt install -y rdma-core ibutils ibverbs-utils rdmacm-utils # ibverbs-provider

sudo systemctl enable opensm
sudo systemctl start opensm
```

After the installation we should be able to use `ibstat` and utils like `ib_[read|write]_[lat|bw]`.

And we can configure IPs for the Infiniband devices now. Either `ifconfig` or `netplan` should work.

## Install remoteswap kernel module
Refer the README in the remoteswap repo for the instructions.
For now only the `5.14-batch-store` branch in the remoteswap repo works with this particular branch.
Note that because of our dirty hack in the kernel, the kernel probably **cannot** swap to disks even when the remoteswap module is not installed.

## Tune Hermit parameters
Hermit parameters can be read an reset via debugfs files under `/sys/kernel/debug/hermit`.
We have a `setup.sh` script under the `client/` directory in the remoteswap repo, which set the default parameters I use now.

```bash
# hermit
echo Y > /sys/kernel/debug/hermit/vaddr_swapout # to reduce the overhead of reverse mapping. We record a PA->VA mapping manually for now
echo Y > /sys/kernel/debug/hermit/batch_swapout # batched paging out dirty pages in the page reclamation
echo Y > /sys/kernel/debug/hermit/swap_thread # enable async page reclamation (swap-out)
echo Y > /sys/kernel/debug/hermit/bypass_swapcache # for pages that are not shared by multiple PTEs, we can skip adding it into the swap cache and map it directly to the faulting PTE
echo Y > /sys/kernel/debug/hermit/speculative_io # speculative RDMA read
echo Y > /sys/kernel/debug/hermit/speculative_lock # speculative mmap_lock. Mostly useful for java applications
echo N > /sys/kernel/debug/hermit/prefetch_thread # disable async prefetching for now

echo 16 > /sys/kernel/debug/hermit/sthd_cnt # number of page reclamation threads. The default cores these threads run on are set in the kernel (https://github.com/ivanium/linux-5.14-rc5/blob/separate-swapout-codepath/mm/hermit.c#L114)
```

## Run applications
We use cgroup-v1 to limit the memory an application can use. Fastswap requires cgroup-v2 which provide the `memory.high` interface for them to trigger the reclamation offloading. In our ported version, we dirty hacked and set the `memory.high` counter along with `memory.limit_in_bytes` with a specified headroom. Anyway, cgroup-v1 should be functional enough in our experiments.

I usually use cgexec and taskset to set the memory cgroup and what CPU cores the application will have:
```bash
mkdir /sys/fs/cgroup/memory/<cgroup name>
echo <limit size, e.g., 2560m> > /sys/fs/cgroup/memory/<cgroup name>/memory.limit_in_bytes
cgexec --sticky -g memory:<cgroup name> taskset -c <cores> /usr/bin/time -v <command>
```

## Hermit extended syscalls
I provide a `syscaller.py` script under `tools/hermit` as a user-level script to manage extended syscalls.
* `python3 tools/hermit/syscaller.py stats` will show the page fault handling latencies, breakdowns, and other statistics about swap in `dmesg`. It prints the prefetching contribution and accuracy as well in the terminal.
* `python3 tools/hermit/syscaller.py reset` will reset the stats and latencies numbers in kernel, which enables recollecting stats for the next run. So I would suggest reset the stats before running an application, and check the stats again after finishing running the application.
* `python3 tools/hermit/syscaller.py sthd_cores` will set the cores where sthds are scheduled on. The script is stupid so we have to edit its `set_sthd_cores` function and hard code the core IDs as an array.