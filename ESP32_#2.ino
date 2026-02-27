#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <TinyGPSPlus.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_wpa2.h"

/* ===================== CONFIG ===================== */
const char* apSSID = "CATS-ALARM";
const char* apPassword = "secure123";

#define USE_ENTERPRISE_WIFI true
const char* extSSID = "ESUHSD";
const char* extUsername = ".......";  //  email
const char* extPassword = ".........";  //  password
const char* extSSID = "ESUHSD";
const char* extPassword = "......";

// Telegram Bot
const char* telegramToken = ".....";
const char* telegramChatID = "......";

/* ===================== OBJECTS ===================== */
WebServer server(80);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);  // GPS on UART1
HTTPClient http;

/* ===================== STATE ===================== */
String systemStatus = "DISARMED";
String lastAlert = "None";
String alertLevel = "0";
String gpsLocation = "Acquiring...";
unsigned long lastAlertTime = 0;
unsigned long systemArmedTime = 0;

float lastLat = 0.0;
float lastLng = 0.0;
bool gpsValid = false;
bool internetConnected = false;

// Telegram message queue
String pendingTelegramMessage = "";
bool telegramPending = false;

/* ===================== ALERT MESSAGE ===================== */
typedef struct {
  uint8_t level;  // 0=disarm, 1=warning, 2=alert, 3=armed
  float lat;
  float lng;
} AlertMsg;

AlertMsg receivedMsg;

/* ===================== TELEGRAM ===================== */
void sendTelegram(String message) {
  Serial.print("📱 Telegram attempt... Internet: ");
  Serial.println(internetConnected ? "YES" : "NO");
  
  if (!internetConnected) {
    Serial.println("❌ Skipped - no internet");
    return;
  }

  // Small delay to let system settle
  delay(100);

  String url = "https://api.telegram.org/bot";
  url += telegramToken;
  url += "/sendMessage?chat_id=";
  url += telegramChatID;
  url += "&text=";
  
  for (int i = 0; i < message.length(); i++) {
    char c = message[i];
    if (c == ' ')       url += "%20";
    else if (c == '\n') url += "%0A";
    else if (c == '!')  url += "%21";
    else                url += c;
  }

  Serial.println("   Sending HTTPS request...");
  
  // create secure WiFi client
  WiFiClientSecure client;
  client.setInsecure();  

  HTTPClient https;
  https.begin(client, url);
  https.setTimeout(15000); 
  https.addHeader("User-Agent", "ESP32");
  
  int httpCode = https.GET();

  Serial.print("   HTTP code: ");
  Serial.println(httpCode);
  
  if (httpCode == HTTP_CODE_OK) {
    Serial.println("   ✅ Telegram sent");
  } else {
    Serial.println("   ❌ Failed");
  }
  https.end();
}

/* ===================== ESP-NOW CALLBACK ===================== */
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(AlertMsg)) return;
  
  memcpy(&receivedMsg, data, sizeof(receivedMsg));
  
  switch (receivedMsg.level) {
    case 0:  // Disarm
      systemStatus = "DISARMED";
      lastAlert = "None";
      alertLevel = "0";
      Serial.println("🔓 Disarmed");
      pendingTelegramMessage = "🔓 CATS Security: Your vehicle has been disarmed.";
      telegramPending = true;
      break;
      
    case 1:  // Warning
      systemStatus = "ARMED - Warning";
      lastAlert = "Motion Warning";
      alertLevel = "1";
      lastAlertTime = millis();
      Serial.println("Warning");
      pendingTelegramMessage = "CATS Alert: Suspicious activity detected near your vehicle.";
      telegramPending = true;
      break;
      
    case 2:  // Critical Alert
      systemStatus = "CRITICAL ALERT";
      lastAlert = "Vehicle Intrusion";
      alertLevel = "2";
      lastAlertTime = millis();
      
      // Use GPS coordinates
      if (receivedMsg.lat != 0.0 && receivedMsg.lng != 0.0) {
        lastLat = receivedMsg.lat;
        lastLng = receivedMsg.lng;
        gpsValid = true;
      } else if (gps.location.isValid()) {
        lastLat = gps.location.lat();
        lastLng = gps.location.lng();
        gpsValid = true;
      }
      
      Serial.println("🚨 CRITICAL ALERT");
      
      // Queue Telegram with GPS if available
      if (gpsValid) {
        pendingTelegramMessage = "🚨 CATS SECURITY BREACH\n\n";
        pendingTelegramMessage += "Vehicle intrusion detected. Possible theft attempt in progress.\n\n";
        pendingTelegramMessage += "Location: ";
        pendingTelegramMessage += String(lastLat, 6);
        pendingTelegramMessage += ", ";
        pendingTelegramMessage += String(lastLng, 6);
        pendingTelegramMessage += "\n\n";
        pendingTelegramMessage += "View on map: https://maps.google.com/?q=";
        pendingTelegramMessage += String(lastLat, 6);
        pendingTelegramMessage += ",";
        pendingTelegramMessage += String(lastLng, 6);
      } else {
        pendingTelegramMessage = "🚨 CATS SECURITY BREACH. Vehicle intrusion detected. Possible theft attempt. GPS location unavailable.";
      }
      telegramPending = true;
      break;
      
    case 3:  // Armed
      systemStatus = "ARMED";
      lastAlert = "System Armed";
      alertLevel = "3";
      lastAlertTime = millis();
      systemArmedTime = millis();
      Serial.println("🔒 Armed");
      pendingTelegramMessage = "🚗 CATS Security: Your vehicle is now ARMED and protected.";
      telegramPending = true;
      break;
  }
}

/* ===================== WEBPAGE ===================== */
String htmlPage() {
  // Calculate uptime
  unsigned long uptime = millis() / 1000;
  String uptimeStr = String(uptime / 3600) + "h " + String((uptime % 3600) / 60) + "m";
  
  // Time since last alert
  String timeSince = "N/A";
  if (lastAlertTime > 0) {
    unsigned long seconds = (millis() - lastAlertTime) / 1000;
    if (seconds < 60) {
      timeSince = String(seconds) + "s ago";
    } else if (seconds < 3600) {
      timeSince = String(seconds / 60) + "m ago";
    } else {
      timeSince = String(seconds / 3600) + "h ago";
    }
  }
  
  // Status color
  String statusColor = "#00ff00";  // Green
  if (systemStatus.indexOf("Warning") >= 0) {
    statusColor = "#ffaa00";  // Orange
  } else if (systemStatus.indexOf("ALERT") >= 0) {
    statusColor = "#ff0000";  // Red
  } else if (systemStatus.indexOf("ARMED") >= 0) {
    statusColor = "#00aaff";  // Blue
  }

  // Internet status
  String internetStatus = internetConnected ? "Online" : "Offline";
  String internetColor = internetConnected ? "#00ff00" : "#ff0000";
  
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <meta http-equiv='refresh' content='2'>
  <title>CATS Monitoring</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;
      background: linear-gradient(135deg, #1e1e1e 0%, #0a0a0a 100%);
      color: #fff;
      min-height: 100vh;
      padding: 20px;
    }
    
    .container {
      max-width: 600px;
      margin: 0 auto;
    }
    
    h1 {
      text-align: center;
      font-size: 2em;
      margin-bottom: 10px;
      background: linear-gradient(135deg, #00ffcc, #00aaff);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
    }
    
    .subtitle {
      text-align: center;
      color: #888;
      font-size: 0.9em;
      margin-bottom: 30px;
    }
    
    .status-card {
      background: rgba(255, 255, 255, 0.05);
      border: 2px solid )rawliteral" + statusColor + R"rawliteral(;
      border-radius: 15px;
      padding: 30px;
      margin-bottom: 20px;
      text-align: center;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
      backdrop-filter: blur(10px);
    }
    
    .status-label {
      font-size: 0.9em;
      color: #aaa;
      text-transform: uppercase;
      letter-spacing: 2px;
      margin-bottom: 10px;
    }
    
    .status-value {
      font-size: 2em;
      font-weight: bold;
      color: )rawliteral" + statusColor + R"rawliteral(;
      text-shadow: 0 0 20px )rawliteral" + statusColor + R"rawliteral(;
    }
    
    .info-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 15px;
      margin-bottom: 20px;
    }
    
    .info-card {
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.1);
      border-radius: 10px;
      padding: 20px;
      text-align: center;
    }
    
    .info-label {
      font-size: 0.8em;
      color: #888;
      margin-bottom: 8px;
    }
    
    .info-value {
      font-size: 1.2em;
      font-weight: bold;
      color: #fff;
    }
    
    .gps-card {
      background: rgba(0, 170, 255, 0.1);
      border: 1px solid rgba(0, 170, 255, 0.3);
      border-radius: 10px;
      padding: 20px;
      margin-bottom: 20px;
    }
    
    .gps-title {
      font-size: 1.1em;
      margin-bottom: 10px;
      color: #00aaff;
    }
    
    .gps-coords {
      font-family: 'Courier New', monospace;
      font-size: 0.9em;
      color: #aaa;
      margin-bottom: 10px;
    }
    
    .maps-link {
      display: inline-block;
      background: #00aaff;
      color: #000;
      padding: 10px 20px;
      border-radius: 5px;
      text-decoration: none;
      font-weight: bold;
      transition: background 0.3s;
    }
    
    .maps-link:hover {
      background: #00ffcc;
    }

    .telegram-card {
      background: rgba(39, 174, 96, 0.1);
      border: 1px solid rgba(39, 174, 96, 0.3);
      border-radius: 10px;
      padding: 15px 20px;
      margin-bottom: 20px;
      display: flex;
      align-items: center;
      gap: 15px;
    }

    .telegram-icon {
      font-size: 1.8em;
    }

    .telegram-info {
      flex: 1;
    }

    .telegram-title {
      font-size: 1em;
      color: #27ae60;
      font-weight: bold;
    }

    .telegram-sub {
      font-size: 0.8em;
      color: #888;
    }

    .internet-dot {
      display: inline-block;
      width: 10px;
      height: 10px;
      border-radius: 50%;
      margin-right: 5px;
    }
    
    .footer {
      text-align: center;
      color: #555;
      font-size: 0.8em;
      margin-top: 30px;
      padding-top: 20px;
      border-top: 1px solid rgba(255, 255, 255, 0.1);
    }
    
    .pulse {
      animation: pulse 2s infinite;
    }
    
    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
    
    @media (max-width: 600px) {
      .info-grid { grid-template-columns: 1fr; }
      h1 { font-size: 1.5em; }
      .status-value { font-size: 1.5em; }
    }
  </style>
</head>
<body>
  <div class='container'>
    <h1>CATS</h1>
    <div class='subtitle'>Car Alarm Text System</div>
    
    <div class='status-card )rawliteral" + String(alertLevel == "2" ? "pulse" : "") + R"rawliteral('>
      <div class='status-label'>System Status</div>
      <div class='status-value'>)rawliteral" + systemStatus + R"rawliteral(</div>
    </div>
)rawliteral";

  // Only show Telegram card if internet is connected
  if (internetConnected) {
    page += R"rawliteral(
    <div class='telegram-card'>
      <div class='telegram-icon'>✈️</div>
      <div class='telegram-info'>
        <div class='telegram-title'>Telegram Notifications</div>
        <div class='telegram-sub'>
          <span class='internet-dot' style='background: #00ff00;'></span>
          Internet: Online | Push alerts active
        </div>
      </div>
    </div>
)rawliteral";
  }
  
  page += R"rawliteral(
    
    <div class='info-grid'>
      <div class='info-card'>
        <div class='info-label'>Last Alert</div>
        <div class='info-value'>)rawliteral" + lastAlert + R"rawliteral(</div>
      </div>
      <div class='info-card'>
        <div class='info-label'>Time Since</div>
        <div class='info-value'>)rawliteral" + timeSince + R"rawliteral(</div>
      </div>
      <div class='info-card'>
        <div class='info-label'>GPS Status</div>
        <div class='info-value'>)rawliteral" + String(gpsValid ? "Locked" : gpsLocation) + R"rawliteral(</div>
      </div>
      <div class='info-card'>
        <div class='info-label'>Uptime</div>
        <div class='info-value'>)rawliteral" + uptimeStr + R"rawliteral(</div>
      </div>
    </div>
)rawliteral";

  // GPS location card
  if (gpsValid && lastLat != 0.0) {
    String mapsUrl = "https://maps.google.com/?q=" + String(lastLat, 6) + "," + String(lastLng, 6);
    
    page += R"rawliteral(
    <div class='gps-card'>
      <div class='gps-title'>📍 Vehicle Location</div>
      <div class='gps-coords'>
        Lat: )rawliteral" + String(lastLat, 6) + R"rawliteral(<br>
        Lng: )rawliteral" + String(lastLng, 6) + R"rawliteral(
      </div>
      <a href=')rawliteral" + mapsUrl + R"rawliteral(' target='_blank' class='maps-link'>
        Open in Google Maps
      </a>
    </div>
)rawliteral";
  }
  
  page += R"rawliteral(
    <div class='footer'>
      Auto-refreshes every 2 seconds<br>
      Connected to: )rawliteral" + String(apSSID) + R"rawliteral(<br>
      ESP32 #2 Communication Node | Telegram Enabled
    </div>
  </div>
</body>
</html>
)rawliteral";

  return page;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

/* ===================== WIFI SETUP ===================== */
void connectExternalWiFi() {
  Serial.print("📶 Connecting WiFi: ");
  Serial.println(extSSID);
  
#ifdef USE_ENTERPRISE_WIFI
  Serial.println("   Using WPA2-Enterprise authentication...");
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  
  // Configure enterprise settings
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)extUsername, strlen(extUsername));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t *)extUsername, strlen(extUsername));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t *)extPassword, strlen(extPassword));
  esp_wifi_sta_wpa2_ent_enable();
  
  // Connect
  WiFi.begin(extSSID);
  
#else
  // Simple WPA2 (Home WiFi)
  WiFi.begin(extSSID, extPassword);
#endif
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    internetConnected = true;
    
    // Configure DNS servers
    IPAddress dns1(8, 8, 8, 8);      
    IPAddress dns2(1, 1, 1, 1);      
    IPAddress gateway = WiFi.gatewayIP();
    IPAddress subnet = WiFi.subnetMask();
    IPAddress local_ip = WiFi.localIP();
    
    WiFi.config(local_ip, gateway, subnet, dns1, dns2);
    
    Serial.print("\n✅ WiFi connected | IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("   Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("   DNS: ");
    Serial.print(WiFi.dnsIP(0));
    Serial.print(", ");
    Serial.println(WiFi.dnsIP(1));
  } else {
    internetConnected = false;
    Serial.println("\n❌ WiFi failed - Telegram disabled");
  }
}

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== CATS ESP32 #2 - COMM HUB ===");
  
  // GPS
  Serial.println("🛰️  GPS init...");
  gpsSerial.begin(9600, SERIAL_8N1, 4, 2);
  delay(1000);
  
  int testBytes = 0;
  unsigned long testStart = millis();
  while (millis() - testStart < 2000) {
    if (gpsSerial.available()) {
      testBytes++;
      gpsSerial.read();
    }
  }
  
  if (testBytes > 0) {
    Serial.print("✅ GPS detected (");
    Serial.print(testBytes);
    Serial.println(" bytes)");
  } else {
    Serial.println("❌ No GPS data");
  }
  
  // WiFi - AP + STA
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  
  // Force channel 1 for ESP-NOW compatibility
  WiFi.softAP(apSSID, apPassword, 1);  // Channel 1
  
  // Print MAC addresses for verification
  Serial.print("\n📍 My STA MAC (for ESP-NOW): ");
  Serial.println(WiFi.macAddress());
  Serial.print("📍 My AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.print("📡 WiFi Channel: ");
  Serial.println(WiFi.channel());
  
  IPAddress ip = WiFi.softAPIP();
  Serial.print("📶 AP: ");
  Serial.print(apSSID);
  Serial.print(" | http://");
  Serial.println(ip);
  
  // Connect to external WiFi for Telegram
  connectExternalWiFi();
  
  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW failed");
    return;
  }
  esp_now_register_recv_cb(onReceive);
  
  uint8_t esp32_1_mac[] = {0xD0, 0xEF, 0x76, 0x58, 0x6E, 0xC4};
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, esp32_1_mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  
  if (esp_now_add_peer(&peer) == ESP_OK) {
    Serial.println("✅ ESP-NOW ready");
  }
  
  // Web server
  server.on("/", handleRoot);
  server.begin();
  Serial.println("✅ Web server ready");

  // Queue test Telegram message if internet connected
  if (internetConnected) {
    pendingTelegramMessage = "CATS System Online";
    telegramPending = true;
  }
  
  Serial.println("\n=== SYSTEM READY ===\n");
}

/* ===================== LOOP ===================== */
void loop() {
  server.handleClient();
  updateGPS();
  
  // Send pending Telegram messages
  if (telegramPending) {
    sendTelegram(pendingTelegramMessage);
    telegramPending = false;
    pendingTelegramMessage = "";
  }
  
  //  check internet connection
  static unsigned long lastInternetCheck = 0;
  if (millis() - lastInternetCheck > 30000) {
    lastInternetCheck = millis();
    if (WiFi.status() != WL_CONNECTED && !internetConnected) {
      connectExternalWiFi();
    }
  }
  
  // Show GPS status 
  static unsigned long lastGPSStatus = 0;
  if (millis() - lastGPSStatus > 30000) {  // Every 30s instead of 10s
    lastGPSStatus = millis();
    
    if (gps.location.isValid()) {
      Serial.print("📍 GPS: ");
      Serial.print(gps.location.lat(), 6);
      Serial.print(", ");
      Serial.print(gps.location.lng(), 6);
      Serial.print(" (");
      Serial.print(gps.satellites.value());
      Serial.println(" sats)");
      
      lastLat = gps.location.lat();
      lastLng = gps.location.lng();
      gpsValid = true;
    } else {
      Serial.print("📡 GPS: ");
      Serial.print(gps.satellites.value());
      Serial.println(" sats");
      gpsLocation = "Searching (" + String(gps.satellites.value()) + " sats)";
    }
  }
}

/* ===================== GPS ===================== */
void updateGPS() {
  static bool firstRun = true;
  
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
  }
  
  // Stop showing raw data after 30 seconds
  if (firstRun && millis() > 30000) {
    firstRun = false;
  }
}
