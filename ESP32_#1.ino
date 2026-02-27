#include <esp_now.h>
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

/* ===================== CONFIG ===================== */
#define BUZZER_PIN 25
#define SS_PIN     5
#define RST_PIN    27
#define SW420_PIN  34 

#define ARMING_DELAY_MS     3000
#define RFID_COOLDOWN_MS    1000
#define ACTIVE_HOLD_MS      120
#define SLEEP_INTERVAL_US   400000

// MPU6050 (for tilt/lifting detection)
#define TILT_WARNING_DELTA  0.5   // Very sensitive catches small tilts
#define TILT_ALERT_DELTA    1.0   // Even lower instant alarm on any significant tilt

// SW-420 (for vibration/glass break/engine start)
#define VIBRATION_TRIGGER_COUNT  3    // Number of vibrations before alert
#define VIBRATION_WINDOW_MS      2000 // Time for counting vibrations
#define VIBRATION_DEBOUNCE_MS    50   // Debounce

#define WARNING_LIMIT       3
#define ALERT_LIMIT         1  // Instant alarm on tilt

#define ALERT_SEND_COOLDOWN 1000

/* ===================== STATE ===================== */
enum SystemState { DISARMED, ARMING, ARMED, ALERT };
SystemState state = DISARMED;

unsigned long armingStart = 0;
unsigned long lastRFID = 0;
unsigned long lastAlertSend = 0;
unsigned long lastArmingBeep = 0;

// Alert type tracker (0=none, 1=vibration, 2=tilt)
int alertType = 0;

// MPU6050 counters
int tiltWarningHits = 0;
int tiltAlertHits = 0;

// SW-420 vibration detection
unsigned long vibrationTimes[10]; 
int vibrationCount = 0;
unsigned long lastVibration = 0;
bool lastVibrationState = LOW;

/* ===================== OBJECTS ===================== */
MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_MPU6050 mpu;

float baselineMag = 9.81;

// ESP32 #2 MAC Address
uint8_t peerAddress[] = {0x14, 0x2B, 0x2F, 0xEB, 0xD7, 0x90};

typedef struct {
  uint8_t level;       // 0=disarm, 1=warning, 2=alert
  float lat;
  float lng;
} AlertMsg;

AlertMsg msg;

/* ===================== AUTHORIZED RFID ===================== */
byte authorizedUIDs[][4] = {
  {0x55, 0xFF, 0x0A, 0x06},
  {0x50, 0x3A, 0xD3, 0x61},
  {0xEB, 0xC9, 0x2D, 0x06}
};

bool isAuthorized() {
  for (int i = 0; i < 3; i++) {
    bool match = true;
    for (int j = 0; j < 4; j++) {
      if (rfid.uid.uidByte[j] != authorizedUIDs[i][j]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

/* ===================== ESP-NOW ===================== */
void onSend(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("✅ Alert delivered to ESP32 #2");
  } else {
    Serial.print("❌ Alert send failed to MAC: ");
    for (int i = 0; i < 6; i++) {
      Serial.printf("%02X", info->des_addr[i]);
      if (i < 5) Serial.print(":");
    }
    Serial.println();
    Serial.println("   Check: Is ESP32 #2 powered on");
    Serial.println("   Check: Are both on same WiFi channel");
  }
}

void sendAlert(uint8_t level) {
  // Critical commands (disarm/arm) bypass cooldown
  if (level != 0 && level != 3) {
    if (millis() - lastAlertSend < ALERT_SEND_COOLDOWN) {
      Serial.println("⏸️  Alert throttled (cooldown)");
      return;
    }
  }
  lastAlertSend = millis();

  msg.level = level;
  msg.lat = 0.0;
  msg.lng = 0.0;
  
  esp_err_t result = esp_now_send(peerAddress, (uint8_t*)&msg, sizeof(msg));
  
  if (result == ESP_OK) {
    Serial.print("📤 Alert level ");
    Serial.println(level);
  } else {
    Serial.print("❌ ESP-NOW failed: ");
    switch(result) {
      case ESP_ERR_ESPNOW_NOT_INIT:
        Serial.println("Not initialized");
        break;
      case ESP_ERR_ESPNOW_ARG:
        Serial.println("Invalid argument");
        break;
      case ESP_ERR_ESPNOW_INTERNAL:
        Serial.println("Internal error");
        break;
      case ESP_ERR_ESPNOW_NO_MEM:
        Serial.println("Out of memory");
        break;
      case ESP_ERR_ESPNOW_NOT_FOUND:
        Serial.println("Peer not found - Check ESP32 #2 MAC");
        break;
      case ESP_ERR_ESPNOW_IF:
        Serial.println("WiFi interface error");
        break;
      default:
        Serial.print("Code ");
        Serial.println(result);
    }
  }
  
  delay(50);
}

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== CATS ESP32 #1 - SENSOR UNIT ===");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SW420_PIN, INPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // I2C for MPU6050
  Wire.begin(22, 21);
  if (!mpu.begin()) {
    Serial.println("❌ MPU6050 NOT FOUND!");
    while (1) {
      tone(BUZZER_PIN, 500); delay(200);
      noTone(BUZZER_PIN); delay(200);
    }
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("✅ MPU6050 ready");
  
  // RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("✅ RFID ready");

  // WiFi + ESP-NOW
  WiFi.mode(WIFI_STA);
  delay(100);
  
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW failed!");
    return;
  }
  esp_now_register_send_cb(onSend);

  // Add ESP32 #2 as peer
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, peerAddress, 6);
  peer.channel = 1; 
  peer.encrypt = false;
  
  Serial.print("Adding peer: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", peerAddress[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("❌ Failed to add peer");
    Serial.println("   CRITICAL: Check ESP32 #2 MAC address!");
  } else {
    Serial.println("✅ ESP-NOW ready");
  }

  // Wait for ESP32 #2 to finish booting
  Serial.println(" Waiting for ESP32 #2");
  delay(3000);

  // Startup sound
  tone(BUZZER_PIN, 800); delay(100);
  tone(BUZZER_PIN, 1000); delay(100);
  tone(BUZZER_PIN, 1200); delay(100);
  noTone(BUZZER_PIN);

  Serial.println("DISARMED - Scan RFID to ARM\n");
}

/* ===================== LOOP ===================== */
void loop() {
  handleRFID();
  handleState();
}

/* ===================== RFID ===================== */
void handleRFID() {
  if (millis() - lastRFID < RFID_COOLDOWN_MS) return;

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  lastRFID = millis();

  Serial.print("🔍 Card UID: ");
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();

  if (isAuthorized()) {
    if (state == DISARMED) {
      arm();
    } else {
      // Disarm from any state (ARMING, ARMED, or ALERT)
      disarm();
    }
  } else {
    Serial.println("❌ UNAUTHORIZED CARD!");
    // Angry rejection sound
    tone(BUZZER_PIN, 400); delay(150);
    tone(BUZZER_PIN, 300); delay(150);
    tone(BUZZER_PIN, 200); delay(150);
    noTone(BUZZER_PIN);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

/* ===================== STATE MACHINE ===================== */
void handleState() {
  switch (state) {

    case ARMING: {
      unsigned long elapsed = millis() - armingStart;
      
      // Countdown beeps
      if (elapsed - lastArmingBeep > 1000) {
        lastArmingBeep = elapsed;
        tone(BUZZER_PIN, 1500); delay(50); noTone(BUZZER_PIN);
        Serial.print("⏳ Arming... ");
        Serial.print((ARMING_DELAY_MS - elapsed) / 1000);
        Serial.println("s");
      }
      
      if (elapsed >= ARMING_DELAY_MS) {
        calibrateSensors();
        state = ARMED;
        
        // Send ARMED status to ESP32 #2
        sendAlert(3);  // Level 3 = ARMED
        
        // Armed confirmation sound
        tone(BUZZER_PIN, 2500); delay(150); noTone(BUZZER_PIN);
        Serial.println("✅ ARMED - Dual sensor monitoring active");
        Serial.println("   MPU6050: Monitoring tilt/lifting");
        Serial.println("   SW-420: Monitoring vibrations\n");
      }
      break;
    }

    case ARMED:
      checkVibration();
      if (state != ARMED) break;
      checkTilt();
      if (state != ARMED) break;
      sleepCycle();
      break;

    case ALERT:
      // Continuous alternating siren - only stops with RFID
      tone(BUZZER_PIN, (millis() / 300) % 2 ? 2500 : 1800);
      delay(10);
      break;

    case DISARMED:
      digitalWrite(BUZZER_PIN, LOW);
      break;
  }
}

/* ===================== SW-420 VIBRATION DETECTION ===================== */
void checkVibration() {
  int currentState = digitalRead(SW420_PIN);
  
  // Detect rising edge
  if (currentState == HIGH && lastVibrationState == LOW) {
    if (millis() - lastVibration > VIBRATION_DEBOUNCE_MS) {
      lastVibration = millis();
      
      // Add to vibration history
      for (int i = 9; i > 0; i--) {
        vibrationTimes[i] = vibrationTimes[i-1];
      }
      vibrationTimes[0] = millis();
      
      // Count vibrations within time window
      vibrationCount = 0;
      for (int i = 0; i < 10; i++) {
        if (millis() - vibrationTimes[i] < VIBRATION_WINDOW_MS) {
          vibrationCount++;
        }
      }
      
      Serial.print("💥 Vibration ");
      Serial.print(vibrationCount);
      Serial.print("/");
      Serial.println(VIBRATION_TRIGGER_COUNT);
      
      // Check if threshold exceeded
      if (vibrationCount >= VIBRATION_TRIGGER_COUNT) {
        Serial.println("🚨 VIBRATION ALERT");
        sendAlert(2);
        state = ALERT;
        alertType = 1;  // 1 = vibration alert
        noTone(BUZZER_PIN);
        vibrationCount = 0;
      } else if (vibrationCount >= 2) {
        Serial.println("⚠️  Warning");
        sendAlert(1);
      }
    }
  }
  
  lastVibrationState = currentState;
}

/* ===================== MPU6050 TILT DETECTION ===================== */
void calibrateSensors() {
  Serial.println("📊 Calibrating MPU6050 tilt baseline");
  
  sensors_event_t a, g, t;
  float sum = 0;

  for (int i = 0; i < 20; i++) {
    mpu.getEvent(&a, &g, &t);
    float mag = sqrt(
      a.acceleration.x * a.acceleration.x +
      a.acceleration.y * a.acceleration.y +
      a.acceleration.z * a.acceleration.z
    );
    sum += mag;
    delay(25);
  }

  baselineMag = sum / 20.0;
  tiltWarningHits = 0;
  tiltAlertHits = 0;
  vibrationCount = 0;
  
  Serial.print("✅ Tilt baseline set to: ");
  Serial.print(baselineMag);
  Serial.println(" m/s²");
}

void checkTilt() {
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  float mag = sqrt(
    a.acceleration.x * a.acceleration.x +
    a.acceleration.y * a.acceleration.y +
    a.acceleration.z * a.acceleration.z
  );

  float delta = abs(mag - baselineMag);

  // Debug: Show tilt delta every 50
  static int debugCounter = 0;
  if (debugCounter++ % 50 == 0) {
    Serial.print("📐 Tilt Δ=");
    Serial.print(delta, 2);
    Serial.println(" m/s²");
  }

  if (delta > TILT_ALERT_DELTA) {
    tiltAlertHits++;
    Serial.print("🚨 Tilt Δ=");
    Serial.print(delta);
    Serial.print(" (");
    Serial.print(tiltAlertHits);
    Serial.print("/");
    Serial.print(ALERT_LIMIT);
    Serial.println(")");
  } else if (delta > TILT_WARNING_DELTA) {
    tiltWarningHits++;
    tiltAlertHits = 0;
    Serial.print("⚠️  TILT warning: Δ=");
    Serial.print(delta);
    Serial.print(" m/s² (");
    Serial.print(tiltWarningHits);
    Serial.print("/");
    Serial.print(WARNING_LIMIT);
    Serial.println(")");
  } else {
    tiltWarningHits = 0;
    tiltAlertHits = 0;
  }

  if (tiltWarningHits >= WARNING_LIMIT) {
    Serial.println("⚠️  Tilt warning");
    sendAlert(1);
    tiltWarningHits = 0;
  }

  if (tiltAlertHits >= ALERT_LIMIT) {
    Serial.println("🚨 VEHICLE LIFTING ALERT");
    sendAlert(2);
    state = ALERT;
    alertType = 2;  // 2 = tilt alert
    noTone(BUZZER_PIN);
  }
}

/* ===================== POWER ===================== */
void sleepCycle() {
  delay(ACTIVE_HOLD_MS + 400);  
}

/* ===================== CONTROL ===================== */
void arm() {
  Serial.println("\n🔒 ARMING DUAL SENSOR SYSTEM");
  armingStart = millis();
  lastArmingBeep = 0;
  state = ARMING;
  
  // Single confirmation beep
  tone(BUZZER_PIN, 2000);
  delay(100);
  noTone(BUZZER_PIN);
}

void disarm() {
  Serial.println("\n🔓 SYSTEM DISARMED\n");
  state = DISARMED;
  alertType = 0;
  tiltWarningHits = 0;
  tiltAlertHits = 0;
  vibrationCount = 0;
  
  // Clear vibration history
  for (int i = 0; i < 10; i++) {
    vibrationTimes[i] = 0;
  }
  
  noTone(BUZZER_PIN);
  
  // Send disarm signal to ESP32 #2
  sendAlert(0);
  
  // Double beep confirmation
  tone(BUZZER_PIN, 2500); delay(100); noTone(BUZZER_PIN); delay(100);
  tone(BUZZER_PIN, 2500); delay(100); noTone(BUZZER_PIN);
}
