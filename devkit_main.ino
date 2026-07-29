#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <MPU6050.h>
#include <TinyGPSPlus.h>
#include <time.h>

// ============================================================
// FIREBASE
// ============================================================
const char* FIREBASE_HOST =
  "safecampus-4c8a2-default-rtdb.europe-west1.firebasedatabase.app";

// ============================================================
// CAMERA DISCOVERY
// ============================================================
const char* PRIMARY_CAMERA_ID = "ESP32_CAM_01";

// ============================================================
// PINS
// ============================================================
#define BUTTON_PIN   4
#define LED_PIN      2
#define WIFI_LED_PIN 5
#define BUZZER_PIN   18

#define GPS_RX_PIN  16
#define GPS_TX_PIN  17
#define GPS_BAUD    9600

// ============================================================
// TIMING CONSTANTS
// ============================================================
#define CANCEL_WINDOW_MS        5000
#define DOUBLE_PRESS_WINDOW_MS  1500
#define TRIGGER_COOLDOWN_MS     8000
#define WIFI_RETRY_INTERVAL     30000
#define ACK_POLL_INTERVAL       5000
#define WIFI_CONNECT_TIMEOUT    15000
#define CAM_HTTP_TIMEOUT_MS     3000
#define GPS_FIX_MAX_AGE_MS      8000

#define MAX_POLYGON_POINTS 8

struct PolygonPoint {
  float lat;
  float lng;
};

struct KnownWiFi {
  const char* ssid;
  const char* password;
  const char* zone_name;
  PolygonPoint polygon[MAX_POLYGON_POINTS];
  int point_count;
};

const KnownWiFi knownNetworks[] = {
  {
    "Hailey Lebopo's A32",
    "htfa7479",
    "FOB",
    {
      { -24.661232459360363f, 25.936525630939430f },
      { -24.662170702732567f, 25.936752037193514f },
      { -24.662195393252148f, 25.937657662205936f },
      { -24.661199538411820f, 25.937884068459056f },
      { -24.661232459360363f, 25.936525630939430f }
    },
    5
  },
  {
    "Flybox_DA7E",
    "w3xdi4yte6ea",
    "FET",
    {
      { -24.661586359004460f, 25.934515143411630f },
      { -24.661989638771330f, 25.933917430902824f },
      { -24.663199470252210f, 25.934324962158428f },
      { -24.662639821773254f, 25.936561855940310f },
      { -24.661619279850940f, 25.936616193441694f }
    },
    5
  }
};

const int KNOWN_WIFI_COUNT = sizeof(knownNetworks) / sizeof(knownNetworks[0]);
int currentWifiZone = -1;

struct LocationResult {
  String source;
  String zone;
  float lat;
  float lng;
  bool valid;
};

TinyGPSPlus gpsParser;
bool gpsSearchActive = false;

MPU6050 mpu;
int spikeCount = 0;
unsigned long firstSpikeTime = 0;

bool alertPending = false;
unsigned long alertStartTime = 0;
String pendingSource = "";

int cancelPressCount = 0;
unsigned long lastCancelPress = 0;

unsigned long lastTriggerTime = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastAckCheck = 0;

String lastAlertID = "";
bool awaitingAck = false;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void feedGPS();
void triggerCamera(const String& alertID);

// ============================================================
// BUZZER HELPERS
// ============================================================
void buzzerOn()  { digitalWrite(BUZZER_PIN, HIGH); }
void buzzerOff() { digitalWrite(BUZZER_PIN, LOW); }

void beep(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    buzzerOn();  delay(onMs);
    buzzerOff(); delay(offMs);
  }
}

// ============================================================
// CONNECTED WIFI ZONE MATCHING
// ============================================================
int getConnectedWifiZone() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connected Wi-Fi zone check failed: Wi-Fi is not connected");
    return -1;
  }

  String connectedSSID = WiFi.SSID();

  Serial.println("Checking active Wi-Fi connection for location mapping...");
  Serial.println("Connected SSID: " + connectedSSID);

  for (int i = 0; i < KNOWN_WIFI_COUNT; i++) {
    if (connectedSSID == String(knownNetworks[i].ssid)) {
      Serial.printf("Mapped location zone: %s\n", knownNetworks[i].zone_name);
      return i;
    }
  }

  Serial.println("Active Wi-Fi connection does not match a known location zone");
  return -1;
}

// ============================================================
// WIFI CONNECTION MANAGER
// ============================================================
bool connectKnownWiFi() {
  Serial.println("Attempting known Wi-Fi connections...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  delay(100);

  for (int i = 0; i < KNOWN_WIFI_COUNT; i++) {
    Serial.printf("Trying network: %s\n", knownNetworks[i].ssid);

    WiFi.begin(knownNetworks[i].ssid, knownNetworks[i].password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < WIFI_CONNECT_TIMEOUT) {
      delay(250);
      feedGPS();
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connection successful");
      Serial.println("Connected to known network: " + WiFi.SSID());
      Serial.println("IP address: " + WiFi.localIP().toString());

      currentWifiZone = getConnectedWifiZone();

      if (currentWifiZone >= 0) {
        Serial.printf("Mapped zone: %s\n", knownNetworks[currentWifiZone].zone_name);
      } else {
        Serial.println("Connected network has no mapped zone");
      }

      digitalWrite(WIFI_LED_PIN, HIGH);
      return true;
    }
    Serial.printf("Connection failed: %s\n", knownNetworks[i].ssid);
    WiFi.disconnect(false, true);
    delay(200);
  }

  currentWifiZone = -1;
  digitalWrite(WIFI_LED_PIN, LOW);
  Serial.println("No known Wi-Fi network available");
  return false;
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    int mappedZone = getConnectedWifiZone();

    if (mappedZone != currentWifiZone) {
      currentWifiZone = mappedZone;
      if (currentWifiZone >= 0) {
        Serial.printf("Active Wi-Fi zone updated: %s\n",
                      knownNetworks[currentWifiZone].zone_name);
      }
    }

    digitalWrite(WIFI_LED_PIN, HIGH);
    return;
  }

  currentWifiZone = -1;
  digitalWrite(WIFI_LED_PIN, LOW);

  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL) return;

  lastWifiRetry = millis();
  Serial.println("WiFi lost — reconnecting to known networks...");
  connectKnownWiFi();
}

// ============================================================
// GPS
// ============================================================
void feedGPS() {
  while (Serial2.available() > 0) {
    gpsParser.encode(Serial2.read());
  }
}

LocationResult getGPSLocation() {
  LocationResult result;
  result.valid = false;
  result.source = "GPS";
  result.zone = "";
  result.lat = 0.0f;
  result.lng = 0.0f;

  if (!gpsParser.location.isValid()) {
    Serial.println("Waiting for GPS fix...");
    return result;
  }

  if (gpsParser.location.age() > GPS_FIX_MAX_AGE_MS) {
    Serial.println("GPS fix too old — discarding");
    return result;
  }

  result.lat = (float)gpsParser.location.lat();
  result.lng = (float)gpsParser.location.lng();
  result.valid = true;

  Serial.printf("GPS fix acquired | lat=%.6f lng=%.6f sats=%u hdop=%.2f\n",
                result.lat,
                result.lng,
                gpsParser.satellites.isValid() ? gpsParser.satellites.value() : 0,
                gpsParser.hdop.isValid() ? gpsParser.hdop.hdop() : 0.0);

  return result;
}

// ============================================================
// POLYGON FALLBACK COORDINATE
// ============================================================
void generateZoneCoordinate(int zoneIndex, float &lat, float &lng) {
  const KnownWiFi& zone = knownNetworks[zoneIndex];

  int a = random(0, zone.point_count);
  int b = random(0, zone.point_count);
  float t = (float)random(0, 101) / 100.0f;

  lat = zone.polygon[a].lat + t * (zone.polygon[b].lat - zone.polygon[a].lat);
  lng = zone.polygon[a].lng + t * (zone.polygon[b].lng - zone.polygon[a].lng);

  Serial.println("Generated polygon fallback coordinate");
  Serial.printf("Fallback coordinate: %.6f, %.6f | zone=%s\n",
                lat, lng, zone.zone_name);
}

// ============================================================
// MASTER LOCATION RESOLVER
// ============================================================
LocationResult getLocation() {
  LocationResult gpsResult = getGPSLocation();

  if (gpsResult.valid) {
    return gpsResult;
  }

  Serial.println("GPS unavailable — using connected Wi-Fi fallback");

  int zoneIdx = getConnectedWifiZone();

  if (zoneIdx >= 0) {
    currentWifiZone = zoneIdx;

    LocationResult wifiResult;
    wifiResult.source = "Connected WiFi Polygon Fallback";
    wifiResult.zone = String(knownNetworks[zoneIdx].zone_name);
    wifiResult.valid = true;

    generateZoneCoordinate(zoneIdx, wifiResult.lat, wifiResult.lng);
    return wifiResult;
  }

  LocationResult none;
  none.source = "Unavailable";
  none.zone = "";
  none.valid = false;
  none.lat = 0.0f;
  none.lng = 0.0f;

  Serial.println("Location unavailable — no GPS fix and active Wi-Fi is unmapped");
  return none;
}

// ============================================================
// ALERT ID / TIMESTAMP
// ============================================================
String generateAlertID() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "ALERT_FALLBACK_" + String(millis());
  }

  char buf[30];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &timeinfo);
  return "ALERT_" + String(buf) + "_" + String(random(1000, 9999));
}

void formatTimestamp(char* strBuf,
                     size_t strSize,
                     char* unixBuf,
                     size_t unixSize,
                     bool& validOut) {
  time_t now;
  time(&now);
  validOut = now > 100000;

  if (validOut) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(strBuf, strSize, "%Y-%m-%d %H:%M:%S", &timeinfo);
    snprintf(unixBuf, unixSize, "%ld", (long)now);
  } else {
    strncpy(strBuf, "unavailable", strSize);
    strncpy(unixBuf, "null", unixSize);
  }
}

// ============================================================
// BUILD ALERT PAYLOAD
// ============================================================
String buildPayload(const String& triggerSource,
                    const String& alertID,
                    const LocationResult& loc) {
  char tsStr[25], tsUnix[15];
  bool validTime;
  formatTimestamp(tsStr, sizeof(tsStr), tsUnix, sizeof(tsUnix), validTime);

  String payload = "{";
  payload += "\"alert_id\":\"" + alertID + "\",";
  payload += "\"device_id\":\"SOS_BRACELET_01\",";
  payload += "\"trigger\":\"" + triggerSource + "\",";
  payload += "\"timestamp\":\"" + String(tsStr) + "\",";
  payload += "\"timestamp_unix\":" + String(tsUnix) + ",";
  payload += "\"time_valid\":" + String(validTime ? "true" : "false") + ",";

  payload += "\"gps\":{";
  if (loc.valid) {
    payload += "\"lat\":" + String(loc.lat, 6) + ",";
    payload += "\"lng\":" + String(loc.lng, 6) + ",";
    payload += "\"source\":\"" + loc.source + "\"";

    if (loc.source != "GPS" && loc.zone.length() > 0) {
      payload += ",\"zone\":\"" + loc.zone + "\"";
      payload += ",\"connected_ssid\":\"" + WiFi.SSID() + "\"";
    }
  } else {
    payload += "\"source\":\"Unavailable\"";
  }
  payload += "},";

  payload += "\"status\":\"ACTIVE\",";
  payload += "\"acknowledged\":false,";
  payload += "\"ai_status\":\"PENDING\"";
  payload += "}";

  return payload;
}

// ============================================================
// FIREBASE RTDB HELPERS
// ============================================================
String stripFirebaseString(String value) {
  value.trim();

  if (value == "null" || value.length() == 0) {
    return "";
  }

  if (value.startsWith("\"") && value.endsWith("\"") && value.length() >= 2) {
    value = value.substring(1, value.length() - 1);
  }

  value.replace("\\\"", "\"");
  value.trim();

  return value;
}

bool firebaseGET(const String& path, String& responseOut) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase GET skipped — Wi-Fi disconnected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://" + String(FIREBASE_HOST) + path;

  http.begin(client, url);
  http.setTimeout(5000);

  int code = http.GET();

  if (code != 200) {
    Serial.printf("Firebase GET failed | path=%s code=%d\n", path.c_str(), code);
    http.end();
    return false;
  }

  responseOut = http.getString();
  http.end();

  return true;
}

bool isValidIPv4(const String& ip) {
  if (ip.length() < 7 || ip.length() > 15) return false;

  int dots = 0;
  int segmentValue = 0;
  int segmentDigits = 0;

  for (int i = 0; i < ip.length(); i++) {
    char c = ip.charAt(i);

    if (c == '.') {
      if (segmentDigits == 0 || segmentValue > 255) return false;
      dots++;
      segmentValue = 0;
      segmentDigits = 0;
      continue;
    }

    if (c < '0' || c > '9') return false;

    segmentValue = segmentValue * 10 + (c - '0');
    segmentDigits++;

    if (segmentDigits > 3 || segmentValue > 255) return false;
  }

  return dots == 3 && segmentDigits > 0 && segmentValue <= 255;
}

// ============================================================
// CAMERA DISCOVERY
// ============================================================
String getCameraIP(const String& cameraID = PRIMARY_CAMERA_ID) {
  Serial.println("Requesting camera IP from Firebase...");

  String response;
  String path = "/camera_devices/" + cameraID + "/ip.json";

  if (!firebaseGET(path, response)) {
    Serial.println("Camera unavailable");
    return "";
  }

  String ip = stripFirebaseString(response);

  if (!isValidIPv4(ip)) {
    Serial.println("Invalid camera IP");
    Serial.println("Camera IP response: " + response);
    return "";
  }

  Serial.println("Camera IP found: " + ip);
  return ip;
}

bool isCameraOnline(const String& cameraID = PRIMARY_CAMERA_ID) {
  Serial.println("Checking camera online status...");

  String response;
  String path = "/camera_devices/" + cameraID + "/online.json";

  if (!firebaseGET(path, response)) {
    Serial.println("Camera online check failed");
    return false;
  }

  response.trim();

  if (response == "true") {
    Serial.println("Camera online");
    return true;
  }

  Serial.println("Camera unavailable");
  Serial.println("Camera online response: " + response);
  return false;
}

// ============================================================
// SEND ALERT TO FIREBASE
// ============================================================
bool sendAlertHTTP(const String& payload, const String& alertID) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://" + String(FIREBASE_HOST) +
               "/active_alerts/" + alertID + ".json";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(7000);

  int code = http.PUT(payload);
  http.end();

  Serial.printf("Firebase PUT response: %d\n", code);
  return code == 200 || code == 201;
}

// ============================================================
// DYNAMIC CAMERA TRIGGER
// ============================================================
void triggerCamera(const String& alertID) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Camera trigger skipped — Wi-Fi disconnected");
    return;
  }

  if (!isCameraOnline(PRIMARY_CAMERA_ID)) {
    Serial.println("Camera trigger skipped — camera offline");
    return;
  }

  String cameraIP = getCameraIP(PRIMARY_CAMERA_ID);

  if (cameraIP.length() == 0) {
    Serial.println("Camera trigger skipped — no IP available");
    return;
  }

  String url = "http://" + cameraIP + "/capture?alert_id=" + alertID;

  Serial.println("Sending capture request to dynamic camera IP");
  Serial.println("Camera URL: " + url);

  WiFiClient cameraClient;
  HTTPClient http;

  if (!http.begin(cameraClient, url)) {
    Serial.println("Camera HTTP begin failed");
    return;
  }

  http.setTimeout(CAM_HTTP_TIMEOUT_MS);

  int code = http.GET();
  String response = http.getString();

  Serial.printf("Camera HTTP response: %d\n", code);

  if (code <= 0) {
    Serial.println("Camera HTTP error: " + http.errorToString(code));
  } else if (code != 200) {
    Serial.println("Camera returned non-OK response: " + response);
  }

  http.end();
}

// ============================================================
// OFFLINE FALLBACK
// ============================================================
void emergencyOfflineMode(const String& alertID) {
  Serial.println("OFFLINE — emergency buzzer mode");

  for (int i = 0; i < 20; i++) {
    buzzerOn();  delay(150);
    buzzerOff(); delay(150);
  }

  triggerCamera(alertID);
}

// ============================================================
// HANDLE PENDING ALERT
// ============================================================
void handlePendingAlert() {
  if (!alertPending) return;

  if (millis() - alertStartTime > CANCEL_WINDOW_MS) {
    Serial.println("Cancel window expired — resolving location...");

    LocationResult loc = getLocation();

    String alertID = generateAlertID();
    String payload = buildPayload(pendingSource, alertID, loc);

    Serial.println("Sending alert: " + alertID);
    Serial.println("Location source: " + loc.source);

    if (sendAlertHTTP(payload, alertID)) {
      beep(2, 80, 80);
      triggerCamera(alertID);
      lastAlertID = alertID;
      awaitingAck = true;
    } else {
      emergencyOfflineMode(alertID);
    }

    alertPending = false;
    cancelPressCount = 0;
    lastTriggerTime = millis();
    digitalWrite(LED_PIN, LOW);
  }
}

// ============================================================
// BUTTON HANDLER
// ============================================================
void handleButton() {
  static bool lastState = HIGH;
  static unsigned long pressStart = 0;

  bool current = digitalRead(BUTTON_PIN);

  if (lastState == HIGH && current == LOW) {
    pressStart = millis();
  }

  if (lastState == LOW && current == HIGH) {
    if (alertPending) {
      unsigned long now = millis();

      if (now - lastCancelPress < DOUBLE_PRESS_WINDOW_MS) {
        cancelPressCount++;
      } else {
        cancelPressCount = 1;
        Serial.println("Cancel: 1 press — press again within 1.5s to cancel alert");
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
      }

      lastCancelPress = now;

      if (cancelPressCount >= 2) {
        alertPending = false;
        cancelPressCount = 0;
        digitalWrite(LED_PIN, LOW);
        beep(3, 60, 60);
        Serial.println("*** Alert CANCELLED by double-press ***");
      }

      lastState = current;
      return;
    }

    if (millis() - lastTriggerTime > TRIGGER_COOLDOWN_MS) {
      alertPending = true;
      alertStartTime = millis();
      cancelPressCount = 0;
      pendingSource = "BUTTON";
      gpsSearchActive = true;

      digitalWrite(LED_PIN, HIGH);
      beep(2, 100, 300);
      Serial.println("BUTTON alert triggered — press twice within 5s to cancel");
    }
  }

  lastState = current;
}

// ============================================================
// MPU / ACCELEROMETER
// ============================================================
void checkMPU() {
  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float g = sqrt(
    pow(ax / 16384.0f, 2) +
    pow(ay / 16384.0f, 2) +
    pow(az / 16384.0f, 2)
  );

  if (g > 2.2f) {
    if (spikeCount == 0) firstSpikeTime = millis();
    spikeCount++;
  }

  if (spikeCount > 0 && millis() - firstSpikeTime > 1000) {
    if (spikeCount >= 2 &&
        !alertPending &&
        millis() - lastTriggerTime > TRIGGER_COOLDOWN_MS) {

      alertPending = true;
      alertStartTime = millis();
      cancelPressCount = 0;
      pendingSource = "MPU";
      gpsSearchActive = true;

      digitalWrite(LED_PIN, HIGH);
      beep(2, 100, 300);
      Serial.println("MPU alert triggered — press twice within 5s to cancel");
    }

    spikeCount = 0;
  }
}

// ============================================================
// ACKNOWLEDGEMENT CHECK
// ============================================================
bool checkAcknowledgement(const String& alertID) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String response;
  String path = "/active_alerts/" + alertID + "/acknowledged.json";

  if (!firebaseGET(path, response)) {
    return false;
  }

  response.trim();
  return response == "true";
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("SOS Bracelet starting...");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(WIFI_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  Wire.begin(21, 22);
  mpu.initialize();
  Serial.println("MPU6050 ready");

  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("Starting GPS...");

  connectKnownWiFi();

  configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP time sync requested");

  Serial.println("Ready.");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  feedGPS();
  maintainWiFi();

  handleButton();
  checkMPU();
  handlePendingAlert();

  if (awaitingAck && WiFi.status() == WL_CONNECTED) {
    if (millis() - lastAckCheck > ACK_POLL_INTERVAL) {
      lastAckCheck = millis();

      if (checkAcknowledgement(lastAlertID)) {
        beep(4, 50, 50);
        awaitingAck = false;
        Serial.println("Alert acknowledged by dashboard");
      }
    }
  }

  delay(20);
}
