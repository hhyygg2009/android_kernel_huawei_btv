# Makefile for the irda drivers.
#

# Each configuration option enables a list of files.

obj-$(CONFIG_HUAWEI_TS)  += huawei_touchscreen.o
obj-$(CONFIG_HUAWEI_TS) += panel/
obj-$(CONFIG_HUAWEI_TS)  += huawei_touchscreen_algo.o
obj-$(CONFIG_HUAWEI_TS)  += huawei_touchscreen_csvparse.o

obj-$(CONFIG_HUAWEI_TP_COLOR_INFO) += tp_color.o

EXTRA_CFLAGS += -Idrivers/hisi/tzdriver
EXTRA_CFLAGS += -Idrivers/tzdriver
