# USM

Userspace memory management project.

# Environment #

To keep things simple, we'll all try to use the same environment. We tested many ones lately, and, while it does generally be doable under any situation, the hassle of even just the dependencies hunting, greatly depending on your hardware, is enough a reason not to try out that road ; scout's word.

We'll hence be using Ubuntu 22.04, in a VM (Qemu/KVM), coupled with the maximum of CPU cores available, and preferably 8 Gigs of RAM (at least 6), 2 of which'll be reserved for some contiguous memory we'll be managing (using DMA_CMA). We recommend, too, using as CPU architecture the one closer to your host's model (probably Broadwell-noTSX-IBRS).

The latter's checkable through :

```sh
virsh -c qemu:///system
capabilities
```

The first listed should be it.

# Dependencies #

## Runtime dependencies ##

 * libsyscall_intercept -- the syscall hooker we use (mainly for Metrics (on the way, code sanitizing step), basically disposable)
 * userfaultfd -- the page faults passing to userspace mechanism, quite important for synchronization matters, otherwise disposable
 * libcapstone -- used by syscall_intercept, disassembly engine

## Build dependencies ##

The build dependencies are mainly classical ones (i.e. kernel building). Only one precaution needs to be taken with dwarves, which has to be inferior to the 1.24 version, hence why it is included for convenience. It should stay on 1.22 on Ubuntu 22.04 for quite a while, but we never know.

 * build-essential
 * bison
 * flex
 * libncurses-dev
 * libssl-dev
 * libelf-dev
 * dwarves
 * cmake
 * pkg-config
 
A baseline's gettable with :
```sh
sudo apt install build-essential bison flex libncurses-dev libssl-dev libelf-dev dwarves cmake pkg-config libcapstone-dev clang
```
 
### How to build ###

We need to first check dwarves' installed version :
```sh
pahole --version
```
If it is any higher than 1.22, we'd need to uninstall it, then go into the dwarves-1.22 dependency folder and do :
```sh
mkdir build
cd build
cmake -D__LIB=lib -DBUILD_SHARED_LIBS=OFF ..
sudo make install
```
Then we simply compile and install the kernel and its headers. We start with, in the Kernel folder :
```sh
cp /boot/config-$(uname -r) .config
```
Our current version uses **CMA**. We hence need activating it in the just copied config (*CONFIG_CMA* and anything *DMA_CMA* related (may be non-existant, on which case simply add the line `CONFIG_DMA_CMA=y`), while commenting any relevant\* line containing _TRANSPARENT_, _THP_, _HUGE_ and _SWAP/ZSWAP_ (Ctrl+W on nano) :
```sh
nano .config
```
We'd then need to do this next command, activating only the modules in current use, while answering no to the maximum prompts possible (basically all except CMA related ones) :
```sh
make localmodconfig
```
You must have noticed the CMA size prompt. That is configured by adding **cma=2G** to the DEFAULT linux args, right after *quiet splash*, while adding *transparent_hugepage=never* just to be doubly sure THP's out of our scope for the moment :
```sh
sudo nano /etc/default/grub
```
Disabling key signing might as well be a good idea, to further mitigate possible related issues. It is still done by modifying _.config_, using provided scripts :
```sh
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
scripts/config --set-str CONFIG_SYSTEM_TRUSTED_KEYS ""
scripts/config --set-str CONFIG_SYSTEM_REVOCATION_KEYS ""
```
We then do the main part with :
```sh
sudo make -j `getconf _NPROCESSORS_ONLN`
sudo make modules_install
sudo make install
sudo make headers_install INSTALL_HDR_PATH=/usr
```
`make install` already updates grub, but we can do so again, especially if you modified the grub after the latter, with :
```sh
sudo update-grub
```
Speaking of which, you might want to comment out its line containing *hidden*, put a number on its *TIMEOUT* value, and add `GRUB_DEFAULT=saved` plus `GRUB_SAVEDEFAULT=true`, which could save you some time. We too recommend copying/saving whenever possible your disk image, which'd help a lot whenever your environment gets corrupted. It is located, by default, at */var/lib/libvirt/images/name_of_your_disk_image*.
Installing syscall_intercept is just as easy as dwarves' case, and is done as follows, after going into the relevant folder :
```sh
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
make
sudo make install
```
As with THP's temporary case, we recommend further disabling swapping, by either issuing a _swapoff_ command, adding `vm.swappiness = 0` to */etc/sysctl.conf*, or commenting out the line starting with `/swapfile` in */etc/fstab* ; you should do them all (!) :
```sh
sudo swapoff -a
sudo nano /etc/sysctl.conf
sudo nano /etc/fstab
```
After now rebooting, the allocator and tagger can then be generated with :
```sh
make
```
The latters need the module loaded to properly function :
```sh
make
sudo insmod usm_lkmRM.ko
```
At this point, you might be interested in blocking any annoying update kernel related :
```sh
sudo apt-mark hold linux-image-generic
sudo apt-mark hold linux-headers-generic
```
# Synopsis #

```sh
sudo ./usmAllocator

LD_PRELOAD=./usmTagger ./application_wanted_managed_by_usm
```

##### Description: #####

In the works (interfaces&co.).
