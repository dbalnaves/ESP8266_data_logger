#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

CFLAGS = -D SNTP_CALC_TIME_US

PROJECT_NAME := data_logger

include $(IDF_PATH)/make/project.mk

