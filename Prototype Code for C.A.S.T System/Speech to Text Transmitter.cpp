#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <vector>
#include <thread>
#include <chrono>

#include <portaudio.h>
#include <sndfile.h>

#include <wiringPi.h>
#include <RF24/RF24.h>

// Audio and filter configuration
#define SAMPLE_RATE 16000
#define FRAMES_PER_BUFFER 256
#define RECORD_SECONDS 5
#define CHANNELS 1
#define HIGHPASS_CUTOFF 200.0f

// RF24 GPIO pin definitions
#define PIN_CE 17
#define PIN_CSN 0

using namespace std;

// Apply a basic high pass filter to remove low frequency noise
class HighPassFilter {
public:
    HighPassFilter(float cutoff, float sampleRate) {
        float timeConst = 1.0f / (2.0f * M_PI * cutoff);
        alpha = timeConst / (timeConst + 1.0f / sampleRate);
        lastInput = lastOutput = 0.0f;
    }
    float process(float input) {
        float output = alpha * (lastOutput + input - lastInput);
        lastInput = input;
        lastOutput = output;
        return output;
    }
private:
    float alpha, lastInput, lastOutput;
};

// Audio callback - apply high pass filter and store recorded samples
vector<float> recordedSamples;
HighPassFilter HPFilter(HIGHPASS_CUTOFF, SAMPLE_RATE);
static int audioCallback(const void* inputBuffer, void*, unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    const float* in = static_cast<const float*>(inputBuffer);
    if (!in) return paContinue;
    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        float filtered = HPFilter.process(in[i]);
        recordedSamples.push_back(filtered);
    }
    return paContinue;
}

// Save the recorded and filtered samples as a 16 bit .wav file
bool saveWavFile(const string& filename, const vector<float>& samples) {
    SF_INFO sfinfo;
    sfinfo.channels = CHANNELS;
    sfinfo.samplerate = SAMPLE_RATE;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* file = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
    if (!file) return false;

    vector<short> intSamples(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        intSamples[i] = static_cast<short>(samples[i] * 32767.0f);

    sf_write_short(file, intSamples.data(), intSamples.size());
    sf_close(file);
    return true;
}

// Transmit a text message over RF24 in 32 byte chunks ending with EOF marker transmission
void sendMessage(RF24& radio, const string& message) {
    size_t index = 0, chunkSize = 32;
    while (index < message.length()) {
        string chunk = message.substr(index, chunkSize);
        radio.write(chunk.c_str(), chunk.size());
        index += chunkSize;
        delay(500);
    }
    radio.write("EOF", 4);
}

int main() {
    // Set up PortAudio input stream for recording
    Pa_Initialize();
    PaStream* stream;
    PaStreamParameters inputParams;
    inputParams.device = Pa_GetDefaultInputDevice();
    inputParams.channelCount = CHANNELS;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    Pa_OpenStream(&stream, &inputParams, nullptr, SAMPLE_RATE, FRAMES_PER_BUFFER,
                  paClipOff, audioCallback, nullptr);
    Pa_StartStream(stream);

    cout << "Recording for 5 seconds...\n";
    this_thread::sleep_for(chrono::seconds(RECORD_SECONDS));

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    // Export recorded audio to .wav file
    const string wavFile = "/home/will/FinalCodes/STT/jfk.wav";
    if (!saveWavFile(wavFile, recordedSamples)) {
        cerr << "Error saving WAV file.\n";
        return 1;
    }
    cout << "Saved WAV: " << wavFile << endl;

    // Transcribe audio using Whisper AI
    const string modelPath = "/home/will/whisper.cpp/models/ggml-tiny.bin";
    const string outputFile = "/home/will/FinalCodes/STT/transcription_output.txt";

    chdir("/home/will/whisper.cpp/build/bin");
    string command = "./whisper-cli -m " + modelPath +
                     " -f " + wavFile +
                     " -otxt -of /home/will/FinalCodes/STT/transcription_output";
    cout << "Running Whisper: " << command << endl;

    if (system(command.c_str()) != 0) {
        cerr << "Whisper failed.\n";
        return 1;
    }

    // Read transcribed text from file
    ifstream transFile(outputFile);
    if (!transFile.is_open()) {
        cerr << "Failed to open transcription output.\n";
        return 1;
    }

    ostringstream transcription;
    string line;
    while (getline(transFile, line)) transcription << line << "\n";
    transFile.close();

    cout << "Transcription:\n" << transcription.str() << endl;

    // Initialize RF24 radio for transmission
    wiringPiSetupGpio();
    RF24 radio(PIN_CE, PIN_CSN);
    radio.begin();
    radio.setChannel(121);
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_2MBPS);
    radio.setAutoAck(false);
    radio.enableDynamicPayloads();
    radio.setRetries(15, 15);
    radio.openWritingPipe(0x7878787878LL);

    // Transmit mode identifier followed by transcription
    sendMessage(radio, "STT");
    this_thread::sleep_for(chrono::milliseconds(500));
    sendMessage(radio, transcription.str());

    return 0;
}
