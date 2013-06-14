#Enables the listed display HAL modules
#libs to be built for QCOM targets only

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(TARGET_QCOM_DISPLAY_VARIANT),legacy)
display-hals := libgralloc libgenlock libcopybit
display-hals += libhwcomposer liboverlay libqdutils
endif
endif

ifneq ($(TARGET_PROVIDES_LIBLIGHT),true)
display-hals += liblight
endif

display-hals += libtilerenderer

include $(call all-named-subdir-makefiles,$(display-hals))
