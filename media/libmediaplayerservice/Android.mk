LOCAL_PATH:= $(call my-dir)

#
# libmediaplayerservice
#

include $(CLEAR_VARS)

#BUILD_FF_PALYER := true
#BUILD_RKBOX_PALYER := true



LOCAL_SRC_FILES:=               \
    ActivityManager.cpp         \
    Crypto.cpp                  \
    Drm.cpp                     \
    HDCP.cpp                    \
    MediaPlayerFactory.cpp      \
    MediaPlayerService.cpp      \
    MediaRecorderClient.cpp     \
    MetadataRetrieverClient.cpp \
    MidiFile.cpp                \
    MidiMetadataRetriever.cpp   \
    RemoteDisplay.cpp           \
    SharedLibrary.cpp           \
    StagefrightPlayer.cpp       \
    StagefrightRecorder.cpp     \
    TestPlayerStub.cpp          \
    ApePlayer.cpp 		\
    FFPlayer.cpp	        \
    RKBOXFFPlayer.cpp

ifeq ($(PLATFORM_VERSION),4.4.2)	
LOCAL_CFLAGS := -DAVS44 		 
endif

LOCAL_SHARED_LIBRARIES :=       \
    libbinder                   \
    libcamera_client            \
    libcutils                   \
    liblog                      \
    libdl                       \
    libgui                      \
    libmedia                    \
    libsonivox                  \
    librkffplayer               \
    librkboxffplayer            \
    libstagefright              \
    libstagefright_foundation   \
    libstagefright_httplive     \
    libstagefright_omx          \
    libstagefright_wfd          \
    libutils                    \
    libvorbisidec               \
    libapedec			\
    


LOCAL_STATIC_LIBRARIES :=       \
    libstagefright_nuplayer     \
    libstagefright_rtsp         \
    libstagefright_urlcheck	\

LOCAL_C_INCLUDES :=                                                 \
    external/mac  \
    $(call include-path-for, graphics corecg)                       \
    $(TOP)/frameworks/av/media/libstagefright/include               \
    $(TOP)/frameworks/av/media/rk_ffplayer                          \
    $(TOP)/external/ffmpeg                                          \
    $(TOP)/frameworks/av/media/rkbox_ffplayer                       \
    $(TOP)/frameworks/av/media/libstagefright/rtsp                  \
    $(TOP)/frameworks/av/media/libstagefright/wifi-display          \
    $(TOP)/frameworks/native/include/media/openmax                  \
    $(TOP)/frameworks/av/media/libstagefright/libvpu/common	    \
    $(TOP)/frameworks/av/media/libstagefright/libvpu/common/include \
    $(TOP)/external/tremolo/Tremolo                                 \
    $(TOP)/hardware/rk29/libon2					    \
    $(LOCAL_PATH)/urlcheck                       		    \
    
ifeq ($(BUILD_FF_PALYER),true)
	LOCAL_SRC_FILES += \
			FFPlayer.cpp

	LOCAL_CFLAGS +=	\
		-DUSE_FFPLAYER

	LOCAL_SHARED_LIBRARIES += \
			  librkffplayer

 	LOCAL_C_INCLUDES += \
	         $(TOP)/frameworks/av/media/rk_ffplayer  
endif  

ifeq ($(BUILD_RKBOX_PALYER),true)
	LOCAL_SRC_FILES += \
			RKBOXFFPlayer.cpp
			
	LOCAL_CFLAGS +=	\
		-DUSE_RKBOX_FFPLAYER

	LOCAL_SHARED_LIBRARIES += \
			  librkboxffplayer

 	LOCAL_C_INCLUDES += \
	         $(TOP)/frameworks/av/media/rkbox_ffplayer  
endif   
 

LOCAL_MODULE:= libmediaplayerservice

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
