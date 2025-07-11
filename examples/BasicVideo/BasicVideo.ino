#include <WiFi.h>
#include <ESP32-RTSPServer.h>
#include "esp_camera.h"

// Reference: Camera pin definitions and setup adapted from MJPEG2SD project by s60sc (https://github.com/s60sc/ESP32-CAM_MJPEG2SD)
// ===================
// Select camera model
// ===================
// User's ESP32 cam board
#if defined(CONFIG_IDF_TARGET_ESP32)
#define CAMERA_MODEL_AI_THINKER 
//#define CAMERA_MODEL_WROVER_KIT 
//#define CAMERA_MODEL_ESP_EYE 
//#define CAMERA_MODEL_M5STACK_PSRAM 
//#define CAMERA_MODEL_M5STACK_V2_PSRAM 
//#define CAMERA_MODEL_M5STACK_WIDE 
//#define CAMERA_MODEL_M5STACK_ESP32CAM
//#define CAMERA_MODEL_M5STACK_UNITCAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL 
//#define CAMERA_MODEL_ESP32_CAM_BOARD
//#define CAMERA_MODEL_TTGO_T_CAMERA_PLUS
//#define CAMERA_MODEL_UICPAL_ESP32
//#define AUXILIARY

// User's ESP32S3 cam board
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define CAMERA_MODEL_FREENOVE_ESP32S3_CAM
//#define CAMERA_MODEL_PCBFUN_ESP32S3_CAM
//#define CAMERA_MODEL_XIAO_ESP32S3 
//#define CAMERA_MODEL_NEW_ESPS3_RE1_0
//#define CAMERA_MODEL_M5STACK_CAMS3_UNIT
//#define CAMERA_MODEL_ESP32S3_EYE 
//#define CAMERA_MODEL_ESP32S3_CAM_LCD
//#define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3
//#define CAMERA_MODEL_DFRobot_Romeo_ESP32S3
//#define CAMERA_MODEL_XENOIONEX
//#define CAMERA_MODEL_Waveshare_ESP32_S3_ETH
//#define CAMERA_MODEL_DFRobot_ESP32_S3_AI_CAM
#endif
#include "camera_pins.h"

// ===========================
// Enter your WiFi credentials
// ===========================
const char *ssid = "**********";
const char *password = "**********";

// RTSPServer instance
RTSPServer rtspServer;

// Variable to hold quality for RTSP frame
int quality;
// Task handles
TaskHandle_t videoTaskHandle = NULL; 

/** 
 * @brief Sets up the camera with the specified configuration. 
*/
// Camera setup function
bool setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  // for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // Initialize camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif
  Serial.println("Camera Setup Complete");
  return true;
}

/** 
 * @brief Retrieves the current frame quality from the camera. 
*/
void getFrameQuality() { 
  sensor_t * s = esp_camera_sensor_get(); 
  quality = s->status.quality; 
  Serial.printf("Camera Quality is: %d\n", quality);
}

/** 
 * @brief Task to send jpeg frames via RTP. 
*/
void sendVideo(void* pvParameters) { 
  while (true) { 
    // Send frame via RTP
    if(rtspServer.readyToSendFrame()) {
      camera_fb_t* fb = esp_camera_fb_get();
      rtspServer.sendRTSPFrame(fb->buf, fb->len, quality, fb->width, fb->height);
      esp_camera_fb_return(fb);
    }
    vTaskDelay(pdMS_TO_TICKS(1)); 
  }
}

void setup() {
  // Initialize serial communication
  Serial.begin(115200);

  // Set ESP32 core debug level to Info for verbose logging
  esp_log_level_set("*", ESP_LOG_INFO);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  // Setup camera
  if (!setupCamera()) {
    Serial.println("Camera setup failed. Halting.");
    while (true);
  }
  getFrameQuality();
  
  if (rtspServer.init()) { 
    Serial.printf("RTSP server started successfully using default values, Connect to rtsp://%s:554/\n", WiFi.localIP().toString().c_str());
  } else { 
    Serial.println("Failed to start RTSP server"); 
  }
  
  // Create tasks for sending video, and subtitles
  xTaskCreate(sendVideo, "Video", 8192, NULL, 9, &videoTaskHandle);
}

void loop() {
  delay(1000);
  vTaskDelete(NULL); // free 8k ram and delete the loop
}
