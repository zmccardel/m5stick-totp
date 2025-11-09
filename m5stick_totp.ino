/*
 * M5StickCPlus2 TOTP Authenticator with BLE HID
 * 
 * Required Libraries:
 * - M5StickCPlus2
 * - TOTP-Arduino by lucadentella
 * 
 * WiFi credentials and accounts can be configured via web interface
 */

#include <M5StickCPlus2.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <TOTP.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEHIDDevice.h>
#include <esp_gap_ble_api.h>

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

// WiFi Network Structure
struct WiFiNetwork {
  char ssid[50];
  char password[64];
};

WiFiNetwork wifiNetworks[5];
int numWifiNetworks = 0;

// TOTP Account Structure
struct TOTPAccount {
  char name[50];
  uint8_t secret[32];
  int secretLength;
};

TOTPAccount accounts[10];
int numAccounts = 0;
int currentAccount = 0;

// BLE HID Keyboard
BLEHIDDevice* hid;
BLECharacteristic* inputKeyboard;
BLEServer* pServer;
BLEAdvertising* pAdvertising;
bool bleConnected = false;

bool timeInitialized = false;
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

bool setupMode = false;
const byte DNS_PORT = 53;
const IPAddress apIP(192, 168, 4, 1);

// Forward declarations
void loadWifiNetworks();
void loadAccounts();
bool addWifiNetwork(String ssid, String password);
void deleteWifiNetwork(int index);
bool addAccount(String name, String secret);
void deleteAccount(int index);
int base32Decode(String input, uint8_t* output);
int parseAndImportAccounts(String data);
String generateTOTP(TOTPAccount& account);
int getTimeRemaining();
void drawProgressBar(int timeLeft);
void displayCurrentAccount();
void updateProgressBar();
void showSetupMessage();
void sendCodeViaBLE();

// BLE Server Callbacks
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected = true;
    Serial.println("BLE Client Connected");
    
    if (inputKeyboard) {
      BLEDescriptor *desc = inputKeyboard->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
      if (desc) {
        uint8_t val[] = {0x01, 0x00};
        desc->setValue(val, 2);
        Serial.println("CCCD reset - notifications enabled");
      }
    }
  }

  void onDisconnect(BLEServer* pServer) {
    bleConnected = false;
    Serial.println("BLE Client Disconnected");
    delay(500);
    pAdvertising->start();
    Serial.println("Restarted advertising");
  }
};

// HID Report Descriptor
const uint8_t hidReportDescriptor[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07,
  0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
  0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
  0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

void initBLE() {
  Serial.println("Initializing BLE...");
  
  if (BLEDevice::getInitialized()) {
    Serial.println("BLE was initialized, deinitializing...");
    BLEDevice::deinit(true);
    delay(1000);
  }
  
  BLEDevice::init("M5Stick TOTP");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  hid = new BLEHIDDevice(pServer);
  inputKeyboard = hid->inputReport(1);

  hid->manufacturer()->setValue("M5Stack");
  hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
  hid->hidInfo(0x00, 0x01);

  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);

  hid->reportMap((uint8_t*)hidReportDescriptor, sizeof(hidReportDescriptor));
  hid->startServices();

  pAdvertising = pServer->getAdvertising();
  pAdvertising->setAppearance(HID_KEYBOARD);
  pAdvertising->addServiceUUID(hid->hidService()->getUUID());
  pAdvertising->start();
  
  Serial.println("BLE HID Keyboard started");
}

void sendBLEKey(uint8_t key) {
  if (!bleConnected || !inputKeyboard) return;
  
  uint8_t report[8] = {0, 0, key, 0, 0, 0, 0, 0};
  inputKeyboard->setValue(report, sizeof(report));
  inputKeyboard->notify();
  delay(20);
  
  uint8_t reportRelease[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  inputKeyboard->setValue(reportRelease, sizeof(reportRelease));
  inputKeyboard->notify();
  delay(20);
}

void sendBLEString(String text) {
  for (int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c >= '0' && c <= '9') {
      uint8_t keycode = (c == '0') ? 0x27 : 0x1E + (c - '1');
      sendBLEKey(keycode);
    }
  }
}

const char* htmlHeader = R"(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:Arial;margin:0;padding:20px;background:#1a1a1a;color:#fff}
h1{color:#00d4ff;margin-bottom:10px}
input,textarea{width:100%;padding:12px;margin:8px 0;background:#2a2a2a;color:#fff;border:1px solid #00d4ff;border-radius:5px;box-sizing:border-box}
button{background:#00d4ff;color:#000;padding:12px 24px;border:none;border-radius:5px;cursor:pointer;font-size:16px;margin:5px 0;width:100%}
button:hover{background:#00a8cc}
.account{background:#2a2a2a;padding:12px;margin:10px 0;border-radius:5px;border-left:3px solid #00d4ff;display:flex;justify-content:space-between;align-items:center}
.success{color:#00ff00;padding:10px;background:#1a3a1a;border-radius:5px;margin:10px 0}
.error{color:#ff0000;padding:10px;background:#3a1a1a;border-radius:5px;margin:10px 0}
.btn-delete{background:#ff4444;width:auto;padding:8px 16px}
</style>
</head><body><div style="max-width:600px;margin:0 auto">
)";

const char* htmlFooter = "</div></body></html>";

String getMainPage() {
  String html = htmlHeader;
  html += "<h1>M5Stick TOTP Setup</h1>";
  
  html += "<h2>WiFi Networks (" + String(numWifiNetworks) + "/5)</h2>";
  if (numWifiNetworks > 0) {
    for (int i = 0; i < numWifiNetworks; i++) {
      html += "<div class='account'><span>" + String(wifiNetworks[i].ssid) + "</span>";
      html += "<button class='btn-delete' onclick=\"if(confirm('Delete?'))location.href='/deletewifi?id=" + String(i) + "'\">Delete</button></div>";
    }
  } else {
    html += "<p>No WiFi networks configured</p>";
  }
  
  html += "<h3>Add WiFi Network</h3>";
  html += "<form action='/addwifi' method='POST'>";
  html += "SSID: <input name='ssid' required><br>";
  html += "Password: <input type='password' name='pass'><br>";
  html += "<button type='submit'>Add WiFi Network</button></form>";
  
  html += "<h2>TOTP Accounts (" + String(numAccounts) + "/10)</h2>";
  for (int i = 0; i < numAccounts; i++) {
    html += "<div class='account'><span>" + String(accounts[i].name) + "</span>";
    html += "<button class='btn-delete' onclick=\"if(confirm('Delete?'))location.href='/delete?id=" + String(i) + "'\">Delete</button></div>";
  }
  
  html += "<h2>Add Account</h2>";
  html += "<form action='/add' method='POST'>";
  html += "Name: <input name='name' placeholder='GitHub' required><br>";
  html += "Secret (Base32): <input name='secret' placeholder='JBSWY3DPEHPK3PXP' required><br>";
  html += "<button type='submit'>Add Account</button></form>";
  
  html += "<h2>Import Multiple (CSV)</h2>";
  html += "<form action='/import' method='POST'>";
  html += "<textarea name='csv' rows='6' placeholder='GitHub,JBSWY3DPEHPK3PXP&#10;Google,HXDMVJECJJWSRB3H'></textarea>";
  html += "<button type='submit'>Import Accounts</button></form>";
  
  html += "<button onclick=\"if(confirm('Restart?'))location.href='/restart'\">Restart Device</button>";
  html += "<button onclick=\"if(confirm('Clear BLE?'))location.href='/bleforget'\">Forget Bluetooth</button>";
  html += htmlFooter;
  return html;
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getMainPage());
  });
  
  server.on("/addwifi", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String html = htmlHeader;
    if (addWifiNetwork(ssid, pass)) {
      html += "<h1>WiFi Added!</h1><p class='success'>SSID: " + ssid + "</p>";
    } else {
      html += "<h1>Error!</h1><p class='error'>Failed to add</p>";
    }
    html += "<a href='/'><button>Back</button></a>" + String(htmlFooter);
    server.send(200, "text/html", html);
  });
  
  server.on("/deletewifi", HTTP_GET, []() {
    deleteWifiNetwork(server.arg("id").toInt());
    server.sendHeader("Location", "/");
    server.send(302);
  });
  
  server.on("/add", HTTP_POST, []() {
    String name = server.arg("name");
    String secret = server.arg("secret");
    String html = htmlHeader;
    if (addAccount(name, secret)) {
      html += "<h1>Success!</h1><p class='success'>Account '" + name + "' added</p>";
    } else {
      html += "<h1>Error!</h1><p class='error'>Failed to add</p>";
    }
    html += "<a href='/'><button>Back</button></a>" + String(htmlFooter);
    server.send(200, "text/html", html);
  });
  
  server.on("/import", HTTP_POST, []() {
    int imported = parseAndImportAccounts(server.arg("csv"));
    String html = htmlHeader;
    html += "<h1>Import Complete</h1><p class='success'>Imported " + String(imported) + " accounts</p>";
    html += "<a href='/'><button>Back</button></a>" + String(htmlFooter);
    server.send(200, "text/html", html);
  });
  
  server.on("/delete", HTTP_GET, []() {
    deleteAccount(server.arg("id").toInt());
    server.sendHeader("Location", "/");
    server.send(302);
  });
  
  server.on("/restart", HTTP_GET, []() {
    server.send(200, "text/html", String(htmlHeader) + "<h1>Restarting...</h1>" + htmlFooter);
    delay(2000);
    ESP.restart();
  });
  
  server.on("/bleforget", HTTP_GET, []() {
    int dev_num = esp_ble_get_bond_device_num();
    esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
    esp_ble_get_bond_device_list(&dev_num, dev_list);
    for (int i = 0; i < dev_num; i++) {
      esp_ble_remove_bond_device(dev_list[i].bd_addr);
    }
    free(dev_list);
    String html = htmlHeader;
    html += "<h1>Bluetooth Cleared</h1><p class='success'>Restart to pair again</p>";
    html += "<a href='/'><button>Back</button></a>" + String(htmlFooter);
    server.send(200, "text/html", html);
  });
  
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302);
  });
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(3);
  M5.Display.fillScreen(BLACK);
  
  Serial.begin(115200);
  Serial.println("M5StickCPlus2 TOTP");
  
  preferences.begin("totp", false);
  loadWifiNetworks();
  loadAccounts();
  initBLE();
  
  if (numWifiNetworks == 0 || numAccounts == 0) {
    setupMode = true;
    startSetupMode();
  } else {
    if (connectToWiFi()) {
      initTime();
    }
    if (numAccounts > 0) {
      displayCurrentAccount();
    }
  }
}

void startSetupMode() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, 20);
  M5.Display.setTextColor(YELLOW);
  M5.Display.println("Setup Mode");
  M5.Display.setCursor(5, 40);
  M5.Display.setTextColor(WHITE);
  M5.Display.println("WiFi: M5Stick-TOTP");
  M5.Display.setCursor(5, 60);
  M5.Display.println("URL: 192.168.4.1");
  M5.Display.setCursor(5, 90);
  M5.Display.setTextColor(DARKGREY);
  M5.Display.println("Press A to exit");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("M5Stick-TOTP");
  
  dnsServer.start(DNS_PORT, "*", apIP);
  setupWebServer();
  server.begin();
}

bool connectToWiFi() {
  if (numWifiNetworks == 0) return false;
  
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(10, 50);
  M5.Display.setTextSize(1);
  M5.Display.println("Connecting...");
  
  WiFi.mode(WIFI_STA);
  
  for (int i = 0; i < numWifiNetworks; i++) {
    WiFi.begin(wifiNetworks[i].ssid, wifiNetworks[i].password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi connected");
      return true;
    }
    WiFi.disconnect();
  }
  
  return false;
}

void initTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    delay(1000);
    attempts++;
  }
  if (getLocalTime(&timeinfo)) {
    timeInitialized = true;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void loadWifiNetworks() {
  numWifiNetworks = preferences.getInt("numWifi", 0);
  for (int i = 0; i < numWifiNetworks && i < 5; i++) {
    String keySSID = "wifi" + String(i) + "ssid";
    String keyPass = "wifi" + String(i) + "pass";
    strncpy(wifiNetworks[i].ssid, preferences.getString(keySSID.c_str(), "").c_str(), 49);
    wifiNetworks[i].ssid[49] = '\0';
    strncpy(wifiNetworks[i].password, preferences.getString(keyPass.c_str(), "").c_str(), 63);
    wifiNetworks[i].password[63] = '\0';
  }
}

bool addWifiNetwork(String ssid, String password) {
  if (numWifiNetworks >= 5) return false;
  
  for (int i = 0; i < numWifiNetworks; i++) {
    if (String(wifiNetworks[i].ssid) == ssid) {
      strncpy(wifiNetworks[i].password, password.c_str(), 63);
      wifiNetworks[i].password[63] = '\0';
      preferences.putString(("wifi" + String(i) + "pass").c_str(), password);
      return true;
    }
  }
  
  strncpy(wifiNetworks[numWifiNetworks].ssid, ssid.c_str(), 49);
  wifiNetworks[numWifiNetworks].ssid[49] = '\0';
  strncpy(wifiNetworks[numWifiNetworks].password, password.c_str(), 63);
  wifiNetworks[numWifiNetworks].password[63] = '\0';
  
  preferences.putString(("wifi" + String(numWifiNetworks) + "ssid").c_str(), ssid);
  preferences.putString(("wifi" + String(numWifiNetworks) + "pass").c_str(), password);
  
  numWifiNetworks++;
  preferences.putInt("numWifi", numWifiNetworks);
  return true;
}

void deleteWifiNetwork(int index) {
  if (index < 0 || index >= numWifiNetworks) return;
  
  for (int i = index; i < numWifiNetworks - 1; i++) {
    wifiNetworks[i] = wifiNetworks[i + 1];
    preferences.putString(("wifi" + String(i) + "ssid").c_str(), 
                          preferences.getString(("wifi" + String(i + 1) + "ssid").c_str(), ""));
    preferences.putString(("wifi" + String(i) + "pass").c_str(), 
                          preferences.getString(("wifi" + String(i + 1) + "pass").c_str(), ""));
  }
  
  numWifiNetworks--;
  preferences.remove(("wifi" + String(numWifiNetworks) + "ssid").c_str());
  preferences.remove(("wifi" + String(numWifiNetworks) + "pass").c_str());
  preferences.putInt("numWifi", numWifiNetworks);
}

void loadAccounts() {
  numAccounts = preferences.getInt("numAccounts", 0);
  
  for (int i = 0; i < numAccounts && i < 10; i++) {
    String value = preferences.getString(("acc" + String(i)).c_str(), "");
    
    if (value.length() > 0) {
      int pipePos = value.indexOf('|');
      if (pipePos > 0) {
        String name = value.substring(0, pipePos);
        String secret = value.substring(pipePos + 1);
        
        strncpy(accounts[i].name, name.c_str(), 49);
        accounts[i].name[49] = '\0';
        accounts[i].secretLength = base32Decode(secret, accounts[i].secret);
      }
    }
  }
}

bool addAccount(String name, String secret) {
  if (numAccounts >= 10) return false;
  
  secret.replace(" ", "");
  secret.toUpperCase();
  
  int secretLen = base32Decode(secret, accounts[numAccounts].secret);
  if (secretLen <= 0) return false;
  
  strncpy(accounts[numAccounts].name, name.c_str(), 49);
  accounts[numAccounts].name[49] = '\0';
  accounts[numAccounts].secretLength = secretLen;
  
  preferences.putString(("acc" + String(numAccounts)).c_str(), name + "|" + secret);
  numAccounts++;
  preferences.putInt("numAccounts", numAccounts);
  return true;
}

void deleteAccount(int index) {
  if (index < 0 || index >= numAccounts) return;
  
  for (int i = index; i < numAccounts - 1; i++) {
    accounts[i] = accounts[i + 1];
    preferences.putString(("acc" + String(i)).c_str(), 
                          preferences.getString(("acc" + String(i + 1)).c_str(), ""));
  }
  
  numAccounts--;
  preferences.remove(("acc" + String(numAccounts)).c_str());
  preferences.putInt("numAccounts", numAccounts);
  
  if (currentAccount >= numAccounts && currentAccount > 0) {
    currentAccount = numAccounts - 1;
  }
}

int base32Decode(String input, uint8_t* output) {
  const char* base32Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  int outputLen = 0;
  int buffer = 0;
  int bitsLeft = 0;
  
  for (int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '=') break;
    
    const char* p = strchr(base32Chars, c);
    if (!p) continue;
    
    int val = p - base32Chars;
    buffer = (buffer << 5) | val;
    bitsLeft += 5;
    
    if (bitsLeft >= 8) {
      output[outputLen++] = (buffer >> (bitsLeft - 8)) & 0xFF;
      bitsLeft -= 8;
    }
  }
  
  return outputLen;
}

int parseAndImportAccounts(String data) {
  int imported = 0;
  int startPos = 0;
  
  while (startPos < data.length()) {
    int lineEnd = data.indexOf('\n', startPos);
    if (lineEnd == -1) lineEnd = data.length();
    
    String line = data.substring(startPos, lineEnd);
    line.trim();
    
    if (line.length() > 0) {
      int commaPos = line.indexOf(',');
      if (commaPos > 0) {
        String name = line.substring(0, commaPos);
        String secret = line.substring(commaPos + 1);
        name.trim();
        secret.trim();
        
        if (addAccount(name, secret)) {
          imported++;
        }
      }
    }
    
    startPos = lineEnd + 1;
  }
  
  return imported;
}

String generateTOTP(TOTPAccount& account) {
  if (!timeInitialized) {
    return "------";
  }
  
  time_t now = time(nullptr);
  TOTP totp(account.secret, account.secretLength);
  char* code = totp.getCode(now);
  return String(code);
}

int getTimeRemaining() {
  time_t now = time(nullptr);
  return 30 - (now % 30);
}

void drawProgressBar(int timeLeft) {
  int barWidth = M5.Display.width() - 20;
  int barHeight = 6;
  int barX = 10;
  int barY = M5.Display.height() - 20;
  
  M5.Display.fillRect(barX, barY, barWidth, barHeight, DARKGREY);
  
  float progress = (float)timeLeft / 30.0;
  int fillWidth = (int)(barWidth * progress);
  
  uint16_t color = GREEN;
  if (timeLeft < 5) color = RED;
  else if (timeLeft < 10) color = YELLOW;
  
  M5.Display.fillRect(barX, barY, fillWidth, barHeight, color);
  M5.Display.drawRect(barX, barY, barWidth, barHeight, WHITE);
}

void displayCurrentAccount() {
  M5.Display.fillScreen(BLACK);
  
  M5.Display.setTextSize(2);
  M5.Display.setCursor(5, 5);
  M5.Display.setTextColor(CYAN);
  M5.Display.println(accounts[currentAccount].name);
  
  M5.Display.setCursor(180, 5);
  M5.Display.setTextColor(DARKGREY);
  char posStr[10];
  sprintf(posStr, "%d/%d", currentAccount + 1, numAccounts);
  M5.Display.println(posStr);
  
  String code = generateTOTP(accounts[currentAccount]);
  M5.Display.setTextSize(4);
  M5.Display.setTextColor(WHITE);
  
  int charWidth = 24;
  int codeWidth = code.length() * charWidth;
  int xPos = (M5.Display.width() - codeWidth) / 2;
  int yPos = (M5.Display.height() / 2) - 16;
  
  M5.Display.setCursor(xPos, yPos);
  M5.Display.println(code);
  
  int timeLeft = getTimeRemaining();
  drawProgressBar(timeLeft);
  
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, M5.Display.height() - 30);
  if (bleConnected) {
    M5.Display.setTextColor(GREEN);
  } else {
    M5.Display.setTextColor(RED);
  }
  M5.Display.println("BLE");
}

void updateProgressBar() {
  int timeLeft = getTimeRemaining();
  drawProgressBar(timeLeft);
}

void showSetupMessage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 30);
  M5.Display.setTextColor(YELLOW);
  M5.Display.println("No accounts!");
  M5.Display.setCursor(10, 50);
  M5.Display.setTextColor(WHITE);
  M5.Display.println("Hold B 3s for setup");
}

void sendCodeViaBLE() {
  if (!bleConnected) {
    M5.Display.fillScreen(RED);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(20, 50);
    M5.Display.println("NOT PAIRED!");
    delay(2000);
    displayCurrentAccount();
    return;
  }
  
  String code = generateTOTP(accounts[currentAccount]);
  
  M5.Display.fillScreen(GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(BLACK);
  M5.Display.setCursor(40, 60);
  M5.Display.println("SENDING");
  
  delay(200);
  sendBLEString(code);
  
  M5.update();
  if (M5.BtnA.isPressed()) {
    sendBLEKey(0x28); // Enter
  }
  
  delay(500);
  displayCurrentAccount();
}

void loop() {
  if (setupMode) {
    // In setup mode - only handle web server and A button to exit
    dnsServer.processNextRequest();
    server.handleClient();
    
    M5.update();
    if (M5.BtnA.wasPressed()) {
      ESP.restart();
    }
    
    delay(10);
    return;  // Critical: exit loop here, don't do anything else
  }
  
  // NOT in setup mode - normal operation
  M5.update();
  
  static unsigned long bPressStart = 0;
  static bool bWasPressed = false;
  static bool progressShown = false;
  
  if (M5.BtnB.wasPressed()) {
    bPressStart = millis();
    bWasPressed = true;
    progressShown = false;
  }
  
  if (bWasPressed && M5.BtnB.isPressed()) {
    unsigned long holdTime = millis() - bPressStart;
    
    if (holdTime > 500 && !progressShown) {
      progressShown = true;
      M5.Display.fillRect(0, M5.Display.height() - 40, M5.Display.width(), 40, BLACK);
      M5.Display.setTextSize(1);
      M5.Display.setCursor(10, M5.Display.height() - 30);
      M5.Display.setTextColor(YELLOW);
      M5.Display.println("Hold B for setup...");
    }
    
    if (holdTime >= 3000) {
      M5.Display.fillScreen(BLUE);
      M5.Display.setCursor(30, 60);
      M5.Display.setTextSize(2);
      M5.Display.setTextColor(WHITE);
      M5.Display.println("Setup Mode");
      delay(1000);
      bWasPressed = false;
      progressShown = false;
      
      // Set flag and call setup - next loop iteration will handle it
      setupMode = true;
      startSetupMode();
      return;
    }
  }
  
  if (M5.BtnB.wasReleased()) {
    unsigned long holdTime = millis() - bPressStart;
    
    if (bWasPressed && holdTime < 3000) {
      if (numAccounts > 0) {
        currentAccount = (currentAccount + 1) % numAccounts;
        displayCurrentAccount();
      }
    }
    bWasPressed = false;
    progressShown = false;
  }
  
  if (M5.BtnPWR.wasPressed()) {
    if (numAccounts > 0) {
      currentAccount = (currentAccount - 1 + numAccounts) % numAccounts;
      displayCurrentAccount();
    }
  }
  
  if (M5.BtnA.wasPressed()) {
    if (numAccounts > 0) {
      sendCodeViaBLE();
    }
  }
  
  // Progress bar updates - only when accounts exist and not holding B
  static unsigned long lastUpdate = 0;
  static int lastTimeLeft = -1;
  if (millis() - lastUpdate > 1000) {
    lastUpdate = millis();
    if (numAccounts > 0 && !bWasPressed) {
      int currentTimeLeft = getTimeRemaining();
      if (lastTimeLeft == 1 && currentTimeLeft > 25) {
        displayCurrentAccount();
      } else {
        updateProgressBar();
      }
      lastTimeLeft = currentTimeLeft;
    } else if (numAccounts == 0) {
      showSetupMessage();
    }
  }
  
  delay(10);
}
