#!/bin/bash

set +x

./install.sh
if [ "$?" != "0" ]; then
	exit $?
fi

# make both the uncompressed and compressed kernels
../scripts/make_cbf.py
cp kernel.cbf /home/lfu

../scripts/make_compressed_cbf.py
cp kernel_comp.cbf /home/lfu

# construct a tar of the loadable modules
pushd $ROOTFS_PATH
tar -cvf modules.tar ./lib/modules
cp modules.tar /home/lfu
popd
