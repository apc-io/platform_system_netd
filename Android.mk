LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS) 

LOCAL_SRC_FILES += netd_mt7601:system/bin/netd_mt7601
LOCAL_SRC_FILES += netd:system/bin/netd
LOCAL_SRC_FILES += ndc:system/bin/ndc
LOCAL_SRC_FILES += netd_mtk6620:system/bin/netd_mtk6620
LOCAL_SRC_FILES += netd_mt5931:system/bin/netd_mt5931
LOCAL_SRC_FILES += netd_eagle:system/bin/netd_eagle
LOCAL_SRC_FILES += import_includes:obj/EXECUTABLES/netd_mtk6620_intermediates/import_includes

include $(WMT_PREBUILT)
