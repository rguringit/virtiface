# virtiface Build and Test Manual

## 1. Project description

`virtiface` is aLinux kernel module that registers a virtual
network interface named `virtiface0`.

Module does:

- creates the `virtiface0` netdevice;
- creates `/proc/virtiface/ipv4`;
- stores one IPv4 address to procfs file;
- handles ping command

### Important implementation details

- A host route such as `ip route add 192.0.2.10/32 dev virtiface0` is required
- Interface counters: `ip -s link show virtiface0`.
- Writing `unset`, `none`, or `0` to `/proc/virtiface/ipv4` resets the
  configured address.

## 1. Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) kmod iproute2 iputils-ping
```

## 3. Build

From the project directory:

```bash
make clean
make
```

Output artifact:

- `virtiface.ko`

Optional verification:

```bash
modinfo ./virtiface.ko
file ./virtiface.ko
```
Expected result:
```bash
modinfo ./virtiface.ko
filename:       /home/rgurin/virtiface/./virtiface.ko
license:        GPL
description:    Virtual IPv4 pingable Linux network interface
author:         rgurin
srcversion:     EBFF9C5DEE3D1F215524766
depends:        
retpoline:      Y
name:           virtiface
vermagic:       6.11.0-29-generic SMP preempt mod_unload modversions 

file ./virtiface.ko
./virtiface.ko: ELF 64-bit LSB relocatable, x86-64, version 1 (SYSV), BuildID[sha1]=3119e67b59788782d887498c2bdee73394603254, with debug_info, not stripped
rgurin@rgurin-mclfxx:~/virtiface$
```
## 4. Ping Test

This is the shortest full-path validation of the driver.

1. Load the module:

```bash
sudo insmod ./virtiface.ko
```
If Secure Boot enabled and you can't load the module do steps from Troubleshooting section.

2. Confirm that the interface and procfs node exist:

```bash
ip link show virtiface0
ls -l /proc/virtiface/ipv4
```

3. Bring the interface up:

```bash
sudo ip link set virtiface0 up
```

4. Configure the virtual IPv4 address:

```bash
echo 192.0.2.10 | sudo tee /proc/virtiface/ipv4
cat /proc/virtiface/ipv4
```

Expected output:

```text
192.0.2.10
```

5. Add a host route to  `virtiface0`:

```bash
sudo ip route add 192.0.2.10/32 dev virtiface0
```

If the route already exists, use:

```bash
sudo ip route replace 192.0.2.10/32 dev virtiface0
```

6. Send ping:

```bash
ping -I virtiface0 192.0.2.10
```

Expected result:
```bash
ping -I virtiface0 192.0.2.10
PING 192.0.2.10 (192.0.2.10) from 192.168.31.150 virtiface0: 56(84) bytes of data.
64 bytes from 192.0.2.10: icmp_seq=1 ttl=64 time=0.072 ms
64 bytes from 192.0.2.10: icmp_seq=2 ttl=64 time=0.029 ms
64 bytes from 192.0.2.10: icmp_seq=3 ttl=64 time=0.047 ms
```

7. Inspect counters and recent kernel messages:

```bash
ip -s link show virtiface0
dmesg | tail -n 20
```
Counters should increase after ping
```bash
ip -s link show virtiface0
5: virtiface0: <POINTOPOINT,NOARP,UP,LOWER_UP> mtu 1500 qdisc noqueue state UNKNOWN mode DEFAULT group default qlen 1000
    link/none 
    RX:  bytes packets errors dropped  missed   mcast           
           252       3      0       0       0       0 
    TX:  bytes packets errors dropped carrier collsns           
           492       8      0       5       0       0 

ping -I virtiface0 192.0.2.10
PING 192.0.2.10 (192.0.2.10) from 192.168.31.150 virtiface0: 56(84) bytes of data.
64 bytes from 192.0.2.10: icmp_seq=1 ttl=64 time=0.072 ms
64 bytes from 192.0.2.10: icmp_seq=2 ttl=64 time=0.029 ms
64 bytes from 192.0.2.10: icmp_seq=3 ttl=64 time=0.047 ms
^C
--- 192.0.2.10 ping statistics ---
3 packets transmitted, 3 received, 0% packet loss, time 2047ms
rtt min/avg/max/mdev = 0.029/0.049/0.072/0.017 ms

ip -s link show virtiface0
5: virtiface0: <POINTOPOINT,NOARP,UP,LOWER_UP> mtu 1500 qdisc noqueue state UNKNOWN mode DEFAULT group default qlen 1000
    link/none 
    RX:  bytes packets errors dropped  missed   mcast           
           504       6      0       0       0       0 
    TX:  bytes packets errors dropped carrier collsns           
           744      11      0       5       0       0 
```
## 5. Functional Checks

### Read current IPv4 state

```bash
cat /proc/virtiface/ipv4
```

Expected output:

- configured IP, for example `192.0.2.10`
- or `unset`

### Reset the configured address

```bash
echo unset | sudo tee /proc/virtiface/ipv4
cat /proc/virtiface/ipv4
```

Expected output:

```text
unset
```

After reset, ping to the previous address should stop working.

### Invalid input test

```bash
echo not-an-ip | sudo tee /proc/virtiface/ipv4
```

Expected result:

- the write fails with `Invalid argument`;
- the previously configured state remains unchanged.

## 6. Cleanup

Remove the route first, then unload the module:

```bash
sudo ip route del 192.0.2.10/32 dev virtiface0
sudo rmmod virtiface
```

Optional confirmation:

```bash
ip link show virtiface0
ls /proc/virtiface
```

Both should fail once the module is unloaded.

## 7. Troubleshooting

### `make` fails because of missing kernel headers

Install headers for the running kernel:

```bash
sudo apt install -y linux-headers-$(uname -r)
```

### `insmod: ERROR: could not insert module ... Key was rejected by service`

This means Secure Boot is enabled and the kernel refuses to load an
unsigned module.

Check Secure Boot state:

```bash
mokutil --sb-state
```

If it reports `SecureBoot enabled`, use one of these options:

Option 1: disable Secure Boot in UEFI firmware settings, then reboot.

Option 2: sign the module and enroll your Machine Owner Key (recommended if you
want to keep Secure Boot enabled). (Used this option)

Signing flow on Ubuntu:

```bash
openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der -nodes -days 36500 -subj "/CN=virtiface module signing/"
sudo mokutil --import MOK.der
```

Reboot, complete MOK enrollment and then sign the
module:

```bash
/usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 ./MOK.priv ./MOK.der ./virtiface.ko
```

After that, try loading it again:

```bash
sudo insmod ./virtiface.ko
```
 Module should be succesfully installed
