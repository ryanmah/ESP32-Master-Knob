# Updated crowpanel_ha_controller.ino

#include <WiFi.h>
#include <HTTPClient.h>

// Configuration
const char* wiimDeviceUrl = "http://your_wiim_device_url_here"; // Change as needed

// Existing Home Assistant Webhook
const char* homeAssistantWebhook = "http://your_home_assistant_url_here";

// Function to control volume on Wiim device
void setVolume(int volume) {
    HTTPClient http;
    String url = String(wiimDeviceUrl) + "/volume/" + String(volume);
    http.begin(url);
    int httpResponseCode = http.GET();
    http.end();
}

// Function to play/pause media
void togglePlayPause() {
    HTTPClient http;
    String url = String(wiimDeviceUrl) + "/playpause";
    http.begin(url);
    int httpResponseCode = http.GET();
    http.end();
}

// Function to play next track
void playNextTrack() {
    HTTPClient http;
    String url = String(wiimDeviceUrl) + "/next";
    http.begin(url);
    int httpResponseCode = http.GET();
    http.end();
}

// Function to play previous track
void playPreviousTrack() {
    HTTPClient http;
    String url = String(wiimDeviceUrl) + "/previous";
    http.begin(url);
    int httpResponseCode = http.GET();
    http.end();
}

// Trigger webhooks from the media control panel
void mediaControlPanel() {
    // Assuming you have some way to detect changes in media controls
    togglePlayPause(); // Example: toggle play/pause
}

// Adjust volume function triggered from the default screen
void adjustVolume(int volume) {
    setVolume(volume);
}

void setup() {
    // Initialize WiFi
    WiFi.begin("your_SSID", "your_PASSWORD");
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
    }
}

void loop() {
    // Read input from Screen 5 or default screen
    mediaControlPanel();
    delay(1000);
}

