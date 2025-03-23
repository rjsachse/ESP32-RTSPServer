#include "ESP32-RTSPServer.h"
#include "audioConverter.h"

const char* RTSPServer::LOG_TAG = "RTSPServer";

RTSPServer::RTSPServer()
  : rtpFps(0),
    // User can change these settings
    transport(VIDEO_AND_SUBTITLES), // Default transport 
    sampleRate(0),
    rtspPort(554),
    rtpIp(IPAddress(239, 255, 0, 1)), // Default RTP IP 
    rtpTTL(64), // Default TTL
    rtpVideoPort(5430),
    rtpAudioPort(5432),
    rtpAudioIPort(5436),
    rtpSubtitlesPort(5434),
    maxRTSPClients(3),
    //
    rtspSocket(-1),
    videoUnicastSocket(-1),
    audioUnicastSocket(-1), 
    audioIUnicastSocket(-1), 
    subtitlesUnicastSocket(-1),
    videoMulticastSocket(-1),
    audioMulticastSocket(-1),
    subtitlesMulticastSocket(-1),
    activeRTSPClients(0),
    maxClients(1),
    rtpVideoTaskHandle(NULL),
    rtspTaskHandle(NULL),
    rtpAudioITaskHandle(NULL), // Reordered initialization
    rtspStreamBuffer(NULL),
    rtspStreamBufferSize(0),
    rtpFrameSent(true),
    rtpAudioSent(true),
    rtpSubtitlesSent(true),
    vQuality(0),
    vWidth(0),
    vHeight(0),
    videoSequenceNumber(0),
    videoTimestamp(0),
    audioSequenceNumber(0),
    audioTimestamp(0),
    subtitlesSequenceNumber(0),
    subtitlesTimestamp(0),
    rtpFrameCount(0),
    lastRtpFPSUpdateTime(0),
    videoCh(0),
    audioCh(0),
    audioICh(0),
    subtitlesCh(0),
    isVideo(false),
    isAudio(false),
    isSubtitles(false),
    isPlaying(false),
    firstClientConnected(false),
    firstClientIsMulticast(false),
    firstClientIsTCP(false),
    authEnabled(false) // Reordered initialization
{
    isPlayingMutex = xSemaphoreCreateMutex(); // Initialize the mutex
    sendTcpMutex = xSemaphoreCreateMutex(); // Initialize the mutex
    maxClientsMutex = xSemaphoreCreateMutex();
#ifdef RTSP_LOGGING_ENABLED
    esp_log_level_set(LOG_TAG, ESP_LOG_DEBUG); // Set log level to DEBUG
#endif
  // Init SpeexDSP with all features
  audioProcessor.beginAEC(256, 1024, 16000);         // AEC: 256 samples, 16 kHz
  audioProcessor.beginPreprocess(256, 16000);        // NS, AGC, VAD
  audioProcessor.enableNoiseSuppression(true);
  audioProcessor.setNoiseSuppressionLevel(-30);
  audioProcessor.enableAGC(true, 1.0f);
  audioProcessor.enableVAD(true);
  audioProcessor.beginJitterBuffer(20);              // 20 ms step
  audioProcessor.beginBuffer(2048);                  // Ring buffer: 2048 samples
  audioProcessor.beginResampler(8000, 16000, 5);     // G.711 8 kHz -> 16 kHz
}

RTSPServer::~RTSPServer() {
  // Clean up resources
  deinit();
  vSemaphoreDelete(this->isPlayingMutex);
  vSemaphoreDelete(this->sendTcpMutex);
  vSemaphoreDelete(this->maxClientsMutex);
}

bool RTSPServer::init(TransportType transport, uint16_t rtspPort, uint32_t sampleRate, uint16_t port1, uint16_t port2, uint16_t port3, IPAddress rtpIp, uint8_t rtpTTL) {
  this->transport = (transport != NONE) ? transport : this->transport;
  this->rtspPort = (rtspPort != 0) ? rtspPort : this->rtspPort;
  this->rtpIp = (rtpIp != IPAddress()) ? rtpIp : this->rtpIp;
  this->rtpTTL = (rtpTTL != 255) ? rtpTTL : this->rtpTTL;

  if (transport == AUDIO_ONLY || transport == VIDEO_AND_AUDIO || transport == AUDIO_AND_SUBTITLES || transport == VIDEO_AUDIO_SUBTITLES) {
    if (this->sampleRate == 0 && sampleRate == 0) {
      if (Serial) {
        Serial.printf("RTSP Server Error: Sample rate must be set to use audio\n");
      }
      return false;
    }
    if (sampleRate != 0) {
      this->sampleRate = sampleRate;
    }
  }

  switch (this->transport) {
    case VIDEO_ONLY:
      this->rtpVideoPort = (port1 != 0) ? port1 : this->rtpVideoPort;
      this->isVideo = true;
      break;
    case AUDIO_ONLY:
      this->rtpAudioPort = (port1 != 0) ? port1 : this->rtpAudioPort;
      this->isAudio = true;
      break;
    case SUBTITLES_ONLY:
      this->rtpSubtitlesPort = (port1 != 0) ? port1 : this->rtpSubtitlesPort;
      this->isSubtitles = true;
      break;
    case VIDEO_AND_AUDIO:
      this->rtpVideoPort = (port1 != 0) ? port1 : this->rtpVideoPort;
      this->rtpAudioPort = (port2 != 0) ? port2 : this->rtpAudioPort;
      this->isVideo = true;
      this->isAudio = true;
      break;
    case VIDEO_AND_SUBTITLES:
      this->rtpVideoPort = (port1 != 0) ? port1 : this->rtpVideoPort;
      this->rtpSubtitlesPort = (port2 != 0) ? port2 : this->rtpSubtitlesPort;
      this->isVideo = true;
      this->isSubtitles = true;
      break;
    case AUDIO_AND_SUBTITLES:
      this->rtpAudioPort = (port1 != 0) ? port1 : this->rtpAudioPort;
      this->rtpSubtitlesPort = (port2 != 0) ? port2 : this->rtpSubtitlesPort;
      this->isAudio = true;
      this->isSubtitles = true;
      break;
    case VIDEO_AUDIO_SUBTITLES:
      this->rtpVideoPort = (port1 != 0) ? port1 : this->rtpVideoPort;
      this->rtpAudioPort = (port2 != 0) ? port2 : this->rtpAudioPort;
      this->rtpSubtitlesPort = (port3 != 0) ? port3 : this->rtpSubtitlesPort;
      this->isVideo = true;
      this->isAudio = true;
      this->isSubtitles = true;
      break;
    case NONE:
      RTSP_LOGE(LOG_TAG, "Transport type can not be NONE");
      return false;
    default:
      RTSP_LOGE(LOG_TAG, "Invalid transport type for this init method");
      return false;
  }

  return prepRTSP();
}

void RTSPServer::deinit() {
  if (this->rtspTaskHandle != NULL) {
    vTaskDelete(this->rtspTaskHandle);
    this->rtspTaskHandle = NULL;
  }
  if (this->rtpVideoTaskHandle != NULL) {
    vTaskDelete(this->rtpVideoTaskHandle);
    this->rtpVideoTaskHandle = NULL;
  }
  if (this->rtpAudioITaskHandle != NULL) {
    vTaskDelete(this->rtpAudioITaskHandle);
    this->rtpAudioITaskHandle = NULL;
  }
  if (this->rtspSocket >= 0) {
    close(this->rtspSocket);
    this->rtspSocket = -1;
  }
  
  closeSockets();
  
  if (this->rtspStreamBuffer) {
    free(this->rtspStreamBuffer);
  }

  RTSP_LOGI(LOG_TAG, "RTSP server deinitialized.");
}

bool RTSPServer::reinit() {
  deinit();
  return init();
}

void RTSPServer::closeSockets() {
  if (videoUnicastSocket != -1) {
    close(videoUnicastSocket);
    videoUnicastSocket = -1;
  }
  if (audioUnicastSocket != -1) {
    close(audioUnicastSocket);
    audioUnicastSocket = -1;
  }
  if (subtitlesUnicastSocket != -1) {
    close(subtitlesUnicastSocket);
    subtitlesUnicastSocket = -1;
  }
  if (videoMulticastSocket != -1) {
    close(videoMulticastSocket);
    videoMulticastSocket = -1;
  }
  if (audioMulticastSocket != -1) {
    close(audioMulticastSocket);
    audioMulticastSocket = -1;
  }
  if (subtitlesMulticastSocket != -1) {
    close(subtitlesMulticastSocket);
    subtitlesMulticastSocket = -1;
  }
}

bool RTSPServer::prepRTSP() {
  uint64_t mac = ESP.getEfuseMac();
  this->videoSSRC = static_cast<uint32_t>(mac & 0xFFFFFFFF);
  this->audioSSRC = static_cast<uint32_t>((mac >> 32) & 0xFFFFFFFF);
  this->subtitlesSSRC = static_cast<uint32_t>((mac >> 48) & 0xFFFFFFFF);

  this->rtspSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (this->rtspSocket < 0) {
    RTSP_LOGE(LOG_TAG, "Failed to create RTSP socket.");
    return false;
  }

  if (!setNonBlocking(this->rtspSocket)) {
    RTSP_LOGE(LOG_TAG, "Failed to set RTSP socket to non-blocking mode.");
    close(this->rtspSocket);
    return false;
  }

  struct sockaddr_in serverAddr;
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(this->rtspPort);

  if (bind(this->rtspSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
    RTSP_LOGE(LOG_TAG, "Failed to bind RTSP socket: %d", this->rtspSocket);
    close(this->rtspSocket);
    return false;
  }
  
  if (listen(this->rtspSocket, 5) < 0) {
    RTSP_LOGE(LOG_TAG, "Failed to listen on RTSP socket.");
    close(this->rtspSocket);
    return false;
  }

  if (this->rtspTaskHandle == NULL) {
    if (xTaskCreate(rtspTaskWrapper, "rtspTask", RTSP_STACK_SIZE, this, RTSP_PRI, &this->rtspTaskHandle) != pdPASS) {
      RTSP_LOGE(LOG_TAG, "Failed to create RTSP task.");
      close(this->rtspSocket);
      return false;
    }
  }

  RTSP_LOGI(LOG_TAG, "RTSP server setup completed, listening on port: %d", this->rtspPort);
  return true;
}

void RTSPServer::rtspTaskWrapper(void* pvParameters) {
  RTSPServer* server = static_cast<RTSPServer*>(pvParameters);
  server->rtspTask();
}

void RTSPServer::rtspTask() {
  struct sockaddr_in clientAddr;
  socklen_t addr_len = sizeof(clientAddr);
  fd_set read_fds;
  int client_sockets[MAX_CLIENTS] = {0};
  int max_sd, activity, client_sock;

  while (true) {
    FD_ZERO(&read_fds);
    FD_SET(this->rtspSocket, &read_fds);
    max_sd = this->rtspSocket;

    uint8_t currentMaxClients = getMaxClients();

    for (int i = 0; i < currentMaxClients; i++) {
      int sd = client_sockets[i];
      if (sd > 0) FD_SET(sd, &read_fds);
      if (sd > max_sd) max_sd = sd;
    }

    activity = select(max_sd + 1, &read_fds, NULL, NULL, NULL);

    if (activity < 0 && errno != EINTR) {
      RTSP_LOGE(LOG_TAG, "Select error");
      continue;
    }

    if (FD_ISSET(this->rtspSocket, &read_fds)) {
      if (getActiveRTSPClients() >= currentMaxClients) {
        client_sock = accept(this->rtspSocket, (struct sockaddr *)&clientAddr, &addr_len);
        if (client_sock < 0) {
          RTSP_LOGE(LOG_TAG, "Accept error");
          continue;
        }

        const char* response = "RTSP/1.0 503 Service Unavailable\r\n\r\n";
        write(client_sock, response, strlen(response));
        close(client_sock);
        RTSP_LOGE(LOG_TAG, "Max clients reached. Sent 503 error to new client.");
        continue;
      }

      client_sock = accept(this->rtspSocket, (struct sockaddr *)&clientAddr, &addr_len);
      if (client_sock < 0) {
        RTSP_LOGE(LOG_TAG, "Accept error");
        continue;
      }

      if (!setNonBlocking(client_sock)) {
        RTSP_LOGE(LOG_TAG, "Failed to set RTSP socket to non-blocking mode.");
        close(client_sock);
        continue;
      }

      RTSP_LOGI(LOG_TAG, "New client connected");

      // Create a new session for the new client
      RTSP_Session session = {
        esp_random(),  // sessionID
        client_sock,   // sock
        0,            // cseq
        0,            // cVideoPort
        0,            // cAudioPort
        0,            // cSrtPort
        false,        // isMulticast
        false,        // isPlaying
        false,        // isTCP
        false,        // isHttp
        -1,           // httpSock
        {0}           // sessionCookie (initialized as empty)
      };
      sessions[session.sessionID] = session;

      for (int i = 0; i < currentMaxClients; i++) {
        if (client_sockets[i] == 0) {
          client_sockets[i] = client_sock;
          incrementActiveRTSPClients();
          RTSP_LOGI(LOG_TAG, "Added to list of sockets as %d", i);
          break;
        }
      }
    }

    for (int i = 0; i < currentMaxClients; i++) {
      int sd = client_sockets[i];

      if (FD_ISSET(sd, &read_fds)) {
        // Get the session for this client
        RTSP_Session* session = nullptr;
        for (auto& sess : sessions) {
          if (sess.second.sock == sd) {
            session = &sess.second;
            break;
          }
        }
        if (session) {
          bool keepConnection = handleRTSPRequest(*session);
          if (!keepConnection) {
            if (getActiveRTSPClients() == 1) {
              setIsPlaying(false);
              closeSockets();
              RTSP_LOGD(LOG_TAG, "All clients disconnected. Resetting firstClientConnected flag."); 
              this->firstClientConnected = false; 
              this->firstClientIsMulticast = false; 
              this->firstClientIsTCP = false; 
            }
            close(sd);
            client_sockets[i] = 0;
            sessions.erase(session->sessionID); // Remove session when client disconnects
            decrementActiveRTSPClients();
          }
        }
      }
    }
  }
}

void RTSPServer::rtpAudioITaskWrapper(void* pvParameters) {
  RTSPServer* server = static_cast<RTSPServer*>(pvParameters);
  server->rtpAudioITask();
}

void RTSPServer::logRawAudioData(const uint8_t* data, size_t length) {
  char logBuffer[1024]; // Adjust size as needed
  size_t index = 0;

  for (size_t i = 0; i < length && index < sizeof(logBuffer) - 3; i++) {
      index += snprintf(logBuffer + index, sizeof(logBuffer) - index, "%02X ", data[i]);
  }

  logBuffer[index] = '\0'; // Null-terminate the string
  RTSP_LOGI(LOG_TAG, "Audio Data (Hex): %s", logBuffer);
}

// void RTSPServer::rtpAudioITask() {
//   uint8_t* buffer = (uint8_t*)malloc(1500); // Allocate buffer on the heap
//   int16_t* l16Data = (int16_t*)malloc(2560 * sizeof(int16_t)); // Allocate l16Data on the heap
//   size_t l16Len = 0;

//   if (!buffer || !l16Data) {
//     RTSP_LOGE(LOG_TAG, "Failed to allocate memory for RTP audio task");
//     if (buffer) free(buffer);
//     if (l16Data) free(l16Data);
//     vTaskDelete(NULL); // Terminate the task
//     return;
//   }

//   struct sockaddr_in srcAddr;
//   socklen_t addrLen = sizeof(srcAddr);

//   while (true) {
//     int len = recvfrom(this->audioIUnicastSocket, buffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
//     if (len > 0) {
//       const int rtpHeaderSize = 12; // RTP header size
//       if (len > rtpHeaderSize) {
//         uint8_t* payload = buffer + rtpHeaderSize;
//         int payloadLen = len - rtpHeaderSize;

//         convertG711ToL16Upsampled(payload, payloadLen, l16Data, &l16Len);
//         if (this->audioReceiveCallback) {
//           sendReceivedAudioToMain(l16Data, l16Len, this->audioReceiveCallback);
//         } else {
//           RTSP_LOGE(LOG_TAG, "audioReceiveCallback is null");
//         }
//       } else {
//         RTSP_LOGE(LOG_TAG, "Received packet too small to contain RTP header");
//       }
//     } else {
//       vTaskDelay(10 / portTICK_PERIOD_MS);  // Delay to prevent busy loop
//     }
//   }

//   free(buffer);
//   free(l16Data);
// }


void RTSPServer::rtpAudioITask() {
  uint8_t* rtpBuffer = (uint8_t*)malloc(1500);
  int16_t* micBuffer = (int16_t*)malloc(256 * sizeof(int16_t));
  int16_t* processedBuffer = (int16_t*)malloc(256 * sizeof(int16_t));

  if (!rtpBuffer || !micBuffer || !processedBuffer) {
      RTSP_LOGE(LOG_TAG, "Failed to allocate memory");
      free(rtpBuffer);
      free(micBuffer);
      free(processedBuffer);
      vTaskDelete(NULL);
      return;
  }

  struct sockaddr_in srcAddr;
  socklen_t addrLen = sizeof(srcAddr);

  while (true) {
      int len = recvfrom(this->audioIUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
      if (len > 0) {
          Serial.printf("Raw RTP Packet: %d bytes\n", len);

          ESP32SpeexDSP::RTPPacket rtp;
          if (audioProcessor.parseRTPPacket(rtpBuffer, len, rtp)) {
              Serial.printf("RTP Payload: %d bytes\n", rtp.payloadLen);

              int16_t* pcm8k = (int16_t*)malloc(rtp.payloadLen * sizeof(int16_t));
              audioProcessor.decodeG711(rtp.payload, pcm8k, rtp.payloadLen, true);
              Serial.printf("PCM 8k: %d samples (%d bytes)\n", rtp.payloadLen, rtp.payloadLen * 2);

              int16_t* pcm16k = (int16_t*)malloc(rtp.payloadLen * 2 * sizeof(int16_t));
              int resampledLen = audioProcessor.resample(pcm8k, rtp.payloadLen, pcm16k, rtp.payloadLen * 2);
              Serial.printf("Resampled 16k: %d samples (%d bytes)\n", resampledLen, resampledLen * 2);

              for (int i = 0; i < resampledLen; i += 256) {
                int chunkSize = min(256, resampledLen - i);
                memcpy(processedBuffer, pcm16k + i, chunkSize * sizeof(int16_t));
                Serial.printf("Sending Chunk: %d samples\n", chunkSize);

                // Get speaker data from ring buffer
                int micSamples = audioProcessor.readBuffer(micBuffer, 256);
                if (micSamples < 256) {
                    memset(micBuffer + micSamples, 0, (256 - micSamples) * sizeof(int16_t));
                }
                Serial.printf("Mic Buffer: %d samples\n", micSamples);

                // Apply AEC
                audioProcessor.processAEC(processedBuffer, micBuffer, processedBuffer);

                if (this->audioReceiveCallback) {
                    sendReceivedAudioToMain(processedBuffer, chunkSize, this->audioReceiveCallback);
                }
            }

              // for (int i = 0; i < resampledLen; i += 256) {
              //     int chunkSize = min(256, resampledLen - i);
              //     Serial.printf("Jitter Chunk: %d samples\n", chunkSize);
              //     audioProcessor.putJitterPacket(pcm16k + i, chunkSize, rtp.timestamp);
              // }

              // int jitterOutSamples = audioProcessor.getJitterPacket(processedBuffer, 256);
              // while (jitterOutSamples > 0) {
              //     Serial.printf("Jitter Out: %d samples\n", jitterOutSamples);
              //     int micSamples = audioProcessor.readBuffer(micBuffer, 256);
              //     if (micSamples < 256) {
              //         memset(micBuffer + micSamples, 0, (256 - micSamples) * sizeof(int16_t));
              //     }

              //     audioProcessor.processAEC(processedBuffer, micBuffer, processedBuffer);
              //     audioProcessor.preprocessAudio(processedBuffer);

              //     if (this->audioReceiveCallback) {
              //         sendReceivedAudioToMain(processedBuffer, jitterOutSamples, this->audioReceiveCallback);
              //     }

              //     jitterOutSamples = audioProcessor.getJitterPacket(processedBuffer, 256);
              // }

              free(pcm8k);
              free(pcm16k);
          } else {
              RTSP_LOGE(LOG_TAG, "Failed to parse RTP packet");
          }
      } else {
          vTaskDelay(10 / portTICK_PERIOD_MS);
      }
  }

  free(rtpBuffer);
  free(micBuffer);
  free(processedBuffer);
}

void RTSPServer::setAudioReceiveCallback(void (*callback)(const int16_t*, size_t)) {
    this->audioReceiveCallback = callback;
}

void RTSPServer::sendReceivedAudioToMain(const int16_t* l16Data, size_t len, void (*callback)(const int16_t*, size_t)) {
    if (callback) {
        callback(l16Data, len);
    }
}