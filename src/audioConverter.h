#ifndef AUDIO_CONVERTER_H
#define AUDIO_CONVERTER_H

#include <stdint.h>
#include <stddef.h>

int16_t ulawToLinear(uint8_t ulawByte);
void convertG711ToL16(const uint8_t* g711Data, size_t g711Len, int16_t* l16Data);
void convertG711ToL16Upsampled(const uint8_t* g711Data, size_t g711Len, int16_t* l16Data, size_t* l16Len);

// Function to send received audio to the main sketch

#endif // AUDIO_CONVERTER_H
