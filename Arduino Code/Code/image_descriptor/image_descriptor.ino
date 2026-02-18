#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <WebServer.h>
#include <ArduinoJson.h>
#include "Audio.h"

// =====================
// EDIT THESE
// =====================
// Mobile name";
// const char* password = "Your_WIFI_Password";
// const char* apiKey = "Your_API_Key";

const char* ssid = "Your_WIFI_Name";
const char* password = "Your_WIFI_Password";
const char* apiKey = "Your_API_Key"; // From https://aistudio.google.com/apikey

// Audio pins for MAX98357A
#define BCLK_PIN 14
#define LRCK_PIN 15
#define DIN_PIN 13

// The model you want to call once you have the file URI:
const char* modelName = "gemini-2.0-flash";

// The user prompt we send to the AI model:
String userPrompt = "Describe this image in detail in one sentence";

// The auto-classification interval (milliseconds):
unsigned long classificationInterval = 30000; // 30 seconds

// LED pin to blink for 200ms if classification_result=true
#define LED_PIN 4

// ~~~~~ Camera pins: AI Thinker ~~~~~
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// =============================================================
// Global variables
// =============================================================
WiFiClientSecure client; // for TLS
Audio audio; // Audio player

static uint8_t* lastImageBuf = nullptr;
static size_t lastImageLen = 0;
static bool lastClassificationResult = false;
static int totalImages = 0;
static int totalTrueClassified = 0;
static String lastDescription = "";

WebServer server(80);

// Forward declarations
String uploadImage(camera_fb_t *fb);
bool performClassification(const String& fileUri);
void triggerAlarm();
void speakDescription(const String& description);
String urlEncode(const String& text);
void handleRoot();
void setupCamera();

String response;

// --------------------------------------------------------------------------
// Setup camera
void setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_QQVGA;
        config.jpeg_quality = 14;
        config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        delay(1000);
        ESP.restart();
    }
}

// --------------------------------------------------------------------------
// Upload the image to Google and get back the fileUri
// --------------------------------------------------------------------------
String uploadImage(camera_fb_t *fb) {
    const char* host = "generativelanguage.googleapis.com";
    const int httpsPort = 443;
    client.setInsecure(); // for simplicity

    if (!client.connect(host, httpsPort)) {
        Serial.println("Connection to Google failed (upload)");
        return "";
    }

    String boundary = "====ESP32CAM_boundary====";
    String contentType = "multipart/related; boundary=" + boundary;

    String metadataJson =
        "{"
        "\"file\": {"
        "\"display_name\": \"esp32-cam.jpg\""
        "}"
        "}";

    String part1 =
        "--" + boundary + "\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n\r\n"
        + metadataJson + "\r\n"
        "--" + boundary + "\r\n"
        "Content-Type: image/jpeg\r\n\r\n";

    String part2 =
        "\r\n--" + boundary + "--\r\n";

    uint32_t totalLength = part1.length() + fb->len + part2.length();

    String url = "/upload/v1beta/files?uploadType=multipart&key=" + String(apiKey);
    String request =
        "POST " + url + " HTTP/1.1\r\n" +
        "Host: " + host + "\r\n" +
        "X-Goog-Upload-Command: start, upload, finalize\r\n" +
        "X-Goog-Upload-Header-Content-Length: " + String(fb->len) + "\r\n" +
        "X-Goog-Upload-Header-Content-Type: image/jpeg\r\n" +
        "Content-Type: " + contentType + "\r\n" +
        "Content-Length: " + String(totalLength) + "\r\n" +
        "Connection: close\r\n\r\n";

    client.print(request);
    client.print(part1);
    client.write(fb->buf, fb->len);
    client.print(part2);

    String body;
    bool headersEnded = false;
    unsigned long t0 = millis();
    while (client.connected() && (millis() - t0 < 5000)) {
        while (client.available()) {
            String line = client.readStringUntil('\n');
            if (!headersEnded && line == "\r") {
                headersEnded = true;
            } else if (headersEnded) {
                body += line + "\n";
            }
            t0 = millis();
        }
    }
    client.stop();

    Serial.println("Upload response:\n" + body);

    // Naive parse for "uri": "..."
    int idx = body.indexOf("\"uri\":");
    if (idx < 0) {
        Serial.println("No 'uri' found in response");
        return "";
    }
    int start = body.indexOf("\"", idx + 6) + 1;
    int end = body.indexOf("\"", start);
    if (start < 0 || end < 0) {
        Serial.println("URI parse failure");
        return "";
    }
    String fileUri = body.substring(start, end);
    Serial.println("Parsed fileUri: " + fileUri);
    return fileUri;
}

// --------------------------------------------------------------------------
// Perform classification by calling generateContent on the uploaded file
// --------------------------------------------------------------------------
bool performClassification(const String& fileUri) {
    Serial.println("performClassification called with fileUri: " + fileUri);

    if (fileUri == "") {
        Serial.println("Empty fileUri, skipping");
        return false;
    }

    const char* host = "generativelanguage.googleapis.com";
    const int httpsPort = 443;
    client.setInsecure(); // skip cert check

    Serial.println("Connecting to: " + String(host) + ":" + String(httpsPort));
    if (!client.connect(host, httpsPort)) {
        Serial.println("Connection to Google failed (generateContent)");
        return false;
    }
    Serial.println("Connection successful.");

    response = ""; // Clear the response string!

    String body =
        "{"
        "\"contents\":["
        "{"
        "\"role\":\"user\","
        "\"parts\":["
        "{"
        "\"fileData\":{"
        "\"fileUri\":\"" + fileUri + "\","
        "\"mimeType\":\"image/jpeg\""
        "}"
        "},"
        "{"
        "\"text\":\"" + userPrompt + "\"}"
        "]"
        "}"
        "],"
        "\"generationConfig\":{"
        "\"temperature\":1,"
        "\"topK\":40,"
        "\"topP\":0.95,"
        "\"maxOutputTokens\":1024"
        "}"
        "}";

    String url = "/v1beta/models/" + String(modelName) + ":generateContent?key=" + String(apiKey);
    String request =
        "POST " + url + " HTTP/1.1\r\n" +
        "Host: " + host + "\r\n" +
        "Content-Type: application/json\r\n" +
        "Content-Length: " + body.length() + "\r\n" +
        "Connection: close\r\n\r\n";

    Serial.println("Sending request:\n" + request + "\nBody:\n" + body);

    client.print(request);
    client.print(body);

    bool headersEnded = false;
    unsigned long t0 = millis();
    while (client.connected() && (millis() - t0 < 10000)) {
        while (client.available()) {
            String line = client.readStringUntil('\n');
            if (!headersEnded && line == "\r") {
                headersEnded = true;
            } else if (headersEnded) {
                response += line + "\n";
            }
            t0 = millis();
        }
    }

    if (client.connected()) {
        String status = client.readStringUntil('\n');
        Serial.println("HTTP Status: " + status);
    }

    client.stop();

    Serial.println("GenerateContent response:\n" + response);

    // Parse the response to get the description
    const size_t capacity = 2048;
    DynamicJsonDocument doc(capacity);

    // The response might have some garbage at the beginning
    int jsonStart = response.indexOf('{');
    if (jsonStart < 0) {
        Serial.println("No JSON found in response");
        return false;
    }

    String jsonResponse = response.substring(jsonStart);
    DeserializationError error = deserializeJson(doc, jsonResponse);

    if (error) {
        Serial.print(F("JSON deserialization failed: "));
        Serial.println(error.f_str());
        return false;
    }

    // Extract the description
    if (doc.containsKey("candidates") &&
        doc["candidates"][0].containsKey("content") &&
        doc["candidates"][0]["content"].containsKey("parts") &&
        doc["candidates"][0]["content"]["parts"][0].containsKey("text")) {

        lastDescription = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
        Serial.println("Description: " + lastDescription);

        // Speak the description
        speakDescription(lastDescription);

        return true;
    }

    Serial.println("Could not parse description from response");
    return false;
}

// --------------------------------------------------------------------------
// Speak the description using TTS
// --------------------------------------------------------------------------
void speakDescription(const String& description) {
    if (description.length() == 0) {
        Serial.println("No description to speak");
        return;
    }

    Serial.println("Speaking: " + description);

    String encodedText = urlEncode(description);
    String url = "https://translate.google.com/translate_tts?ie=UTF-8&q=" +
                 encodedText + "&tl=en&client=tw-ob";

    Serial.println("TTS URL: " + url);

    // Stop any previous audio if it's running
    // if (audio.isRunning()) {
    //     // Replace with the correct stop function for your library if 'stop()' isn't it
    //     // For example: audio.stopStream(); or audio.disconnect();
    //     // If your library doesn't have an explicit stop, you might need to rely on
    //     // the new connecttohost() call to interrupt the previous one.
    //     delay(100); // Give it a moment to stop if you uncomment the stop function
    // }

    Serial.println("Connecting to audio host...");
    audio.connecttohost(url.c_str());

    unsigned long startTime = millis();
    bool started = false;
    while (millis() - startTime < 10000) { // Check for up to 10 seconds
        if (audio.isRunning()) {
            Serial.println("Audio stream started.");
            started = true;
            break;
        }
        delay(500); // Check status every 500ms
    }

    if (!started) {
        Serial.println("Audio stream did not start after 10 seconds.");
    }

    // No additional delay here, let audio.loop() handle the streaming.
}

// --------------------------------------------------------------------------
// URL encode a string
// --------------------------------------------------------------------------
String urlEncode(const String& text) {
    // String encoded = "";
    // for (unsigned int i = 0; i < text.length(); i++) {
    //     char c = text[i];
    //     if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
    //         encoded += c;
    //     } else if (c == ' ') {
    //         encoded += "%20";
    //     } else {
    //         encoded += "%" + String(c, HEX);
    //     }
    // }
    // return encoded;

    String encoded = "";
    for (unsigned int i = 0; i < text.length(); i++) {
        char c = text[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += "%20";
        } else {
            encoded += "%";
            if ((uint8_t)c < 0x10) encoded += "0";  // Add leading zero for single-digit hex
            encoded += String((uint8_t)c, HEX);
        }
    }
    return encoded;
}

// --------------------------------------------------------------------------
// Trigger an "alarm" - flash an LED for 200ms
// --------------------------------------------------------------------------
void triggerAlarm() {
    Serial.println("Triggering Alarm!");
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
}

// --------------------------------------------------------------------------
// Handle Root and display response.
// --------------------------------------------------------------------------
void handleRoot() {
    if (lastDescription.length() == 0) {
        server.send(200, "text/html", "No description available yet.");
        return;
    }

    String html = "<h1>Image Description</h1><p>" + lastDescription + "</p>";
    server.send(200, "text/html", html);
}

// --------------------------------------------------------------------------
// Setup
// --------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println();

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Connect Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.println("Connecting to WiFi...");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nWiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Setup camera
    setupCamera();

    // Setup audio
    audio.setPinout(BCLK_PIN, LRCK_PIN, DIN_PIN);
    audio.setVolume(10); // Try a lower initial volume

    // WebServer routes
    server.on("/", HTTP_GET, handleRoot);
    server.begin();
    Serial.println("HTTP server started");
}

// --------------------------------------------------------------------------
// Main loop
// --------------------------------------------------------------------------
void loop() {
    server.handleClient();
    audio.loop(); // Handle audio streaming

    static unsigned long prev = 0;
    unsigned long now = millis();
    if (now - prev > classificationInterval) {
        prev = now;

        Serial.println("\n=== Auto-capture and classification ===");
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            return;
        }

        if (lastImageBuf) {
            free(lastImageBuf);
            lastImageBuf = nullptr;
        }
        lastImageBuf = (uint8_t*) malloc(fb->len);
        if (lastImageBuf) {
            memcpy(lastImageBuf, fb->buf, fb->len);
            lastImageLen = fb->len;
        } else {
            lastImageLen = 0;
            Serial.println("Failed to allocate memory for auto-capture");
        }
        esp_camera_fb_return(fb);

        // Now do classification
        if (lastImageBuf && lastImageLen > 0) {
            camera_fb_t fb2;
            fb2.buf = lastImageBuf;
            fb2.len = lastImageLen;
            fb2.width = 0;
            fb2.height = 0;
            fb2.format = PIXFORMAT_JPEG;

            String fileUri = uploadImage(&fb2);
            bool result = performClassification(fileUri);

            totalImages++;
            if (result) {
                totalTrueClassified++;
                triggerAlarm();
            }
            lastClassificationResult = result;
        }
    }
}
