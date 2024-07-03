# Atlas

Atlas is a hybrid data plane that simultaneously enables accesses to far memory via both kernel path and runtime path to provide high efficiency for real-world applications.

## Prerequisites

You will need two servers with RDMA connection to use Atlas. One will serve as the host server and another will serve as the memory server. Atlas is developed and tested under the following settings:

Hardware:

+ Infiniband: Mellanox ConnectX-5 (100Gbps)
+ RoCE: Mellanox ConnectX-5 100GbE

Software:

+ OS: Ubuntu 18.04/20.04
+ GCC: 7.5.0/9.4.0
+ Mellanox OFED driver: 5.5-1.0.3.2

## Building and running

### Building Atlas kernel

```bash
cd linux-5.14-rc5
cp config .config
sudo apt install -y build-essential bc python2 bison flex libelf-dev libssl-dev libncurses-dev libncurses5-dev libncursesw5-dev
./build_kernel.sh build
./build_kernel.sh install
./build_kernel.sh headers-install
## edit GRUB_DEFAULT="Advanced options for Ubuntu>Ubuntu, with Linux 5.14.0-rc5+", or what ever the new kernel version code it
## GRUB_CMDLINE_LINUX="nokaslr transparent_hugepage=never processor.max_cstate=0 intel_idle.max_cstate=0 tsx=on tsx_async_abort=off mitigations=off quiet splash noibrs noibpb nospec_store_bypass_disable no_stf_barrier"
sudo vim /etc/default/grub
sudo update-grub
# ensure the right version of kernel is booted and the correct boot options are used. The command below should have output
sudo reboot

# Install the correct version of MLNX_OFED driver based on your OS version
wget https://content.mellanox.com/ofed/MLNX_OFED-5.5-1.0.3.2/MLNX_OFED_LINUX-5.5-1.0.3.2-ubuntu18.04-x86_64.tgz
wget https://content.mellanox.com/ofed/MLNX_OFED-5.5-1.0.3.2/MLNX_OFED_LINUX-5.5-1.0.3.2-ubuntu20.04-x86_64.tgz
wget https://content.mellanox.com/ofed/MLNX_OFED-5.6-1.0.3.3/MLNX_OFED_LINUX-5.6-1.0.3.3-ubuntu22.04-x86_64.tgz
# Use Ubuntu 18.04 as an example below
tar xzf MLNX_OFED_LINUX-5.5-1.0.3.2-ubuntu18.04-x86_64.tgz
cd MLNX_OFED_LINUX-5.5-1.0.3.2-ubuntu18.04-x86_64
sudo apt install -y bzip2
sudo ./mlnxofedinstall --add-kernel-support

sudo /etc/init.d/openibd restart

sudo update-rc.d opensmd remove -f
sudo sed "s/# Default-Start: null/# Default-Start: 2 3 4 5/g" /etc/init.d/opensmd -i
sudo systemctl enable opensmd
# before reboot, may need to manually enable the service
sudo service opensmd start
# check the status of the service, should be active
service opensmd status

# get device name, mlx5_2 here 
ibstat
# LINK_TYPE_P1=1 - IB LINK_TYPE_P1=2 - Ethernet
sudo mstconfig -d mlx5_2 set LINK_TYPE_P1=2
sudo mstfwreset -d mlx5_2 -l3 -y reset
ibstat mlx5_2
# Maybe need to manually bring up the IB interface
sudo ifconfig ibs5f0 up
# The IP of the InfiniBand interface may need to be manually set up
sudo vim /etc/netplan/*.yaml
sudo netplan apply
# or with network manager
sudo nmtui
```

### Building Atlas runtime

```bash
cd atlas-runtime
cd third_party
git clone --depth 1 -b 54eaed1d8b56b1aa528be3bdd1877e59c56fa90c https://github.com/jemalloc/jemalloc.git
# on memory server
cd bks_module/remoteswap/server
make
# on CPU server
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt install g++-9
sudo apt install cgroup-tools
cd bks_module/remoteswap/client
make -j
# compile kernel module
cd ../../bks_drv
make -j
# compile main program
cd ../..
mkdir build
cd build
# Use gcc-9
sudo update-alternatives --config gcc
cmake ..
make -j

cd atlas-runtime/bks_module/remoteswap/server
#./rswap-server <memory server ip> <memory server port> <memory pool size in GB> <number of cores on CPU server>
./rswap-server 172.172.76.1 9999 48 96

cd atlas-runtime/bks_module/remoteswap/client
## edit accordingly
bash manage_rswap_client.sh install

cd ../../../scripts
device_name=bks_dev
drv_name=bks_drv
bks_path=../bks_module
sudo /sbin/rmmod ${drv_name} 2>/dev/null
sudo /sbin/insmod "${bks_path}/bks_drv/$drv_name.ko"
major=$(cat /proc/devices | grep "$device_name")
major_number=($major)
sudo rm -f /dev/${device_name} 2>/dev/null
sudo mknod /dev/${device_name} c ${major_number[0]} 0
major=$(cat /proc/devices | grep "$device_name")
sudo chmod 777 /dev/${device_name}
sudo bash -c "echo 0 > /proc/sys/kernel/randomize_va_space"
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
```
### Test Atlas runtime
```bash
cd atlas-runtime/build/tests/runtime/unique_ptr
bash test.sh ./unique_ptr_test
```

## Paper
+ [A Tale of Two Paths: Toward a Hybrid Data Plane for Efficient Far-Memory Applications](https://www.usenix.org/conference/osdi24/presentation/chen-lei)

  Lei Chen, Shi Liu (co-first), Chenxi Wang, Haoran Ma, Yifan Qiao, Zhe Wang, Chenggang Wu, Youyou Lu, Xiaobing Feng, Huimin Cui, Shan Lu, Harry Xu

  The 18th USENIX Symposium on Operating Systems Design and Implementation (OSDI'24)
