#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "FS.h"
#include "SD.h"
#include <SPI.h>
#include <time.h>

// ============================================================
// DEVICE CONFIG
// ============================================================
const char* DEVICE_ID = "ESP32_CAM_01";
const char* MDNS_NAME = "esp32cam01";

// ============================================================
// FIREBASE
// ============================================================
const char* FIREBASE_HOST   = "firebasestorage.googleapis.com";
const char* FIREBASE_BUCKET = "safecampus-4c8a2.firebasestorage.app";
const char* RTDB_HOST       = "safecampus-4c8a2-default-rtdb.europe-west1.firebasedatabase.app";

// ============================================================
// MULTI-WIFI LOCATION ARCHITECTURE
// Each known Wi-Fi network maps the camera to a zone.
// ============================================================
struct KnownWiFi {
  const char* ssid;
  const char* password;
  const char* zone_name;
};

const KnownWiFi knownNetworks[] = {
  { "DESKTOP-D3EN5B1 0958", "k29|59W3",           "Library" },
  { "Flybox_DA7E",              "w3xdi4yte6ea", "FET" },
  { "Hailey Lebopo's A32",         "htfa7479", "FOB" }
};

const int KNOWN_WIFI_COUNT = sizeof(knownNetworks) / sizeof(knownNetworks[0]);

int currentNetworkIndex = -1;
int candidateNetworkIndex = 0;

bool wifiConnectInProgress = false;
bool mdnsStarted = false;

String lastPublishedIP = "";
String lastPublishedSSID = "";

unsigned long wifiAttemptStart = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastPresenceUpdate = 0;

#define WIFI_CONNECT_TIMEOUT_MS       12000
#define WIFI_RETRY_INTERVAL_MS        10000
#define CAMERA_PRESENCE_INTERVAL_MS   30000

// ============================================================
// SERVER / CAMERA
// ============================================================
WebServer server(80);

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

#define SD_CS   13
#define SD_MOSI 15
#define SD_MISO 14
#define SD_SCK   2

#define FRAMES_PER_ALERT     3
#define CAPTURE_INTERVAL_MS  500
#define FLASH_LED            4

bool sdAvailable = false;
bool captureInProgress = false;

struct AlertStatus {
  String state;
  int captured;
  int uploaded;
};

AlertStatus currentAlert;
String currentAlertID = "";

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void updateFrameStatus(String alertID, int uploaded, int expected);
void updateCameraPresence();
void maintainWiFi();

// ============================================================
// WIFI HELPERS
// ============================================================
String getCurrentZoneName() {
  if (currentNetworkIndex >= 0 && currentNetworkIndex < KNOWN_WIFI_COUNT) {
    return String(knownNetworks[currentNetworkIndex].zone_name);
  }
  return "Unmapped";
}

int findConnectedNetworkIndex() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String connectedSSID = WiFi.SSID();

  for (int i = 0; i < KNOWN_WIFI_COUNT; i++) {
    if (connectedSSID == String(knownNetworks[i].ssid)) {
      return i;
    }
  }

  return -1;
}

void startWiFiAttempt(int index) {
  if (index < 0 || index >= KNOWN_WIFI_COUNT) return;

  Serial.printf("Trying network: %s\n", knownNetworks[index].ssid);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  delay(100);
  WiFi.begin(knownNetworks[index].ssid, knownNetworks[index].password);

  candidateNetworkIndex = index;
  wifiAttemptStart = millis();
  wifiConnectInProgress = true;
}

void startKnownWiFiConnectionCycle() {
  Serial.println("Attempting known Wi-Fi connections...");
  currentNetworkIndex = -1;
  candidateNetworkIndex = 0;
  startWiFiAttempt(candidateNetworkIndex);
}

void startMDNS() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.println("mDNS started: http://" + String(MDNS_NAME) + ".local/capture");
  } else {
    Serial.println("mDNS start failed");
  }
}

void handleConnectedWiFi() {
  int mappedIndex = findConnectedNetworkIndex();
  currentNetworkIndex = mappedIndex;

  Serial.println("Connected to network: " + WiFi.SSID());
  Serial.println("Current IP: " + WiFi.localIP().toString());

  if (currentNetworkIndex >= 0) {
    Serial.printf("Mapped camera zone: %s\n", knownNetworks[currentNetworkIndex].zone_name);
  } else {
    Serial.println("Connected Wi-Fi is not mapped to a camera zone");
  }

  startMDNS();
  updateCameraPresence();

  lastPublishedIP = WiFi.localIP().toString();
  lastPublishedSSID = WiFi.SSID();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    int mappedIndex = findConnectedNetworkIndex();

    if (mappedIndex != currentNetworkIndex) {
      currentNetworkIndex = mappedIndex;
      Serial.println("Connected Wi-Fi mapping changed");
      updateCameraPresence();
    }

    String currentIP = WiFi.localIP().toString();
    String currentSSID = WiFi.SSID();

    if (currentIP != lastPublishedIP || currentSSID != lastPublishedSSID) {
      Serial.println("Wi-Fi IP or SSID changed — updating Firebase presence");
      updateCameraPresence();
      lastPublishedIP = currentIP;
      lastPublishedSSID = currentSSID;
    }

    if (millis() - lastPresenceUpdate > CAMERA_PRESENCE_INTERVAL_MS) {
      updateCameraPresence();
    }

    return;
  }

  if (!wifiConnectInProgress) {
    if (millis() - lastWifiRetry > WIFI_RETRY_INTERVAL_MS) {
      Serial.println("Wi-Fi disconnected — reconnecting...");
      lastWifiRetry = millis();
      startKnownWiFiConnectionCycle();
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectInProgress = false;
    handleConnectedWiFi();
    return;
  }

  if (millis() - wifiAttemptStart > WIFI_CONNECT_TIMEOUT_MS) {
    Serial.printf("Connection failed: %s\n", knownNetworks[candidateNetworkIndex].ssid);

    candidateNetworkIndex++;

    if (candidateNetworkIndex >= KNOWN_WIFI_COUNT) {
      Serial.println("No known Wi-Fi network available — continuing offline");
      wifiConnectInProgress = false;
      currentNetworkIndex = -1;
      return;
    }

    startWiFiAttempt(candidateNetworkIndex);
  }
}

// ============================================================
// FIREBASE CAMERA PRESENCE
// Publishes dynamic IP so the bracelet can trigger:
// http://<published_ip>/capture?alert_id=...
// ============================================================
void updateCameraPresence() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Updating Firebase camera presence...");

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(RTDB_HOST, 443)) {
    Serial.println("RTDB camera presence connect failed");
    return;
  }

  String ip = WiFi.localIP().toString();
  String ssid = WiFi.SSID();
  String zone = getCurrentZoneName();

  String url = "/camera_devices/" + String(DEVICE_ID) + ".json";

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"ip\":\"" + ip + "\",";
  payload += "\"capture_url\":\"http://" + ip + "/capture\",";
  payload += "\"status_url\":\"http://" + ip + "/status\",";
  payload += "\"mdns\":\"http://" + String(MDNS_NAME) + ".local\",";
  payload += "\"ssid\":\"" + ssid + "\",";
  payload += "\"zone\":\"" + zone + "\",";
  payload += "\"online\":true,";
  payload += "\"last_seen_ms\":" + String(millis());
  payload += "}";

  String request =
    "PUT " + url + " HTTP/1.1\r\n"
    "Host: " + String(RTDB_HOST) + "\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + String(payload.length()) + "\r\n"
    "Connection: close\r\n\r\n" + payload;

  client.print(request);

  unsigned long timeout = millis() + 5000;
  while (client.connected() && millis() < timeout) {
    while (client.available()) client.read();
    delay(1);
  }

  lastPresenceUpdate = millis();
  Serial.println("Camera presence updated");
}

// ============================================================
// SD INIT
// ============================================================
void initSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD card not found");
    sdAvailable = false;
  } else {
    Serial.println("SD ready");
    sdAvailable = true;
    if (!SD.exists("/frames")) SD.mkdir("/frames");
  }
}

// ============================================================
// CAMERA INIT
// ============================================================
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count     = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.println("Camera init failed");
    while (true) delay(1000);
  }

  Serial.println("Camera initialized");
}

// ============================================================
// FIREBASE STORAGE UPLOAD
// ============================================================
bool uploadToFirebase(camera_fb_t* fb, String filename, String alertID) {
  WiFiClientSecure client;
  client.setInsecure();

  String url =
    "/v0/b/" + String(FIREBASE_BUCKET) +
    "/o?name=frames%2F" + alertID + "%2F" + filename;

  if (!client.connect(FIREBASE_HOST, 443)) {
    Serial.println("Firebase Storage connect failed");
    return false;
  }

  String request =
    "POST " + url + " HTTP/1.1\r\n"
    "Host: " + String(FIREBASE_HOST) + "\r\n"
    "Content-Type: image/jpeg\r\n"
    "Content-Length: " + String(fb->len) + "\r\n"
    "Connection: close\r\n\r\n";

  client.print(request);
  client.write(fb->buf, fb->len);

  unsigned long timeout = millis() + 10000;
  while (!client.available() && millis() < timeout) delay(10);

  String response = "";
  while (client.available()) response += client.readString();

  bool ok = response.indexOf("\"name\"") >= 0 ||
            response.indexOf("200 OK") >= 0;

  Serial.println(ok ? ("Uploaded: " + filename) : "Upload failed");
  return ok;
}

// ============================================================
// SD SAVE FALLBACK
// ============================================================
void saveToSD(camera_fb_t* fb, String filename) {
  if (!sdAvailable) return;

  File f = SD.open("/frames/" + filename, FILE_WRITE);
  if (!f) {
    Serial.println("SD write failed");
    return;
  }

  f.write(fb->buf, fb->len);
  f.close();

  Serial.println("Saved to SD: " + filename);
}

// ============================================================
// CAPTURE + UPLOAD
// ============================================================
void captureAndUpload(String alertID) {
  Serial.println("Starting capture for: " + alertID);

  for (int i = 0; i < FRAMES_PER_ALERT; i++) {
    server.handleClient();
    maintainWiFi();

    digitalWrite(FLASH_LED, HIGH);
    delay(60);

    camera_fb_t* fb = esp_camera_fb_get();

    digitalWrite(FLASH_LED, LOW);

    if (!fb) {
      Serial.println("Capture failed for frame " + String(i));
      continue;
    }

    currentAlert.captured++;

    String filename = alertID + "_frame_" + String(i) + ".jpg";
    bool ok = false;

    if (WiFi.status() == WL_CONNECTED) {
      currentAlert.state = "UPLOADING";
      ok = uploadToFirebase(fb, filename, alertID);
    }

    if (ok) {
      currentAlert.uploaded++;
    } else {
      saveToSD(fb, filename);
    }

    esp_camera_fb_return(fb);

    updateFrameStatus(alertID, currentAlert.uploaded, FRAMES_PER_ALERT);
    delay(CAPTURE_INTERVAL_MS);
  }

  if (currentAlert.uploaded == FRAMES_PER_ALERT) {
    currentAlert.state = "DONE";
  } else if (currentAlert.uploaded > 0) {
    currentAlert.state = "PARTIAL";
  } else {
    currentAlert.state = "FAILED";
  }

  updateFrameStatus(alertID, currentAlert.uploaded, FRAMES_PER_ALERT);
}

// ============================================================
// HTTP: /capture
// ============================================================
void handleCapture() {
  if (!server.hasArg("alert_id")) {
    server.send(400, "application/json",
      "{\"ack\":\"ERROR\",\"reason\":\"missing alert_id\"}");
    return;
  }

  if (captureInProgress) {
    server.send(503, "application/json",
      "{\"ack\":\"BUSY\",\"reason\":\"capture already in progress\"}");
    return;
  }

  currentAlertID = server.arg("alert_id");
  currentAlert = { "STARTED", 0, 0 };

  Serial.println("Capture request: " + currentAlertID);

  String response = "{";
  response += "\"ack\":\"OK\",";
  response += "\"state\":\"STARTED\",";
  response += "\"device\":\"" + String(DEVICE_ID) + "\",";
  response += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  response += "\"ssid\":\"" + WiFi.SSID() + "\",";
  response += "\"zone\":\"" + getCurrentZoneName() + "\"";
  response += "}";

  server.send(200, "application/json", response);

  captureInProgress = true;
}

// ============================================================
// HTTP: /status
// ============================================================
void handleStatus() {
  String response = "{";
  response += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  response += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  response += "\"ssid\":\"" + WiFi.SSID() + "\",";
  response += "\"zone\":\"" + getCurrentZoneName() + "\",";
  response += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  response += "\"capture_in_progress\":" + String(captureInProgress ? "true" : "false") + ",";
  response += "\"mdns\":\"http://" + String(MDNS_NAME) + ".local\",";
  response += "\"current_alert\":\"" + currentAlertID + "\",";
  response += "\"state\":\"" + currentAlert.state + "\",";
  response += "\"captured\":" + String(currentAlert.captured) + ",";
  response += "\"uploaded\":" + String(currentAlert.uploaded);
  response += "}";

  server.send(200, "application/json", response);
}

// ============================================================
// RTDB: UPDATE FRAME STATUS
// ============================================================
void updateFrameStatus(String alertID, int uploaded, int expected) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping frame status update — Wi-Fi offline");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(RTDB_HOST, 443)) {
    Serial.println("RTDB frame status connect failed");
    return;
  }

  String url = "/active_alerts/" + alertID + ".json";

  String payload = "{";
  payload += "\"frames_uploaded\":" + String(uploaded) + ",";
  payload += "\"frames_expected\":" + String(expected) + ",";
  payload += "\"camera_device\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"camera_ip\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"camera_ssid\":\"" + WiFi.SSID() + "\",";
  payload += "\"camera_zone\":\"" + getCurrentZoneName() + "\"";
  payload += "}";

  String request =
    "PATCH " + url + " HTTP/1.1\r\n"
    "Host: " + String(RTDB_HOST) + "\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + String(payload.length()) + "\r\n"
    "Connection: close\r\n\r\n" + payload;

  client.print(request);

  unsigned long timeout = millis() + 5000;
  while (client.connected() && millis() < timeout) {
    while (client.available()) client.read();
    delay(1);
  }

  Serial.println("Frame status updated: " +
                 String(uploaded) + "/" + String(expected));
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-CAM SOS starting...");

  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  initCamera();
  initSD();

  startKnownWiFiConnectionCycle();

  unsigned long bootConnectDeadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && millis() < bootConnectDeadline) {
    maintainWiFi();
    delay(20);
  }

  if (WiFi.status() == WL_CONNECTED) {
    handleConnectedWiFi();
  } else {
    Serial.println("Boot Wi-Fi timeout — HTTP server will start and reconnect in loop");
  }

  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();

  Serial.println("Camera HTTP server ready");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient();
  maintainWiFi();

  if (captureInProgress) {
    captureInProgress = false;
    captureAndUpload(currentAlertID);
  }
}
