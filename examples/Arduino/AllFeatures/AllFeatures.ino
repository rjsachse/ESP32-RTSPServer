#include <WiFi.h>
#include <ESP32-RTSPServer.h>
#include "esp_camera.h"

// Reference: Camera pin definitions and setup adapted from MJPEG2SD project by s60sc (https://github.com/s60sc/ESP32-CAM_MJPEG2SD)
// ===================
// Select camera model
// ===================
// If CAMERA_MODEL is not defined by build flags, let user select here
// User's ESP32 cam board
#if defined(CONFIG_IDF_TARGET_ESP32)
  #if defined(ARDUINO) && !defined(PLATFORMIO)
    #define CAMERA_MODEL_AI_THINKER
    // Uncomment ONE of the following to select your board:
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
  #endif
// User's ESP32-S3 cam board
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  #if defined(ARDUINO) && !defined(PLATFORMIO)
    #define CAMERA_MODEL_FREENOVE_ESP32S3_CAM
    // Uncomment ONE of the following to select your board:
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
#endif
#include "camera_pins.h"

// ===========================
// Enter your WiFi credentials
// ===========================
#ifndef SSID_NAME
#define SSID_NAME "**********"
#endif
#ifndef SSID_PASSWORD
#define SSID_PASSWORD "**********"
#endif

// RTSPServer instance
RTSPServer rtspServer;

// Can set a username and password for RTSP authentication or leave blank for no authentication
#ifndef RTSP_USER
#define RTSP_USER ""
#endif
#ifndef RTSP_PASSWORD
#define RTSP_PASSWORD ""
#endif

// Define HAVE_AUDIO to include audio-related code
//#define HAVE_AUDIO // Comment out if don't have audio

#ifdef HAVE_AUDIO
#include <ESP_I2S.h>
// I2SClass object for I2S communication
I2SClass I2S;

// I2S pins configuration
#ifndef I2S_SCK
#define I2S_SCK 3   // Serial Clock (SCK) or Bit Clock (BCLK)
#endif
#ifndef I2S_WS
#define I2S_WS  46  // Word Select (WS) or Left Right Clock (LRCLK)
#endif
#ifndef I2S_SDI
#define I2S_SDI 45  // Serial Data In (Mic)
#endif
#ifndef I2S_SDO
#define I2S_SDO -1  // Serial Data Out (Amp), optional, set to -1 if no speaker
#endif

// Audio parameters
static const int sampleRate = 8000;          // 8 kHz for G.711
static const size_t sampleCount = 160;       // 20 ms at 8 kHz
static const size_t sampleBytes = sampleCount * sizeof(int16_t); // 320 bytes
static const size_t g711Bytes = sampleCount; // 160 bytes
static int16_t* sampleBuffer = NULL;         // PCM buffer for I2S
static uint8_t* g711Buffer = NULL;           // G.711 buffer for RTSP
#endif

// Variable to hold quality for RTSP frame
int quality;
// Task handles
TaskHandle_t videoTaskHandle = NULL; 
TaskHandle_t audioTaskHandle = NULL; 
TaskHandle_t subtitlesTaskHandle = NULL;

/** 
 * @brief Sets up the camera with the specified configuration. 
 */
void setupCamera() {
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
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
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
}

/** 
 * @brief Retrieves the current frame quality from the camera. 
 */
void getFrameQuality() { 
  sensor_t * s = esp_camera_sensor_get(); 
  quality = s->status.quality; 
  Serial.printf("Camera Quality is: %d\n", quality);
}

#ifdef HAVE_AUDIO
/** 
 * @brief Sets up the I2S microphone and speaker (if I2S_SDO is defined). 
 * 
 * @return true if setup is successful, false otherwise. 
 */
static bool setupMic() {
  bool res;
  // Configure I2S pins, SDO is optional (-1 if no speaker)
  I2S.setPins(I2S_SCK, I2S_WS, I2S_SDO, I2S_SDI, -1);  // No MCLK

  // Initialize I2S: 8 kHz, 16-bit, mono
  res = I2S.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT, 
                  I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
  if (!res) {
    Serial.println("I2S initialization failed");
    return false;
  }

  // Allocate buffers
#if defined(USE_PSRAM) || defined(BOARD_HAS_PSRAM) // Check for PSRAM
  if (sampleBuffer == NULL) sampleBuffer = (int16_t*)ps_malloc(sampleBytes);
  if (g711Buffer == NULL) g711Buffer = (uint8_t*)ps_malloc(g711Bytes);
#else
  if (sampleBuffer == NULL) sampleBuffer = (int16_t*)malloc(sampleBytes);
  if (g711Buffer == NULL) g711Buffer = (uint8_t*)malloc(g711Bytes);
#endif

  if (sampleBuffer == NULL || g711Buffer == NULL) {
    Serial.println("Buffer allocation failed");
    return false;
  }

  Serial.printf("Free heap after audio setup: %u bytes\n", ESP.getFreeHeap());
  return true;
}

/** 
 * @brief Reads audio data from the I2S microphone. 
 * 
 * @return The number of bytes read. 
 */
static size_t micInput() {
  // read esp mic
  size_t bytesRead = 0;
  bytesRead = I2S.readBytes((char*)sampleBuffer, sampleBytes);
  return bytesRead;
}

/**
 * @brief Task to send audio data via RTP. 
 */
void sendAudio(void* pvParameters) { 
  Serial.printf("[Audio] Started on Core %d\n", xPortGetCoreID());
  while (true) { 
    if (rtspServer.readyToSendAudio()) {
      size_t bytesRead = micInput();
      if (bytesRead) {
        rtspServer.sendRTSPAudio(sampleBuffer, bytesRead);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/**
 * @brief Callback to handle received audio from client.
 * If I2S_SDO is -1, no speaker output is attempted.
 */
void receivedAudio(const uint8_t* l16Data, size_t len) {
  if (I2S_SDO != -1) { // Only write to speaker if I2S_SDO is defined
    size_t bytesWritten = I2S.write(l16Data, len);
    if (bytesWritten != len) {
      Serial.printf("I2S write failed: Wrote %d of %d bytes\n", bytesWritten, len);
    }
  } else {
    Serial.printf("Received audio (%d bytes), no speaker configured\n", len);
  }
}
#endif

/** 
 * @brief Task to send jpeg frames via RTP. 
 */
void sendVideo(void* pvParameters) { 
  Serial.printf("[Video] Started on Core %d\n", xPortGetCoreID());
  while (true) { 
    // Send frame via RTP
    if (rtspServer.readyToSendFrame()) {
      camera_fb_t* fb = esp_camera_fb_get();
      rtspServer.sendRTSPFrame(fb->buf, fb->len, quality, fb->width, fb->height);
      esp_camera_fb_return(fb);
    }
    vTaskDelay(pdMS_TO_TICKS(1)); 
  }
}

/**
 * @brief Task to send subtitles via RTP. 
 */
void sendSubtitles(void* pvParameters) {
  char data[100];
  while (true) {
    if (rtspServer.readyToSendSubtitles()) {
      size_t len = snprintf(data, sizeof(data), "FPS: %lu", rtspServer.rtpFps);
      rtspServer.sendRTSPSubtitles(data, len);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Delay for 1 second
  }
}

// Timer callback function 
void onSubtitles(void* arg) { 
  char data[100];
  if (rtspServer.readyToSendSubtitles()) {
    size_t len = snprintf(data, sizeof(data), "FPS: %lu", rtspServer.rtpFps);
    rtspServer.sendRTSPSubtitles(data, len);
  }
}

void onClientActivity(ClientActivityType activity, const char* clientIp, uint16_t clientPort, uint8_t activeClients) {
  switch (activity) {
    case ClientActivityType::CONNECTED:
      Serial.printf("Client connected: %s:%d, Active clients: %d\n", clientIp, clientPort, activeClients);
      break;
    case ClientActivityType::DISCONNECTED:
      Serial.printf("Client disconnected: %s:%d, Active clients: %d\n", clientIp, clientPort, activeClients);
      if (activeClients == 0) {
        Serial.println("All clients disconnected.");
      }
      break;
    case ClientActivityType::REFUSED_MAX_CLIENTS:
      Serial.printf("Client refused (max clients): %s:%d, Active clients: %d\n", clientIp, clientPort, activeClients);
      break;
  }
}

void printDeviceInfo() {
  // Local function to format size
  auto fmtSize = [](size_t bytes) -> String {
    const char* sizes[] = { "B", "KB", "MB", "GB" };
    int order = 0;
    while (bytes >= 1024 && order < 3) {
      order++;
      bytes = bytes / 1024;
    }
    return String(bytes) + " " + sizes[order];
  };

  // Print device information
  Serial.println("");
  Serial.println("==== Device Information ====");
  Serial.printf("ESP32 Chip ID: %u\n", ESP.getEfuseMac());
  Serial.printf("Flash Chip Size: %s\n", fmtSize(ESP.getFlashChipSize()));
  if (psramFound()) {
    Serial.printf("PSRAM Size: %s\n", fmtSize(ESP.getPsramSize()));
  } else {
    Serial.println("No PSRAM is found");
  }
  Serial.println("");
  // Print sketch information
  Serial.println("==== Sketch Information ====");
  Serial.printf("Sketch Size: %s\n", fmtSize(ESP.getSketchSize()));
  Serial.printf("Free Sketch Space: %s\n", fmtSize(ESP.getFreeSketchSpace()));
  Serial.printf("Sketch MD5: %s\n", ESP.getSketchMD5().c_str());
  Serial.println("");
  // Print task information
  Serial.println("==== Task Information ====");
  Serial.printf("Total tasks: %u\n", uxTaskGetNumberOfTasks() - 1);
  Serial.println("");
  // Print network information
  Serial.println("==== Network Information ====");
  Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
  Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  Serial.println("");
  // Print RTSP server information
  Serial.println("==== RTSP Server Information ====");
  Serial.printf("RTSP Port: %d\n", rtspServer.rtspPort);
  Serial.printf("Sample Rate: %d\n", rtspServer.sampleRate);
  Serial.printf("Transport Type: %d\n", rtspServer.transport);
  Serial.printf("Video Port: %d\n", rtspServer.rtpVideoPort);
  Serial.printf("Audio Port: %d\n", rtspServer.rtpAudioPort);
  Serial.printf("Subtitles Port: %d\n", rtspServer.rtpSubtitlesPort);
  Serial.printf("RTP IP: %s\n", rtspServer.rtpIp.toString().c_str());
  Serial.printf("RTP TTL: %d\n", rtspServer.rtpTTL);
#ifdef HAVE_AUDIO
  Serial.printf("Audio Out Codec: %s\n", rtspServer.audioOutCodec == RTSPServer::G711_ULAW ? "G711_ULAW" : 
                                    rtspServer.audioOutCodec == RTSPServer::G711_ALAW ? "G711_ALAW" : "L16");
  Serial.printf("Audio In Codec: %s\n", rtspServer.audioInCodec == RTSPServer::G711_ULAW ? "G711_ULAW" : 
                                    rtspServer.audioInCodec == RTSPServer::G711_ALAW ? "G711_ALAW" : "L16");
#endif
  Serial.println("");
  Serial.printf("RTSP Address: rtsp://%s:%d\n", WiFi.localIP().toString().c_str(), rtspServer.rtspPort);
  Serial.println("==============================");
  Serial.println("");
}

void setup() {
  // Initialize serial communication
  Serial.begin(115200);

  // Connect to WiFi
  WiFi.begin(SSID_NAME, SSID_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  // Setup camera
  setupCamera();
  getFrameQuality();

#ifdef HAVE_AUDIO
  // Setup microphone and speaker (if I2S_SDO is defined)
  if (setupMic()) {
    Serial.println("Microphone and Speaker Setup Complete");
    // Create task for sending audio, pinned to Core 1
    xTaskCreatePinnedToCore(sendAudio, "Audio", 8192, NULL, 8, &audioTaskHandle, 1);
    // Register callback for receiving audio
    rtspServer.setAudioReceiveCallback(receivedAudio);
  } else {
    Serial.println("Mic and Speaker Setup Failed!");
  }

#ifdef USE_SPEEXDSP
  // Configure audio processing settings via audioProcessor
  rtspServer.audioProcessor.enableAEC(true); // Enable to remove echo from mic input
  rtspServer.audioProcessor.enableMicNoiseSuppression(false); // Disable noise suppression for mic
  rtspServer.audioProcessor.setMicNoiseSuppressionLevel(-25); // Set noise reduction strength
  rtspServer.audioProcessor.enableMicAGC(true, 0.8f); // Enable Automatic Gain Control
  rtspServer.audioProcessor.enableMicVAD(false); // Disable Voice Activity Detection
  rtspServer.audioProcessor.setMicVADThreshold(80); // Set VAD sensitivity
  rtspServer.audioProcessor.enableSpeakerNoiseSuppression(false); // Disable noise suppression for speaker
  rtspServer.audioProcessor.setSpeakerNoiseSuppressionLevel(-15); // Set speaker NS strength
  rtspServer.audioProcessor.enableSpeakerAGC(true, 0.9f); // Enable AGC for speaker
#endif
#endif

  // Create task for sending video, pinned to Core 0
  xTaskCreatePinnedToCore(sendVideo, "Video", 8192, NULL, 9, &videoTaskHandle, 0);

  // Use timer for subtitles
  rtspServer.startSubtitlesTimer(onSubtitles); // 1-second period

  rtspServer.maxRTSPClients = 5; // Set max RTSP clients
  rtspServer.setCredentials(RTSP_USER, RTSP_PASSWORD); // Set RTSP authentication
  rtspServer.setClientActivityCallback(onClientActivity); // Register client activity callback

  // Initialize the RTSP server
  /**
   * @brief Initializes the RTSP server with the specified configuration.
   * 
   * This method can be called with specific parameters, or the parameters
   * can be set directly in the RTSPServer instance before calling init().
   * If any parameter is not explicitly set, the method uses default values.
   * 
   * @param transport The transport type. Default is VIDEO_AND_SUBTITLES. Options are (VIDEO_ONLY, AUDIO_ONLY, VIDEO_AND_AUDIO, VIDEO_AND_SUBTITLES, AUDIO_AND_SUBTITLES, VIDEO_AUDIO_SUBTITLES).
   * @param rtspPort The RTSP port to use. Default is 554.
   * @param sampleRate The sample rate for audio streaming. Default is 0, must be set if using audio.
   * @param port1 The first port (used for video, audio, or subtitles depending on transport). Default is 5430.
   * @param port2 The second port (used for audio or subtitles depending on transport). Default is 5432.
   * @param port3 The third port (used for subtitles). Default is 5434.
   * @param rtpIp The IP address for RTP multicast streaming. Default is IPAddress(239, 255, 0, 1).
   * @param rtpTTL The TTL value for RTP multicast packets. Default is 64.
   * @param outCodec The codec for audio streamed from ESP32 (mic) to client. Default is L16. Options are (G711_ULAW, G711_ALAW, L16).
   * @param inCodec The codec for audio received from client to ESP32 (speaker). Default is G711_ULAW. Options are (G711_ULAW, G711_ALAW, L16).
   * @return true if initialization is successful, false otherwise.
   *
   * Example usage:
   * // Option 1: Start RTSP server with default values
   * if (rtspServer.init()) { 
   *   Serial.println("RTSP server started successfully on port 554"); 
   * } else { 
   *   Serial.println("Failed to start RTSP server"); 
   * }
   * 
   * // Option 2: Set variables directly and then call init
   * rtspServer.transport = RTSPServer::VIDEO_AUDIO_SUBTITLES; 
   * rtspServer.sampleRate = 8000; 
   * rtspServer.rtspPort = 8554; 
   * rtspServer.rtpIp = IPAddress(239, 255, 0, 1); 
   * rtspServer.rtpTTL = 64; 
   * rtspServer.rtpVideoPort = 5004; 
   * rtspServer.rtpAudioPort = 5006; 
   * rtspServer.rtpSubtitlesPort = 5008;
   * rtspServer.audioOutCodec = RTSPServer::G711_ULAW; // Mic to client
   * rtspServer.audioInCodec = RTSPServer::G711_ULAW;  // Client to speaker
   * if (rtspServer.init()) { 
   *   Serial.println("RTSP server started successfully"); 
   * } else { 
   *   Serial.println("Failed to start RTSP server"); 
   * }
   * 
   * // Option 3: Set variables in the init call
   * if (rtspServer.init(RTSPServer::VIDEO_AUDIO_SUBTITLES, 554, 8000, 5004, 5006, 5008, 
   *                     IPAddress(239, 255, 0, 1), 64, RTSPServer::G711_ULAW, RTSPServer::G711_ULAW)) { 
   *   Serial.println("RTSP server started successfully"); 
   * } else { 
   *   Serial.println("Failed to start RTSP server"); 
   * }
   *
   * Also have deinit() and reinit() for either deinitialize or reinitialize the RTSP server.
   * Use reinit() if changing settings.
   */
#ifdef HAVE_AUDIO
  if (rtspServer.init(RTSPServer::VIDEO_AUDIO_SUBTITLES, 554, sampleRate, 5430, 5432, 5434, 
                      IPAddress(239, 255, 0, 1), 64, RTSPServer::L16, RTSPServer::G711_ULAW)) {
    Serial.printf("RTSP server started successfully, Connect to rtsp://%s:554/\n", WiFi.localIP().toString().c_str());
  } else { 
    Serial.println("Failed to start RTSP server"); 
  }
#else
  if (rtspServer.init(RTSPServer::VIDEO_AND_SUBTITLES, 554)) { 
    Serial.printf("RTSP server started successfully using default values, Connect to rtsp://%s:554/\n", WiFi.localIP().toString().c_str());
  } else { 
    Serial.println("Failed to start RTSP server"); 
  }
#endif
}

void loop() {
  printDeviceInfo(); // Print device info
  delay(1000);
  vTaskDelete(NULL); // Free 8k RAM and delete the loop
}
