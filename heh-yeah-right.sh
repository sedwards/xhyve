#./build/xhyve -A -c 1 -m 256M -s 0:0,hostbridge -s 2:0,virtio-net -s 4:0,virtio-blk,/Users/sedwards//VirtualMachines/Slackware-ARM/disk.img -s 31,lpc -f kexec,test/vmlinuz,test/initrd.gz,earlyprintk=serial console=ttyS0
./build/xhyve  -c 1 -m 256M
