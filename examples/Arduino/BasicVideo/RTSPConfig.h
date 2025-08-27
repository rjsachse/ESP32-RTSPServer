// RTSPConfig.h
#ifndef RTSP_CONFIG_H
#define RTSP_CONFIG_H

// Define ESP32_RTSP_LOGGING_ENABLED to enable logging
//#define RTSP_LOGGING_ENABLED // save 7.7kb of flash

// User defined options in sketch
//#define OVERRIDE_RTSP_SINGLE_CLIENT_MODE // Override the default behavior of allowing only one client for unicast or TCP
//#define RTSP_VIDEO_NONBLOCK // Enable non-blocking video streaming by creating a separate task for video streaming, preventing it from blocking the main video task.

#endif // RTSP_CONFIG_H
