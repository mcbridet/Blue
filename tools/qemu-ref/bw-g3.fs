\ Make QEMU's g3beige device tree look enough like a B&W G3 (PowerMac1,1)
\ for the Trampoline in 1.x NewWorld "Mac OS ROM" files. Lines starting with a
\ backslash are dropped by run.sh; everything else is joined into boot-command.
dev /
" PowerMac1,1" encode-string
" compatible" property
new-device
" rtas" device-name
finish-device
device-end
dev /pci@80000000/mac-io@10
new-device
" interrupt-controller" device-name
" interrupt-controller" device-type
" heathrow" encode-string
" compatible" property
10 encode-int 20 encode-int encode+
" reg" property
0 0 " interrupt-controller" property
1 encode-int " #interrupt-cells" property
finish-device
device-end
