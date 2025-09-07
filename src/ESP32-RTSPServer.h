#ifndef ESP32_RTSP_SERVER_H
#define ESP32_RTSP_SERVER_H

#include <WiFi.h>
#include "lwip/sockets.h"
#include <esp_log.h>
#include <map>
#include <algorithm>

#ifdef __cplusplus
extern "C" {
#endif
#include "codecs/g7xx/g72x.h"
#ifdef __cplusplus
}
#endif

#define MAX_RTSP_BUFFER (512 * 1024)
#define RTP_STACK_SIZE (1024 * 4)
#define RTP_PRI 10
#define RTSP_STACK_SIZE (1024 * 8)
#define RTSP_PRI 10
#define MAX_CLIENTS 10 // max rtsp clients

#define RTSP_BUFFER_SIZE 8092

// Optionally include RTSPConfig.h if available
#ifdef __has_include
  #if __has_include("RTSPConfig.h")
    #include "RTSPConfig.h"
  #endif
#endif

#ifdef USE_SPEEXDSP
#include <ESP32-SpeexDSP.h>
#endif

// Logging macros
#ifdef RTSP_LOGGING_ENABLED
  #define RTSP_LOGI(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
  #define RTSP_LOGW(tag, format, ...) ESP_LOGW(tag, format, ##__VA_ARGS__)
  #define RTSP_LOGE(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
  #define RTSP_LOGD(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
#else
  #define RTSP_LOGI(tag, format, ...)
  #define RTSP_LOGW(tag, format, ...)
  #define RTSP_LOGE(tag, format, ...)
  #define RTSP_LOGD(tag, format, ...)
#endif

#define MAX_COOKIE_LENGTH 128 // max length of session cookie

// Define client activity types
enum class ClientActivityType {
  CONNECTED,
  DISCONNECTED,
  REFUSED_MAX_CLIENTS
};

// Callback function type for client activity
typedef void (*ClientActivityCallback)(ClientActivityType activity, const char* clientIp, uint16_t clientPort, uint8_t activeClients);

struct RTSP_Session {
  uint32_t sessionID;
  int sock;
  int cseq;
  uint16_t cVideoPort;
  uint16_t cAudioPort;
  uint16_t cSrtPort;
  bool isMulticast;
  bool isPlaying;
  bool isTCP;
  bool isHttp;  // Flag for HTTP tunneling
  int httpSock;  // HTTP socket storage
  char sessionCookie[MAX_COOKIE_LENGTH];  // Storage for session cookie
  uint16_t cAudioIPort;  // Client port for incoming audio
};

class RTSPServer {
public:
  enum TransportType {
    VIDEO_ONLY,
    AUDIO_ONLY,
    SUBTITLES_ONLY,
    VIDEO_AND_AUDIO,
    VIDEO_AND_SUBTITLES,
    AUDIO_AND_SUBTITLES,
    VIDEO_AUDIO_SUBTITLES,
    NONE,
  };

  enum AudioCodec {
    G711_ULAW,  // G.711 μ-law (payload type 0)
    G711_ALAW,  // G.711 A-law (payload type 8)
    L16         // 16-bit PCM, big-endian (payload type 97)
  };

  RTSPServer();  // Defined in ESP32-RTSPServer.cpp
  ~RTSPServer();  // Destructor, defined in ESP32-RTSPServer.cpp

  bool init(TransportType transport = NONE, uint16_t rtspPort = 0, uint32_t sampleRate = 0, uint16_t port1 = 0, uint16_t port2 = 0, uint16_t port3 = 0, IPAddress rtpIp = IPAddress(), uint8_t rtpTTL = 255, AudioCodec outCodec = G711_ULAW, AudioCodec inCodec = G711_ULAW);  // Defined in ESP32-RTSPServer.cpp
  
  void deinit();  // Defined in ESP32-RTSPServer.cpp

  bool reinit();  // Defined in ESP32-RTSPServer.cpp

  void sendRTSPFrame(const uint8_t* data, size_t len, int quality, int width, int height);  // Defined in rtp.cpp

  void sendRTSPAudio(int16_t* data, size_t len);  // Defined in rtp.cpp

  void sendRTSPSubtitles(char* data, size_t len);  // Defined in rtp.cpp

  void startSubtitlesTimer(esp_timer_cb_t userCallback);  // Defined in utils.cpp

  bool readyToSendFrame() const;  // Defined in utils.cpp

  bool readyToSendAudio() const;  // Defined in utils.cpp

  bool readyToSendSubtitles() const;  // Defined in utils.cpp

  bool setCredentials(const char* username, const char* password); // Set credentials for authentication

  void setClientActivityCallback(ClientActivityCallback callback);
  
  // Set callback for audio received from client
  void setAudioReceiveCallback(void (*callback)(const uint8_t*, size_t));

  // G.711 Codec
  void decodeG711(uint8_t* inG711, int16_t* out, int numSamples, bool ulaw = true);
  void encodeG711(int16_t* in, uint8_t* outG711, int numSamples, bool ulaw = true);

  // RTP Parsing
  struct RTPPacket {
      uint8_t version;
      bool padding;
      bool extension;
      uint8_t csrcCount;
      bool marker;
      uint8_t payloadType;
      uint16_t sequenceNumber;
      uint32_t timestamp;
      uint32_t ssrc;
      uint8_t* payload;
      int payloadLen;
  };
  
  bool parseRTPPacket(uint8_t* packet, int packetLen, RTPPacket& rtp);

  float computeRMS(int16_t *data, int len);

  uint32_t rtpFps;
  TransportType transport;
  uint32_t sampleRate;
  int rtspPort;
  IPAddress rtpIp;
  uint8_t rtpTTL;
  uint16_t rtpVideoPort;
  uint16_t rtpAudioPort;
  uint16_t rtpAudioIPort;
  uint16_t rtpSubtitlesPort;
  uint8_t maxRTSPClients;
  AudioCodec audioOutCodec;  // Codec for audio streamed from ESP32 (mic) to client, default L16
  AudioCodec audioInCodec;   // Codec for audio received from client to ESP32 (speaker), default G711_ULAW
#ifdef USE_SPEEXDSP
  ESP32SpeexDSP audioProcessor;  // Only included if USE_SPEEXDSP is defined
#endif

private:
  int rtspSocket;
  int videoUnicastSocket; 
  int audioUnicastSocket; 
  int audioIUnicastSocket; 
  int subtitlesUnicastSocket; 
  int videoMulticastSocket; 
  int audioMulticastSocket; 
  int audioIMulticastSocket;  // Socket for incoming audio (multicast)
  int subtitlesMulticastSocket;
  uint8_t activeRTSPClients; 
  uint8_t maxClients;
  TaskHandle_t rtpVideoTaskHandle;
  TaskHandle_t rtspTaskHandle;
  TaskHandle_t rtpAudioITaskHandle;  // Task handle for incoming audio
  std::map<uint32_t, RTSP_Session> sessions;
  byte* rtspStreamBuffer;
  size_t rtspStreamBufferSize;
  bool rtpFrameSent;
  bool rtpAudioSent;
  bool rtpSubtitlesSent;
  uint8_t vQuality;
  uint16_t vWidth;
  uint16_t vHeight;
  uint16_t videoSequenceNumber;
  uint32_t videoTimestamp;
  uint32_t videoSSRC;
  uint16_t audioSequenceNumber;
  uint32_t audioTimestamp;
  uint32_t audioSSRC;
  uint16_t subtitlesSequenceNumber;
  uint32_t subtitlesTimestamp;
  uint32_t subtitlesSSRC;
  uint32_t rtpFrameCount;
  uint32_t lastRtpFPSUpdateTime;
  uint8_t videoCh;
  uint8_t audioCh;
  uint8_t audioICh;  // Channel for incoming audio
  uint8_t subtitlesCh;
  bool isVideo;
  bool isAudio;
  bool isSubtitles;
  bool isPlaying;
  bool firstClientConnected; 
  bool firstClientIsMulticast; 
  bool firstClientIsTCP;
  bool authEnabled; // Flag for authentication
  char base64Credentials[128]; // Store base64 encoded credentials
  esp_timer_handle_t sendSubtitlesTimer;
  SemaphoreHandle_t isPlayingMutex;  // Mutex for protecting access
  SemaphoreHandle_t sendTcpMutex;  // Mutex for protecting TCP send access
  SemaphoreHandle_t maxClientsMutex; // FreeRTOS mutex for maxClients
  SemaphoreHandle_t sessionsMutex;
  
  ClientActivityCallback clientActivityCallback;

  void closeSockets();  // Defined in ESP32-RTSPServer.cpp
  
  void sendTcpPacket(const uint8_t* packet, size_t packetSize, int sock);  // Defined in network.cpp

  void checkAndSetupUDP(int& rtpSocket, bool isMulticast, uint16_t rtpPort, IPAddress rtpIp = IPAddress());  // Defined in network.cpp

  void sendRtpSubtitles(const char* data, size_t len, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast);  // Defined in rtp.cpp

  void sendRtpAudio(const uint8_t* data, size_t len, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast);  // Defined in rtp.cpp

  void sendRtpFrame(const uint8_t* data, size_t len, uint8_t quality, uint16_t width, uint16_t height, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast);  // Defined in rtp.cpp

  static void rtpVideoTaskWrapper(void* pvParameters);  // Defined in rtp.cpp

  void rtpVideoTask();  // Defined in rtp.cpp

  bool createAudioIn(); // Returns true if task starts, false if it fails
  static void rtpAudioITaskWrapper(void* pvParameters);
  void rtpAudioITask();

  void setMaxClients(uint8_t newMaxClients);  // Defined in utils.cpp

  uint8_t getMaxClients();  // Defined in utils.cpp

  uint8_t getActiveClients();  // Defined in utils.cpp
  
  void incrementActiveRTSPClients();  // Defined in utils.cpp

  void decrementActiveRTSPClients();  // Defined in utils.cpp

  uint8_t getActiveRTSPClients();  // Defined in utils.cpp

  void updateIsPlayingStatus();  // Defined in utils.cpp
  
  void setIsPlaying(bool playing);  // Defined in utils.cpp
  
  bool getIsPlaying() const;  // Defined in utils.cpp

  int captureCSeq(char* request);  // Defined in utils.cpp

  uint32_t generateSessionID();  // Defined in utils.cpp

  uint32_t extractSessionID(char* request);  // Defined in utils.cpp

  const char* dateHeader();  // Defined in utils.cpp

  void handleOptions(char* request, RTSP_Session& session);  // Defined in rtsp_requests.cpp

  void handleDescribe(const RTSP_Session& session);  // Defined in rtsp_requests.cpp

  void handleSetup(char* request, RTSP_Session& session);  // Defined in rtsp_requests.cpp

  void handlePlay(RTSP_Session& session);  // Defined in rtsp_requests.cpp

  void handlePause(RTSP_Session& session);  // Defined in rtsp_requests.cpp

  void handleTeardown(RTSP_Session& session);  // Defined in rtsp_requests.cpp

  bool handleRTSPRequest(RTSP_Session& session);  // Defined in rtsp_requests.cpp

  bool setNonBlocking(int sockfd);  // Defined in network.cpp

  bool prepRTSP();  // Defined in ESP32-RTSPServer.cpp

  static void rtspTaskWrapper(void* pvParameters);  // Defined in ESP32-RTSPServer.cpp

  void rtspTask();  // Defined in ESP32-RTSPServer.cpp

  static const char* LOG_TAG;  // Define a log tag for the class

  void sendUnauthorizedResponse(RTSP_Session& session); // Send 401 Unauthorized response
  void extractSessionCookie(const char* buffer, char* sessionCookie, size_t maxLen);
  bool isBase64Encoded(const char* buffer, size_t length);
  bool handleRTSPCommand(char* command, RTSP_Session& session);
  bool decodeBase64(const char* input, size_t inputLen, char* output, size_t* outputLen);
  void wrapInHTTP(char* buffer, size_t len, char* response, size_t maxLen);
  RTSP_Session* findSessionByCookie(const char* cookie);
  
  void getClientAddress(const struct sockaddr_in& clientAddr, char* ipBuffer, size_t ipBufferSize, uint16_t& port);
  void setupFdSet(fd_set& read_fds, int* client_sockets, int max_clients, int& max_sd);
  bool handleNewClient(int& client_sock, struct sockaddr_in& clientAddr, socklen_t addr_len, int* client_sockets, uint8_t currentMaxClients);
  void handleExistingClients(fd_set& read_fds, int* client_sockets, uint8_t currentMaxClients);
  
  void logRawAudioData(const uint8_t* data, size_t length);
  void logAudioSamples(const char* label, int16_t* buffer, size_t len, size_t maxSamples);
  void sendReceivedAudioToMain(const uint8_t* l16Data, size_t len, void (*callback)(const uint8_t*, size_t));
  void (*audioReceiveCallback)(const uint8_t*, size_t) = nullptr; // Callback for received audio
};

#endif // ESP32_RTSP_SERVER_H