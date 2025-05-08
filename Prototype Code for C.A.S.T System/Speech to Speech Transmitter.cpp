#include <iostream>
#include <fstream>
#include <vector>
#include <wiringPi.h>
#include <RF24/RF24.h>
#include <alsa/asoundlib.h>
#include "codec2.h"
#include <chrono>
#include <filesystem>

// Audio and transmission configuration
#define SAMPLE_RATE 8000
#define CHANNELS 1
#define PACKET_SIZE 32
#define PIN_CE 17
#define PIN_CSN 0

RF24 radio(PIN_CE, PIN_CSN); // NRF24L01+ radio module

// Record 5 seconds of audio, encode with Codec2, and transmit via RF24
void transmit() {
    std::string outputFilename = "logs/STS/STS.raw";

    // asoundlib Audio Capture Setup
    snd_pcm_t *pcmHandle;
    snd_pcm_hw_params_t *params;
    unsigned int rate = SAMPLE_RATE;
    int dir;

    if (snd_pcm_open(&pcmHandle, "default", SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "Error opening PCM device for capture.\n";
        return;
    }

    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(pcmHandle, params);
    snd_pcm_hw_params_set_access(pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcmHandle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcmHandle, params, CHANNELS);
    snd_pcm_hw_params_set_rate_near(pcmHandle, params, &rate, &dir);
    snd_pcm_hw_params(pcmHandle, params);

    //Record Audio for 5 Seconds
    size_t bufferSize = SAMPLE_RATE * CHANNELS * sizeof(short) * 5;
    std::vector<short> buffer(SAMPLE_RATE * CHANNELS * 5);
    size_t totalSamples = 0;

    auto startTime = std::chrono::high_resolution_clock::now();
    while (true) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float> elapsedTime = currentTime - startTime;
        if (elapsedTime.count() >= 5.0f) break;

        if (snd_pcm_readi(pcmHandle, buffer.data() + totalSamples, SAMPLE_RATE) < 0) {
            std::cerr << "Error capturing audio.\n";
            snd_pcm_close(pcmHandle);
            return;
        }

        totalSamples += SAMPLE_RATE;
    }

    // Save Raw Audio File
    std::filesystem::create_directories("logs/STS");
    std::ofstream outFile(outputFilename, std::ios::binary);
    outFile.write(reinterpret_cast<char*>(buffer.data()), totalSamples * sizeof(short));
    outFile.close();
    snd_pcm_close(pcmHandle);

    // Codec2 Setup
    struct CODEC2 *codec2 = codec2_create(CODEC2_MODE_700C);
    FILE *fin = fopen(outputFilename.c_str(), "rb");
    if (!fin) {
        std::cerr << "Error opening recorded file for transmission.\n";
        codec2_destroy(codec2);
        return;
    }

    size_t nsam = codec2_samples_per_frame(codec2);
    std::vector<short> speechSamples(nsam);
    std::vector<unsigned char> compressedBytes(codec2_bytes_per_frame(codec2));

    // Codec 2 Encoding & RF24 Transmission 
    radio.stopListening();
    std::cout << "Starting transmission...\n";

    while (fread(speechSamples.data(), sizeof(short), nsam, fin) == nsam) {
        codec2_encode(codec2, compressedBytes.data(), speechSamples.data());

        size_t sent = 0;
        while (sent < compressedBytes.size()) {
            size_t chunkSize = std::min((size_t)(PACKET_SIZE - 1), compressedBytes.size() - sent);
            std::vector<unsigned char> packet(PACKET_SIZE, 0);
            packet[0] = static_cast<unsigned char>(chunkSize);
            std::copy(compressedBytes.begin() + sent, compressedBytes.begin() + sent + chunkSize, packet.begin() + 1);

            radio.write(packet.data(), PACKET_SIZE);
            sent += chunkSize;
            std::cout << "Sent packet (" << static_cast<int>(chunkSize) << " bytes)\n";
        }
    }

    fclose(fin);
    codec2_destroy(codec2);

    // Transmit EOF Marker
    std::vector<unsigned char> eofPacket(PACKET_SIZE, 0xEE);
    eofPacket[0] = 0xFF;
    radio.write(eofPacket.data(), PACKET_SIZE);
    std::cout << "[TX] Sent EOF marker.\n";
}

// RF24 Initilization & Main Transmit Function
int main() {
    // Initialize GPIO and configure RF24 module
    if (wiringPiSetupGpio() == -1) {
        std::cerr << "WiringPi initialization failed" << std::endl;
        return 1;
    }

    radio.begin();
    radio.setChannel(121);
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_2MBPS);
    radio.setAutoAck(true);
    radio.enableDynamicPayloads();
    radio.setRetries(15, 15);
    radio.openWritingPipe(0x7878787878LL);
    radio.openReadingPipe(1, 0x7878787878LL);

    // Start transmit process
    transmit();

    return 0;
}
