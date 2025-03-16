#include "audioConverter.h"

#include <stdint.h>
#include <cmath>

// μ-law to Linear PCM (L16) lookup table
static const int16_t ulawToLinearTable[256] = {
    -32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956,
    -23932, -22908, -21884, -20860, -19836, -18812, -17788, -16764,
    -15996, -15484, -14972, -14460, -13948, -13436, -12924, -12412,
    -11900, -11388, -10876, -10364, -9852, -9340, -8828, -8316,
    -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
    -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
    -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
    -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
    -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
    -1372, -1308, -1244, -1180, -1116, -1052, -988, -924,
    -876, -844, -812, -780, -748, -716, -684, -652,
    -620, -588, -556, -524, -492, -460, -428, -396,
    -372, -356, -340, -324, -308, -292, -276, -260,
    -244, -228, -212, -196, -180, -164, -148, -132,
    -120, -112, -104, -96, -88, -80, -72, -64,
    -56, -48, -40, -32, -24, -16, -8, 0,
    32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
    23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
    15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
    11900, 11388, 10876, 10364, 9852, 9340, 8828, 8316,
    7932, 7676, 7420, 7164, 6908, 6652, 6396, 6140,
    5884, 5628, 5372, 5116, 4860, 4604, 4348, 4092,
    3900, 3772, 3644, 3516, 3388, 3260, 3132, 3004,
    2876, 2748, 2620, 2492, 2364, 2236, 2108, 1980,
    1884, 1820, 1756, 1692, 1628, 1564, 1500, 1436,
    1372, 1308, 1244, 1180, 1116, 1052, 988, 924,
    876, 844, 812, 780, 748, 716, 684, 652,
    620, 588, 556, 524, 492, 460, 428, 396,
    372, 356, 340, 324, 308, 292, 276, 260,
    244, 228, 212, 196, 180, 164, 148, 132,
    120, 112, 104, 96, 88, 80, 72, 64,
    56, 48, 40, 32, 24, 16, 8, 0
};

// void convertG711ToL16(const uint8_t* g711Data, size_t g711Len, int16_t* l16Data) {
//     for (size_t i = 0; i < g711Len; ++i) {
//         l16Data[i] = ulawToLinearTable[g711Data[i]]; // Corrected variable name
//     }
// }

void convertG711ToL16Upsampled(const uint8_t* g711Data, size_t g711Len, int16_t* l16Data, size_t* l16Len) {
    size_t outputIndex = 0;

    for (size_t i = 0; i < g711Len; ++i) {
        // Convert μ-law to L16
        int16_t sample = ulawToLinearTable[g711Data[i]];
        l16Data[outputIndex++] = sample; // Original sample

        if (i < g711Len - 1) {
            // Linear interpolation for upsampling
            int16_t nextSample = ulawToLinearTable[g711Data[i + 1]];
            int16_t interpolated = (sample + nextSample) / 2;
            l16Data[outputIndex++] = interpolated; // Insert interpolated sample
        } else {
            // Last sample doesn't have a next sample to interpolate with
            l16Data[outputIndex++] = sample; // Repeat last sample
        }
    }

    // Update the output length to reflect the upsampled data
    *l16Len = outputIndex;
}

// Function to send received audio to the main sketch
void sendReceivedAudioToMain(const int16_t* l16Data, size_t len, void (*callback)(const int16_t*, size_t)) {
    if (callback) {
        callback(l16Data, len);
    } else {
        // Log an error if the callback is null
        //Serial.println("Error: Audio callback is null");
    }
}
