#include "ESP32-RTSPServer.h"
//#include "audioConverter.h"

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
    rtpAudioITaskHandle(NULL),
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
    authEnabled(false), // Reordered initialization
    audioOutCodec(L16),      // Default output: L16
    audioInCodec(G711_ULAW) // Default input: G.711 μ-law
{
    isPlayingMutex = xSemaphoreCreateMutex(); // Initialize the mutex
    sendTcpMutex = xSemaphoreCreateMutex(); // Initialize the mutex
    maxClientsMutex = xSemaphoreCreateMutex();
#ifdef RTSP_LOGGING_ENABLED
    esp_log_level_set(LOG_TAG, ESP_LOG_DEBUG); // Set log level to DEBUG
#endif
#ifdef USE_SPEEXDSP
  // Init SpeexDSP with all features
  audioProcessor.beginAEC(128, 256, 8000);
  audioProcessor.enableAEC(true);
  audioProcessor.beginResampler(8000, 16000, 5);
  audioProcessor.beginBuffer(2024);

  // Mic preprocessing
  audioProcessor.beginMicPreprocess(128, 8000);
  audioProcessor.enableMicNoiseSuppression(true); // NS on for mic
  audioProcessor.setMicNoiseSuppressionLevel(-20);
  audioProcessor.enableMicAGC(true, 0.75f); // AGC on for mic
  audioProcessor.enableMicVAD(false); // VAD for mic
  audioProcessor.setMicVADThreshold(80);

  // Speaker preprocessing
  audioProcessor.beginSpeakerPreprocess(128, 8000);
  audioProcessor.enableSpeakerNoiseSuppression(false); // NS off for speaker
  audioProcessor.enableSpeakerAGC(true, 0.75f); // AGC off for speaker
#endif
}

RTSPServer::~RTSPServer() {
  // Clean up resources
  deinit();
  vSemaphoreDelete(this->isPlayingMutex);
  vSemaphoreDelete(this->sendTcpMutex);
  vSemaphoreDelete(this->maxClientsMutex);
}

bool RTSPServer::init(TransportType transport, uint16_t rtspPort, uint32_t sampleRate, uint16_t port1, uint16_t port2, uint16_t port3, IPAddress rtpIp, uint8_t rtpTTL, AudioCodec outCodec, AudioCodec inCodec) {
  this->transport = (transport != NONE) ? transport : this->transport;
  this->rtspPort = (rtspPort != 0) ? rtspPort : this->rtspPort;
  this->rtpIp = (rtpIp != IPAddress()) ? rtpIp : this->rtpIp;
  this->rtpTTL = (rtpTTL != 255) ? rtpTTL : this->rtpTTL;
  this->audioOutCodec = outCodec;
  this->audioInCodec = inCodec;

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
#ifdef USE_SPEEXDSP
    audioProcessor.beginAEC(128, 256, this->sampleRate);
    audioProcessor.beginResampler(this->sampleRate, 16000, 5);
    audioProcessor.beginMicPreprocess(128, this->sampleRate);
    audioProcessor.beginSpeakerPreprocess(128, this->sampleRate);
#endif
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
        {0},           // sessionCookie (initialized as empty)
        0
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



void RTSPServer::logRawAudioData(const uint8_t* data, size_t length) {
  char logBuffer[1024]; // Adjust size as needed
  size_t index = 0;

  for (size_t i = 0; i < length && index < sizeof(logBuffer) - 3; i++) {
      index += snprintf(logBuffer + index, sizeof(logBuffer) - index, "%02X ", data[i]);
  }

  logBuffer[index] = '\0'; // Null-terminate the string
  RTSP_LOGI(LOG_TAG, "Audio Data (Hex): %s", logBuffer);
}


// Helper function to log audio samples
void RTSPServer::logAudioSamples(const char* label, int16_t* buffer, size_t len, size_t maxSamples) {
  size_t count = std::min(len, maxSamples);
  char logStr[128];
  snprintf(logStr, sizeof(logStr), "%s: ", label);
  for (size_t i = 0; i < count; i++) {
    char temp[16];
    snprintf(temp, sizeof(temp), "%d ", buffer[i]);
    strncat(logStr, temp, sizeof(logStr) - strlen(logStr) - 1);
  }
  RTSP_LOGI(LOG_TAG, "%s", logStr);
}
// void RTSPServer::rtpAudioITask() {
//   uint8_t* rtpBuffer = (uint8_t*)malloc(1500);           // Buffer for incoming RTP packets
//   int16_t* processedBuffer = (int16_t*)malloc(256 * sizeof(int16_t)); // Buffer for processed audio

//   if (!rtpBuffer || !processedBuffer) {
//     RTSP_LOGE(LOG_TAG, "Failed to allocate memory");
//     free(rtpBuffer); free(processedBuffer);
//     vTaskDelete(NULL);
//     return;
//   }

//   struct sockaddr_in srcAddr;
//   socklen_t addrLen = sizeof(srcAddr);

//   while (true) {
//     int len = recvfrom(this->audioIUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
//     if (len > 0) {
//       ESP32SpeexDSP::RTPPacket rtp;
//       if (audioProcessor.parseRTPPacket(rtpBuffer, len, rtp)) {
//         int16_t* pcm8k = (int16_t*)malloc(rtp.payloadLen * sizeof(int16_t));
//         audioProcessor.decodeG711(rtp.payload, pcm8k, rtp.payloadLen, true); // Decode G.711 to PCM (μ-law)
//         int16_t* pcm16k = (int16_t*)malloc(rtp.payloadLen * 2 * sizeof(int16_t));
//         int resampledLen = audioProcessor.resample(pcm8k, rtp.payloadLen, pcm16k, rtp.payloadLen * 2); // Resample to 16 kHz

//         for (int i = 0; i < resampledLen; i += 256) {
//           int chunkSize = min(256, resampledLen - i); // chunkSize in samples
//           memcpy(processedBuffer, pcm16k + i, chunkSize * sizeof(int16_t));

//           // Speaker-specific preprocessing (NS/AGC if enabled)
//           audioProcessor.preprocessSpeakerAudio(processedBuffer);

//           // Store for AEC reference
//           audioProcessor.writeBuffer(processedBuffer, chunkSize);

//           // Send to speaker as uint8_t* with length in bytes
//           if (this->audioReceiveCallback) {
//             size_t byteLen = chunkSize * sizeof(int16_t); // Convert samples to bytes
//             sendReceivedAudioToMain((uint8_t*)processedBuffer, byteLen, this->audioReceiveCallback);
//           }
//         }
//         free(pcm8k);
//         free(pcm16k);
//       } else {
//         RTSP_LOGE(LOG_TAG, "Failed to parse RTP packet");
//       }
//     } else {
//       vTaskDelay(10 / portTICK_PERIOD_MS);
//     }
//   }
//   free(rtpBuffer);
//   free(processedBuffer);
// }

bool RTSPServer::createAudioIn() {
  if (this->rtpAudioITaskHandle == NULL) {

    BaseType_t result = xTaskCreate(RTSPServer::rtpAudioITaskWrapper, "rtpAudioITask", RTP_STACK_SIZE, this, RTP_PRI, &this->rtpAudioITaskHandle);
    if (result == pdPASS) {
      RTSP_LOGI(LOG_TAG, "Task created successfully, handle: %p", this->rtpAudioITaskHandle);
      return true;
    } else {
      RTSP_LOGE(LOG_TAG, "Failed to create task, result: %d, stack size: %d, priority: %d", result, RTP_STACK_SIZE, RTP_PRI);
      RTSP_LOGI(LOG_TAG, "Free heap after failure: %u bytes", esp_get_free_heap_size());
      return false;
    }
  } else {
    RTSP_LOGW(LOG_TAG, "Incoming audio task already running, handle: %p", this->rtpAudioITaskHandle);
    return true; // Task exists, consider it "successful"
  }
}

void RTSPServer::rtpAudioITaskWrapper(void* pvParameters) {
  RTSP_LOGI(LOG_TAG, "Audio in task wrapper started");
  RTSPServer* server = static_cast<RTSPServer*>(pvParameters);
  server->rtpAudioITask();
  RTSP_LOGI(LOG_TAG, "Audio in task wrapper exiting"); // Shouldn’t reach here
  vTaskDelete(NULL);
}

// void RTSPServer::rtpAudioITask() {
//   RTSP_LOGI(LOG_TAG, "Audio in task started");

//   uint8_t* rtpBuffer = (uint8_t*)malloc(1500); // Buffer for incoming RTP packets
//   if (!rtpBuffer) {
//     RTSP_LOGE(LOG_TAG, "Failed to allocate memory for rtpBuffer: %u bytes", 1500);
//     vTaskDelete(NULL); // No free() needed, malloc failed
//     return;
//   }
//   RTSP_LOGI(LOG_TAG, "Allocated rtpBuffer of size: %zu", 1500);
//   RTSP_LOGI(LOG_TAG, "AudioIUnicastSocket: %d", this->audioIUnicastSocket);

//   struct sockaddr_in srcAddr;
//   socklen_t addrLen = sizeof(srcAddr);

//   while (true) {
//     int len = recvfrom(this->audioIUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
//     if (len > 0) {
//       RTSP_LOGI(LOG_TAG, "Received RTP packet of length: %d", len);

//       RTPPacket rtp;
//       if (parseRTPPacket(rtpBuffer, len, rtp)) {
//         RTSP_LOGI(LOG_TAG, "Parsed RTP: payloadLen=%d, timestamp=%u", rtp.payloadLen, rtp.timestamp);

//         size_t inputSamples = rtp.payloadLen;
//         int16_t* pcm8k = (int16_t*)malloc(inputSamples * sizeof(int16_t));
//         if (!pcm8k) {
//           RTSP_LOGE(LOG_TAG, "Failed to allocate pcm8k");
//           free(pcm8k);
//           continue;
//         }
//         decodeG711(rtp.payload, pcm8k, inputSamples, true);
//         RTSP_LOGI(LOG_TAG, "Decoded G.711: %d samples", inputSamples);

//         size_t maxOutputSamples = inputSamples * 2;
//         int16_t* pcm16k = (int16_t*)malloc(maxOutputSamples * sizeof(int16_t));
//         if (!pcm16k) {
//           RTSP_LOGE(LOG_TAG, "Failed to allocate pcm16k");
//           free(pcm8k);
//           free(pcm16k);
//           continue;
//         }
//         int resampledLen = audioProcessor.resample(pcm8k, inputSamples, pcm16k, maxOutputSamples);
//         RTSP_LOGI(LOG_TAG, "Resampled to 16 kHz: %d samples", resampledLen);

//         size_t chunkSizeBytes = 1024;
//         size_t chunkSizeSamples = chunkSizeBytes / sizeof(int16_t); // 512 samples
//         int16_t processedBuffer[512];

//         for (size_t i = 0; i < resampledLen; i += chunkSizeSamples) {
//           size_t samplesToProcess = std::min(chunkSizeSamples, resampledLen - i);
//           size_t bytesToProcess = samplesToProcess * sizeof(int16_t);

//           memcpy(processedBuffer, pcm16k + i, bytesToProcess);
//           audioProcessor.preprocessSpeakerAudio(processedBuffer);
//           audioProcessor.writeBuffer(processedBuffer, samplesToProcess);

//           if (this->audioReceiveCallback) {
//             RTSP_LOGI(LOG_TAG, "Sending to callback: %d bytes", bytesToProcess);
//             sendReceivedAudioToMain((uint8_t*)processedBuffer, bytesToProcess, this->audioReceiveCallback);
//           } else {
//             RTSP_LOGE(LOG_TAG, "No audioReceiveCallback set");
//           }
//         }

//         free(pcm8k);
//         free(pcm16k);
//       } else {
//         RTSP_LOGE(LOG_TAG, "Failed to parse RTP packet");
//       }
//     } else if (len == 0) {
//       RTSP_LOGI(LOG_TAG, "Received empty packet");
//       vTaskDelay(10 / portTICK_PERIOD_MS);
//     } else {
//       //RTSP_LOGE(LOG_TAG, "recvfrom failed: %d", len);
//       vTaskDelay(10 / portTICK_PERIOD_MS);
//     }
//   }
//   free(rtpBuffer);
//   RTSP_LOGI(LOG_TAG, "Audio in task exiting"); // Shouldn’t reach here
// }

// Receive and process incoming audio (Updated)
// void RTSPServer::rtpAudioITask() {
//   RTSP_LOGI(LOG_TAG, "Audio in task started");
//   uint8_t* rtpBuffer = (uint8_t*)malloc(1500);
//   if (!rtpBuffer) {
//     RTSP_LOGE(LOG_TAG, "Failed to allocate rtpBuffer");
//     return;
//   }

//   struct sockaddr_in srcAddr;
//   socklen_t addrLen = sizeof(srcAddr);

//   while (true) {
//     int len = recvfrom(this->audioIUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
//     if (len > 0) {
//       RTPPacket rtp;
//       if (parseRTPPacket(rtpBuffer, len, rtp)) {
//         size_t numSamples = (audioInCodec == L16) ? rtp.payloadLen / 2 : rtp.payloadLen;
//         int16_t* pcm = (int16_t*)malloc(numSamples * sizeof(int16_t));
//         if (!pcm) {
//           RTSP_LOGE(LOG_TAG, "Failed to allocate pcm buffer");
//           free(pcm);
//           continue;
//         }

//         if (audioInCodec == G711_ULAW || audioInCodec == G711_ALAW) {
//           decodeG711(rtp.payload, pcm, numSamples, audioInCodec == G711_ULAW);
//         } else {  // L16
//           for (size_t i = 0; i < numSamples; i++) {
//             pcm[i] = (int16_t)((rtp.payload[i * 2] << 8) | rtp.payload[i * 2 + 1]);
//           }
//         }

// #ifdef USE_SPEEXDSP
//         audioProcessor.preprocessSpeakerAudio(pcm);
//         audioProcessor.writeBuffer(pcm, numSamples);
// #endif

//         if (this->audioReceiveCallback) {
//           sendReceivedAudioToMain((uint8_t*)pcm, numSamples * sizeof(int16_t), this->audioReceiveCallback);
//         }

//         free(pcm);
//       } else {
//         RTSP_LOGE(LOG_TAG, "Failed to parse RTP packet");
//       }
//     } else {
//       vTaskDelay(10 / portTICK_PERIOD_MS);
//     }
//   }
//   free(rtpBuffer);
// }

void RTSPServer::rtpAudioITask() {
  RTSP_LOGI(LOG_TAG, "Audio in task started");
  uint8_t* rtpBuffer = (uint8_t*)malloc(1500);
  if (!rtpBuffer) {
    RTSP_LOGE(LOG_TAG, "Failed to allocate rtpBuffer");
    return;
  }

  struct sockaddr_in srcAddr;
  socklen_t addrLen = sizeof(srcAddr);

  while (true) {
    // Use select() to monitor both sockets
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(this->audioIUnicastSocket, &readfds);
    FD_SET(this->audioUnicastSocket, &readfds);
    int max_fd = max(this->audioIUnicastSocket, this->audioUnicastSocket) + 1;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;  // 10ms timeout

    int ready = select(max_fd, &readfds, NULL, NULL, &tv);
    if (ready < 0) {
      RTSP_LOGE(LOG_TAG, "select() failed: %d", errno);
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    if (ready == 0) {
      // No data, short delay
      vTaskDelay(1 / portTICK_PERIOD_MS);
      continue;
    }

    // Check which socket has data
    int len = -1;
    int activeSocket = -1;
    if (FD_ISSET(this->audioIUnicastSocket, &readfds)) {
      len = recvfrom(this->audioIUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
      activeSocket = this->audioIUnicastSocket;
    } else if (FD_ISSET(this->audioUnicastSocket, &readfds)) {
      len = recvfrom(this->audioUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
      activeSocket = this->audioUnicastSocket;
    }

    if (len > 0) {
      RTPPacket rtp;
      if (parseRTPPacket(rtpBuffer, len, rtp)) {
        size_t numSamples = (audioInCodec == L16) ? rtp.payloadLen / 2 : rtp.payloadLen;
        int16_t* pcm = (int16_t*)malloc(numSamples * sizeof(int16_t));
        if (!pcm) {
          RTSP_LOGE(LOG_TAG, "Failed to allocate pcm buffer");
          free(pcm);
          continue;
        }

        // Decode based on input codec
        if (audioInCodec == G711_ULAW || audioInCodec == G711_ALAW) {
          decodeG711(rtp.payload, pcm, numSamples, audioInCodec == G711_ULAW);
        } else {  // L16
          for (size_t i = 0; i < numSamples; i++) {
            pcm[i] = (int16_t)((rtp.payload[i * 2] << 8) | rtp.payload[i * 2 + 1]);
          }
        }

#ifdef USE_SPEEXDSP
        audioProcessor.preprocessSpeakerAudio(pcm);
        audioProcessor.writeBuffer(pcm, numSamples);
#endif

        // Send to callback with source port info for debugging
        if (this->audioReceiveCallback) {
          //RTSP_LOGD(LOG_TAG, "Received audio from port %d", activeSocket == this->audioIUnicastSocket ? rtpAudioIPort : rtpAudioPort);
          sendReceivedAudioToMain((uint8_t*)pcm, numSamples * sizeof(int16_t), this->audioReceiveCallback);
        }

        free(pcm);
      } else {
        RTSP_LOGE(LOG_TAG, "Failed to parse RTP packet from port %d", 
                  activeSocket == this->audioIUnicastSocket ? rtpAudioIPort : rtpAudioPort);
      }
    } else if (len < 0) {
      RTSP_LOGE(LOG_TAG, "recvfrom failed on port %d: %d", 
                activeSocket == this->audioIUnicastSocket ? rtpAudioIPort : rtpAudioPort, errno);
    }
  }

  free(rtpBuffer);
}

// void RTSPServer::rtpAudioITask() {
//   RTSP_LOGI(LOG_TAG, "Audio in task started");
//   static uint8_t rtpBuffer[1500];
//   static int16_t pcm[256];
//   struct sockaddr_in srcAddr;
//   socklen_t addrLen = sizeof(srcAddr);

//   while (true) {
//     // Validate sockets
//     if (audioIUnicastSocket < 0 && audioUnicastSocket < 0) {
//       RTSP_LOGE(LOG_TAG, "No valid sockets");
//       vTaskDelay(100 / portTICK_PERIOD_MS);
//       continue;
//     }

//     // Use select() to monitor sockets
//     fd_set readfds;
//     FD_ZERO(&readfds);
//     int max_fd = -1;
//     if (audioIUnicastSocket >= 0) {
//       FD_SET(audioIUnicastSocket, &readfds);
//       max_fd = audioIUnicastSocket;
//     }
//     if (audioUnicastSocket >= 0) {
//       FD_SET(audioUnicastSocket, &readfds);
//       max_fd = max(max_fd, audioUnicastSocket);
//     }

//     struct timeval tv;
//     tv.tv_sec = 0;
//     tv.tv_usec = 10000; // 10ms timeout

//     int ready = select(max_fd + 1, &readfds, NULL, NULL, &tv);
//     if (ready < 0) {
//       RTSP_LOGE(LOG_TAG, "select() failed: %d", errno);
//       vTaskDelay(10 / portTICK_PERIOD_MS);
//       continue;
//     }
//     if (ready == 0) {
//       vTaskDelay(1 / portTICK_PERIOD_MS);
//       continue;
//     }

//     // Check which socket has data
//     int len = -1;
//     int activeSocket = -1;
//     if (audioIUnicastSocket >= 0 && FD_ISSET(audioIUnicastSocket, &readfds)) {
//       len = recvfrom(audioIUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
//       activeSocket = audioIUnicastSocket;
//     } else if (audioUnicastSocket >= 0 && FD_ISSET(audioUnicastSocket, &readfds)) {
//       len = recvfrom(audioUnicastSocket, rtpBuffer, 1500, 0, (struct sockaddr*)&srcAddr, &addrLen);
//       activeSocket = audioUnicastSocket;
//     }

//     if (len > 0) {
//       RTPPacket rtp;
//       if (parseRTPPacket(rtpBuffer, len, rtp)) {
//         size_t numSamples = (audioInCodec == L16) ? rtp.payloadLen / 2 : rtp.payloadLen;

//         // Decode based on input codec
//         if (audioInCodec == G711_ULAW || audioInCodec == G711_ALAW) {
//           decodeG711(rtp.payload, pcm, numSamples, audioInCodec == G711_ULAW);
//         } else { // L16
//           for (size_t i = 0; i < numSamples; i++) {
//             pcm[i] = (int16_t)((rtp.payload[i * 2] << 8) | rtp.payload[i * 2 + 1]);
//           }
//         }

// #ifdef USE_SPEEXDSP
//         audioProcessor.preprocessSpeakerAudio(pcm);
//         audioProcessor.writeBuffer(pcm, numSamples);
// #endif

//         // Send to callback
//         if (audioReceiveCallback) {
//           RTSP_LOGD(LOG_TAG, "Received audio from port %d", activeSocket == audioIUnicastSocket ? rtpAudioIPort : rtpAudioPort);
//           sendReceivedAudioToMain((uint8_t*)pcm, numSamples * sizeof(int16_t), audioReceiveCallback);
//         }
//       } else {
//         RTSP_LOGE(LOG_TAG, "Failed to parse RTP packet from port %d", 
//                   activeSocket == audioIUnicastSocket ? rtpAudioIPort : rtpAudioPort);
//       }
//     } else if (len < 0) {
//       RTSP_LOGE(LOG_TAG, "recvfrom failed on port %d: %d", 
//                 activeSocket == audioIUnicastSocket ? rtpAudioIPort : rtpAudioPort, errno);
//     }
//     vTaskDelay(1 / portTICK_PERIOD_MS);
//   }
// }

void RTSPServer::setAudioReceiveCallback(void (*callback)(const uint8_t*, size_t)) {
    this->audioReceiveCallback = callback;
}

void RTSPServer::sendReceivedAudioToMain(const uint8_t* l16Data, size_t len, void (*callback)(const uint8_t*, size_t)) {
    if (callback) {
        callback(l16Data, len);
    }
}

// G.711 Codec (unchanged)
void RTSPServer::decodeG711(uint8_t* inG711, int16_t* out, int numSamples, bool ulaw) {
  for (int i = 0; i < numSamples; i++) {
      out[i] = ulaw ? ulaw2linear(inG711[i]) : alaw2linear(inG711[i]);
  }
}

void RTSPServer::encodeG711(int16_t* in, uint8_t* outG711, int numSamples, bool ulaw) {
  for (int i = 0; i < numSamples; i++) {
      outG711[i] = ulaw ? linear2ulaw(in[i]) : linear2alaw(in[i]);
  }
}

// RTP Parsing (unchanged)
bool RTSPServer::parseRTPPacket(uint8_t* packet, int packetLen, RTPPacket& rtp) {
  if (!packet || packetLen < 12) return false;

  rtp.version = (packet[0] >> 6) & 0x03;
  rtp.padding = (packet[0] & 0x20) != 0;
  rtp.extension = (packet[0] & 0x10) != 0;
  rtp.csrcCount = packet[0] & 0x0F;
  rtp.marker = (packet[1] & 0x80) != 0;
  rtp.payloadType = packet[1] & 0x7F;
  rtp.sequenceNumber = (packet[2] << 8) | packet[3];
  rtp.timestamp = (packet[4] << 24) | (packet[5] << 16) | (packet[6] << 8) | packet[7];
  rtp.ssrc = (packet[8] << 24) | (packet[9] << 16) | (packet[10] << 8) | packet[11];

  int headerLen = 12 + (rtp.csrcCount * 4);
  if (rtp.extension) {
      if (packetLen < headerLen + 4) return false;
      headerLen += 4 + ((packet[headerLen + 2] << 8) | packet[headerLen + 3]) * 4;
  }

  if (packetLen < headerLen) return false;
  rtp.payload = packet + headerLen;
  rtp.payloadLen = packetLen - headerLen;

  return rtp.version == 2;
}

// Utility
float RTSPServer::computeRMS(int16_t *data, int len) {
  if (len <= 0) return 0.0f;
  float sum = 0.0f;
  for (int i = 0; i < len; i++) {
      float sample = (float)data[i] / 32768.0f;
      sum += sample * sample;
  }
  return sqrtf(sum / len);
}