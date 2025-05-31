#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>

// ===== OLED Display Setup =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_MOSI 23
#define OLED_CLK  18
#define OLED_DC   17
#define OLED_CS   5
#define OLED_RESET 16
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);

// ===== DHT22 Sensor =====
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ===== Battery Monitoring =====
const int BATTERY_PIN = 34;
const float R1 = 100000.0, R2 = 47000.0;

// ===== Relay Pins =====
#define RELAY_LIVING 26
#define RELAY_BED    27
#define RELAY_KITCHEN 14
#define RELAY_GARDEN 12

// ===== WiFi and MQTT =====
const char* ssid = "SLT_FIBRE";
const char* password = "WPKB9328";
const char* mqtt_server = "broker.hivemq.com";
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ===== HTTP Upload Server =====
const char* httpServer = "https://phys.cmb.ac.lk/esp/post_data.php";
String apiKeyValue = "7cac68de958b354865fb8c4d6e9e95e6";
int location = 238;

// ===== NTP Time =====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
time_t et;

// ===== Web Server =====
WebServer server(80);

// ===== UDP =====
WiFiUDP udp;
const unsigned int udpPort = 4210;

// ===== Globals =====
float temp = 0.0, hum = 0.0;
String gardenOnTime = "", gardenOffTime = "";
unsigned long lastSensorRead = 0;
unsigned long lastTimeUpdate = 0;

unsigned long lastHttpUpload = 0;
const unsigned long httpInterval = 120000; // 2 minutes

// ===== Helper Functions =====
String formatTime12(String ntp) {
  int hour = ntp.substring(0, 2).toInt();
  int min = ntp.substring(3, 5).toInt();
  String ampm = hour >= 12 ? "PM" : "AM";
  hour = hour % 12;
  if (hour == 0) hour = 12;
  char buf[10];
  sprintf(buf, "%02d:%02d %s", hour, min, ampm.c_str());
  return String(buf);
}

float getBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < 15; i++) sum += analogRead(BATTERY_PIN);
  float avg = sum / 15.0;
  float vOut = avg * (3.3 / 4095.0);
  return vOut * ((R1 + R2) / R2);
}

String batteryIcon(float v) {
  if (v >= 8.0) return "[||||]";
  if (v >= 7.5) return "[||| ]";
  if (v >= 7.0) return "[||  ]";
  if (v >= 6.5) return "[|   ]";
  return "[LOW]";
}

void updateOLED(String timeStr) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Time: " + formatTime12(timeStr));

  display.setCursor(0, 12);
  display.print("Temp: "); display.print(temp, 1); display.print(" C");

  display.setCursor(0, 24);
  display.print("Hum : "); display.print(hum, 1); display.print(" %");

  float v = getBatteryVoltage();
  display.setCursor(0, 36);
  display.print("Bat : "); display.print(v, 2); display.print("V ");
  display.print(batteryIcon(v));

  display.setCursor(0, 48);
  display.print("Relays: ");
  display.print(digitalRead(RELAY_LIVING) ? "L " : "- ");
  display.print(digitalRead(RELAY_BED) ? "B " : "- ");
  display.print(digitalRead(RELAY_KITCHEN) ? "K " : "- ");
  display.print(digitalRead(RELAY_GARDEN) ? "G" : "-");

  display.display();
}

void checkSchedule() {
  String now = timeClient.getFormattedTime().substring(0, 5);
  if (now == gardenOnTime) digitalWrite(RELAY_GARDEN, HIGH);
  if (now == gardenOffTime) digitalWrite(RELAY_GARDEN, LOW);
}

// ===== Web Handlers =====
void handleWeb() {
  String html = R"rawliteral(
    <!DOCTYPE html><html><head><meta name='viewport' content='width=device-width'>
    <title>ESP32 Control Panel</title>
    <script>
      function fetchStatus() {
        fetch('/status').then(r=>r.json()).then(data=>{
          document.getElementById("time").textContent = data.time;
          document.getElementById("temp").textContent = data.temp +"°C";
          document.getElementById("hum").textContent = data.hum + "%";
          document.getElementById("bat").textContent = data.battery + " V";
        });
      }
      setInterval(fetchStatus, 1000);
    </script></head><body onload='fetchStatus()'>
    <h3>Smart Home Control</h3>
    <p>Time: <span id="time">--:--</span></p>
    <p>Temp: <span id="temp">--</span></p>
    <p>Humidity: <span id="hum">--</span></p>
    <p>Battery: <span id="bat">--</span></p>
    <hr>
    <button onclick="location.href='/relay?room=living&state=on'">Living ON</button>
    <button onclick="location.href='/relay?room=living&state=off'">Living OFF</button><br>
    <button onclick="location.href='/relay?room=bed&state=on'">Bed ON</button>
    <button onclick="location.href='/relay?room=bed&state=off'">Bed OFF</button><br>
    <button onclick="location.href='/relay?room=kitchen&state=on'">Kitchen ON</button>
    <button onclick="location.href='/relay?room=kitchen&state=off'">Kitchen OFF</button><br>
    <button onclick="location.href='/relay?room=garden&state=on'">Garden ON</button>
    <button onclick="location.href='/relay?room=garden&state=off'">Garden OFF</button><br>
    <form action='/settime'>
      <h4>Schedule Garden</h4>
      ON: <input type='time' name='on'> OFF: <input type='time' name='off'>
      <input type='submit' value='Set Schedule'>
    </form></body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleRelay() {
  String room = server.arg("room");
  String state = server.arg("state");
  int pin = -1;
  if (room == "living") pin = RELAY_LIVING;
  else if (room == "bed") pin = RELAY_BED;
  else if (room == "kitchen") pin = RELAY_KITCHEN;
  else if (room == "garden") pin = RELAY_GARDEN;

  if (pin != -1) {
    digitalWrite(pin, state == "on" ? HIGH : LOW);
    server.send(200, "text/plain", room + " turned " + state);
    Serial.printf("HTTP Relay %s: %s\n", room.c_str(), state.c_str());
  } else {
    server.send(400, "Invalid room");
  }
}

void handleSetTime() {
  gardenOnTime = server.arg("on");
  gardenOffTime = server.arg("off");
  server.send(200, "text/plain", "Garden schedule updated");
}

void handleStatus() {
  DynamicJsonDocument doc(256);
  doc["time"] = formatTime12(timeClient.getFormattedTime());
  doc["temp"] = temp;
  doc["hum"] = hum;
  doc["battery"] = getBatteryVoltage();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ===== MQTT Callbacks =====
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg = String((char*)payload).substring(0, len);
  String t = String(topic);
  if (t == "home/relay/living") digitalWrite(RELAY_LIVING, msg == "ON" ? HIGH : LOW);
  else if (t == "home/relay/bed") digitalWrite(RELAY_BED, msg == "ON" ? HIGH : LOW);
  else if (t == "home/relay/kitchen") digitalWrite(RELAY_KITCHEN, msg == "ON" ? HIGH : LOW);
  else if (t == "home/relay/garden") digitalWrite(RELAY_GARDEN, msg == "ON" ? HIGH : LOW);
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect("ESP32Client")) {
      Serial.println("connected");
      mqttClient.subscribe("home/relay/living");
      mqttClient.subscribe("home/relay/bed");
      mqttClient.subscribe("home/relay/kitchen");
      mqttClient.subscribe("home/relay/garden");
    } else {
      Serial.print("failed, rc=");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  dht.begin();
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  pinMode(RELAY_LIVING, OUTPUT);
  pinMode(RELAY_BED, OUTPUT);
  pinMode(RELAY_KITCHEN, OUTPUT);
  pinMode(RELAY_GARDEN, OUTPUT);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  Serial.println("WiFi Connected");
  Serial.println("IP Address: " + WiFi.localIP().toString());

  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println("OLED failed");
    while (1);
  }

  timeClient.begin();
  timeClient.setTimeOffset(19800);
  while (!timeClient.update()) timeClient.forceUpdate();

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);

  server.on("/", handleWeb);
  server.on("/relay", handleRelay);
  server.on("/settime", handleSetTime);
  server.on("/status", handleStatus);
  server.begin();
  udp.begin(udpPort);
}

// ===== Main Loop =====
void loop() {
  mqttClient.loop();
  if (!mqttClient.connected()) reconnectMQTT();
  server.handleClient();

  int packetSize = udp.parsePacket();
  if (packetSize) {
    char buf[50]; int len = udp.read(buf, 50);
    if (len > 0) buf[len] = 0;
    String cmd = String(buf); cmd.trim(); cmd.toUpperCase();
    if (cmd == "LIVING ON") digitalWrite(RELAY_LIVING, HIGH);
    else if (cmd == "LIVING OFF") digitalWrite(RELAY_LIVING, LOW);
    else if (cmd == "BED ON") digitalWrite(RELAY_BED, HIGH);
    else if (cmd == "BED OFF") digitalWrite(RELAY_BED, LOW);
    else if (cmd == "KITCHEN ON") digitalWrite(RELAY_KITCHEN, HIGH);
    else if (cmd == "KITCHEN OFF") digitalWrite(RELAY_KITCHEN, LOW);
    else if (cmd == "GARDEN ON") digitalWrite(RELAY_GARDEN, HIGH);
    else if (cmd == "GARDEN OFF") digitalWrite(RELAY_GARDEN, LOW);
  }

  unsigned long now = millis();
  if (now - lastSensorRead > 5000) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t < 80) {
      temp = t; hum = h;
    }
    lastSensorRead = now;
  }

  if (now - lastHttpUpload > httpInterval) {
    HTTPClient http;
    time_t epoch = timeClient.getEpochTime();

    auto send = [&](const char* param, float val) {
      http.begin(httpServer);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      char body[150];
      sprintf(body, "api_key=%s&sensor=2&location=%d&Parameter=%s&Value=%.2f&Reading_Time=%lu",
              apiKeyValue.c_str(), location, param, val, epoch);
      int code = http.POST(body);
      Serial.printf("HTTP POST %s = %.2f → %s\n", param, val, code > 0 ? "OK" : "FAIL");
      http.end();
      delay(200);
    };

    send("Temperature", temp);
    send("Humidity", hum);
    send("Relay_Living", digitalRead(RELAY_LIVING));
    send("Relay_Bed", digitalRead(RELAY_BED));
    send("Relay_Kitchen", digitalRead(RELAY_KITCHEN));
    send("Relay_Garden", digitalRead(RELAY_GARDEN));

    lastHttpUpload = now;
  }

  if (now - lastTimeUpdate >= 1000) {
    if (!timeClient.update()) timeClient.forceUpdate();
    updateOLED(timeClient.getFormattedTime());
    checkSchedule();
    lastTimeUpdate = now;
  }
}
