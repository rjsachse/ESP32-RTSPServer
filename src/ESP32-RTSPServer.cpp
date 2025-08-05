#include "ESP32-RTSPServer.h"

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
    rtpSubtitlesPort(5434),
    maxRTSPClients(3),
    //
    rtspSocket(-1),
    videoUnicastSocket(-1),
    audioUnicastSocket(-1), 
    subtitlesUnicastSocket(-1),
    videoMulticastSocket(-1),
    audioMulticastSocket(-1),
    subtitlesMulticastSocket(-1),
    activeRTSPClients(0),
    maxClients(1),
    rtpVideoTaskHandle(NULL),
    rtspTaskHandle(NULL),
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
    subtitlesCh(0),
    isVideo(false),
    isAudio(false),
    isSubtitles(false),
    isPlaying(false),
    firstClientConnected(false),
    firstClientIsMulticast(false),
    firstClientIsTCP(false),
    authEnabled(false) // Initialize authEnabled to false
{
    isPlayingMutex = xSemaphoreCreateMutex(); // Initialize the mutex
    sendTcpMutex = xSemaphoreCreateMutex(); // Initialize the mutex
    maxClientsMutex = xSemaphoreCreateMutex();
    sessionsMutex = xSemaphoreCreateMutex();
#ifdef RTSP_LOGGING_ENABLED
    esp_log_level_set(LOG_TAG, ESP_LOG_DEBUG); // Set log level to DEBUG
#endif
}

RTSPServer::~RTSPServer() {
  // Clean up resources
  deinit();
  vSemaphoreDelete(this->isPlayingMutex);
  vSemaphoreDelete(this->sendTcpMutex);
  vSemaphoreDelete(this->maxClientsMutex);
  vSemaphoreDelete(sessionsMutex);
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
  if (this->rtspSocket >= 0) {
    close(this->rtspSocket);
    this->rtspSocket = -1;
  }
  
  closeSockets();
  
  if (this->rtspStreamBuffer) {
    free(this->rtspStreamBuffer);
  }
  if (xSemaphoreTake(sessionsMutex, portMAX_DELAY) == pdTRUE) {
    sessions.clear();
    xSemaphoreGive(sessionsMutex);
  } else {
    RTSP_LOGE(LOG_TAG, "Failed to acquire sessions mutex");
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

void RTSPServer::setClientActivityCallback(ClientActivityCallback callback) {
  clientActivityCallback = callback;
}

void RTSPServer::setupFdSet(fd_set& read_fds, int* client_sockets, int max_clients, int& max_sd) {
  FD_ZERO(&read_fds);
  FD_SET(this->rtspSocket, &read_fds);
  max_sd = this->rtspSocket;

  for (int i = 0; i < max_clients; i++) {
    int sd = client_sockets[i];
    if (sd > 0) {
      FD_SET(sd, &read_fds);
      if (sd > max_sd) max_sd = sd;
    }
  }
}

bool RTSPServer::handleNewClient(int& client_sock, struct sockaddr_in& clientAddr, socklen_t addr_len, int* client_sockets, uint8_t currentMaxClients) {
  client_sock = accept(this->rtspSocket, (struct sockaddr *)&clientAddr, &addr_len);
  if (client_sock < 0) {
    RTSP_LOGE(LOG_TAG, "Accept error");
    return false;
  }

  // Get client IP and port
  char clientIp[INET_ADDRSTRLEN];
  uint16_t clientPort;
  getClientAddress(clientAddr, clientIp, INET_ADDRSTRLEN, clientPort);

  if (getActiveRTSPClients() >= currentMaxClients) {
    const char* response = "RTSP/1.0 503 Service Unavailable\r\n\r\n";
    write(client_sock, response, strlen(response));
    close(client_sock);
    RTSP_LOGE(LOG_TAG, "Max clients reached. Sent 503 error to new client.");

    if (clientActivityCallback) {
      clientActivityCallback(ClientActivityType::REFUSED_MAX_CLIENTS, clientIp, clientPort, getActiveRTSPClients());
    }
    return false;
  }

  if (!setNonBlocking(client_sock)) {
    RTSP_LOGE(LOG_TAG, "Failed to set RTSP socket to non-blocking mode.");
    close(client_sock);
    return false;
  }

  RTSP_LOGI(LOG_TAG, "New client connected");

  // Create a new session for the client
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
    {0}           // sessionCookie
  };
  if (xSemaphoreTake(sessionsMutex, portMAX_DELAY) == pdTRUE) {
    sessions[session.sessionID] = session;
    xSemaphoreGive(sessionsMutex);
  } else {
    RTSP_LOGE(LOG_TAG, "Failed to acquire sessions mutex");
    close(client_sock);
    return false;
  }

  for (int i = 0; i < currentMaxClients; i++) {
    if (client_sockets[i] == 0) {
      client_sockets[i] = client_sock;
      incrementActiveRTSPClients();
      RTSP_LOGI(LOG_TAG, "Added to list of sockets as %d", i);

      if (clientActivityCallback) {
        clientActivityCallback(ClientActivityType::CONNECTED, clientIp, clientPort, getActiveRTSPClients());
      }
      return true;
    }
  }
  return false;
}

void RTSPServer::handleExistingClients(fd_set& read_fds, int* client_sockets, uint8_t currentMaxClients) {
  for (int i = 0; i < currentMaxClients; i++) {
    int sd = client_sockets[i];
    if (sd > 0 && FD_ISSET(sd, &read_fds)) {
      RTSP_Session* session = nullptr;
      if (xSemaphoreTake(sessionsMutex, portMAX_DELAY) == pdTRUE) {
        for (auto& sess : sessions) {
          if (sess.second.sock == sd) {
            session = &sess.second;
            break;
          }
        }
        xSemaphoreGive(sessionsMutex);
      } else {
        RTSP_LOGE(LOG_TAG, "Failed to acquire sessions mutex");
        continue;
      }
      if (session) {
        bool keepConnection = handleRTSPRequest(*session);
        if (!keepConnection) {
          // Get client IP and port
          char clientIp[INET_ADDRSTRLEN];
          uint16_t clientPort = 0;
          struct sockaddr_in clientAddr;
          socklen_t addr_len = sizeof(clientAddr);
          if (getpeername(sd, (struct sockaddr*)&clientAddr, &addr_len) == 0) {
            getClientAddress(clientAddr, clientIp, INET_ADDRSTRLEN, clientPort);
          } else {
            strcpy(clientIp, "unknown");
          }

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
          uint8_t activeClients = getActiveRTSPClients();
          decrementActiveRTSPClients();

          if (clientActivityCallback) {
            clientActivityCallback(ClientActivityType::DISCONNECTED, clientIp, clientPort, activeClients);
          }
          if (xSemaphoreTake(sessionsMutex, portMAX_DELAY) == pdTRUE) {
            sessions.erase(session->sessionID);
            xSemaphoreGive(sessionsMutex);
          } else {
            RTSP_LOGE(LOG_TAG, "Failed to acquire sessions mutex");
          }
        }
      }
    }
  }
}

void RTSPServer::rtspTask() {
  struct sockaddr_in clientAddr;
  socklen_t addr_len = sizeof(clientAddr);
  fd_set read_fds;
  int client_sockets[MAX_CLIENTS] = {0};
  int max_sd, activity;

  while (true) {
    setupFdSet(read_fds, client_sockets, getMaxClients(), max_sd);

    activity = select(max_sd + 1, &read_fds, NULL, NULL, NULL);
    if (activity < 0 && errno != EINTR) {
      RTSP_LOGE(LOG_TAG, "Select error");
      continue;
    }

    if (FD_ISSET(this->rtspSocket, &read_fds)) {
      int client_sock;
      handleNewClient(client_sock, clientAddr, addr_len, client_sockets, getMaxClients());
    }

    handleExistingClients(read_fds, client_sockets, getMaxClients());
  }
}
