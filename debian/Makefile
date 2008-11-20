all:	build

build:
	$(MAKE) -C modules/linux/pvscsi
	$(MAKE) -C modules/linux/vmblock
	$(MAKE) -C modules/linux/vmci
	$(MAKE) -C modules/linux/vmhgfs
	$(MAKE) -C modules/linux/vmmemctl
	$(MAKE) -C modules/linux/vmsync
	$(MAKE) -C modules/linux/vmxnet
	$(MAKE) -C modules/linux/vmxnet3
	$(MAKE) -C modules/linux/vsock

clean:
	$(MAKE) -C modules/linux/pvscsi clean
	$(MAKE) -C modules/linux/vmblock clean
	$(MAKE) -C modules/linux/vmci clean
	$(MAKE) -C modules/linux/vmhgfs clean
	$(MAKE) -C modules/linux/vmmemctl clean
	$(MAKE) -C modules/linux/vmsync clean
	$(MAKE) -C modules/linux/vmxnet clean
	$(MAKE) -C modules/linux/vmxnet3 clean
	$(MAKE) -C modules/linux/vsock clean

distclean:	clean
