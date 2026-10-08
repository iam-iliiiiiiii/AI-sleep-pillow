/*
 * SmartSleepPillowAgent.ino
 * 基於 AMB82-Mini 的智慧睡眠抱枕韌體完整架構
 * 
 * 整合功能清單:
 * 1. 溫控 (加熱/致冷) 與硬體安全 Timeout
 * 2. 舒壓香氛釋放與硬體安全 Timeout
 * 3. 共振呼吸燈與 LED 表情
 * 4. 淺眠喚醒與擁抱安撫震動
 * 5. 藍牙音響 (A2DP Sink) - 供手機端播放/暫停音樂
 * 6. 麥克風異音與夢話主動推播 (Event Push)
 * 7. 感測器資料/事件 JSON 回報 (體溫、睡眠狀態等)
 */

#include <WiFi.h>
// 引入藍牙音訊函式庫 (請依據 AMB82-Mini 或使用之微控制器實際支援的 A2DP 函式庫替換)
#include "BluetoothA2DPSink.h" 

// ---------- 網路與伺服器設定 ----------
char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";
WiFiServer server(8080);

// ---------- 代理模型 (Agent) 設定 (用於主動推播事件) ----------
const char* agentHost = "192.168.1.100"; // 請替換為執行 Agent 的 Android 手機 IP
const int agentPort = 3000;

BluetoothA2DPSink a2dp_sink;

// ---------- 安全與狀態定義 ----------
enum Emotion {
  EMOTION_HAPPY,
  EMOTION_SLEEPY,
  EMOTION_CALM,
  EMOTION_OFF
};

Emotion currentEmotion = EMOTION_OFF;

// 硬體安全逾時 (Timeout) 狀態追蹤
unsigned long lastActionTime = 0;
bool isHeating = false;
bool isCooling = false;
bool isVibrating = false;
bool isScenting = false;

// 安全限制設定 (單位：毫秒)
const unsigned long MAX_HEAT_DURATION = 15 * 60 * 1000;  // 15 分鐘
const unsigned long MAX_VIBE_DURATION = 5 * 60 * 1000;   // 5 分鐘
const unsigned long MAX_SCENT_DURATION = 2 * 60 * 1000;  // 2 分鐘
const float MAX_SAFE_TEMP = 38.0;                        // 最高安全溫度

// 音訊事件狀態
unsigned long lastAudioEventTime = 0;
const int AUDIO_COOLDOWN = 5000; // 避免連續觸發，設定 5 秒冷卻時間

// ---------- Forward declarations ----------
void sendJson(WiFiClient &client, int statusCode, const String &json);
void sendCorsPreflight(WiFiClient &client);
String getRequestPath(const String &requestLine);
void setEmotion(Emotion e);
void startBreathingLight(const String &rate);
void startVibration(const String &mode);
void setTemperatureControl(const String &mode);
void releaseScent(const String &scentType);
void stopAllActions();
void checkSafetyTimeouts();
void checkAudioEvents();
void sendEventToAgent(String jsonPayload);
String getSensorData();

void setup() {
  Serial.begin(115200);
  delay(500);

  // 1. 初始化藍牙音響 (手機配對名稱為 Smart_Pillow_Audio)
  a2dp_sink.start("Smart_Pillow_Audio");
  Serial.println("藍牙音響已啟動，等待配對...");

  // TODO: 初始化 LED、致冷/加熱 PWM、震動馬達、擴香模組、溫度感測器與麥克風腳位

  // 2. 初始化 Wi-Fi
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  Serial.print("Pillow IP: ");
  Serial.println(WiFi.localIP());

  // 3. 啟動 API 伺服器
  server.begin();
  Serial.println("Smart Sleep Pillow API listening on port 8080");
}

void loop() {
  // 1. 檢查硬體安全限制 (防過熱與超時)
  checkSafetyTimeouts();

  // 2. 持續監聽麥克風，處理環境噪音與夢話事件 (主動推播)
  checkAudioEvents();

  // 3. 處理 HTTP API 請求 (被動接收控制)
  WiFiClient client = server.available();
  if (!client) return;

  client.setTimeout(1000);
  String requestLine = client.readStringUntil('\r');
  client.readStringUntil('\n');
  Serial.println(requestLine);

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  if (requestLine.startsWith("OPTIONS ")) {
    sendCorsPreflight(client);
    client.stop();
    return;
  }

  if (!requestLine.startsWith("GET ")) {
    sendJson(client, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    client.stop();
    return;
  }

  String path = getRequestPath(requestLine);

  // ---------- API 路由定義 ----------
  if (path == "/status") {
    sendJson(client, 200, getSensorData());
  }
  else if (path == "/temp/warm") {
    setTemperatureControl("warm");
    sendJson(client, 200, "{\"ok\":true,\"action\":\"temp_control\",\"mode\":\"warm\"}");
  }
  else if (path == "/temp/cool") {
    setTemperatureControl("cool");
    sendJson(client, 200, "{\"ok\":true,\"action\":\"temp_control\",\"mode\":\"cool\"}");
  }
  else if (path == "/vibrate/wakeup") {
    startVibration("wakeup");
    sendJson(client, 200, "{\"ok\":true,\"action\":\"vibrate\",\"mode\":\"wakeup\"}");
  }
  else if (path == "/vibrate/heartbeat") {
    startVibration("heartbeat");
    sendJson(client, 200, "{\"ok\":true,\"action\":\"vibrate\",\"mode\":\"heartbeat\"}");
  }
  else if (path == "/light/breathe_slow") {
    startBreathingLight("slow");
    sendJson(client, 200, "{\"ok\":true,\"action\":\"breathing_light\",\"rate\":\"slow\"}");
  }
  else if (path == "/scent/lavender") {
    releaseScent("lavender");
    sendJson(client, 200, "{\"ok\":true,\"action\":\"scent\",\"value\":\"lavender\"}");
  }
  else if (path == "/emotion/happy") {
    setEmotion(EMOTION_HAPPY);
    sendJson(client, 200, "{\"ok\":true,\"emotion\":\"happy\"}");
  }
  else if (path == "/stop") {
    stopAllActions();
    sendJson(client, 200, "{\"ok\":true,\"action\":\"stop\"}");
  }
  else {
    sendJson(client, 404, "{\"ok\":false,\"error\":\"unknown_command\"}");
  }

  delay(1);
  client.stop();
}

// ---------------------------------------------------------------------------
// 網路輔助函數
// ---------------------------------------------------------------------------
String getRequestPath(const String &requestLine) {
  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  if (firstSpace < 0 || secondSpace < 0) return "/";
  return requestLine.substring(firstSpace + 1, secondSpace);
}

void sendJson(WiFiClient &client, int statusCode, const String &json) {
  const char *statusText = "OK";
  if (statusCode == 400) statusText = "Bad Request";
  else if (statusCode == 404) statusText = "Not Found";
  else if (statusCode == 405) statusText = "Method Not Allowed";

  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.print(' ');
  client.println(statusText);
  client.println("Content-Type: application/json; charset=utf-8");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();
  client.print(json);
}

void sendCorsPreflight(WiFiClient &client) {
  client.println("HTTP/1.1 204 No Content");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET, OPTIONS");
  client.println("Connection: close");
  client.println();
}

// ---------------------------------------------------------------------------
// 硬體安全機制 (Safety First)
// ---------------------------------------------------------------------------
void checkSafetyTimeouts() {
  unsigned long currentTime = millis();
  
  // TODO: 實際讀取溫度感測器數值
  float currentTemp = 36.5; 
  
  if (isHeating && currentTemp >= MAX_SAFE_TEMP) {
      Serial.println("SAFETY ALERT: 達到溫度上限，強制停止加熱。");
      stopAllActions();
      return;
  }
  if (isHeating && (currentTime - lastActionTime > MAX_HEAT_DURATION)) {
      Serial.println("TIMEOUT: 加熱超時，自動停止。");
      stopAllActions();
  }
  if (isVibrating && (currentTime - lastActionTime > MAX_VIBE_DURATION)) {
      Serial.println("TIMEOUT: 震動超時，自動停止。");
      stopAllActions();
  }
  if (isScenting && (currentTime - lastActionTime > MAX_SCENT_DURATION)) {
      Serial.println("TIMEOUT: 擴香超時，自動停止。");
      stopAllActions();
  }
}

void stopAllActions() {
  isHeating = false;
  isCooling = false;
  isVibrating = false;
  isScenting = false;
  currentEmotion = EMOTION_OFF;
  
  // TODO: 傳送停止 PWM 訊號給致冷晶片/加熱模組、震動馬達、關閉擴香模組與 LED
  Serial.println("所有硬體動作已安全停止。");
}

// ---------------------------------------------------------------------------
// 音訊事件偵測與主動通報 (Event Push)
// ---------------------------------------------------------------------------
void checkAudioEvents() {
  unsigned long currentTime = millis();
  if (currentTime - lastAudioEventTime < AUDIO_COOLDOWN) return;

  // TODO: 從麥克風讀取並分析聲音。0:安靜, 1:環境噪音, 2:夢話/異音
  int detectedAudioType = 0; 
  
  if (detectedAudioType == 1) {
    Serial.println("偵測到環境噪音，發送事件給代理模型...");
    sendEventToAgent("{\"event\":\"noise_detected\", \"type\":\"environmental\"}");
    lastAudioEventTime = currentTime;
  } 
  else if (detectedAudioType == 2) {
    Serial.println("偵測到睡眠異音，發送事件進行紀錄...");
    sendEventToAgent("{\"event\":\"audio_anomaly\", \"type\":\"sleep_talk\"}");
    lastAudioEventTime = currentTime;
  }
}

void sendEventToAgent(String jsonPayload) {
  WiFiClient client;
  if (client.connect(agentHost, agentPort)) {
    client.println("POST /events HTTP/1.1");
    client.println("Host: " + String(agentHost));
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(jsonPayload.length());
    client.println();
    client.println(jsonPayload);
    client.stop();
  } else {
    Serial.println("無法連線至代理模型發送事件。");
  }
}

// ---------------------------------------------------------------------------
// 硬體動作配接器 (Hardware Adapters)
// ---------------------------------------------------------------------------
void setTemperatureControl(const String &mode) {
  stopAllActions(); 
  lastActionTime = millis();
  if (mode == "warm") {
    isHeating = true;
    Serial.println("硬體: 開始溫和加熱");
  } else if (mode == "cool") {
    isCooling = true;
    Serial.println("硬體: 開始舒適降溫");
  }
}

void startVibration(const String &mode) {
  stopAllActions();
  lastActionTime = millis();
  isVibrating = true;
  if (mode == "wakeup") {
    Serial.println("硬體: 啟動淺眠喚醒震動");
  } else if (mode == "heartbeat") {
    Serial.println("硬體: 啟動擁抱安撫(心跳)震動");
  }
}

void startBreathingLight(const String &rate) {
  if (rate == "slow") Serial.println("硬體: 啟動舒眠共振呼吸燈");
}

void releaseScent(const String &scentType) {
  if (scentType == "lavender") {
    stopAllActions();
    lastActionTime = millis();
    isScenting = true;
    Serial.println("硬體: 釋放薰衣草香氛");
  }
}

void setEmotion(Emotion e) {
  currentEmotion = e;
}

// ---------------------------------------------------------------------------
// 感測器資料回報
// ---------------------------------------------------------------------------
String getSensorData() {
  float temp = 36.4; // TODO: 讀取實際體溫
  String sleepStage = "light_sleep"; // TODO: 根據感測器評估睡眠狀態
  bool isHugged = false; // TODO: 讀取壓力感測器
  
  String json = "{";
  json += "\"ok\":true,";
  json += "\"body_temp\":" + String(temp) + ",";
  json += "\"sleep_stage\":\"" + sleepStage + "\",";
  json += "\"hugged\":" + String(isHugged ? "true" : "false") + ",";
  json += "\"is_heating\":" + String(isHeating ? "true" : "false") + ",";
  json += "\"is_cooling\":" + String(isCooling ? "true" : "false");
  json += "}";
  return json;
}
