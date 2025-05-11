// generate_pong_wavs.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// Define standard audio parameters
#define SAMPLE_RATE 44100
#define NUM_CHANNELS 1    // Mono
#define BITS_PER_SAMPLE 16
#define BYTE_RATE (SAMPLE_RATE * NUM_CHANNELS * BITS_PER_SAMPLE / 8)
#define BLOCK_ALIGN (NUM_CHANNELS * BITS_PER_SAMPLE / 8)
#define AUDIO_FORMAT 1    // PCM

// Structure for the WAV file header (RIFF, fmt, data chunks)
// Using packed attribute to ensure correct structure layout, though it might not be
// strictly necessary for these simple structures on most platforms.
#ifdef _MSC_VER
#include <pshpack1.h>
#endif

typedef struct {
    char     chunk_id[4];     // "RIFF"
    uint32_t chunk_size;      // 36 + SubChunk2Size (size of data chunk)
    char     format[4];       // "WAVE"
} RiffChunk;

typedef struct {
    char     subchunk1_id[4];    // "fmt "
    uint32_t subchunk1_size;     // 16 for PCM
    uint16_t audio_format;       // 1 for PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;

typedef struct {
    char     subchunk2_id[4];    // "data"
    uint32_t subchunk2_size;     // Number of bytes in the data.
} DataChunk;

#ifdef _MSC_VER
#include <poppack.h>
#endif


// Function to write the WAV header to a file
void write_wav_header(FILE* file, uint32_t data_size) {
    RiffChunk riff;
    FmtChunk fmt;
    DataChunk data;

    // RIFF chunk
    memcpy(riff.chunk_id, "RIFF", 4);
    riff.chunk_size = 36 + data_size; // Total file size - 8 bytes
    memcpy(riff.format, "WAVE", 4);
    fwrite(&riff, 1, sizeof(RiffChunk), file);

    // fmt chunk
    memcpy(fmt.subchunk1_id, "fmt ", 4);
    fmt.subchunk1_size = 16; // Size of fmt subchunk (excluding subchunk1_id and subchunk1_size)
    fmt.audio_format = AUDIO_FORMAT;
    fmt.num_channels = NUM_CHANNELS;
    fmt.sample_rate = SAMPLE_RATE;
    fmt.byte_rate = BYTE_RATE;
    fmt.block_align = BLOCK_ALIGN;
    fmt.bits_per_sample = BITS_PER_SAMPLE;
    fwrite(&fmt, 1, sizeof(FmtChunk), file);

    // data chunk header (size is filled before writing data)
    memcpy(data.subchunk2_id, "data", 4);
    data.subchunk2_size = data_size; // Size of the raw audio data
    fwrite(&data, 1, sizeof(DataChunk), file);
}

// Function to generate a square wave and fill a buffer
// buffer: pointer to allocated memory (int16_t array)
// num_samples: total number of samples to generate
// frequency: desired frequency of the square wave
// amplitude: peak amplitude (e.g., INT16_MAX / 4)
void generate_square_wave(int16_t* buffer, uint32_t num_samples, double frequency, int amplitude) {
    double samples_per_cycle = (double)SAMPLE_RATE / frequency;
    for (uint32_t i = 0; i < num_samples; ++i) {
        // Determine position within the current cycle
        double cycle_pos = fmod(i, samples_per_cycle);

        // Set amplitude based on the first half of the cycle (positive) vs second half (negative)
        if (cycle_pos < samples_per_cycle / 2.0) {
            buffer[i] = amplitude;
        } else {
            buffer[i] = -amplitude;
        }
    }
}

int main() {
    // --- Generate Paddle Hit Sound ---
    const double paddle_hit_freq = 880.0; // Higher frequency for paddle (A5)
    const double paddle_hit_duration_sec = 0.08; // Shorter duration (80 ms)
    uint32_t paddle_hit_num_samples = SAMPLE_RATE * paddle_hit_duration_sec;
    // Data size in bytes = number of samples * bytes per sample (16 bits = 2 bytes) * number of channels
    uint32_t paddle_hit_data_size = paddle_hit_num_samples * (BITS_PER_SAMPLE / 8) * NUM_CHANNELS;

    // Allocate buffer for sample data (each sample is 16-bit, so sizeof(int16_t))
    int16_t* paddle_hit_buffer = (int16_t*)malloc(paddle_hit_num_samples * sizeof(int16_t));
    if (!paddle_hit_buffer) {
        perror("Failed to allocate buffer for paddle hit sound");
        return EXIT_FAILURE;
    }

    // Generate the square wave data
    generate_square_wave(paddle_hit_buffer, paddle_hit_num_samples, paddle_hit_freq, INT16_MAX / 6); // Adjust amplitude

    // Open the output file for writing in binary mode
    FILE* paddle_hit_file = fopen("paddle_hit.wav", "wb");
    if (!paddle_hit_file) {
        perror("Failed to open paddle_hit.wav for writing");
        free(paddle_hit_buffer); // Free allocated memory before exiting
        return EXIT_FAILURE;
    }

    // Write the WAV header and the audio data
    write_wav_header(paddle_hit_file, paddle_hit_data_size);
    fwrite(paddle_hit_buffer, 1, paddle_hit_data_size, paddle_hit_file);

    // Close the file and free the buffer
    fclose(paddle_hit_file);
    free(paddle_hit_buffer);
    printf("Generated paddle_hit.wav\n");


    // --- Generate Wall Hit Sound ---
    const double wall_hit_freq = 440.0; // Lower frequency for wall (A4)
    const double wall_hit_duration_sec = 0.1; // Slightly longer duration (100 ms)
    uint32_t wall_hit_num_samples = SAMPLE_RATE * wall_hit_duration_sec;
    uint32_t wall_hit_data_size = wall_hit_num_samples * (BITS_PER_SAMPLE / 8) * NUM_CHANNELS;

    int16_t* wall_hit_buffer = (int16_t*)malloc(wall_hit_num_samples * sizeof(int16_t));
    if (!wall_hit_buffer) {
        perror("Failed to allocate buffer for wall hit sound");
        return EXIT_FAILURE;
    }

    generate_square_wave(wall_hit_buffer, wall_hit_num_samples, wall_hit_freq, INT16_MAX / 8); // Adjust amplitude

    FILE* wall_hit_file = fopen("wall_hit.wav", "wb");
    if (!wall_hit_file) {
        perror("Failed to open wall_hit.wav for writing");
        free(wall_hit_buffer);
        return EXIT_FAILURE;
    }

    write_wav_header(wall_hit_file, wall_hit_data_size);
    fwrite(wall_hit_buffer, 1, wall_hit_data_size, wall_hit_file);

    fclose(wall_hit_file);
    free(wall_hit_buffer);
    printf("Generated wall_hit.wav\n");


    return EXIT_SUCCESS; // Indicate successful execution
}
