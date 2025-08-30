#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> // For specific integer types like int16_t, uint32_t
#include <string.h> // For memcpy
#include <math.h>   // For sin, M_PI, pow

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Helper to write little-endian values (WAV files use little-endian)
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
    const char *filename = "bounce.wav"; // Output filename
    FILE *fout;

    // Audio parameters
    const uint32_t SAMPLE_RATE = 22050;    // Hz
    const uint16_t NUM_CHANNELS = 1;       // Mono
    const uint16_t BITS_PER_SAMPLE = 16;
    const double DURATION = 0.25;          // Seconds (a bit longer for more boom)

    // Sound characteristics for a "boomy" bounce
    const double START_FREQ_MAIN = 400.0;  // Lower starting frequency for the main component
    const double END_FREQ_MAIN = 100.0;    // Lower ending frequency
    const double START_FREQ_SUB = START_FREQ_MAIN / 2.0; // Sub-octave for boominess
    const double END_FREQ_SUB = END_FREQ_MAIN / 2.0;

    const int16_t MAX_AMPLITUDE = 22000;  // Peak amplitude for 16-bit audio
    const double SUB_AMPLITUDE_FACTOR = 0.6; // How loud the sub-octave is relative to main

    uint32_t num_samples = (uint32_t)(SAMPLE_RATE * DURATION);
    uint32_t data_size = num_samples * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);

    int16_t *audio_buffer = (int16_t *)malloc(data_size);
    if (!audio_buffer) {
        fprintf(stderr, "Error: Could not allocate memory for audio buffer.\n");
        return 1;
    }

    double current_phase_main = 0.0;
    double current_phase_sub = 0.0;

    for (uint32_t i = 0; i < num_samples; ++i) {
        double t_norm = (double)i / (num_samples -1); // Normalized time (0.0 to 1.0)

        // Pitch sweep (logarithmic or exponential can sound more natural, but linear is simpler)
        double current_freq_main = START_FREQ_MAIN - (START_FREQ_MAIN - END_FREQ_MAIN) * t_norm;
        double current_freq_sub = START_FREQ_SUB - (START_FREQ_SUB - END_FREQ_SUB) * t_norm;
        
        // Amplitude envelope: fast attack, moderate decay (e.g., exponential decay)
        // Using (1 - t_norm) for linear decay. For exponential: pow(decay_base, t_norm * some_factor)
        // A simpler approach for a "boomy" impact is a sharp attack then a slightly slower decay.
        // Let's use a simple linear decay for now, but ensure it's punchy.
        double amplitude_envelope = 1.0 - t_norm; // Linear decay from 1.0 to 0.0
        // For a more percussive feel, you might want a steeper initial decay.
        // Example: amplitude_envelope = pow(0.1, t_norm); // Faster exponential decay (0.1 is a base)

        // Generate main sine wave component
        double sample_main = sin(2.0 * M_PI * current_phase_main);

        // Generate sub-octave sine wave component
        double sample_sub = sin(2.0 * M_PI * current_phase_sub);

        // Combine components and apply envelope
        double final_sample_float = (double)MAX_AMPLITUDE * amplitude_envelope * (sample_main + sample_sub * SUB_AMPLITUDE_FACTOR);
        
        // Normalize if necessary (though with careful amplitude setting, it might not be)
        // The combined amplitude could exceed MAX_AMPLITUDE if SUB_AMPLITUDE_FACTOR is high.
        // Let's keep it simple for now. Max possible value here is MAX_AMPLITUDE * (1 + SUB_AMPLITUDE_FACTOR) at t=0.
        // If this sum is > 1 (relative to MAX_AMPLITUDE as 1.0), then clipping could occur or normalization is needed.
        // With MAX_AMPLITUDE=22000 and SUB_AMPLITUDE_FACTOR=0.6, max combined could be 22000 * 1.6 = 35200,
        // which exceeds 32767. So we should scale down.
        
        final_sample_float = final_sample_float / (1.0 + SUB_AMPLITUDE_FACTOR); // Normalize to avoid clipping

        // Convert to 16-bit integer, ensuring it's within range
        if (final_sample_float > 32767.0) final_sample_float = 32767.0;
        if (final_sample_float < -32768.0) final_sample_float = -32768.0;
        audio_buffer[i] = (int16_t)final_sample_float;

        // Increment phases
        current_phase_main += current_freq_main / SAMPLE_RATE;
        if (current_phase_main >= 1.0) current_phase_main -= 1.0;

        current_phase_sub += current_freq_sub / SAMPLE_RATE;
        if (current_phase_sub >= 1.0) current_phase_sub -= 1.0;
    }

    fout = fopen(filename, "wb");
    if (!fout) {
        fprintf(stderr, "Error: Could not open file %s for writing.\n", filename);
        free(audio_buffer);
        return 1;
    }

    // Write WAV Header
    fwrite("RIFF", 1, 4, fout);
    uint32_t chunk_size = 36 + data_size;
    writeLittleEndianUint32(fout, chunk_size);
    fwrite("WAVE", 1, 4, fout);

    fwrite("fmt ", 1, 4, fout);
    writeLittleEndianUint32(fout, 16); 
    writeLittleEndianUint16(fout, 1);  
    writeLittleEndianUint16(fout, NUM_CHANNELS);
    writeLittleEndianUint32(fout, SAMPLE_RATE);
    uint32_t byte_rate = SAMPLE_RATE * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    writeLittleEndianUint32(fout, byte_rate);
    uint16_t block_align = NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    writeLittleEndianUint16(fout, block_align);
    writeLittleEndianUint16(fout, BITS_PER_SAMPLE);

    fwrite("data", 1, 4, fout);
    writeLittleEndianUint32(fout, data_size);

    size_t items_written = fwrite(audio_buffer, BITS_PER_SAMPLE / 8, num_samples * NUM_CHANNELS, fout);
    if (items_written != num_samples * NUM_CHANNELS) {
        fprintf(stderr, "Error writing audio data to file.\n");
        fclose(fout);
        free(audio_buffer);
        return 1;
    }

    printf("Successfully created boomy %s\n", filename);

    fclose(fout);
    free(audio_buffer);

    return 0;
}
