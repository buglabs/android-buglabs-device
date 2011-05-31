#!/bin/bash

if [ -z $ANDROID_BUILD_TOP ]; then
    echo "Error: ANDROID_BUILD_TOP not set, please source build/envsetup.sh"
    exit 1;
fi

MYDIR=$ANDROID_BUILD_TOP/device/buglabs/mbm
DRY=""

################################################################
# This is the easies way to get the correct path for each patch.
# Every function patch_xxxx SHALL have the correct project path!
################################################################

patch_0001() {
    
	cd $ANDROID_BUILD_TOP/kernel
	patch -p1 $DRY < $MYDIR/patches/0001-patch-for-adding-cdc_ncm-and-makefile-changes.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
	return $ret
}

patch_0002() {
	cd $ANDROID_BUILD_TOP/kernel
	patch -p1 $DRY < $MYDIR/patches/0002-patch-for-cdc_ether-autosuspend-mbm_guid-and-rmnet.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
        return $ret
}

patch_0003() {
	cd $ANDROID_BUILD_TOP/kernel
	patch -p1 $DRY < $MYDIR/patches/0003-patch-for-autosuspend-acm-fixes.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
	return $ret
}

patch_0004() {
	cd $ANDROID_BUILD_TOP/kernel
	patch -p1 $DRY < $MYDIR/patches/0004-patch-for-backport-of-ehci-tegra-from-tegra-10_11_4.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
	return $ret
}

patch_0005() {
	cd $ANDROID_BUILD_TOP/system/core
	patch -p1 $DRY < $MYDIR/patches/0005-patch-for-start-of-mbm-ril-on-boot.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
        return $ret
}

patch_0006() {
	cd $ANDROID_BUILD_TOP/vendor/nvidia
	patch -p1 $DRY < $MYDIR/patches/0006-patch-for-adding-mbm-to-nvidia-initrc.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
        return $ret
}

patch_0007() {
	cd $ANDROID_BUILD_TOP/vendor/nvidia
	patch -p1 $DRY < $MYDIR/patches/0007-patch-removing-bcm-gps.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
        return $ret
}

patch_0008() {
	cd $ANDROID_BUILD_TOP/frameworks/base
	patch -p1 $DRY < $MYDIR/patches/0008-patch-network-stats-mbm.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
        return $ret
}

patch_0009() {
	cd $ANDROID_BUILD_TOP/vendor/mbm/mbm-ril
	patch -p1 $DRY < $MYDIR/patches/0009-patch-for-default-route.patch
	ret=$?
	if [ $ret -ne 0 ]; then exit $ret; fi
        return $ret
}

#####################################
#####################################
help() {
    echo "Usage: "$0" [--dry-run] [patch-number]"
    echo "Ex: % "$0" 0001 ; will only apply patch serie 0001"
    echo "    % "$0" --dry-run ; only test if all the patches applies"
    echo "    % "$0" --dry-run 0001 ; only test if patch 0001 applies"
}

if [ $# -gt 2 ]; then
    help
    exit 1
fi

if [ $# == 1 ] && [ $1 == "--dry-run" ]; then
    DRY="-N "$1
fi
if [ $# == 2 ] && [ $1 == "--dry-run" ]; then
    DRY="-N "$1
fi

if [ $# == 1 ] && [ -n $1 ] && [  -z "$DRY" ]; then
    patch_$1
    exit $?
fi

if [ $# == 2 ] && [ -n $2 ] && [ "$DRY" == "-N --dry-run" ]; then
    patch_$2
    exit $?
fi

if [ $# -le 1 ]; then
    patch_0001
    patch_0002
    patch_0003
#    patch_0004 ; for BSP 10.11.1
    patch_0005
#    patch_0006
#    patch_0007
#    patch_0008
    patch_0009
fi
