#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <wiringPi.h>
#include <RF24/RF24.h>
#include <sndfile.h>
#include "codec2.h"

using namespace std;
namespace fs = std::filesystem;

// === Configuration ===
#define SAMPLE_RATE 8000
#define CHANNELS 1
#define PACKET_SIZE 32
#define PIN_CE 17
#define PIN_CSN 0
#define GPIO_LED 22

RF24 radio(PIN_CE, PIN_CSN);

//---- Utility Functions -----------------------------------------------------

// Generate current timestamp string for filenames
string getTimestamp() {
    time_t now = time(0);
    tm* ltm = localtime(&now);
    stringstream ss;
    ss << put_time(ltm, "%Y%m%d_%H%M%S");
    return ss.str();
}

// Add event info to central log CSV
void logToCSV(const string& type, const string& filename, const string& message = "") {
    fs::create_directories("logs");
    ofstream csv("logs/log_summary.csv", ios::app);
    csv << getTimestamp() << "," << type << "," << filename << "," << message << endl;
}

// Speak text out loud
void speakText(const string& text) {
    string cmd = "espeak \"" + text + "\"";
    system(cmd.c_str());
}

// Blink LED and announce emergency
void blinkLED(int durationMs, int rateMs) {
    int elapsed = 0;
    while (elapsed < durationMs) {
        speakText("Incoming Emergency");
        digitalWrite(GPIO_LED, HIGH);
        delay(rateMs);
        digitalWrite(GPIO_LED, LOW);
        delay(rateMs);
        elapsed += 2 * rateMs;
    }
}

// Detect emergency keywords in .txt file
bool detectEmergencyKeywords(const string& message) {
    const vector<string> keywords = {"emergency", "help", "urgent", "danger", "alarm"};
    string lower = message;
    transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const string& word : keywords) {
        if (lower.find(word) != string::npos) {
            return true;
        }
    }
    return false;
}

// ---- Text Mode Handling -----------------------------------------------

// Save received message and log it
string saveMessageToLogFile(const string& message, const string& mode) {
    fs::create_directories("logs");
    string timestamp = getTimestamp();
    string filename = "logs/" + mode + "_" + timestamp + ".txt";
    ofstream out(filename);
    out << message;
    out.close();
    logToCSV(mode, filename, message);
    return filename;
}

// Receive text file over RF24
string receiveFile(RF24& radio) {
    radio.startListening();
    stringstream messageStream;
    char buffer[32];

    while (true) {
        if (radio.available()) {
            int len = radio.getDynamicPayloadSize();
            radio.read(buffer, len);
            if (len >= 3 && strncmp(buffer, "EOF", 3) == 0) break;
            messageStream.write(buffer, len);
        }
        delay(100);
    }

    return messageStream.str();
}

// Save .wav file function
bool saveWavFile(const string& filename, const vector<short>& samples) {
    SF_INFO sfinfo;
    sfinfo.channels = CHANNELS;
    sfinfo.samplerate = SAMPLE_RATE;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* file = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
    if (!file) return false;

    sf_write_short(file, samples.data(), samples.size());
    sf_close(file);
    return true;
}

// ---- Speech Mode Handling -----------------------------------------------

// STS Receiver - Audio Receiver, Decoder and Save
void receiveSTS() {
    struct CODEC2 *codec2 = codec2_create(CODEC2_MODE_700C);
    size_t nsam = codec2_samples_per_frame(codec2);
    size_t nbytes = codec2_bytes_per_frame(codec2);

    radio.startListening();
    vector<unsigned char> buffer;
    vector<short> allSamples;

    cout << "[STS] Listening for audio packets...\n";

    string timestamp = getTimestamp();
    string rawFile = "logs/STT/RECV_" + timestamp + ".raw";
    string wavFile = "logs/STT/RECV_" + timestamp + ".wav";
    fs::create_directories("logs/STT");
    ofstream rawOut(rawFile, ios::binary);

    while (true) {
        if (radio.available()) {
            vector<unsigned char> packet(PACKET_SIZE);
            radio.read(packet.data(), PACKET_SIZE);

            unsigned char length = packet[0];
            if (length == 0xFF) {
                cout << "[STS] EOF received.\n";
                break;
            }

            if (length == 0 || length > PACKET_SIZE - 1) continue;

            buffer.insert(buffer.end(), packet.begin() + 1, packet.begin() + 1 + length);

            while (buffer.size() >= nbytes) {
                vector<short> samples(nsam);
                codec2_decode(codec2, samples.data(), buffer.data());

                allSamples.insert(allSamples.end(), samples.begin(), samples.end());
                rawOut.write(reinterpret_cast<char*>(samples.data()), samples.size() * sizeof(short));

                buffer.erase(buffer.begin(), buffer.begin() + nbytes);
            }
        }
    }

    rawOut.close();
    codec2_destroy(codec2);

    // Save WAV and log
    if (saveWavFile(wavFile, allSamples)) {
        cout << "[STS] Audio saved as WAV: " << wavFile << endl;
        logToCSV("STS", wavFile, "Audio saved as WAV");
    } else {
        cerr << "[STS] Failed to save WAV.\n";
    }
}

// Main Communication Loop
int main() {
    wiringPiSetupGpio();
    pinMode(GPIO_LED, OUTPUT);

    // RF24 setup
    radio.begin();
    radio.setChannel(121);
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_2MBPS);
    radio.setAutoAck(true);
    radio.enableDynamicPayloads();
    radio.setRetries(15, 15);
    radio.openWritingPipe(0x7878787878LL);
    radio.openReadingPipe(1, 0x7878787878LL);

    // Main dispatch loop
    while (true) {
        cout << "\n[WAITING] Awaiting mode...\n";
        string mode = receiveFile(radio);
        cout << "[MODE] Received: " << mode << endl;

        if (mode == "STS") {
            receiveSTS();
        } else if (mode == "STT" || mode == "TTS" || mode == "TTT") {
            cout << "[TEXT] Awaiting message...\n";
            string message = receiveFile(radio);
            cout << "[TEXT] Message: " << message << endl;

            string file = saveMessageToLogFile(message, mode);

            if (detectEmergencyKeywords(message)) {
                blinkLED(10000, 50);
                speakText("Emergency message received!");
            }

            speakText(message);
        } else {
            cout << "[UNKNOWN] Mode: " << mode << endl;
        }
    }

    return 0;
}
