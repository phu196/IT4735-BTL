#include <WiFi.h>
#include <Firebase_ESP_Client.h>  // ✅ FIX: Changed from Firebase.h
#include <ArduinoJson.h>  
#include <DHT.h>
#include <time.h>
#include <Preferences.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <esp_task_wdt.h>

// ==================== WIFI CONFIGURATION ====================
#define WIFI_SSID "vivo V23e"
#define WIFI_PASSWORD "12345678"

// ==================== FIREBASE CONFIGURATION ====================
#define DATABASE_URL "https://flutter-chat-app-3e625-default-rtdb.asia-southeast1.firebasedatabase.app"
#define API_KEY "AIzaSyA3Y0n-Jdjxym66sjvNJ3pptxRrMMJGUps"
#define USER_EMAIL "darkrising2207@gmail.com"     
#define USER_PASSWORD "Luke12345!"          

// ==================== HARDWARE PIN CONFIGURATION ====================
#define SOIL_MOISTURE_PIN 34
#define DHT_PIN 4
#define RELAY_PIN 26
#define DHT_TYPE DHT11

// ==================== DEVICE IDENTIFICATION ====================
String DEVICE_MAC = "";
String DEVICE_UNIQUE_ID = "";

// ==================== CALIBRATION ====================
const int SOIL_DRY_VALUE = 4095;
const int SOIL_WET_VALUE = 1500;

// ==================== WATCHDOG TIMER ====================
#define WDT_TIMEOUT 30

// ==================== GLOBAL OBJECTS ====================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
DHT dht(DHT_PIN, DHT_TYPE);
Preferences preferences;

// ==================== DEVICE REGISTRY ====================
struct DeviceRegistry {
  String userId = "";
  String zoneId = "";
  String deviceId = "";
  String soilSensorId = "";
  String tempSensorId = "";
  String humidSensorId = "";
  String myNewSensorId = "";
  bool isRegistered = false;
  unsigned long lastRegistryCheck = 0;
} registry;

// ==================== DEVICE STATE ====================
struct DeviceState {
  bool status = false;
  int currentDuration = 0;
  unsigned long startTime = 0;
  bool isWatering = false;
  unsigned long lastSync = 0;
  unsigned long lastDurationUpdate = 0;  
  bool manualMode = false;       // true = manual, false = auto
} deviceState;

// ==================== SENSOR DATA ====================
struct SensorData {
  String sensorId = "";        // ID sensor trên Firebase
  float value = 0;             // Giá trị sensor hiện tại
  float soilMoisture = 0;
  float temperature = 0;
  float humidity = 0;
  bool valid = false;
  float soilHistory[5] = {0, 0, 0, 0, 0};
  int soilHistoryIndex = 0;
  float minThreshold = 0;       // Ngưỡng tối thiểu (soil khô → tưới)
  float maxThreshold = 100;     // Ngưỡng tối đa (soil đủ ẩm → dừng tưới)
  int defaultDuration = 60;     // Thời gian tưới mặc định (giây)
  unsigned long lastRead = 0;   // Thời điểm đọc sensor cuối cùng
} sensorData;


// ==================== TIMING ====================
unsigned long lastSensorRead = 0;
unsigned long lastFirebaseUpdate = 0;
unsigned long lastDeviceCheck = 0;
unsigned long wateringStartTime = 0;
unsigned long lastWiFiCheck = 0;  
unsigned long lastSensorUpdate = 0;
const unsigned long SENSOR_UPDATE_INTERVAL = 30000; // 30 giây

const unsigned long SENSOR_READ_INTERVAL = 10000;
const unsigned long FIREBASE_UPDATE_INTERVAL = 30000;
const unsigned long DEVICE_CHECK_INTERVAL = 2000;
const unsigned long REGISTRY_CHECK_INTERVAL = 300000;
const unsigned long WIFI_CHECK_INTERVAL = 30000;

enum WateringMode { AUTO, MANUAL };
WateringMode currentMode = AUTO;

bool forcedAuto = false;              
const float SOIL_FORCE_AUTO = 50.0;   

bool manualRelayState = false;

// ==================== FUNCTION PROTOTYPES ====================
void connectWiFi();
void checkWiFiConnection();
void initFirebase();
void initDeviceRegistry();
void autoRegisterDevice();
void scanAndRegisterWithZones();
bool registerDeviceToZone(String zoneId, String zoneName);
void createSensors(String zoneId, String zoneName);
void saveRegistryToNVS();
void loadRegistryFromNVS();
void readSensors();
float getFilteredSoilMoisture();
void updateFirebase();
void checkDeviceCommands();
void controlRelay(bool state, bool updateFirebaseStatus = false);
void updateWateringDuration();
void checkForceAutoBySoil();
void sendSensorReading(String sensorId, float value, bool isAlert = false);  
void logWateringEvent(int actualDuration);
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
String getUserId();
String getFormattedTime();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("==========================================");
  Serial.println("   PI-VERT IOT v2.0 - AUTO DISCOVERY");
  Serial.println("==========================================");
  
  Serial.println("⏱️  Configuring Watchdog Timer...");
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
  Serial.println("✓ Watchdog Timer enabled (30s timeout)");
  
  // Get unique device ID from MAC
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  DEVICE_MAC = String(mac[0], HEX) + ":" + String(mac[1], HEX) + ":" + 
               String(mac[2], HEX) + ":" + String(mac[3], HEX) + ":" + 
               String(mac[4], HEX) + ":" + String(mac[5], HEX);
  DEVICE_UNIQUE_ID = "ESP32_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  
  Serial.print("📱 Device MAC: ");
  Serial.println(DEVICE_MAC);
  Serial.print("🆔 Unique ID: ");
  Serial.println(DEVICE_UNIQUE_ID);
  
  // Initialize NVS
  preferences.begin("pivert", false);
  
  // Initialize Hardware
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  
  Serial.println("✓ Hardware initialized");
  
  Serial.println("📡 Initializing DHT11 sensor...");
  dht.begin();
  delay(2000);
  Serial.println("✓ DHT11 ready");
  
  // Connect WiFi
  connectWiFi();
  
  // Initialize Firebase
  initFirebase();
  
  // Configure NTP
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("✓ NTP configured (GMT+7)");
  
  // Wait for time sync
  Serial.print("⏳ Waiting for time sync");
  int timeAttempts = 0;
  while (time(nullptr) < 100000 && timeAttempts < 20) {
    Serial.print(".");
    delay(500);
    timeAttempts++;
  }
  Serial.println();
  
  if (time(nullptr) > 100000) {
    Serial.println("✓ Time synchronized: " + getFormattedTime());
  } else {
    Serial.println("⚠️  Time sync timeout, continuing anyway...");
  }
  
  // Initialize Device Registry
  initDeviceRegistry();
  
  // Initial sensor reading
  Serial.println("\n📊 Initial sensor reading...");
  for (int i = 0; i < 5; i++) {
    readSensors();
    delay(500);
  }
  
  Serial.println("==========================================");
  Serial.println("       SYSTEM READY");
  Serial.println("==========================================\n");
  
  esp_task_wdt_reset();
}

// ==================== MAIN LOOP ====================
void loop() {
  esp_task_wdt_reset();

  unsigned long currentMillis = millis();

  if (currentMillis - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = currentMillis;
    checkWiFiConnection();
  }

  if (!registry.isRegistered || currentMillis - registry.lastRegistryCheck >= REGISTRY_CHECK_INTERVAL) {
    registry.lastRegistryCheck = currentMillis;
    initDeviceRegistry();
  }

  if (currentMillis - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = currentMillis;
    readSensors();
  }

  if (registry.isRegistered && currentMillis - lastFirebaseUpdate >= FIREBASE_UPDATE_INTERVAL) {
    lastFirebaseUpdate = currentMillis;
    updateFirebase();
  }

  if (registry.isRegistered && currentMillis - lastDeviceCheck >= DEVICE_CHECK_INTERVAL) {
    lastDeviceCheck = currentMillis;
    checkDeviceCommands();  // đọc lệnh manual/auto
  }
if (millis() - lastSensorUpdate >= SENSOR_UPDATE_INTERVAL) {
    lastSensorUpdate = millis();
    updateSensorDataFromFirebase();
    updateSensorThresholdsFromFirebase();
}
  if (registry.isRegistered) {
    checkForceAutoBySoil();
    autoWateringLogic();
  }

  updateWateringDuration();

  delay(2000);
}


// ==================== WIFI CONNECTION ====================
void connectWiFi() {
  Serial.print("📡 Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("  RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("\n✗ WiFi failed! Restarting...");
    delay(3000);
    ESP.restart();
  }
}

// ==================== CHECK WIFI CONNECTION ====================
void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi disconnected! Reconnecting...");
    WiFi.disconnect();
    delay(1000);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
      esp_task_wdt_reset();
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✓ WiFi reconnected");
      Serial.print("  IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\n✗ WiFi reconnect failed! Restarting...");
      delay(3000);
      ESP.restart();
    }
  }
}

// ==================== FIREBASE INITIALIZATION ====================
void initFirebase() {
  Serial.println("🔥 Initializing Firebase...");
  
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  
  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo.setResponseSize(2048);
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  Serial.println("✓ Firebase initialized");
  
  Serial.print("  Authenticating");
  int attempts = 0;
  while (!Firebase.ready() && attempts < 30) {
    Serial.print(".");
    delay(1000);
    attempts++;
    esp_task_wdt_reset();
  }
  
  if (Firebase.ready()) {
    Serial.println("\n✓ Firebase authenticated");
    // ✅ FIX: Convert MB_String to String
    Serial.print("  UID: ");
    Serial.println(auth.token.uid.c_str());  // Changed
  } else {
    Serial.println("\n✗ Authentication failed!");
  }
}

// ==================== DEVICE REGISTRY INITIALIZATION ====================
void initDeviceRegistry() {
    Serial.println("\n🔍 Initializing Device Registry...");

    // Gán cứng device và zone
    registry.deviceId = "ESP32_1b0770007793";
    registry.zoneId = "-OiLTma6U1rWN7sTXm1l";
    registry.soilSensorId = "-OiEMvSNO-P3Y9Q5xbg3";
    registry.tempSensorId = "-OiEMvUkxLo_PI9slivl";
    registry.humidSensorId = "-OiEMvXI8HlHUMk2x_B4";
    // registry.myNewSensorId = "-OiLwADmGGcrb-VCmMKb";

    registry.isRegistered = true;

    // Lưu vào Preferences
    preferences.putString("deviceId", registry.deviceId);
    preferences.putString("zoneId", registry.zoneId);
    preferences.putString("soilSensorId", registry.soilSensorId);
    preferences.putString("tempSensorId", registry.tempSensorId);
    preferences.putString("humidSensorId", registry.humidSensorId);
    // preferences.putString("myNewSensorId", registry.myNewSensorId);

    Serial.println("✅ Device registered with fixed Zone ID");
    Serial.println("   Device ID: " + registry.deviceId);
    Serial.println("   Zone ID: " + registry.zoneId);
}
// ==================== AUTO REGISTER DEVICE ====================
void autoRegisterDevice() {
  if (!Firebase.ready()) {
    Serial.println("✗ Firebase not ready");
    return;
  }
  
  registry.userId = getUserId();
  if (registry.userId.isEmpty()) {
    Serial.println("✗ Could not get User ID");
    return;
  }
  
  Serial.println("👤 User ID: " + registry.userId);
  scanAndRegisterWithZones();
}

// ==================== SCAN AND REGISTER WITH ZONES ====================
// void scanAndRegisterWithZones() {
//   Serial.println("\n🔎 Scanning for available zones...");

//   // Kiểm tra xem device đã được lưu trong Preferences chưa
//   String savedDeviceId = preferences.getString("deviceId", "");
//   if (savedDeviceId.length() > 0) {
//     registry.deviceId = savedDeviceId;
//     registry.zoneId = preferences.getString("zoneId", "");
//     registry.isRegistered = true;  // ✅ Set flag
//     Serial.println("✅ Device already registered:");
//     Serial.println("   Device ID: " + registry.deviceId);
//     Serial.println("   Zone ID: " + registry.zoneId);
//     return;
//   }

//   // Device chưa đăng ký → scan Firebase
//   String zonesPath = "zones";
//   if (!Firebase.RTDB.getJSON(&fbdo, zonesPath.c_str())) {
//     Serial.println("✗ Failed to scan zones");
//     Serial.println("Firebase error: " + fbdo.errorReason());
//     return;
//   }

//   String jsonStr = fbdo.jsonString();
//   Serial.println("Zone JSON (truncated 200 chars):");
//   Serial.println(jsonStr.substring(0, (size_t)min((size_t)200, jsonStr.length())));

//   const size_t capacity = 16 * 1024;
//   DynamicJsonDocument doc(capacity);
//   DeserializationError error = deserializeJson(doc, jsonStr);
//   if (error) {
//     Serial.print("✗ JSON parse failed: ");
//     Serial.println(error.c_str());
//     return;
//   }

//   bool registered = false;

//   for (JsonPair zone : doc.as<JsonObject>()) {
//     String key = zone.key().c_str();
//     JsonObject zoneObj = zone.value().as<JsonObject>();

//     Serial.println("  🔹 Checking zone: " + key);
//     esp_task_wdt_reset();

//     String zoneUserId = zoneObj["userId"] | "";
//     if (zoneUserId != registry.userId) continue;

//     String deviceId = zoneObj["deviceId"] | "";

//     if (deviceId.length() > 0) {
//       Serial.println("    ✓ Found existing device registration");
//       registry.zoneId = key;
//       registry.deviceId = deviceId;
//       registry.isRegistered = true;  // ✅ Set flag
//       preferences.putString("zoneId", registry.zoneId);
//       preferences.putString("deviceId", registry.deviceId);
//       registered = true;
//       break;
//     }

//     if (!registered && registerDeviceToZone(key, zoneObj["name"] | "Unnamed Zone")) {
//       Serial.println("    ✅ Device successfully registered to zone");
//       registry.zoneId = key;
//       registry.isRegistered = true;  // ✅ Set flag
//       preferences.putString("zoneId", registry.zoneId);
//       preferences.putString("deviceId", registry.deviceId);
//       registered = true;
//       break;
//     }
//   }

//   if (!registered) {
//     Serial.println("\n⚠️  No suitable zone found or registration failed");
//     Serial.println("  Please create a zone in the app first");
//   }
// }

// // ==================== REGISTER DEVICE TO ZONE ====================
// bool registerDeviceToZone(String zoneId, String zoneName) {
//   Serial.println("\n📝 Registering device to zone: " + zoneName);

//   String devicesPath = "devices";

//   // Tạo deviceId bằng ESP32 luôn (UUID hoặc uniqueId)
//   registry.deviceId = DEVICE_UNIQUE_ID + String(random(1000,9999)); 
//   registry.zoneId = zoneId;

//   String devicePath = devicesPath + "/" + registry.deviceId;
//   FirebaseJson deviceJson;
//   deviceJson.set("id", registry.deviceId);
//   deviceJson.set("name", "ESP32 Pump - " + DEVICE_UNIQUE_ID);
//   deviceJson.set("zoneId", zoneId);
//   deviceJson.set("type", "pump");
//   deviceJson.set("status", false);
//   deviceJson.set("lastUpdated", (int)time(nullptr) * 1000);
//   deviceJson.set("flowRate", 5.0);
//   deviceJson.set("currentDuration", 0);
//   deviceJson.set("deviceMAC", DEVICE_MAC);
//   deviceJson.set("uniqueId", DEVICE_UNIQUE_ID);

//   String jsonStr;
//   deviceJson.toString(jsonStr, true); // true = prettify, false = không cần
//   Serial.println("Device JSON to push:");
//   Serial.println(jsonStr);

//   if (Firebase.RTDB.setJSON(&fbdo, devicePath.c_str(), &deviceJson)) {
//     Serial.println("✓ Device registered (direct setJSON)");
//   } else {
//     Serial.println("✗ Device registration failed");
//     Serial.println("Firebase error: " + fbdo.errorReason());
//     return false;
//   }

//   // Tạo sensors
//   createSensors(zoneId, zoneName);

//   registry.isRegistered = true;
//   saveRegistryToNVS();

//   Serial.println("✓ Registration complete!");
//   return true;

//   // if (Firebase.RTDB.setJSON(&fbdo, devicePath.c_str(), &deviceJson)) {
//   //   Serial.println("✓ Device registered (direct setJSON)");
    
//   //   createSensors(zoneId, zoneName);

//   //   registry.isRegistered = true;
//   //   saveRegistryToNVS();

//   //   Serial.println("✓ Registration complete!");
//   //   return true;
//   // } else {
//   //   Serial.println("✗ Device registration failed");
//   //   return false;
//   // }
// }


// ==================== CREATE SENSORS ====================
void createSensors(String zoneId, String zoneName) {
  Serial.println("\n🔧 Creating sensors...");
  
  String sensorsPath = "sensors";
  FirebaseJson tempJson;
  tempJson.set(".sv", "timestamp");
  
  // 1. Soil Moisture Sensor
  if (Firebase.RTDB.pushJSON(&fbdo, sensorsPath.c_str(), &tempJson)) {
    String soilKey = fbdo.pushName();
    registry.soilSensorId = soilKey;
    
    FirebaseJson soilJson;
    soilJson.set("id", soilKey);
    soilJson.set("zoneId", zoneId);
    soilJson.set("zoneName", zoneName);
    soilJson.set("name", "Soil Moisture - " + zoneName);
    soilJson.set("type", "soilMoisture");
    soilJson.set("unit", "%");
    soilJson.set("currentValue", 50.0);
    soilJson.set("minThreshold", 30.0);
    soilJson.set("maxThreshold", 80.0);
    soilJson.set("lastUpdated", (int)time(nullptr) * 1000);
    soilJson.set("isActive", true);
    soilJson.set("alertEnabled", true);
    
    String soilPath = sensorsPath + "/" + soilKey;
    if (Firebase.RTDB.setJSON(&fbdo, soilPath.c_str(), &soilJson)) {
      Serial.println("  ✓ Soil moisture sensor created");
    }
  }
  
  esp_task_wdt_reset();
  
  // 2. Temperature Sensor
  if (Firebase.RTDB.pushJSON(&fbdo, sensorsPath.c_str(), &tempJson)) {
    String tempKey = fbdo.pushName();
    registry.tempSensorId = tempKey;
    
    FirebaseJson tempJsonData;
    tempJsonData.set("id", tempKey);
    tempJsonData.set("zoneId", zoneId);
    tempJsonData.set("zoneName", zoneName);
    tempJsonData.set("name", "Temperature - " + zoneName);
    tempJsonData.set("type", "temperature");
    tempJsonData.set("unit", "°C");
    tempJsonData.set("currentValue", 25.0);
    tempJsonData.set("minThreshold", 15.0);
    tempJsonData.set("maxThreshold", 35.0);
    tempJsonData.set("lastUpdated", (int)time(nullptr) * 1000);
    tempJsonData.set("isActive", true);
    tempJsonData.set("alertEnabled", true);
    
    String tempPath = sensorsPath + "/" + tempKey;
    if (Firebase.RTDB.setJSON(&fbdo, tempPath.c_str(), &tempJsonData)) {
      Serial.println("  ✓ Temperature sensor created");
    }
  }
  
  esp_task_wdt_reset();
  
  // 3. Humidity Sensor
  if (Firebase.RTDB.pushJSON(&fbdo, sensorsPath.c_str(), &tempJson)) {
    String humidKey = fbdo.pushName();
    registry.humidSensorId = humidKey;
    
    FirebaseJson humidJson;
    humidJson.set("id", humidKey);
    humidJson.set("zoneId", zoneId);
    humidJson.set("zoneName", zoneName);
    humidJson.set("name", "Humidity - " + zoneName);
    humidJson.set("type", "humidity");
    humidJson.set("unit", "%");
    humidJson.set("currentValue", 60.0);
    humidJson.set("minThreshold", 40.0);
    humidJson.set("maxThreshold", 80.0);
    humidJson.set("lastUpdated", (int)time(nullptr) * 1000);
    humidJson.set("isActive", true);
    humidJson.set("alertEnabled", true);
    
    String humidPath = sensorsPath + "/" + humidKey;
    if (Firebase.RTDB.setJSON(&fbdo, humidPath.c_str(), &humidJson)) {
      Serial.println("  ✓ Humidity sensor created");
    }
  }
  
  Serial.println("✓ All sensors created");
}

// ==================== SAVE/LOAD REGISTRY ====================
void saveRegistryToNVS() {
  preferences.putString("userId", registry.userId);
  preferences.putString("zoneId", registry.zoneId);
  preferences.putString("deviceId", registry.deviceId);
  preferences.putString("soilSensorId", registry.soilSensorId);
  preferences.putString("tempSensorId", registry.tempSensorId);
  preferences.putString("humidSensorId", registry.humidSensorId);
  preferences.putBool("registered", true);
  
  Serial.println("💾 Registry saved to NVS");
}

void loadRegistryFromNVS() {
  registry.userId = preferences.getString("userId", "");
  registry.zoneId = preferences.getString("zoneId", "");
  registry.deviceId = preferences.getString("deviceId", "");
  registry.soilSensorId = preferences.getString("soilSensorId", "");
  registry.tempSensorId = preferences.getString("tempSensorId", "");
  registry.humidSensorId = preferences.getString("humidSensorId", "");
  registry.isRegistered = preferences.getBool("registered", false);
}

// ==================== GET USER ID ====================
String getUserId() {
  if (!Firebase.ready()) return "";
  
  // ✅ FIX: Convert MB_String to String
  String uid = String(auth.token.uid.c_str());
  if (!uid.isEmpty()) {
    return uid;
  }
  
  if (Firebase.RTDB.getJSON(&fbdo, "zones")) {
    FirebaseJson json;
    json.setJsonData(fbdo.jsonString());
    size_t len = json.iteratorBegin();
    
    if (len > 0) {
      String key, value;
      int type;
      json.iteratorGet(0, type, key, value);
      
      String zonePath = "zones/" + key + "/userId";
      if (Firebase.RTDB.getString(&fbdo, zonePath.c_str())) {
        String userId = fbdo.stringData();
        json.iteratorEnd();
        return userId;
      }
    }
    json.iteratorEnd();
  }
  
  return "";
}

// ==================== READ SENSORS ====================
void readSensors() {
    // ===== 1. Đọc soil moisture =====
    int soilRaw = 0;
    for (int i = 0; i < 3; i++) {
        soilRaw += analogRead(SOIL_MOISTURE_PIN);
        delay(10);
    }
    soilRaw /= 3;

    float soilValue = mapFloat(soilRaw, SOIL_DRY_VALUE, SOIL_WET_VALUE, 0, 100);
    soilValue = constrain(soilValue, 0, 100);

    // Cập nhật lịch sử
    sensorData.soilHistory[sensorData.soilHistoryIndex] = soilValue;
    sensorData.soilHistoryIndex = (sensorData.soilHistoryIndex + 1) % 5;

    // Lọc trung bình
    sensorData.soilMoisture = getFilteredSoilMoisture();

    // ===== 2. Đọc DHT11 =====
    float temp = dht.readTemperature();
    float humid = dht.readHumidity();

    if (!isnan(temp) && !isnan(humid)) {
        sensorData.temperature = temp;
        sensorData.humidity = humid;
        sensorData.valid = true; // sensor hợp lệ
    } else {
        Serial.println("⚠️  DHT read failed");
        sensorData.valid = false;
    }

    // ===== 3. In ra serial =====
    Serial.print("📊 Soil: ");
    Serial.print(sensorData.soilMoisture, 1);
    Serial.print("%  Temp: ");
    Serial.print(sensorData.temperature, 1);
    Serial.print("°C  Humid: ");
    Serial.print(sensorData.humidity, 1);
    Serial.print("%  MinThresh: ");
    Serial.print(sensorData.minThreshold, 1);
    Serial.print("%  MaxThresh: ");
    Serial.println(sensorData.maxThreshold, 1);
}


// ==================== GET FILTERED SOIL MOISTURE ====================
float getFilteredSoilMoisture() {
    float sum = 0;
    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (sensorData.soilHistory[i] > 0) {
            sum += sensorData.soilHistory[i];
            count++;
        }
    }
    float avg = count > 0 ? sum / count : 0;
    return constrain(avg, 0, 100);
}


// ==================== UPDATE FIREBASE ====================
void updateFirebase() {
    if (!Firebase.ready() || !registry.isRegistered) return;

    unsigned long now = (unsigned long)time(nullptr) * 1000;

    // ===== Soil Moisture =====
    if (!registry.soilSensorId.isEmpty()) {
        sensorData.soilMoisture = getFilteredSoilMoisture(); // luôn cập nhật giá trị trung bình
        String path = "sensors/" + registry.soilSensorId;
        FirebaseJson json;
        json.set("currentValue", sensorData.soilMoisture);
        json.set("lastUpdated", (int)now);
        Firebase.RTDB.updateNode(&fbdo, path.c_str(), &json);

        // Gửi lịch sử
        sendSensorReading(registry.soilSensorId, sensorData.soilMoisture);
    }

    // ===== Temperature =====
    if (!registry.tempSensorId.isEmpty() && sensorData.valid) {
        String path = "sensors/" + registry.tempSensorId;
        FirebaseJson json;
        json.set("currentValue", sensorData.temperature);
        json.set("lastUpdated", (int)now);
        Firebase.RTDB.updateNode(&fbdo, path.c_str(), &json);

        sendSensorReading(registry.tempSensorId, sensorData.temperature);
    }

    // ===== Humidity =====
    if (!registry.humidSensorId.isEmpty() && sensorData.valid) {
        String path = "sensors/" + registry.humidSensorId;
        FirebaseJson json;
        json.set("currentValue", sensorData.humidity);
        json.set("lastUpdated", (int)now);
        Firebase.RTDB.updateNode(&fbdo, path.c_str(), &json);

        sendSensorReading(registry.humidSensorId, sensorData.humidity);
    }
}


// ==================== CHECK DEVICE COMMANDS ====================
void checkDeviceCommands() {
    if (!Firebase.ready() || registry.deviceId.isEmpty()) return;

    String path = "devices/" + registry.deviceId;

    if (!Firebase.RTDB.getJSON(&fbdo, path.c_str())) return;

    FirebaseJson json;
    json.setJsonData(fbdo.jsonString());
    FirebaseJsonData data;

    /* ===================== 1. ĐỌC MODE ===================== */
    if (json.get(data, "mode")) {
        String modeStr = data.stringValue;

        if (modeStr == "manual") {
            currentMode = MANUAL;
            forcedAuto = false;   // user chủ động → bỏ ép auto
            Serial.println("👤 MODE = MANUAL");
        } else {
            currentMode = AUTO;
            Serial.println("🤖 MODE = AUTO");
        }
    }

    /* ===================== 2. NẾU AUTO → KHÔNG XỬ LÝ MANUAL ===================== */
    if (currentMode == AUTO) return;

    /* ===================== 3. NẾU BỊ ÉP AUTO → KHÓA MANUAL ===================== */
    if (forcedAuto) {
        Serial.println("⛔ MANUAL blocked (forced AUTO)");
        return;
    }

    /* ===================== 4. XỬ LÝ MANUAL ON / OFF ===================== */
    if (!json.get(data, "status")) return;
    bool newStatus = data.boolValue;

    // Không đổi trạng thái → bỏ qua
    if (newStatus == deviceState.status) return;

    deviceState.status = newStatus;

    /* -------- BẬT BƠM -------- */
    if (newStatus) {
        Serial.println("💧 MANUAL → ON");

        if (!deviceState.isWatering) {
            int seconds = 60; // mặc định

            if (json.get(data, "currentDuration")) {
                seconds = data.intValue;
                if (seconds <= 0) seconds = 60;
            }

            deviceState.isWatering = true;
            deviceState.currentDuration = seconds;
            deviceState.startTime = millis();
            deviceState.lastDurationUpdate = millis();

            controlRelay(true);
        }
    }
    /* -------- TẮT BƠM -------- */
    else {
        Serial.println("🛑 MANUAL → OFF");

        deviceState.isWatering = false;
        deviceState.currentDuration = 0;

        controlRelay(false);
    }
}
void updateSensorDataFromFirebase() {
    if (!Firebase.ready() || sensorData.sensorId.isEmpty()) return;

    String path = "sensors/" + sensorData.sensorId;
    if (!Firebase.RTDB.getJSON(&fbdo, path.c_str())) return;

    FirebaseJson json;
    json.setJsonData(fbdo.jsonString());
    FirebaseJsonData data;

    if (json.get(data, "currentValue")) sensorData.value = data.floatValue;
    if (json.get(data, "minThreshold")) sensorData.minThreshold = data.floatValue;
    if (json.get(data, "maxThreshold")) sensorData.maxThreshold = data.floatValue;
    if (json.get(data, "defaultDuration")) sensorData.defaultDuration = data.intValue;

    sensorData.valid = true;
    sensorData.lastRead = millis();
}

void updateSensorThresholdsFromFirebase() {
    if (!Firebase.ready() || !registry.isRegistered) return;

    // Soil sensor
    if (!registry.soilSensorId.isEmpty()) {
        String path = "sensors/" + registry.soilSensorId;
        if (Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
            FirebaseJson json;
            json.setJsonData(fbdo.jsonString());
            FirebaseJsonData data;

            if (json.get(data, "minThreshold")) sensorData.minThreshold = data.floatValue;
            if (json.get(data, "maxThreshold")) sensorData.maxThreshold = data.floatValue;
        }
    }

    // Bạn cũng có thể thêm nhiệt độ / độ ẩm nếu muốn
}

// ==================== CONTROL RELAY ====================
void controlRelay(bool state, bool updateFirebaseStatus) {
    digitalWrite(RELAY_PIN, state ? HIGH : LOW);
    
    Serial.print("💧 Relay: ");
    Serial.println(state ? "ON" : "OFF");
    
    // ✅ FIX: Always update relay state to Firebase if requested
    if (updateFirebaseStatus && Firebase.ready() && !registry.deviceId.isEmpty()) {
        String devicePath = "devices/" + registry.deviceId;
        FirebaseJson json;
        json.set("status", state);
        json.set("lastUpdated", (int)time(nullptr) * 1000);
        
        if (!state) {
            json.set("currentDuration", 0); // Reset duration when turning off
        }
        
        Firebase.RTDB.updateNode(&fbdo, devicePath.c_str(), &json);
        Serial.print("✓ Firebase relay status updated: ");
        Serial.println(state ? "ON" : "OFF");
    }
}

void autoWateringLogic() {
    if (!sensorData.valid) return;  // Sensor hợp lệ

    float soil = getFilteredSoilMoisture();

    // 1️⃣ Nếu đang MANUAL mà soil < minThreshold → chuyển sang AUTO
    if (currentMode == MANUAL && soil < sensorData.minThreshold) {
        Serial.println("⚠️ FORCE AUTO: Soil too low in MANUAL → switch to AUTO");
        currentMode = AUTO;
        forcedAuto = true;

        FirebaseJson json;
        json.set("mode", "auto");
        json.set("forcedAuto", true);
        json.set("lastUpdated", (int)time(nullptr) * 1000);

        Firebase.RTDB.updateNode(
            &fbdo,
            ("devices/" + registry.deviceId).c_str(),
            &json
        );
    }

    // 2️⃣ Chỉ AUTO mới tưới
    if (currentMode != AUTO) return;

    // Bật bơm nếu soil < SOIL_FORCE_AUTO
    if (soil < SOIL_FORCE_AUTO && !deviceState.isWatering) {
        Serial.println("🌱 AUTO: Soil below SOIL_FORCE_AUTO → start watering");

        deviceState.isWatering = true;
        deviceState.status = true;
        deviceState.currentDuration = sensorData.defaultDuration > 0 ? sensorData.defaultDuration : 60;
        deviceState.startTime = millis();
        deviceState.lastDurationUpdate = millis();

        controlRelay(true, true);
    }
    // Tắt bơm nếu soil >= maxThreshold
    else if (soil >= sensorData.maxThreshold && deviceState.isWatering) {
        Serial.println("🌱 AUTO: Soil above maxThreshold → stop watering");

        deviceState.isWatering = false;
        deviceState.status = false;
        deviceState.currentDuration = 0;

        controlRelay(false, true);
    }
}



// void handleWatering() {
//   switch(currentMode) {
//     case AUTO:
//       // Auto tưới khi soil < 50%
//       if (sensorData.soilMoisture < 50) {
//         if (!deviceState.isWatering) {
//           deviceState.isWatering = true;
//           deviceState.startTime = millis();
//           controlRelay(true);
//         }
//       } else {
//         if (deviceState.isWatering) {
//           deviceState.isWatering = false;
//           controlRelay(false);
//         }
//       }

//       // Update duration nếu đang tưới
//       if (deviceState.isWatering) {
//         updateWateringDuration();
//       }
//       break;

//     case MANUAL:
//       // Manual: bật/tắt relay theo manualRelayState
//       controlRelay(manualRelayState);
//       break;
//   }
// }
// void checkSerialInput() {
//   if (Serial.available()) {
//     char c = Serial.read();
//     if (c == 'a') currentMode = AUTO;
//     if (c == 'm') currentMode = MANUAL;
//     if (c == '1') manualRelayState = true;
//     if (c == '0') manualRelayState = false;
//   }
// }

// ==================== UPDATE WATERING DURATION ====================
// ==================== UPDATE WATERING DURATION ====================
void updateWateringDuration() {
    if (!deviceState.isWatering) return;

    unsigned long currentMillis = millis();
    unsigned long elapsed = (currentMillis - deviceState.startTime) / 1000; // seconds
    int totalSeconds = deviceState.currentDuration; 
    int remaining = totalSeconds - elapsed;

    // ✅ DEBUG LOG
    Serial.print("DEBUG: elapsed=");
    Serial.print(elapsed);
    Serial.print("s, total=");
    Serial.print(totalSeconds);
    Serial.print("s, remaining=");
    Serial.println(remaining);

    // ✅ FIX: Check if time's up
    if (remaining <= 0) {
        Serial.println("\n⏰ Watering completed (auto stop)");
        
        // Calculate actual duration for logging
        unsigned long actualDuration = elapsed / 60; // Convert to minutes
        
        // Stop watering
        deviceState.isWatering = false;
        deviceState.currentDuration = 0;
        deviceState.status = false;
        
        controlRelay(false, true); // ✅ FIX: Pass true to update Firebase
        
        // ✅ FIX: Update Firebase status to OFF
        if (Firebase.ready() && !registry.deviceId.isEmpty()) {
            String path = "devices/" + registry.deviceId;
            FirebaseJson json;
            json.set("status", false);
            json.set("currentDuration", 0);
            json.set("lastUpdated", (int)time(nullptr) * 1000);
            Firebase.RTDB.updateNode(&fbdo, path.c_str(), &json);
            Serial.println("✓ Firebase updated: status=false");
        }
        
        // Log watering history
        logWateringEvent(actualDuration);
        
    } else {
        // ✅ FIX: Update countdown to Firebase every 10 seconds
        if (currentMillis - deviceState.lastDurationUpdate >= 10000) {
            deviceState.lastDurationUpdate = currentMillis;
            
            int remainingMinutes = (remaining + 59) / 60; // Round up to minutes
            
            Serial.print("⏱️  Remaining: ");
            Serial.print(remaining);
            Serial.print("s (");
            Serial.print(remainingMinutes);
            Serial.println(" minutes)");
            
            // ✅ FIX: Update Firebase with remaining TIME IN SECONDS
            if (Firebase.ready() && !registry.deviceId.isEmpty()) {
                String path = "devices/" + registry.deviceId + "/currentDuration";
                Firebase.RTDB.setInt(&fbdo, path.c_str(), remaining);
            }
        }
    }
}

void checkForceAutoBySoil() {
    if (!sensorData.valid) return;
    if (currentMode != MANUAL) return;

    float soil = getFilteredSoilMoisture();
    float forceThreshold = sensorData.minThreshold; // từ Firebase

    if (soil < forceThreshold) {
        Serial.println("⚠️ FORCE AUTO: Soil below minThreshold");

        currentMode = AUTO;
        forcedAuto = true;

        FirebaseJson json;
        json.set("mode", "auto");
        json.set("forcedAuto", true);
        json.set("lastUpdated", (int)time(nullptr) * 1000);

        Firebase.RTDB.updateNode(
            &fbdo,
            ("devices/" + registry.deviceId).c_str(),
            &json
        );
    }
}

// ==================== SEND SENSOR READING TO HISTORY ====================
void sendSensorReading(String sensorId, float value, bool isAlert) {
  if (!Firebase.ready() || sensorId.isEmpty()) return;
  
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  
  char dateKey[12];  // ✅ FIX: Increased buffer size
  strftime(dateKey, sizeof(dateKey), "%Y-%m-%d", &timeinfo);
  
  String readingPath = "sensor_readings/" + sensorId + "/" + String(dateKey) + "/" + String(now * 1000);
  
  FirebaseJson json;
  json.set("sensorId", sensorId);
  json.set("value", value);
  json.set("timestamp", (int)now * 1000);
  json.set("isAlert", isAlert);
  
  Firebase.RTDB.setJSON(&fbdo, readingPath.c_str(), &json);
}

// ==================== LOG WATERING EVENT ====================
void logWateringEvent(int durationMinutes) {
  if (!Firebase.ready() || registry.zoneId.isEmpty()) return;
  
  Serial.println("\n📝 Logging watering event...");
  
  time_t now = time(nullptr);
  unsigned long startTimestamp = wateringStartTime * 1000; // Milliseconds
  unsigned long endTimestamp = now * 1000;
  
  // Calculate water used (simplified: 5L/min flow rate)
  float waterUsed = durationMinutes * 5.0;
  
  String historyPath = "watering_history";
  String historyId = String(now) + "_" + registry.zoneId;
  
  // Get zone name from Firebase first
  String zonePath = "zones/" + registry.zoneId + "/name";
  String zoneName = "Unknown Zone";
  
  if (Firebase.RTDB.getString(&fbdo, zonePath.c_str())) {
    zoneName = fbdo.stringData();
  }
  
  // Create history entry
  FirebaseJson json;
  json.set("zoneId", registry.zoneId);
  json.set("zoneName", zoneName);
  json.set("startTime", (int)startTimestamp);
  json.set("endTime", (int)endTimestamp);
  json.set("duration", durationMinutes);
  json.set("waterUsed", waterUsed);
  json.set("source", "manual");
  json.set("completed", true);
  
  String fullPath = historyPath + "/" + historyId;
  
  if (Firebase.RTDB.setJSON(&fbdo, fullPath.c_str(), &json)) {
    Serial.println("✓ Watering event logged");
    Serial.print("  Duration: ");
    Serial.print(durationMinutes);
    Serial.println(" minutes");
    Serial.print("  Water used: ~");
    Serial.print(waterUsed, 1);
    Serial.println("L");
  } else {
    Serial.println("✗ Failed to log event");
  }
}

// ==================== UTILITY: MAP FLOAT ====================
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ==================== UTILITY: GET FORMATTED TIME ====================
String getFormattedTime() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}