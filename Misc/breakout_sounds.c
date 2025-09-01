#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> // For uint32_t, uint16_t, etc.
#include <string.h> // For memcpy
#include <math.h>   // For sin()
// Generate sound files for the breakout Example
// gcc -o breakout_sounds breakout_sounds.c -lm

// WAV Header Structure
typedef struct {
    // RIFF Chunk Descriptor
    char     RIFF[4];        // "RIFF"
    uint32_t ChunkSize;      // Total file size - 8
    char     WAVE[4];        // "WAVE"
    // "fmt " sub-chunk
    char     fmt_[4];        // "fmt "
    uint32_t Subchunk1Size;  // 16 for PCM
    uint16_t AudioFormat;    // 1 for PCM
    uint16_t NumChannels;    // Mono = 1, Stereo = 2
    uint32_t SampleRate;     // e.g., 44100, 22050
    uint32_t ByteRate;       // SampleRate * NumChannels * BitsPerSample/8
    uint16_t BlockAlign;     // NumChannels * BitsPerSample/8
    uint16_t BitsPerSample;  // 8 bits, 16 bits, etc.
    // "data" sub-chunk
    char     Subchunk2ID[4]; // "data"
    uint32_t Subchunk2Size;  // NumSamples * NumChannels * BitsPerSample/8
} WavHeader;

// Function to write a WAV file
void writeWav(const char* filename, uint32_t sample_rate, uint16_t bits_per_sample,
               const unsigned char* data, uint32_t data_size) {
    FILE *outfile = fopen(filename, "wb");
    if (!outfile) {
        perror("Failed to open file for writing");
        return;
    }

    WavHeader header;
    uint16_t num_channels = 1; // Mono

    // RIFF Chunk
    memcpy(header.RIFF, "RIFF", 4);
    memcpy(header.WAVE, "WAVE", 4);
    header.ChunkSize = 36 + data_size; // 36 is size of header fields before data

    // fmt sub-chunk
    memcpy(header.fmt_, "fmt ", 4);
    header.Subchunk1Size = 16; // For PCM
    header.AudioFormat = 1;    // PCM
    header.NumChannels = num_channels;
    header.SampleRate = sample_rate;
    header.BitsPerSample = bits_per_sample;
    header.ByteRate = sample_rate * num_channels * bits_per_sample / 8;
    header.BlockAlign = num_channels * bits_per_sample / 8;

    // data sub-chunk
    memcpy(header.Subchunk2ID, "data", 4);
    header.Subchunk2Size = data_size;

    // Write header
    fwrite(&header, sizeof(WavHeader), 1, outfile);

    // Write data
    fwrite(data, sizeof(unsigned char), data_size, outfile);

    fclose(outfile);
    printf("Generated WAV file: %s (%u bytes of data)\n", filename, data_size);
}

#define SAMPLE_RATE 22050 // Common, lower quality sample rate for small sounds
#define BITS_PER_SAMPLE 8
#define AMPLITUDE 100     // For 8-bit, 0-255. Center is 128. Amplitude around center.
#define PI 3.14159265358979323846

// Generate a simple tone
void generateTone(unsigned char* buffer, uint32_t num_samples, float frequency, float duration_secs) {
    for (uint32_t i = 0; i < num_samples; ++i) {
        float time = (float)i / SAMPLE_RATE;
        // Simple sine wave, scaled and offset for 8-bit unsigned PCM
        // For 8-bit unsigned, samples range from 0 to 255, with 128 as silence.
        float sample_float = sinf(2.0f * PI * frequency * time);
        buffer[i] = (unsigned char)(128.0f + sample_float * AMPLITUDE);
    }
}

// Generate a decaying tone (simple attack-decay envelope)
void generateDecayingTone(unsigned char* buffer, uint32_t num_samples, float start_freq, float end_freq, float duration_secs) {
    for (uint32_t i = 0; i < num_samples; ++i) {
        float time = (float)i / SAMPLE_RATE;
        float progress = time / duration_secs; // 0.0 to 1.0

        // Linear frequency sweep (can be made more complex)
        float current_freq = start_freq + (end_freq - start_freq) * progress;
        
        // Simple decay envelope (linear decay here, could be exponential)
        float current_amplitude = AMPLITUDE * (1.0f - progress);
        if (current_amplitude < 0) current_amplitude = 0;

        float sample_float = sinf(2.0f * PI * current_freq * time);
        buffer[i] = (unsigned char)(128.0f + sample_float * current_amplitude);
    }
}


int main() {
    // --- Generate brick_hit.wav ---
    float brick_hit_duration = 0.08f; // Short blip
    uint32_t brick_hit_samples = (uint32_t)(SAMPLE_RATE * brick_hit_duration);
    unsigned char* brick_hit_data = (unsigned char*)malloc(brick_hit_samples);
    if (!brick_hit_data) { perror("malloc brick_hit_data failed"); return 1; }

    generateDecayingTone(brick_hit_data, brick_hit_samples, 1200.0f, 800.0f, brick_hit_duration); // Higher pitch, quick decay
    // Add a sharper attack (optional, more complex) - for now simple decay
    // For a sharper click, you could make the first few samples max amplitude
    for(int i=0; i < 50 && i < brick_hit_samples; ++i) { // Very short click/pop
        brick_hit_data[i] = (i % 2 == 0) ? (128 + AMPLITUDE) : (128 - AMPLITUDE);
    }


    writeWav("brick_hit.wav", SAMPLE_RATE, BITS_PER_SAMPLE, brick_hit_data, brick_hit_samples);
    free(brick_hit_data);

    // --- Generate lose_life.wav ---
    float lose_life_duration = 0.25f; // Slightly longer
    uint32_t lose_life_samples = (uint32_t)(SAMPLE_RATE * lose_life_duration);
    unsigned char* lose_life_data = (unsigned char*)malloc(lose_life_samples);
    if (!lose_life_data) { perror("malloc lose_life_data failed"); return 1; }

    generateDecayingTone(lose_life_data, lose_life_samples, 440.0f, 220.0f, lose_life_duration); // Descending pitch
    writeWav("lose_life.wav", SAMPLE_RATE, BITS_PER_SAMPLE, lose_life_data, lose_life_samples);
    free(lose_life_data);
    
    // --- Generate paddle_hit.wav (similar to brick_hit but maybe slightly different pitch/duration) ---
    float paddle_hit_duration = 0.06f;
    uint32_t paddle_hit_samples = (uint32_t)(SAMPLE_RATE * paddle_hit_duration);
    unsigned char* paddle_hit_data = (unsigned char*)malloc(paddle_hit_samples);
    if (!paddle_hit_data) { perror("malloc paddle_hit_data failed"); return 1; }

    generateDecayingTone(paddle_hit_data, paddle_hit_samples, 1000.0f, 700.0f, paddle_hit_duration);
    // Add a sharper attack
    for(int i=0; i < 40 && i < paddle_hit_samples; ++i) {
        paddle_hit_data[i] = (i % 2 == 0) ? (128 + AMPLITUDE - 20) : (128 - AMPLITUDE + 20); // Slightly softer pop
    }
    writeWav("paddle_hit.wav", SAMPLE_RATE, BITS_PER_SAMPLE, paddle_hit_data, paddle_hit_samples);
    free(paddle_hit_data);

    // --- Generate wall_hit.wav (a duller thud) ---
    float wall_hit_duration = 0.1f;
    uint32_t wall_hit_samples = (uint32_t)(SAMPLE_RATE * wall_hit_duration);
    unsigned char* wall_hit_data = (unsigned char*)malloc(wall_hit_samples);
    if (!wall_hit_data) { perror("malloc wall_hit_data failed"); return 1; }

    // Low frequency, quick decay for a thud
    generateDecayingTone(wall_hit_data, wall_hit_samples, 200.0f, 100.0f, wall_hit_duration * 0.5f); // Faster decay
    // You might mix this with some noise or a square wave for a better thud
    writeWav("wall_hit.wav", SAMPLE_RATE, BITS_PER_SAMPLE, wall_hit_data, wall_hit_samples);
    free(wall_hit_data);


    return 0;
}
