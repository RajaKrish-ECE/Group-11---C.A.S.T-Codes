#include <iostream>
#include <fstream>
#include <string>
#include <ctime>
#include <sstream>
#include <wiringPi.h>
#include <RF24/RF24.h>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

// GPIO and RF24 pin definitions
#define GPIO_LED 22
#define PIN_CE 17
#define PIN_CSN 0

// Generate timestamp for filenames and logs
string getTimestamp() {
    time_t now = time(0);
    tm* ltm = localtime(&now);
    stringstream ss;
    ss << put_time(ltm, "%Y%m%d_%H%M%S");
    return ss.str();
}

// Append  log entry to the CSV summary file
void logToCSV(const string& type, const string& message, const string& filename) {
    fs::create_directories("logs");
    ofstream csv("logs/log_summary.csv", ios::app);
    string timestamp = getTimestamp();
    csv << timestamp << "," << type << "," << filename << "," << message << endl;
    csv.close();
}

// Save message content to uniquely named log file
string saveMessageToLogFile(const string& message, const string& mode) {
    fs::create_directories("logs");
    string timestamp = getTimestamp();
    string filename = "logs/" + mode + "_" + timestamp + ".txt";
    ofstream out(filename);
    out << message;
    out.close();
    logToCSV(mode, message, filename);
    return filename;
}

// Send the contents of a file over RF24 in 32 byte chunks followed by EOF marker transmission
void sendFile(RF24& radio, const string& filePath) {
    ifstream file(filePath);
    char buffer[32];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        size_t bytesRead = file.gcount();
        radio.write(buffer, bytesRead);
        delay(500);
    }

    // Send EOF marker
    string endMsg = "EOF";
    radio.write(endMsg.c_str(), endMsg.size() + 1);

    // Notify user of successful transmission
    cout << "Text file transmitted!\n";
}

int main() {
    // Initialize GPIO and configure LED pin
    wiringPiSetupGpio();
    pinMode(GPIO_LED, OUTPUT);

    // Set up and configure RF24 radio communication
    RF24 radio(PIN_CE, PIN_CSN);
    radio.begin();
    radio.setChannel(121);
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_2MBPS);
    radio.setAutoAck(false);
    radio.enableDynamicPayloads();
    radio.setRetries(15, 15);
    radio.openWritingPipe(0x7878787878LL);
    radio.openReadingPipe(1, 0x7878787878LL);

    // Send initial mode identifier TTS 
    string mode1 = "TTS";
    string emptyMessage = "1";
    string file = saveMessageToLogFile(emptyMessage, mode1);
    radio.write("TTS", 3);
    sendFile(radio, file);

    // Main loop for user use
    while (true) {
        string mode;
        cout << "\nChoose mode:\n1. Send Message\n2. Send Emergency Message\nChoice: ";
        cin >> mode;
        cin.ignore();

        if (mode == "1") {
            // Standard message input
            string msg;
            cout << "Enter your message (type 'EOF' to finish): ";
            getline(cin, msg);
            string file = saveMessageToLogFile(msg, "TTS");
            sendFile(radio, file);

        } else if (mode == "2") {
            // Emergency preset selection
            vector<string> presets = {
                "Emergency! I need help immediately.",
                "There's a fire!",
                "I'm in danger, call emergency services.",
                "Medical emergency, please respond!",
                "Intruder alert!"
            };

            int choice;
            cout << "\nEmergency Presets:\n";
            for (int i = 0; i < presets.size(); ++i) {
                cout << i + 1 << ". " << presets[i] << endl;
            }

            cout << "Choose a preset (1-" << presets.size() << "): ";
            cin >> choice;
            cin.ignore();

            if (choice < 1 || choice > presets.size()) {
                cout << "Invalid choice, try again.\n";
                continue;
            }

            // Send selected emergency message
            string file = saveMessageToLogFile(presets[choice - 1], "STT-EMERGENCY");
            sendFile(radio, file);

        } else {
            // Invalid input handler
            cout << "Invalid mode. Please try again.\n";
        }
    }

    return 0;
}
