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
ccflags-y += -Wno-packed-bitfield-compat

obj-m := sprdwl_ng.o
sprdwl_ng-y := api_version.o cfg80211.o cmdevt.o core_sc2355.o debug.o defrag.o \
               ibss.o if_sc2355.o main.o mm.o msg.o nan.o npi.o qos.o reorder.o \
               rf_marlin3.o rnd_mac_addr.o rtt.o rx_msg_sc2355.o softap_hook.o \
               tcp_ack.o tracer.o txrx.o tx_msg_sc2355.o vendor.o work.o
