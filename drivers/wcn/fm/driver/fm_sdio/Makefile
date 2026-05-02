# drivers/wcn/fm/driver/fm_sdio/Makefile
ccflags-y += -DDEBUG

obj-m := sprd_fm.o
sprd_fm-y := $(patsubst $(src)/%.c,%.o,$(wildcard $(src)/*.c))
