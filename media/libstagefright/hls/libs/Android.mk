LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := \
	libavformat.a \
	libavcodec.a \
	libavutil.a \
	libswresample.a \
	libavfilter.a \
	libswscale.a \
	libpostproc.a \
	libavdevice.a \
	libavresample.a \

LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)

#------------------
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \

LOCAL_WHOLE_STATIC_LIBRARIES := \
	libavutil \
	libavformat \
	libavcodec \
	libavfilter \
	libswscale \
	libpostproc \
	libavdevice \
    libswresample \
	libavresample \

LOCAL_LDFLAGS += -Wl,--no-warn-shared-textrel,--no-fatal-warnings
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES += libcutils libutils libdl liblog libz
LOCAL_LDLIBS += -ldl -llog
LOCAL_MODULE:= libffmpeg_hls

include $(BUILD_SHARED_LIBRARY)
