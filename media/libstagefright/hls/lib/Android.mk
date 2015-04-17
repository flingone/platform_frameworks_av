LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libavcodec.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libavdevice.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libavfilter.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libavformat.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libavresample.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libavutil.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libpostproc.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libswresample.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := libswscale.so
LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)
