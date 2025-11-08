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

// TOTP Account Structure
struct TOTPAccount {
  char name[50];
  uint8_t secret[32];
  int secretLength;
};

// TOTP accounts - will be loaded from preferences or defaults
TOTPAccount accounts[10]; // Support up to 10 accounts
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

// BLE Server Callbacks
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected = true;
    Serial.println("BLE Client Connected");
    
    // CRITICAL FIX: Reset CCCD descriptor to re-enable notifications after reconnect
    // This fixes the Windows reconnection issue
    if (inputKeyboard) {
      BLEDescriptor *desc = inputKeyboard->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
      if (desc) {
        uint8_t val[] = {0x01, 0x00};  // Enable notifications
        desc->setValue(val, 2);
        Serial.println("CCCD reset - notifications enabled");
      }
    }
  }

  void onDisconnect(BLEServer* pServer) {
    bleConnected = false;
    Serial.println("BLE Client Disconnected");
    
    // Restart advertising after short delay
    delay(500);
    pAdvertising->start();
    Serial.println("Restarted advertising");
  }
};

// HID Report Descriptor for Keyboard
const uint8_t hidReportDescriptor[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07,
  0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
  0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
  0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

void initBLE() {
  Serial.println("Initializing BLE...");
  
  // Deinit first if already initialized
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
  
  Serial.println("BLE HID Keyboard started and advertising");
}

void sendBLEKey(uint8_t key) {
  if (!bleConnected || !inputKeyboard) return;
  
  Serial.print("Sending keycode: 0x");
  Serial.println(key, HEX);
  
  // Send key press - Report ID 1, modifier, reserved, keycode
  uint8_t report[8] = {0, 0, key, 0, 0, 0, 0, 0};
  inputKeyboard->setValue(report, sizeof(report));
  inputKeyboard->notify();
  delay(20);
  
  // Release all keys
  uint8_t reportRelease[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  inputKeyboard->setValue(reportRelease, sizeof(reportRelease));
  inputKeyboard->notify();
  delay(20);
}

void sendBLEString(String text) {
  // HID keycodes for number row: 1-9 are 0x1E-0x26, 0 is 0x27
  for (int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c >= '0' && c <= '9') {
      uint8_t keycode;
      if (c == '0') {
        keycode = 0x27; // 0 key
      } else {
        keycode = 0x1E + (c - '1'); // 1-9 keys
      }
      sendBLEKey(keycode);
    }
  }
}

// HTML pages
const char* htmlHeader = R"(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:Arial;margin:0;padding:20px;background:#1a1a1a;color:#fff}
h1{color:#00d4ff;margin-bottom:10px}
.container{max-width:600px;margin:0 auto}
input,textarea{width:100%;padding:12px;margin:8px 0;background:#2a2a2a;color:#fff;border:1px solid #00d4ff;border-radius:5px;box-sizing:border-box}
button{background:#00d4ff;color:#000;padding:12px 24px;border:none;border-radius:5px;cursor:pointer;font-size:16px;margin:5px 0;width:100%}
button:hover{background:#00a8cc}
.account{background:#2a2a2a;padding:12px;margin:10px 0;border-radius:5px;border-left:3px solid #00d4ff;display:flex;justify-content:space-between;align-items:center}
.success{color:#00ff00;padding:10px;background:#1a3a1a;border-radius:5px;margin:10px 0}
.error{color:#ff0000;padding:10px;background:#3a1a1a;border-radius:5px;margin:10px 0}
.btn-delete{background:#ff4444;width:auto;padding:8px 16px}
a{color:#00d4ff;text-decoration:none}
</style>
</head><body><div class="container">
)";

const char* htmlFooter = R"(
</div></body></html>
)";

String getMainPage() {
  String html = htmlHeader;
  html += "<h1>M5Stick TOTP Setup</h1>";
  
  // WiFi Status
  html += "<h2>WiFi Configuration</h2>";
  String ssid = preferences.getString("ssid", "");
  if (ssid.length() > 0) {
    html += "<p>Current WiFi: <strong>" + ssid + "</strong></p>";
  } else {
    html += "<p>No WiFi configured</p>";
  }
  html += "<form action='/wifi' method='POST'>";
  html += "SSID: <input name='ssid' value='" + ssid + "'><br>";
  html += "Password: <input type='password' name='pass'><br>";
  html += "<button type='submit'>Save WiFi Settings</button></form>";
  
  // Current Accounts
  html += "<h2>TOTP Accounts (" + String(numAccounts) + "/10)</h2>";
  for (int i = 0; i < numAccounts; i++) {
    html += "<div class='account'><span>" + String(accounts[i].name) + "</span>";
    html += "<button class='btn-delete' onclick=\"if(confirm('Delete?'))location.href='/delete?id=" + String(i) + "'\">Delete</button></div>";
  }
  
  // Add Account
  html += "<h2>Add Account</h2>";
  html += "<form action='/add' method='POST'>";
  html += "Name: <input name='name' placeholder='GitHub' required><br>";
  html += "Secret (Base32): <input name='secret' placeholder='JBSWY3DPEHPK3PXP' required><br>";
  html += "<button type='submit'>Add Account</button></form>";
  
  // Import Multiple
  html += "<h2>Import Multiple (CSV)</h2>";
  html += "<form action='/import' method='POST'>";
  html += "<textarea name='csv' rows='6' placeholder='GitHub,JBSWY3DPEHPK3PXP&#10;Google,HXDMVJECJJWSRB3H'></textarea>";
  html += "<button type='submit'>Import Accounts</button></form>";
  
  html += "<button onclick=\"if(confirm('Restart device?'))location.href='/restart'\">Restart Device</button>";
  html += "<button onclick=\"if(confirm('Clear all BLE pairings?'))location.href='/bleforget'\">Forget Bluetooth Pairing</button>";
  html += htmlFooter;
  return html;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(3);
  M5.Display.fillScreen(BLACK);
  
  Serial.begin(115200);
  Serial.println("M5StickCPlus2 TOTP Authenticator");
  
  preferences.begin("totp", false);
  loadAccounts();
  initBLE();
  
  // Check if we should enter setup mode
  String ssid = preferences.getString("ssid", "");
  if (ssid.length() == 0 || numAccounts == 0) {
    setupMode = true;
    startSetupMode();
  } else {
    // Try to connect to WiFi
    if (connectToWiFi()) {
      initTime();
    }
    if (numAccounts > 0) {
      displayCurrentAccount();
    }
  }
}

void startSetupMode() {
  setupMode = true;
  
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, 20);
  M5.Display.setTextColor(YELLOW);
  M5.Display.println("Setup Mode");
  M5.Display.setCursor(5, 40);
  M5.Display.setTextColor(WHITE);
  M5.Display.println("1. Connect to WiFi:");
  M5.Display.setCursor(5, 55);
  M5.Display.setTextColor(CYAN);
  M5.Display.println("  M5Stick-TOTP");
  M5.Display.setCursor(5, 75);
  M5.Display.setTextColor(WHITE);
  M5.Display.println("2. Open browser:");
  M5.Display.setCursor(5, 90);
  M5.Display.setTextColor(GREEN);
  M5.Display.println("  192.168.4.1");
  M5.Display.setCursor(5, 110);
  M5.Display.setTextColor(DARKGREY);
  M5.Display.println("Press A to exit");
  
  // Start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("M5Stick-TOTP");
  
  Serial.println("AP Started: M5Stick-TOTP");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  
  // Start DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", apIP);
  
  // Setup web server routes
  setupWebServer();
  server.begin();
  Serial.println("Web server started");
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getMainPage());
  });
  
  server.on("/wifi", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    
    preferences.putString("ssid", ssid);
    preferences.putString("password", pass);
    
    String html = htmlHeader;
    html += "<h1>WiFi Saved!</h1>";
    html += "<p class='success'>SSID: " + ssid + "</p>";
    html += "<p>Restart device to connect</p>";
    html += "<a href='/'><button>Back</button></a>";
    html += htmlFooter;
    server.send(200, "text/html", html);
  });
  
  server.on("/add", HTTP_POST, []() {
    String name = server.arg("name");
    String secret = server.arg("secret");
    
    String html = htmlHeader;
    if (addAccount(name, secret)) {
      html += "<h1>Success!</h1>";
      html += "<p class='success'>Account '" + name + "' added</p>";
    } else {
      html += "<h1>Error!</h1>";
      html += "<p class='error'>Failed to add account (check secret format or account limit)</p>";
    }
    html += "<a href='/'><button>Back</button></a>";
    html += htmlFooter;
    server.send(200, "text/html", html);
  });
  
  server.on("/import", HTTP_POST, []() {
    String csv = server.arg("csv");
    int imported = parseAndImportAccounts(csv);
    
    String html = htmlHeader;
    html += "<h1>Import Complete</h1>";
    html += "<p class='success'>Imported " + String(imported) + " accounts</p>";
    html += "<a href='/'><button>Back</button></a>";
    html += htmlFooter;
    server.send(200, "text/html", html);
  });
  
  server.on("/delete", HTTP_GET, []() {
    int id = server.arg("id").toInt();
    deleteAccount(id);
    server.sendHeader("Location", "/");
    server.send(302);
  });
  
  server.on("/restart", HTTP_GET, []() {
    String html = htmlHeader;
    html += "<h1>Restarting...</h1>";
    html += "<p>Device will restart in 2 seconds</p>";
    html += htmlFooter;
    server.send(200, "text/html", html);
    delay(2000);
    ESP.restart();
  });
  
  server.on("/bleforget", HTTP_GET, []() {
    // Clear BLE bonds using correct API
    int dev_num = esp_ble_get_bond_device_num();
    esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
    esp_ble_get_bond_device_list(&dev_num, dev_list);
    for (int i = 0; i < dev_num; i++) {
      esp_ble_remove_bond_device(dev_list[i].bd_addr);
    }
    free(dev_list);
    
    // Clear the paired flag so we start advertising again
    preferences.putBool("blePaired", false);
    
    String html = htmlHeader;
    html += "<h1>Bluetooth Pairing Cleared</h1>";
    html += "<p class='success'>All BLE pairings have been forgotten</p>";
    html += "<p>Restart device to pair with a new device</p>";
    html += "<a href='/'><button>Back</button></a>";
    html += htmlFooter;
    server.send(200, "text/html", html);
  });
  
  server.on("/startweb", HTTP_GET, []() {
    String html = htmlHeader;
    html += "<h1>Web Portal Active</h1>";
    html += "<p>You can now configure settings while connected to WiFi</p>";
    html += "<a href='/'><button>Back to Main</button></a>";
    html += htmlFooter;
    server.send(200, "text/html", html);
  });
  
  // Captive portal redirect
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302);
  });
}

bool connectToWiFi() {
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  
  if (ssid.length() == 0) {
    return false;
  }
  
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(10, 50);
  M5.Display.setTextSize(1);
  M5.Display.println("Connecting WiFi...");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    return true;
  }
  return false;
}

void initTime() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(10, 50);
  M5.Display.setTextSize(1);
  M5.Display.println("Syncing time...");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    delay(1000);
    attempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    timeInitialized = true;
    Serial.println("Time synchronized");
  }
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
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

bool addAccount(String name, String secret) {
  if (numAccounts >= 10) return false;
  
  secret.replace(" ", "");
  secret.toUpperCase();
  
  int secretLen = base32Decode(secret, accounts[numAccounts].secret);
  if (secretLen <= 0) return false;
  
  strncpy(accounts[numAccounts].name, name.c_str(), 49);
  accounts[numAccounts].name[49] = '\0';
  accounts[numAccounts].secretLength = secretLen;
  
  String key = "acc" + String(numAccounts);
  String value = name + "|" + secret;
  preferences.putString(key.c_str(), value);
  
  numAccounts++;
  preferences.putInt("numAccounts", numAccounts);
  
  Serial.print("Added account: ");
  Serial.println(name);
  
  return true;
}

void deleteAccount(int index) {
  if (index < 0 || index >= numAccounts) return;
  
  // Shift accounts down
  for (int i = index; i < numAccounts - 1; i++) {
    accounts[i] = accounts[i + 1];
    String key = "acc" + String(i);
    String nextKey = "acc" + String(i + 1);
    String value = preferences.getString(nextKey.c_str(), "");
    preferences.putString(key.c_str(), value);
  }
  
  // Remove last account
  numAccounts--;
  String lastKey = "acc" + String(numAccounts);
  preferences.remove(lastKey.c_str());
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

void loadAccounts() {
  numAccounts = preferences.getInt("numAccounts", 0);
  
  for (int i = 0; i < numAccounts && i < 10; i++) {
    String key = "acc" + String(i);
    String value = preferences.getString(key.c_str(), "");
    
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
  
  Serial.print("Loaded ");
  Serial.print(numAccounts);
  Serial.println(" accounts");
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
  if (timeLeft < 5) {
    color = RED;
  } else if (timeLeft < 10) {
    color = YELLOW;
  }
  
  M5.Display.fillRect(barX, barY, fillWidth, barHeight, color);
  M5.Display.drawRect(barX, barY, barWidth, barHeight, WHITE);
}

void displayCurrentAccount() {
  M5.Display.fillScreen(BLACK);
  
  // Display service name at top (doubled size)
  M5.Display.setTextSize(2);
  M5.Display.setCursor(5, 5);
  M5.Display.setTextColor(CYAN);
  M5.Display.println(accounts[currentAccount].name);
  
  // Display position indicator (n/n) - doubled size
  M5.Display.setCursor(180, 5);
  M5.Display.setTextColor(DARKGREY);
  char posStr[10];
  sprintf(posStr, "%d/%d", currentAccount + 1, numAccounts);
  M5.Display.println(posStr);
  
  // Generate and display TOTP code (large, centered)
  String code = generateTOTP(accounts[currentAccount]);
  M5.Display.setTextSize(4);
  M5.Display.setTextColor(WHITE);
  
  // Center the code horizontally (manual calculation for M5GFX)
  int charWidth = 24;
  int codeWidth = code.length() * charWidth;
  int xPos = (M5.Display.width() - codeWidth) / 2;
  int yPos = (M5.Display.height() / 2) - 16;
  
  M5.Display.setCursor(xPos, yPos);
  M5.Display.println(code);
  
  // Draw progress bar
  int timeLeft = getTimeRemaining();
  drawProgressBar(timeLeft);
  
  // Display BLE status
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, M5.Display.height() - 30);
  if (bleConnected) {
    M5.Display.setTextColor(GREEN);
    M5.Display.println("BLE");
  } else {
    M5.Display.setTextColor(RED);
    M5.Display.println("BLE");
  }
}

void updateProgressBar() {
  // Only redraw the progress bar area
  int timeLeft = getTimeRemaining();
  drawProgressBar(timeLeft);
}

void showSetupMessage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 30);
  M5.Display.setTextColor(YELLOW);
  M5.Display.println("No accounts setup!");
  M5.Display.setCursor(10, 50);
  M5.Display.setTextColor(WHITE);
  M5.Display.println("Hold B button 3s");
  M5.Display.setCursor(10, 65);
  M5.Display.println("to enter setup mode");
}

void sendCodeViaBLE() {
  if (!bleConnected) {
    Serial.println("BLE not connected");
    M5.Display.fillScreen(RED);
    delay(200);
    displayCurrentAccount();
    return;
  }
  
  String code = generateTOTP(accounts[currentAccount]);
  Serial.print("Sending code via BLE: ");
  Serial.println(code);
  
  M5.Display.fillScreen(GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(BLACK);
  M5.Display.setCursor(40, 60);
  M5.Display.println("SENDING");
  
  delay(200);
  sendBLEString(code);
  
  // Check if A button is still held down - if so, send Enter
  M5.update();
  if (M5.BtnA.isPressed()) {
    Serial.println("A button held - sending Enter");
    sendBLEKey(0x28); // Enter/Return keycode
  }
  
  delay(500);
  displayCurrentAccount();
}

void loop() {
  if (setupMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    
    M5.update();
    if (M5.BtnA.wasPressed()) {
      ESP.restart();
    }
    delay(10);
    return;
  }
  
  M5.update();
  
  // Long press B button (side button) to enter setup mode
  static unsigned long bPressStart = 0;
  static bool bWasPressed = false;
  static bool progressShown = false;
  
  // Detect when B button goes down
  if (M5.BtnB.wasPressed()) {
    bPressStart = millis();
    bWasPressed = true;
    progressShown = false;
    Serial.println("B button press started");
  }
  
  // Check if still holding
  if (bWasPressed && M5.BtnB.isPressed()) {
    unsigned long holdTime = millis() - bPressStart;
    
    // Show progress after 500ms
    if (holdTime > 500 && !progressShown) {
      progressShown = true;
      M5.Display.fillRect(0, M5.Display.height() - 40, M5.Display.width(), 40, BLACK);
      M5.Display.setTextSize(1);
      M5.Display.setCursor(10, M5.Display.height() - 30);
      M5.Display.setTextColor(YELLOW);
      M5.Display.println("Hold B for setup...");
      Serial.print("Holding B: ");
      Serial.print(holdTime);
      Serial.println("ms");
    }
    
    // Enter setup after 3 seconds
    if (holdTime >= 3000) {
      Serial.println("Setup mode!");
      M5.Display.fillScreen(BLUE);
      M5.Display.setCursor(30, 60);
      M5.Display.setTextSize(2);
      M5.Display.setTextColor(WHITE);
      M5.Display.println("Setup Mode");
      delay(1000);
      bWasPressed = false;
      progressShown = false;
      startSetupMode();
      return;
    }
  }
  
  // Button was released
  if (M5.BtnB.wasReleased()) {
    unsigned long holdTime = millis() - bPressStart;
    Serial.print("B released after ");
    Serial.print(holdTime);
    Serial.println("ms");
    
    if (bWasPressed && holdTime < 3000) {
      // Short press - cycle down
      Serial.println("B short press - cycle down");
      if (numAccounts > 0) {
        currentAccount = (currentAccount + 1) % numAccounts;
        displayCurrentAccount();
      }
    }
    bWasPressed = false;
    progressShown = false;
  }
  
  // PWR button - cycle up
  if (M5.BtnPWR.wasPressed()) {
    Serial.println("PWR button - cycle up");
    if (numAccounts > 0) {
      currentAccount = (currentAccount - 1 + numAccounts) % numAccounts;
      displayCurrentAccount();
    }
  }
  
  // A button - send code
  if (M5.BtnA.wasPressed()) {
    Serial.println("A button - send code");
    if (numAccounts > 0) {
      if (bleConnected) {
        sendCodeViaBLE();
      } else {
        Serial.println("BLE not connected - cannot send");
        M5.Display.fillScreen(RED);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(WHITE);
        M5.Display.setCursor(20, 50);
        M5.Display.println("NOT PAIRED!");
        M5.Display.setCursor(10, 70);
        M5.Display.println("Pair via Bluetooth");
        delay(2000);
        displayCurrentAccount();
      }
    }
  }
  
  // Only update progress bar, not full screen
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