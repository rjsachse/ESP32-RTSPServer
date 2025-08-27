#include "ESP32-RTSPServer.h"

void RTSPServer::rtpVideoTaskWrapper(void* pvParameters) {
  RTSPServer* server = static_cast<RTSPServer*>(pvParameters);
  server->rtpVideoTask();
}

void RTSPServer::rtpVideoTask() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool multicastSent = false;
    for (const auto& sessionPair : this->sessions) {
      const RTSP_Session& session = sessionPair.second; 
      if (session.isPlaying) {
        if (session.isMulticast) {
          if (!multicastSent) {
            this->sendRtpFrame(this->rtspStreamBuffer, this->rtspStreamBufferSize, this->vQuality, this->vWidth, this->vHeight, session.sock, this->rtpVideoPort, false, true);
            multicastSent = true;
          }
        } else {
          this->sendRtpFrame(this->rtspStreamBuffer, this->rtspStreamBufferSize, this->vQuality, this->vWidth, this->vHeight,  session.isHttp ? session.httpSock : session.sock, session.cVideoPort, session.isTCP, false);
        }
      }
    }
    this->rtspStreamBufferSize = 0;
    this->rtpFrameSent = true;
  }
  vTaskDelete(NULL);
}

void RTSPServer::sendRTSPFrame(const uint8_t* data, size_t len, int quality, int width, int height) {
  this->rtpFrameSent = false;
  static uint32_t lastSendTime = millis(); // Track the last time a frame was sent
  uint32_t currentTime = millis(); // Get the current time in milliseconds

  // Calculate the actual time elapsed since the last frame was sent
  uint32_t actualElapsedTime = currentTime - lastSendTime;
  // Increment the timestamp based on the actual elapsed time
  this->videoTimestamp += (actualElapsedTime * 90000) / 1000;   // Convert milliseconds to 90kHz units

  // Work out the RTP sent FPS to use for subtitles
  this->rtpFrameCount++; 
  // Update FPS every second 
  if (currentTime - this->lastRtpFPSUpdateTime >= 1000) { 
    this->rtpFps = this->rtpFrameCount; // Store the current FPS 
    this->rtpFrameCount = 0; // Reset the frame count for the next second 
    this->lastRtpFPSUpdateTime = currentTime; // Update the last FPS update time 
  }
#ifdef RTSP_VIDEO_NONBLOCK
  this->vQuality = quality;
  this->vWidth = width;
  this->vHeight = height;
  if (!this->rtspStreamBufferSize && this->rtspStreamBuffer != NULL) {
    memcpy(this->rtspStreamBuffer, data, len);
    this->rtspStreamBufferSize = len;
    xTaskNotifyGive(rtpVideoTaskHandle);
  }
#else
  bool multicastSent = false;
  for (const auto& sessionPair : this->sessions) {
    const RTSP_Session& session = sessionPair.second; 
    if (session.isPlaying) {
      if (session.isMulticast) {
        if (!multicastSent) { 
          sendRtpFrame(data, len, quality, width, height, session.sock, this->rtpVideoPort, false, true); 
          multicastSent = true; 
        }
      } else {
        sendRtpFrame(data, len, quality, width, height,  session.isHttp ? session.httpSock : session.sock, session.cVideoPort, session.isTCP, false);
      }
    }
  }
  this->rtpFrameSent = true;
  lastSendTime = currentTime;
#endif
}

//void RTSPServer::sendRTSPAudio(int16_t* data, size_t len) {
//  this->rtpAudioSent = false;
//  bool multicastSent = false;
//  for (const auto& sessionPair : this->sessions) {
//    const RTSP_Session& session = sessionPair.second; 
//    if (session.isPlaying) {
//      if (session.isMulticast) {
//        if (!multicastSent) {
//          this->sendRtpAudio(data, len, session.sock, this->rtpAudioPort, false, true);
//          multicastSent = true;
//        }
//      } else {
//        this->sendRtpAudio(data, len,  session.isHttp ? session.httpSock : session.sock, session.cAudioPort, session.isTCP, false);
//      }
//    }
//  }
//  this->rtpAudioSent = true;
//}

// void RTSPServer::sendRTSPAudio(int16_t* data, size_t len) {
//   // len = 1024 bytes (512 samples at 16-bit)
//   size_t numSamples = len / sizeof(int16_t); // Convert bytes to samples for processing

//   // Buffers sized for full input (assuming len <= 1024 bytes)
//   int16_t out[512];    // Max 512 samples (1024 bytes)
//   int16_t speaker[512]; // Max 512 samples (1024 bytes)

//   // Speaker reference for AEC
//   int speakerSamples = audioProcessor.readBuffer(speaker, numSamples);
//   if (speakerSamples < numSamples) {
//     memset(speaker + speakerSamples, 0, (numSamples - speakerSamples) * sizeof(int16_t));
//   }

//   // AEC + Mic-specific preprocessing on full buffer
//   audioProcessor.processAEC(data, speaker, out);
//   audioProcessor.preprocessMicAudio(out); // NS/AGC/VAD for mic only

//   // Optional VAD for bandwidth (disabled by default with || true)
//   if (audioProcessor.isMicVoiceDetected() || true) {
//     this->rtpAudioSent = false;
//     bool multicastSent = false;
//     for (const auto& sessionPair : this->sessions) {
//       const RTSP_Session& session = sessionPair.second;
//       if (session.isPlaying) {
//         if (session.isMulticast) {
//           if (!multicastSent) {
//             this->sendRtpAudio(out, len, session.sock, this->rtpAudioPort, false, true); // len in bytes
//             multicastSent = true;
//           }
//         } else {
//           this->sendRtpAudio(out, len, session.isHttp ? session.httpSock : session.sock, session.cAudioPort, session.isTCP, false); // len in bytes
//         }
//       }
//     }
//     this->rtpAudioSent = true;
//   }
// }

// void RTSPServer::sendRTSPAudio(int16_t* data, size_t len) {
//   size_t numSamples = len / sizeof(int16_t);
//   uint8_t* rtpData = nullptr;
//   size_t rtpLen = 0;

//   // Buffers
//   int16_t* processed = data;  // Default to raw input
//   uint8_t* g711Out = nullptr;
// #ifdef USE_SPEEXDSP2
//   int16_t* speaker = nullptr;
//   processed = (int16_t*)malloc(len);
//   speaker = (int16_t*)malloc(len);
//   if (!processed || !speaker) {
//     RTSP_LOGE(LOG_TAG, "Failed to allocate SpeexDSP buffers");
//     free(processed); free(speaker);
//     return;
//   }
//   if (numSamples > 256) {
//     RTSP_LOGE(LOG_TAG, "Input too large for SpeexDSP buffers");
//     return;
//   }
//   int speakerSamples = audioProcessor.readBuffer(speaker, numSamples);
//   if (speakerSamples < numSamples) {
//     memset(speaker + speakerSamples, 0, (numSamples - speakerSamples) * sizeof(int16_t));
//   }
//   //audioProcessor.processAEC(data, speaker, processed);
//   audioProcessor.preprocessMicAudio(processed);
// #endif

//   // Encode based on output codec
//   switch (audioOutCodec) {
//     case G711_ULAW:
//     case G711_ALAW:
//       g711Out = (uint8_t*)malloc(numSamples);
//       if (!g711Out) {
//         RTSP_LOGE(LOG_TAG, "Failed to allocate G.711 buffer");
// #ifdef USE_SPEEXDSP2
//         free(processed); free(speaker);
// #endif
//         return;
//       }
//       encodeG711(processed, g711Out, numSamples, audioOutCodec == G711_ULAW);
//       rtpData = g711Out;
//       rtpLen = numSamples;
//       break;

//     case L16:
//       rtpData = (uint8_t*)processed;
//       rtpLen = len;
//       break;
//   }

//   // Send RTP
// #ifdef USE_SPEEXDSP2
//   if (audioProcessor.isMicVoiceDetected() || true) {  // VAD optional
// #else
//   if (true) {  // No VAD without SpeexDSP
// #endif
//     this->rtpAudioSent = false;
//     bool multicastSent = false;
//     for (const auto& sessionPair : this->sessions) {
//       const RTSP_Session& session = sessionPair.second;
//       if (session.isPlaying) {
//         if (session.isMulticast) {
//           if (!multicastSent) {
//             this->sendRtpAudio(rtpData, rtpLen, session.sock, this->rtpAudioPort, false, true);
//             multicastSent = true;
//           }
//         } else {
//           this->sendRtpAudio(rtpData, rtpLen, session.isHttp ? session.httpSock : session.sock, 
//                             session.cAudioPort, session.isTCP, false);
//         }
//       }
//     }
//     this->rtpAudioSent = true;
//   }

//   // Clean up
// #ifdef USE_SPEEXDSP2
//   free(processed);
//   free(speaker);
// #endif
//   free(g711Out);
// }

void RTSPServer::sendRTSPAudio(int16_t* data, size_t len) {
  size_t numSamples = len / sizeof(int16_t);
  uint8_t* rtpData = nullptr;
  size_t rtpLen = 0;

  // Buffers
  int16_t* processed = data;
  static uint8_t g711OutBuf[256];
#ifdef USE_SPEEXDSP
  static int16_t processedBuf[256];
  static int16_t speakerBuf[256];
  if (numSamples > 256) {
    RTSP_LOGE(LOG_TAG, "Input too large for SpeexDSP buffers");
    return;
  }
  processed = processedBuf;
  memcpy(processed, data, len);
  int speakerSamples = audioProcessor.readBuffer(speakerBuf, numSamples);
  //RTSP_LOGI(LOG_TAG, "Speaker samples read: %d", speakerSamples);
  if (speakerSamples < numSamples) {
    memset(speakerBuf + speakerSamples, 0, (numSamples - speakerSamples) * sizeof(int16_t));
  }
  audioProcessor.processAEC(data, speakerBuf, processed);
  audioProcessor.preprocessMicAudio(processed);
#endif

  // Encode based on output codec
  switch (audioOutCodec) {
    case G711_ULAW:
    case G711_ALAW:
      if (numSamples > 256) {
        RTSP_LOGE(LOG_TAG, "Input too large for G.711 buffer");
        return;
      }
      encodeG711(processed, g711OutBuf, numSamples, audioOutCodec == G711_ULAW);
      rtpData = g711OutBuf;
      rtpLen = numSamples;
      break;

    case L16:
      rtpData = (uint8_t*)processed;
      rtpLen = len;
      break;
  }

  // Send RTP
#ifdef USE_SPEEXDSP
  if (audioProcessor.isMicVoiceDetected() || true) {
#else
  if (true) {
#endif
    rtpAudioSent = false;
    bool multicastSent = false;
    for (const auto& sessionPair : sessions) {
      const RTSP_Session& session = sessionPair.second;
      if (session.isPlaying) {
        if (session.isMulticast) {
          if (!multicastSent) {
            sendRtpAudio(rtpData, rtpLen, session.sock, rtpAudioPort, false, true);
            multicastSent = true;
          }
        } else {
          sendRtpAudio(rtpData, rtpLen, session.isHttp ? session.httpSock : session.sock, 
                       session.cAudioPort, session.isTCP, false);
        }
      }
    }
    rtpAudioSent = true;
  }
}

// void RTSPServer::sendRTSPAudio(int16_t* data, size_t len) {
//   this->rtpAudioSent = false;
//   bool multicastSent = false;
//   for (const auto& sessionPair : this->sessions) {
//     const RTSP_Session& session = sessionPair.second; 
//     if (session.isPlaying) {
//       if (session.isMulticast) {
//         if (!multicastSent) {
//           this->sendRtpAudio(data, len, session.sock, this->rtpAudioPort, false, true);
//           multicastSent = true;
//         }
//       } else {
//         this->sendRtpAudio(data, len,  session.isHttp ? session.httpSock : session.sock, session.cAudioPort, session.isTCP, false);
//       }
//     }
//   }
//   this->rtpAudioSent = true;
// }

void RTSPServer::sendRTSPSubtitles(char* data, size_t len) {
  this->rtpSubtitlesSent = false;
  bool multicastSent = false;
  for (const auto& sessionPair : this->sessions) {
    const RTSP_Session& session = sessionPair.second; 
    if (session.isPlaying) {
      if (session.isMulticast) {
          if (!multicastSent) {
            this->sendRtpSubtitles(data, len, session.sock, this->rtpSubtitlesPort, false, true);
            multicastSent = true;
        }
      } else {
        this->sendRtpSubtitles(data, len,  session.isHttp ? session.httpSock : session.sock, session.cSrtPort, session.isTCP, false);
      }
    }
  }
  this->rtpSubtitlesSent = true;
}

void RTSPServer::sendRtpFrame(const uint8_t* data, size_t len, uint8_t quality, uint16_t width, uint16_t height, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast) {
  const int RtpHeaderSize = 20;
  const int MAX_FRAGMENT_SIZE = 1438;
  uint32_t jpegLen = len;

  size_t fragmentOffset = 0;
  while (fragmentOffset < jpegLen) {
    int fragmentLen = MAX_FRAGMENT_SIZE;
    if (fragmentLen + fragmentOffset > jpegLen) {
      fragmentLen = jpegLen - fragmentOffset;
    }

    bool isLastFragment = (fragmentOffset + fragmentLen) == jpegLen;
    int RtpPacketSize = fragmentLen + RtpHeaderSize;

    uint8_t packet[2048];
    memset(packet, 0x00, sizeof(packet));

    // If TCP, we need these first 4 bytes
    packet[0] = '$'; // Magic number 
    packet[1] = this->videoCh; // Channel number for RTP (0 for video)
    packet[2] = (RtpPacketSize >> 8) & 0xFF; // Packet length high byte 
    packet[3] = RtpPacketSize & 0xFF; // Packet length low byte
    
    // RTP header
    packet[4] = 0x80;
    packet[5] = 0x1A | (isLastFragment ? 0x80 : 0x00);
    packet[6] = (this->videoSequenceNumber >> 8) & 0xFF;
    packet[7] = this->videoSequenceNumber & 0xFF;
    packet[8] = (this->videoTimestamp >> 24) & 0xFF;
    packet[9] = (this->videoTimestamp >> 16) & 0xFF;
    packet[10] = (this->videoTimestamp >> 8) & 0xFF;
    packet[11] = this->videoTimestamp & 0xFF;
    packet[12] = (this->videoSSRC >> 24) & 0xFF;
    packet[13] = (this->videoSSRC >> 16) & 0xFF;
    packet[14] = (this->videoSSRC >> 8) & 0xFF;
    packet[15] = this->videoSSRC & 0xFF;

    // JPEG RTP header
    packet[16] = 0x00;
    packet[17] = (fragmentOffset >> 16) & 0xFF;
    packet[18] = (fragmentOffset >> 8) & 0xFF;
    packet[19] = fragmentOffset & 0xFF;
    packet[20] = 0x00;
    packet[21] = quality;
    packet[22] = width / 8;
    packet[23] = height / 8;

    int packetOffset = 24;

    // Copy JPEG data to the packet
    memcpy(packet + packetOffset, data + fragmentOffset, fragmentLen);
    packetOffset += fragmentLen;

    // Send packet using TCP or UDP
    if (useTCP) {
      sendTcpPacket(packet, packetOffset, sock);
    } else {
      struct sockaddr_in client_addr;
      memset(&client_addr, 0, sizeof(client_addr));
      client_addr.sin_family = AF_INET;
      // Determine IP address based on whether it's multicast or unicast
      if (isMulticast) {
        inet_aton(this->rtpIp.toString().c_str(), &client_addr.sin_addr);
      } else {
        socklen_t addrLen = sizeof(client_addr);
        if (getpeername(sock, (struct sockaddr*)&client_addr, &addrLen) == -1) {
          RTSP_LOGE(LOG_TAG, "Failed to get peer IP address");
          return;
        }
      }
      client_addr.sin_port = htons(sendRtpPort);

      int rtpSocket = isMulticast ? this->videoMulticastSocket : this->videoUnicastSocket;

      sendto(rtpSocket, packet + 4, packetOffset - 4, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
    }
    fragmentOffset += fragmentLen;
    this->videoSequenceNumber++;
  }
}

void RTSPServer::sendRtpAudio(const uint8_t* data, size_t len, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast) {
  const int RtpHeaderSize = 12;
  const int MAX_FRAGMENT_SIZE = 1446;
  size_t fragmentOffset = 0;
  static bool firstPacket = true;

  while (fragmentOffset < len) {
    size_t fragmentLen = std::min(static_cast<size_t>(MAX_FRAGMENT_SIZE), len - fragmentOffset);
    bool isLastFragment = (fragmentOffset + fragmentLen) == len;
    int RtpPacketSize = fragmentLen + RtpHeaderSize;

    uint8_t packet[2048];
    memset(packet, 0, sizeof(packet));

    packet[0] = '$';
    packet[1] = this->audioCh;
    packet[2] = (RtpPacketSize >> 8) & 0xFF;
    packet[3] = RtpPacketSize & 0xFF;

    packet[4] = 0x80;
    packet[5] = (audioOutCodec == G711_ULAW ? 0 : audioOutCodec == G711_ALAW ? 8 : 97) | 
    (firstPacket && fragmentOffset == 0 ? 0x80 : 0);
    packet[6] = (this->audioSequenceNumber >> 8) & 0xFF;
    packet[7] = this->audioSequenceNumber & 0xFF;
    packet[8] = (this->audioTimestamp >> 24) & 0xFF;
    packet[9] = (this->audioTimestamp >> 16) & 0xFF;
    packet[10] = (this->audioTimestamp >> 8) & 0xFF;
    packet[11] = this->audioTimestamp & 0xFF;
    packet[12] = (this->audioSSRC >> 24) & 0xFF;
    packet[13] = (this->audioSSRC >> 16) & 0xFF;
    packet[14] = (this->audioSSRC >> 8) & 0xFF;
    packet[15] = this->audioSSRC & 0xFF;

    int packetOffset = RtpHeaderSize + 4;
    if (audioOutCodec == L16) {
      for (size_t i = 0; i < fragmentLen / 2; i++) {
        uint16_t sample = ((uint16_t*)data)[fragmentOffset / 2 + i];
        packet[packetOffset++] = (sample >> 8) & 0xFF;
        packet[packetOffset++] = sample & 0xFF;
      }
    } else {
      memcpy(packet + packetOffset, data + fragmentOffset, fragmentLen);
      packetOffset += fragmentLen;
    }

    if (useTCP) {
      sendTcpPacket(packet, packetOffset, sock);
    } else {
      struct sockaddr_in client_addr;
      memset(&client_addr, 0, sizeof(client_addr));
      client_addr.sin_family = AF_INET;
      if (isMulticast) {
        inet_aton(this->rtpIp.toString().c_str(), &client_addr.sin_addr);
      } else {
        socklen_t addrLen = sizeof(client_addr);
        if (getpeername(sock, (struct sockaddr*)&client_addr, &addrLen) == -1) {
          RTSP_LOGE(LOG_TAG, "Failed to get peer IP address");
          return;
        }
      }
      client_addr.sin_port = htons(sendRtpPort);
      int rtpSocket = isMulticast ? this->audioMulticastSocket : this->audioUnicastSocket;
      sendto(rtpSocket, packet + 4, packetOffset - 4, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
    }

    fragmentOffset += fragmentLen;
    this->audioSequenceNumber++;
    this->audioTimestamp += (audioOutCodec == L16 ? fragmentLen / 2 : fragmentLen);
    if (isLastFragment) firstPacket = false;
  }
}

void RTSPServer::sendRtpSubtitles(const char* data, size_t len, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast) {
  const int RtpHeaderSize = 12; // RTP header size
  int RtpPacketSize = len + RtpHeaderSize;

  uint8_t packet[512];
  memset(packet, 0x00, sizeof(packet));

  // If TCP, we need these first 4 bytes
  packet[0] = '$'; // Magic number 
  packet[1] = this->subtitlesCh; // Channel number for RTP (2 for subtitles)
  packet[2] = (RtpPacketSize >> 8) & 0xFF; // Packet length high byte 
  packet[3] = RtpPacketSize & 0xFF; // Packet length low byte
  
  // RTP header
  packet[4] = 0x80; // Version: 2, Padding: 0, Extension: 0, CSRC Count: 0
  packet[5] = 0x80 | 0x62; // Marker bit set and payload type 98
  packet[6] = (this->subtitlesSequenceNumber >> 8) & 0xFF; // Sequence Number (high byte)
  packet[7] = this->subtitlesSequenceNumber & 0xFF; // Sequence Number (low byte)
  packet[8] = (this->subtitlesTimestamp >> 24) & 0xFF; // Timestamp (high byte)
  packet[9] = (this->subtitlesTimestamp >> 16) & 0xFF; // Timestamp (next byte)
  packet[10] = (this->subtitlesTimestamp >> 8) & 0xFF; // Timestamp (next byte)
  packet[11] = this->subtitlesTimestamp & 0xFF; // Timestamp (low byte)
  packet[12] = (this->subtitlesSSRC >> 24) & 0xFF; // SSRC (high byte)
  packet[13] = (this->subtitlesSSRC >> 16) & 0xFF; // SSRC (next byte)
  packet[14] = (this->subtitlesSSRC >> 8) & 0xFF; // SSRC (next byte)
  packet[15] = this->subtitlesSSRC & 0xFF; // SSRC (low byte)

  int packetOffset = RtpHeaderSize + 4;

  // Copy SRT data to the packet
  memcpy(packet + packetOffset, data, len);
  packetOffset += len;

  // Send packet using TCP or UDP
  if (useTCP) {
    sendTcpPacket(packet, packetOffset, sock);
  } else {
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    // Determine IP address based on whether it's multicast or unicast
    if (isMulticast) {
      inet_aton(this->rtpIp.toString().c_str(), &client_addr.sin_addr);
    } else {
      socklen_t addrLen = sizeof(client_addr);
      if (getpeername(sock, (struct sockaddr*)&client_addr, &addrLen) == -1) {
        RTSP_LOGE(LOG_TAG, "Failed to get peer IP address");
        return;
      }
    }
    client_addr.sin_port = htons(sendRtpPort);  

    int rtpSocket = isMulticast ? this->subtitlesMulticastSocket : this->subtitlesUnicastSocket;

    sendto(rtpSocket, packet + 4, packetOffset - 4, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
  }
  this->subtitlesSequenceNumber++;
  this->subtitlesTimestamp += 1000; // Increment the timestamp
}
