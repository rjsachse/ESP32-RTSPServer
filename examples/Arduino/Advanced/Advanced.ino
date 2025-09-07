#include <WiFi.h>
#include <ESP32-RTSPServer.h>
#include <esp_camera.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>

// Camera model
#define CAMERA_MODEL_FREENOVE_ESP32S3_CAM
#include "camera_pins.h"

// WiFi credentials
#ifndef SSID_NAME
#define SSID_NAME "**********"
#endif
#ifndef SSID_PASSWORD
#define SSID_PASSWORD "**********"
#endif

// RTSP credentials
#ifndef RTSP_USER
#define RTSP_USER ""
#endif
#ifndef RTSP_PASSWORD
#define RTSP_PASSWORD ""
#endif

// I2S pins (use gpio_num_t)
#define I2S_SCK GPIO_NUM_3
#define I2S_WS  GPIO_NUM_46
#define I2S_SDI GPIO_NUM_45
#define I2S_SDO GPIO_NUM_NC  // No speaker

// Audio params
static const int sampleRate = 8000;
static const size_t sampleCount = 160;
static const size_t sampleBytes = sampleCount * sizeof(int16_t);
static int16_t* sampleBuffer = NULL;

// I2S low-level
QueueHandle_t i2sEventQueue = NULL;
const i2s_port_t I2S_PORT = I2S_NUM_0;
static const size_t i2sQueueSize = 10;
i2s_chan_handle_t rx_handle = NULL;
i2s_chan_handle_t tx_handle = NULL;

// Globals
RTSPServer rtspServer;
int quality;
TaskHandle_t videoTaskHandle = NULL;
TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t subtitlesTaskHandle = NULL;

// Forward declaration of I2S callback
static bool IRAM_ATTR i2s_event_callback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_data);

// Camera setup (unchanged)
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
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: %s\n", esp_err_to_name(err));
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, FRAMESIZE_QVGA);
  Serial.println("Camera Setup Complete");
}

void getFrameQuality() {
  sensor_t *s = esp_camera_sensor_get();
  quality = s->status.quality;
  Serial.printf("Camera Quality: %d\n", quality);
}

// Low-level I2S setup
static bool setupMic() {
  Serial.printf("Heap before I2S setup: %u B\n", esp_get_free_heap_size());

  // Create event queue
  i2sEventQueue = xQueueCreate(i2sQueueSize, sizeof(i2s_event_data_t));
  if (i2sEventQueue == NULL) {
    Serial.println("Failed to create I2S event queue");
    return false;
  }

  // I2S channel config
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.dma_frame_num = sampleCount;
  chan_cfg.auto_clear = false;

  // Initialize RX channel
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  if (err != ESP_OK) {
    Serial.printf("I2S RX channel init failed: %s\n", esp_err_to_name(err));
    vQueueDelete(i2sEventQueue);
    i2sEventQueue = NULL;
    return false;
  }

  // RX mode: 8kHz, 16-bit, mono
  i2s_std_config_t rx_std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = I2S_SCK,
      .ws = I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = I2S_SDI,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false
      }
    }
  };
  err = i2s_channel_init_std_mode(rx_handle, &rx_std_cfg);
  if (err != ESP_OK) {
    Serial.printf("I2S RX init failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(rx_handle);
    vQueueDelete(i2sEventQueue);
    i2sEventQueue = NULL;
    return false;
  }

  // Register callback
  i2s_event_callbacks_t cbs = {
    .on_recv = i2s_event_callback,
    .on_recv_q_ovf = NULL,
    .on_sent = NULL,
    .on_send_q_ovf = NULL
  };
  err = i2s_channel_register_event_callback(rx_handle, &cbs, NULL);
  if (err != ESP_OK) {
    Serial.printf("I2S RX callback register failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(rx_handle);
    vQueueDelete(i2sEventQueue);
    i2sEventQueue = NULL;
    return false;
  }

  // Initialize TX channel (if speaker)
  if (I2S_SDO != GPIO_NUM_NC) {
    err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
      Serial.printf("I2S TX channel init failed: %s\n", esp_err_to_name(err));
      i2s_del_channel(rx_handle);
      vQueueDelete(i2sEventQueue);
      i2sEventQueue = NULL;
      return false;
    }

    i2s_std_config_t tx_std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = I2S_SCK,
        .ws = I2S_WS,
        .dout = I2S_SDO,
        .din = I2S_GPIO_UNUSED,
        .invert_flags = {
          .mclk_inv = false,
          .bclk_inv = false,
          .ws_inv = false
        }
      }
    };
    err = i2s_channel_init_std_mode(tx_handle, &tx_std_cfg);
    if (err != ESP_OK) {
      Serial.printf("I2S TX init failed: %s\n", esp_err_to_name(err));
      i2s_del_channel(rx_handle);
      i2s_del_channel(tx_handle);
      vQueueDelete(i2sEventQueue);
      i2sEventQueue = NULL;
      return false;
    }
  }

  // Allocate buffer in PSRAM
  sampleBuffer = (int16_t*)ps_malloc(sampleBytes);
  if (sampleBuffer == NULL) {
    Serial.println("Buffer allocation failed");
    i2s_del_channel(rx_handle);
    if (tx_handle) i2s_del_channel(tx_handle);
    vQueueDelete(i2sEventQueue);
    i2sEventQueue = NULL;
    return false;
  }

  // Enable channels
  err = i2s_channel_enable(rx_handle);
  if (err != ESP_OK) {
    Serial.printf("I2S RX enable failed: %s\n", esp_err_to_name(err));
    free(sampleBuffer);
    i2s_del_channel(rx_handle);
    if (tx_handle) i2s_del_channel(tx_handle);
    vQueueDelete(i2sEventQueue);
    i2sEventQueue = NULL;
    return false;
  }

  if (tx_handle) {
    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
      Serial.printf("I2S TX enable failed: %s\n", esp_err_to_name(err));
      free(sampleBuffer);
      i2s_del_channel(rx_handle);
      i2s_del_channel(tx_handle);
      vQueueDelete(i2sEventQueue);
      i2sEventQueue = NULL;
      return false;
    }
  }

  Serial.printf("Heap after I2S setup: %u B\n", esp_get_free_heap_size());
  return true;
}

// I2S event callback
static bool IRAM_ATTR i2s_event_callback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_data) {
  // No type check needed; on_recv indicates RX done
  BaseType_t higher_priority_task_woken = pdFALSE;
  xQueueSendFromISR(i2sEventQueue, event, &higher_priority_task_woken);
  return (higher_priority_task_woken == pdTRUE);
}

// Audio task
void sendAudio(void* pvParameters) {
  Serial.printf("[Audio] Started on Core %d\n", xPortGetCoreID());
  i2s_event_data_t event;
  while (true) {
    if (xQueueReceive(i2sEventQueue, &event, portMAX_DELAY) == pdTRUE) {
      size_t bytesRead = 0;
      esp_err_t err = i2s_channel_read(rx_handle, sampleBuffer, sampleBytes, &bytesRead, 0);
      if (err == ESP_OK && bytesRead == sampleBytes && rtspServer.readyToSendAudio()) {
        rtspServer.sendRTSPAudio(sampleBuffer, bytesRead);
      } else if (bytesRead != sampleBytes) {
        Serial.printf("I2S read incomplete: %d of %d bytes, err: %s\n", bytesRead, sampleBytes, esp_err_to_name(err));
      }
    }
  }
}

// Audio receive callback
void receivedAudio(const uint8_t* l16Data, size_t len) {
  if (tx_handle) {
    size_t bytesWritten = 0;
    esp_err_t err = i2s_channel_write(tx_handle, l16Data, len, &bytesWritten, portMAX_DELAY);
    if (err != ESP_OK || bytesWritten != len) {
      Serial.printf("I2S write failed: %s, Wrote %d of %d bytes\n", esp_err_to_name(err), bytesWritten, len);
    }
  } else {
    Serial.printf("Received audio (%d bytes), no speaker\n", len);
  }
}

// Video task
void sendVideo(void* pvParameters) {
  Serial.printf("[Video] Started on Core %d\n", xPortGetCoreID());
  while (true) {
    if (rtspServer.readyToSendFrame()) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        rtspServer.sendRTSPFrame(fb->buf, fb->len, quality, fb->width, fb->height);
        esp_camera_fb_return(fb);
      }
    }
    taskYIELD();
  }
}

// Subtitles task
void sendSubtitles(void* pvParameters) {
  char data[100];
  while (true) {
    if (rtspServer.readyToSendSubtitles()) {
      size_t len = snprintf(data, sizeof(data), "FPS: %lu", rtspServer.rtpFps);
      rtspServer.sendRTSPSubtitles(data, len);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

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
      break;
    case ClientActivityType::REFUSED_MAX_CLIENTS:
      Serial.printf("Client refused (max clients): %s:%d, Active clients: %d\n", clientIp, clientPort, activeClients);
      break;
  }
}

void printDeviceInfo() {
  auto fmtSize = [](size_t bytes) -> String {
    const char* sizes[] = { "B", "KB", "MB", "GB" };
    int order = 0;
    while (bytes >= 1024 && order < 3) {
      order++;
      bytes /= 1024;
    }
    return String(bytes) + " " + sizes[order];
  };

  Serial.println("\n==== Device Information ====");
  Serial.printf("ESP32 Chip ID: %u\n", ESP.getEfuseMac());
  Serial.printf("Flash Chip Size: %s\n", fmtSize(ESP.getFlashChipSize()));
  Serial.printf("PSRAM Size: %s\n", fmtSize(ESP.getPsramSize()));
  Serial.println("\n==== Sketch Information ====");
  Serial.printf("Sketch Size: %s\n", fmtSize(ESP.getSketchSize()));
  Serial.printf("Free Sketch Space: %s\n", fmtSize(ESP.getFreeSketchSpace()));
  Serial.printf("Sketch MD5: %s\n", ESP.getSketchMD5().c_str());
  Serial.println("\n==== Task Information ====");
  Serial.printf("Total tasks: %u\n", uxTaskGetNumberOfTasks() - 1);
  Serial.println("\n==== Network Information ====");
  Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
  Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  Serial.println("\n==== RTSP Server Information ====");
  Serial.printf("RTSP Port: %d\n", rtspServer.rtspPort);
  Serial.printf("Sample Rate: %d\n", rtspServer.sampleRate);
  Serial.printf("Transport Type: %d\n", rtspServer.transport);
  Serial.printf("Video Port: %d\n", rtspServer.rtpVideoPort);
  Serial.printf("Audio Port: %d\n", rtspServer.rtpAudioPort);
  Serial.printf("Subtitles Port: %d\n", rtspServer.rtpSubtitlesPort);
  Serial.printf("RTP IP: %s\n", rtspServer.rtpIp.toString().c_str());
  Serial.printf("RTP TTL: %d\n", rtspServer.rtpTTL);
  Serial.printf("Audio Codec: L16\n");
  Serial.printf("\nRTSP Address: rtsp://%s:%d\n", WiFi.localIP().toString().c_str(), rtspServer.rtspPort);
  Serial.println("==============================");
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting setup...");

  // WiFi with retry
  WiFi.begin(SSID_NAME, SSID_PASSWORD);
  int attempts = 0;
  const int maxAttempts = 20;
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(1000);
    Serial.printf("Connecting to WiFi (attempt %d/%d)...\n", attempts + 1, maxAttempts);
    attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed!");
    return;
  }
  Serial.println("Connected to WiFi");

  // Camera setup
  setupCamera();
  getFrameQuality();

  // Audio setup
  if (setupMic()) {
    Serial.println("Microphone Setup Complete");
    xTaskCreatePinnedToCore(sendAudio, "Audio", 8192, NULL, 8, &audioTaskHandle, 1);
    rtspServer.setAudioReceiveCallback(receivedAudio);
  } else {
    Serial.println("Microphone Setup Failed!");
    return;
  }

  // Video task
  xTaskCreatePinnedToCore(sendVideo, "Video", 8192, NULL, 9, &videoTaskHandle, 0);

  // Subtitles timer
  rtspServer.startSubtitlesTimer(onSubtitles);

  // RTSP setup
  rtspServer.maxRTSPClients = 5;
  rtspServer.setCredentials(RTSP_USER, RTSP_PASSWORD);
  rtspServer.setClientActivityCallback(onClientActivity);

  if (rtspServer.init(RTSPServer::VIDEO_AUDIO_SUBTITLES, 554, sampleRate, 5430, 5432, 5434,
                      IPAddress(239, 255, 0, 1), 64, RTSPServer::L16, RTSPServer::L16)) {
    Serial.printf("RTSP server started, Connect to rtsp://%s:554/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Failed to start RTSP server");
  }
}

void loop() {
  printDeviceInfo();
  delay(1000);
  vTaskDelete(NULL);
}