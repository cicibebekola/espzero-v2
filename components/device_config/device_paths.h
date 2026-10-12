#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_SD_MOUNT_POINT   "/sdcard"
#define DEVICE_APPS_DIR         DEVICE_SD_MOUNT_POINT "/apps"
#define DEVICE_MUSIC_DIR        DEVICE_SD_MOUNT_POINT "/music"
#define DEVICE_APPMGR_DIR       DEVICE_SD_MOUNT_POINT "/appmanager"
#define DEVICE_APPMGR_HTML_PATH DEVICE_APPMGR_DIR "/ui.html"

#ifdef __cplusplus
}
#endif
