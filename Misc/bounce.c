#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> // For specific integer types like int16_t, uint32_t
#include <string.h> // For memcpy, strlen
#include <math.h>   // For sin, M_PI

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// WAV Header Structure (simplified for this example)
// Note: Compilers might add padding. For precise control, pack pragmas
// or write byte-by-byte, but for common systems this should work.
typedef struct {
    // RIFF Chunk Descriptor
    char RIFF[4];        // RIFF Header Magic header
    uint32_t ChunkSize;      // RIFF Chunk Size
    char WAVE[4];        // WAVE Header
    // "fmt" sub-chunk
    char fmt[4];         // FMT header
    uint32_t Subchunk1Size;  // Size of the fmt chunk
    uint16_t AudioFormat;    // Audio format 1=PCM, other numbers indicate compression
    uint16_t NumChannels;    // Number of channels 1=Mono 2=Sterio
    uint32_t SampleRate;     // Sampling Frequency in Hz
    uint32_t ByteRate;       // == SampleRate * NumChannels * BitsPerSample/8
    uint16_t BlockAlign;     // == NumChannels * BitsPerSample/8
    uint16_t BitsPerSample;  // Number of bits per sample
    // "data" sub-chunk
    char Subchunk2ID[4]; // "data"  string
    uint32_t Subchunk2Size;  // Sampled data length
} WavHeader;

void writeLittleEndianUint32(FILE *f, uint32_t val) {
    fputc(val & 0xFF, f);
    fputc((val >> 8) & 0xFF, f);
    fputc((val >> 16) & 0xFF, f);
    fputc((val >> 24) & 0xFF, f);
}

void writeLittleEndianUint16(FILE *f, uint16_t val) {
    fputc(val & 0xFF, f);
    fputc((val >> 8) & 0xFF, f);
}

int main() {
    const char *filename = "bounce.wav";
    FILE *fout;

    // Audio parameters
    const uint32_t SAMPLE_RATE = 22050;
    const uint16_t NUM_CHANNELS = 1; // Mono
    const uint16_t BITS_PER_SAMPLE = 16;
    const double DURATION = 0.15; // seconds

    const double START_FREQ = 900.0; // Hz
    const double END_FREQ = 300.0;   // Hz
    const int16_t MAX_AMPLITUDE = 20000; // Max amplitude for 16-bit audio (32767 is max)

    uint32_t num_samples = (uint32_t)(SAMPLE_RATE * DURATION);
    uint32_t data_size = num_samples * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);

    // Allocate buffer for audio data
    int16_t *audio_buffer = (int16_t *)malloc(data_size);
    if (!audio_buffer) {
        fprintf(stderr, "Error: Could not allocate memory for audio buffer.\n");
        return 1;
    }

    double current_phase = 0.0;
    for (uint32_t i = 0; i < num_samples; ++i) {
        double t = (double)i / SAMPLE_RATE; // Current time in seconds

        // Calculate current frequency (linear sweep down)
        double current_freq = START_FREQ - (START_FREQ - END_FREQ) * (t / DURATION);
        if (current_freq < END_FREQ) current_freq = END_FREQ; // Clamp

        // Calculate current amplitude (linear decay)
        double amplitude_factor = 1.0 - (t / DURATION);
        if (amplitude_factor < 0.0) amplitude_factor = 0.0;

        // Generate sine wave sample
        double sample_value_float = (double)MAX_AMPLITUDE * amplitude_factor * sin(2.0 * M_PI * current_phase);

        // Convert to 16-bit integer
        audio_buffer[i] = (int16_t)sample_value_float;

        // Increment phase
        current_phase += current_freq / SAMPLE_RATE;
        if (current_phase >= 1.0) {
            current_phase -= 1.0; // Wrap phase
        }
    }

    // Open file for writing in binary mode
    fout = fopen(filename, "wb");
    if (!fout) {
        fprintf(stderr, "Error: Could not open file %s for writing.\n", filename);
        free(audio_buffer);
        return 1;
    }

    // --- Write WAV Header ---
    // RIFF Chunk Descriptor
    fwrite("RIFF", 1, 4, fout);
    uint32_t chunk_size = 36 + data_size; // 4 (WAVE) + 8 (fmt header) + 16 (fmt data) + 8 (data header) + data_size
    writeLittleEndianUint32(fout, chunk_size);
    fwrite("WAVE", 1, 4, fout);

    // "fmt " sub-chunk
    fwrite("fmt ", 1, 4, fout);
    writeLittleEndianUint32(fout, 16); // Subchunk1Size for PCM
    writeLittleEndianUint16(fout, 1);  // AudioFormat (1 for PCM)
    writeLittleEndianUint16(fout, NUM_CHANNELS);
    writeLittleEndianUint32(fout, SAMPLE_RATE);
    uint32_t byte_rate = SAMPLE_RATE * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    writeLittleEndianUint32(fout, byte_rate);
    uint16_t block_align = NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    writeLittleEndianUint16(fout, block_align);
    writeLittleEndianUint16(fout, BITS_PER_SAMPLE);

    // "data" sub-chunk
    fwrite("data", 1, 4, fout);
    writeLittleEndianUint32(fout, data_size);

    // Write audio data
    // fwrite requires element size and count. We have 16-bit samples.
    size_t items_written = fwrite(audio_buffer, BITS_PER_SAMPLE / 8, num_samples * NUM_CHANNELS, fout);
    if (items_written != num_samples * NUM_CHANNELS) {
        fprintf(stderr, "Error writing audio data to file.\n");
        fclose(fout);
        free(audio_buffer);
        return 1;
    }


    printf("Successfully created %s\n", filename);

    // Cleanup
    fclose(fout);
    free(audio_buffer);

    return 0;
}
