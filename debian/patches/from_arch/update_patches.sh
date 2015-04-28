#!/bin/bash

set -x

git clone https://github.com/davispuh/open-vm-tools-dkms.git _arch
rm *.patch
for i in _arch/[0-9]*.patch; do
    target="${i#_arch/}"
    sed 's,\( [ab]/\)open-vm-tools/\([^ ]*\),\1\2,g' $i > ${target}
done

rm -rf _arch
