# drivers/wcn/wlan/sc2355/Makefile
ccflags-y += -DSC2355_FTR -DSC2355_TX_FILTER -DSC2355_QOS_MAP
ccflags-y += -DIBSS_SUPPORT -DIBSS_RSN_SUPPORT
ccflags-y += -DNAN_SUPPORT
ccflags-y += -DRTT_SUPPORT
ccflags-y += -DSC2355_RX_NO_LOOP
ccflags-y += -DACS_SUPPORT -DSC2355_HW_CSUM
ccflags-y += -DWMMAC_WFA_CERTIFICATION
ccflags-y += -DCOMPAT_SAMPILE_CODE
ccflags-y += -DRND_MAC_SUPPORT
ccflags-y += -DSOFTAP_HOOK
ccflags-y += -DATCMD_ASSERT
ccflags-y += -DTCPACK_DELAY_SUPPORT
ccflags-$(CONFIG_SPRD_WCN_DEBUG) += -DCONFIG_SPRD_WCN_DEBUG
ccflags-y += -DSPLIT_STACK

obj-m := sprdwl_ng.o
sprdwl_ng-y := $(shell find $(src) -name "*.c" | sed 's|$(src)/||' | sed 's/\.c/\.o/')