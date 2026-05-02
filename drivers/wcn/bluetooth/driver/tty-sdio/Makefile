# drivers/wcn/bluetooth/driver/tty-sdio/Makefile
ccflags-y += -DDEBUG

obj-m := sprdbt_tty.o
sprdbt_tty-y := $(wildcard $(src)/*.c)
sprdbt_tty-y += $(wildcard $(src)/alignment/*.c)