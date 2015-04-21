LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	md5.c  \
	UDP.c  \
	sha1.c  \
	utf8.c  \
	base64.c  \
	Errors.c  \
	Websocket.c \
	Handshake.c  \
	Communicate.c  \
	Datastructures.c  \

LOCAL_SHARED_LIBRARIES := \
	liblog \

LOCAL_C_INCLUDES := \
    frameworks/av/services/medialog \

LOCAL_MODULE:= hls_ws

include $(BUILD_EXECUTABLE)
