# Android makefile for display kernel modules

MOORECHIP_DLKM_ENABLE := true
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
       ifeq ($(TARGET_KERNEL_DLKM_MOORECHIP_OVERRIDE), false)
               MOORECHIP_DLKM_ENABLE := false
       endif
endif

ifeq ($(MOORECHIP_DLKM_ENABLE),  true)
       MOORECHIP_SELECT := CONFIG_MSM_MOORECHIP=m
       BOARD_OPENSOURCE_DIR ?= vendor/qcom/opensource
       BOARD_COMMON_DIR ?= device/qcom/common
       
       LOCAL_PATH := $(call my-dir)

       include $(CLEAR_VARS)

       # This makefile is only for DLKM
       ifneq ($(findstring vendor,$(LOCAL_PATH)),)

       ifneq ($(findstring opensource,$(LOCAL_PATH)),)
               MOORECHIP_BLD_DIR := $(shell pwd)/$(BOARD_OPENSOURCE_DIR)/moorechip-drivers
       endif # opensource

       DLKM_DIR := $(TOP)/$(BOARD_COMMON_DIR)/dlkm

       LOCAL_ADDITIONAL_DEPENDENCIES := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)

       # Build
       ###########################################################
       # This is set once per LOCAL_PATH, not per (kernel) module
       KBUILD_OPTIONS := MOORECHIP_ROOT=$(MOORECHIP_BLD_DIR)

       KBUILD_OPTIONS += MODNAME=moorechip_dlkm
       KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
       KBUILD_OPTIONS += $(MOORECHIP_SELECT)

       ###########################################################

       ###########################################################
        include $(CLEAR_VARS)
        LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
        LOCAL_MODULE              := leds-sn3112.ko
        LOCAL_MODULE_KBUILD_NAME  := leds-sn3112.ko
        LOCAL_MODULE_TAGS         := optional
        #LOCAL_MODULE_DEBUG_ENABLE := true
        LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
        include $(DLKM_DIR)/Build_external_kernelmodule.mk
        ###########################################################

       ###########################################################
        include $(CLEAR_VARS)
        LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
        LOCAL_MODULE              := leds-htr3212.ko
        LOCAL_MODULE_KBUILD_NAME  := leds-htr3212.ko
        LOCAL_MODULE_TAGS         := optional
        #LOCAL_MODULE_DEBUG_ENABLE := true
        LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
        include $(DLKM_DIR)/Build_external_kernelmodule.mk
        ###########################################################

       ###########################################################
        include $(CLEAR_VARS)
        LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
        LOCAL_MODULE              := moorechip-joystick.ko
        LOCAL_MODULE_KBUILD_NAME  := moorechip-joystick.ko
        LOCAL_MODULE_TAGS         := optional
        #LOCAL_MODULE_DEBUG_ENABLE := true
        LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
        include $(DLKM_DIR)/Build_external_kernelmodule.mk
        ###########################################################

       ###########################################################
        include $(CLEAR_VARS)
        LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
        LOCAL_MODULE              := focaltech_fp.ko
        LOCAL_MODULE_KBUILD_NAME  := focaltech_fp.ko
        LOCAL_MODULE_TAGS         := optional
        #LOCAL_MODULE_DEBUG_ENABLE := true
        LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
        include $(DLKM_DIR)/Build_external_kernelmodule.mk
        ###########################################################

       endif # DLKM check
endif
