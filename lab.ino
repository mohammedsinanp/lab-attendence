#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h> 
#include <RTClib.h> 
#include <WebServer.h>
#include <Keypad.h> 
#include <DNSServer.h> 
#include <LiquidCrystal_I2C.h> 
#include <Preferences.h> 
#include <HTTPClient.h>  // Native secure web request library
#include "time.h" 

// Fallback Hotspot Configurations
const char* ap_ssid = "RFID_Attendance";
const char* ap_password = "12345678";

// =========================================================================
// GOOGLE SHEETS & WORLD TIME SETTINGS
// =========================================================================
// YOUR UNIQUE DEPLOYED GOOGLE WEB APP URL BAKED IN:
const char* googleScriptUrl = "https://script.google.com/macros/s/AKfycbxAtqpg2fskJeKUYKVLhMF6mDbzSkxFJ4PrFkL3ReN2UYlSTdnnFgiSoDv0ad8Z0GpEbQ/exec";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;     // Time Zone Offset (e.g. 19800 = UTC +5:30 IST)
const int   daylightOffset_sec = 0;    

// DNS Server Configuration
const byte DNS_PORT = 53;
DNSServer dnsServer;

// RFID Pins
#define SS_PIN 5
#define RST_PIN 4 

MFRC522 mfrc522(SS_PIN, RST_PIN);
WebServer server(80);

// LCD Settings (0x27 Address)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Hardware RTC Instance
RTC_DS3231 rtc;
Preferences preferences;

// Keypad Configuration Matrix
const byte ROWS = 4; 
const byte COLS = 4; 
char hexaKeys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27}; 
byte colPins[COLS] = {26, 25, 33, 32}; 

Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);
String keypadInputBuffer = "";

struct User {
  String uid;
  String rollNumber; 
  String pin; 
};

const int MAX_USERS = 40; 
User users[MAX_USERS];
int userCount = 0;

bool activeSlotInsideState[MAX_USERS] = {false};
String trackedSlotName = "";

bool isAdminLoggedIn = false; 
String statusMessage = ""; 
String lastScannedMessage = ""; 
String capturedUID = ""; 
unsigned long stateID = 0; 

struct AttendanceLog {
  String rollNumber;
  String classSection; 
  String status;       
  String dateTime;
};
const int MAX_LOGS = 200;
AttendanceLog logs[MAX_LOGS];
int logCount = 0;

bool isConnectedToStationWiFi = false;
String activeIPAddress = "";

void updatePhysicalLCD(String line1, String line2);
void displayDefaultMessage();
String getDateTimeString();

void saveUsersToStorage() {
  preferences.begin("attendance", false); 
  preferences.putInt("userCount", userCount);
  for (int i = 0; i < userCount; i++) {
    String keyUid = "uid_" + String(i);
    String keyRoll = "roll_" + String(i);
    String keyPin = "pin_" + String(i);
    preferences.putString(keyUid.c_str(), users[i].uid);
    preferences.putString(keyRoll.c_str(), users[i].rollNumber);
    preferences.putString(keyPin.c_str(), users[i].pin);
  }
  preferences.end();
}

void loadUsersFromStorage() {
  preferences.begin("attendance", true); 
  userCount = preferences.getInt("userCount", 0);
  
  if (userCount == 0) {
    preferences.end(); 
    users[0] = {"24:3C:4E:06", "1", "111"};
    users[1] = {"3C:33:19:49", "2", "222"};
    userCount = 2;
    saveUsersToStorage(); 
    return;
  }

  for (int i = 0; i < userCount; i++) {
    String keyUid = "uid_" + String(i);
    String keyRoll = "roll_" + String(i);
    String keyPin = "pin_" + String(i);
    users[i].uid = preferences.getString(keyUid.c_str(), "");
    users[i].rollNumber = preferences.getString(keyRoll.c_str(), "");
    users[i].pin = preferences.getString(keyPin.c_str(), "");
  }
  preferences.end();
}

String getUIDString(byte *uid, byte size) {
  String out = "";
  for (byte i = 0; i < size; i++) {
    if (uid[i] < 0x10) out += "0";
    out += String(uid[i], HEX);
    if (i < size - 1) out += ":";
  }
  out.toUpperCase();
  return out;
}

void syncRTCWithWorldTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  Serial.print("Synchronizing with World Time Server");
  int retryCounter = 0;
  while(!getLocalTime(&timeinfo) && retryCounter < 10) {
    Serial.print(".");
    delay(500);
    retryCounter++;
  }
  if (getLocalTime(&timeinfo)) {
    Serial.println("\nWorld Time successfully fetched via NTP!");
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, 
                        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
    updatePhysicalLCD("WORLD TIME SYNC", "RTC SUCCESSFUL");
    delay(2000);
  }
}

String getDateTimeString() {
  if (!rtc.begin()) return "RTC Error";
  DateTime now = rtc.now();
  char buf[40];
  int displayHour = now.hour();
  String ampm = "AM";
  if (displayHour >= 12) {
    ampm = "PM";
    if (displayHour > 12) displayHour -= 12;
  }
  if (displayHour == 0) displayHour = 12;
  snprintf(buf, sizeof(buf), "%02d-%02d-%04d %02d:%02d:%02d %s", 
           now.day(), now.month(), now.year(), displayHour, now.minute(), now.second(), ampm.c_str());
  return String(buf);
}

String getCurrentScheduledClass() {
  if (!rtc.begin()) return "UNKNOWN";
  DateTime now = rtc.now();
  int currentHour = now.hour(); 
  if (currentHour == 9)  return "10A";       
  if (currentHour == 10) return "10B";       
  if (currentHour == 11) return "11A";       
  if (currentHour == 12) return "11B";       
  if (currentHour == 13) return "12A";       
  if (currentHour == 14) return "LUNCH_BREAK"; 
  if (currentHour == 15) return "12B";       
  if (currentHour == 16) return "12C";       // Updated: Added 4 PM - 5 PM dynamic window
  return "NO_CLASS"; 
}

String computeAnalyticsStatus(bool isEntering) {
  if (!rtc.begin()) return isEntering ? "ENTRY" : "EXIT";
  DateTime now = rtc.now();
  int currentMinute = now.minute();
  if (isEntering) {
    if (currentMinute > 20) return "LATE (" + String(currentMinute) + " mins)";
    return "ENTRY";
  } else {
    if (currentMinute < 50) return "EARLY EXIT (" + String(50 - currentMinute) + " mins early)";
    return "EXIT";
  }
}

// Background Cloud Request to Google Apps Script Endpoint
void postToGoogleSheets(String roll, String classSlot, String status) {
  if (!isConnectedToStationWiFi) return; 

  HTTPClient http;
  http.begin(googleScriptUrl);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = "{\"roll\":\"" + roll + 
                       "\",\"classSlot\":\"" + classSlot + 
                       "\",\"status\":\"" + status + 
                       "\",\"timestamp\":\"" + getDateTimeString() + "\"}";

  Serial.println("Pushing Attendance transaction to cloud storage...");
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Google Cloud Response Code: " + String(httpResponseCode));
    Serial.println("Server Response: " + response);
  } else {
    Serial.println("Cloud Push Error: " + http.errorToString(httpResponseCode));
  }
  http.end();
}

void checkSlotReset() {
  String dynamicSlot = getCurrentScheduledClass();
  if (dynamicSlot != trackedSlotName) {
    trackedSlotName = dynamicSlot;
    for (int i = 0; i < MAX_USERS; i++) activeSlotInsideState[i] = false;
    stateID++;
  }
}

int calculateHistoricalPresentees(String slotName) {
  bool counted[100] = {false}; 
  int totalPresent = 0;
  for (int i = 0; i < logCount; i++) {
    if (logs[i].classSection == slotName && (logs[i].status.startsWith("ENTRY") || logs[i].status.startsWith("LATE"))) {
      int roll = logs[i].rollNumber.toInt();
      if (roll > 0 && roll < 100 && !counted[roll]) {
        counted[roll] = true;
        totalPresent++;
      }
    }
  }
  return totalPresent;
}

void addLog(String rollNumber, String classSection, String status) {
  if (logCount >= MAX_LOGS) {
    for (int i = 1; i < MAX_LOGS; i++) logs[i - 1] = logs[i];
    logCount = MAX_LOGS - 1;
  }
  logs[logCount] = {rollNumber, classSection, status, getDateTimeString()};
  logCount++;
  
  postToGoogleSheets(rollNumber, classSection, status);
}

void updatePhysicalLCD(String line1, String line2) {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(line1.substring(0, 16)); 
  lcd.setCursor(0, 1); lcd.print(line2.substring(0, 16));
}

void displayDefaultMessage() {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("   SCAN YOUR    "); lcd.setCursor(0, 1); lcd.print("    ID CARD     ");
}

void processPhysicalPin(String typedPin) {
  checkSlotReset();
  String currentClass = trackedSlotName;
  if (currentClass == "LUNCH_BREAK" || currentClass == "NO_CLASS" || currentClass == "UNKNOWN") {
    lastScannedMessage = "<div class='alert' style='background:#f8d7da; color:#721c24;'>ACCESS DENIED: Lab Closed!</div>";
    updatePhysicalLCD("LAB CLOSED", "NO ACTIVE SLOT");
    stateID++; delay(3000); displayDefaultMessage(); return;
  }

  bool pinFound = false;
  for (int i = 0; i < userCount; i++) {
    if (users[i].pin == typedPin) {
      pinFound = true;
      activeSlotInsideState[i] = !activeSlotInsideState[i];
      String derivedStatus = computeAnalyticsStatus(activeSlotInsideState[i]);
      String shortLcdStatus = activeSlotInsideState[i] ? "ENTRY" : "EXIT";

      lastScannedMessage = "<div class='alert' style='background:#d4edda; color:#155724; font-size:1.2em;'>PIN GRANTED: Roll " + users[i].rollNumber + "</div>";
      updatePhysicalLCD("ROLL: " + users[i].rollNumber, shortLcdStatus + " - " + currentClass);
      addLog(users[i].rollNumber, currentClass, derivedStatus);
      
      stateID++; delay(3000); displayDefaultMessage(); break;
    }
  }
  if (!pinFound) {
    lastScannedMessage = "<div class='alert' style='background:#f8d7da; color:#721c24; font-size:1.2em;'>PIN DENIED: Invalid Code!</div>";
    updatePhysicalLCD("ACCESS DENIED", "INVALID PIN");
    addLog("UNKNOWN", currentClass, "DENIED (PIN)");
    stateID++; delay(3000); displayDefaultMessage();
  }
}

bool handleCaptivePortalRedirect() {
  if (isConnectedToStationWiFi) return false; 
  String host = server.hostHeader();
  if (host != "192.168.4.1" && host != "rfid.local") {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", ""); return true;
  }
  return false;
}

void handleExportExcel() {
  if (!isAdminLoggedIn) { server.sendHeader("Location", "/admin-login"); server.send(303); return; }
  String requestedSlot = server.hasArg("slot") ? server.arg("slot") : "ALL";
  String csvContent = "Timestamp,Class Slot,Roll Number,Attendance Analytics Status\n";
  for (int i = 0; i < logCount; i++) {
    if (requestedSlot == "ALL" || logs[i].classSection == requestedSlot) {
      csvContent += logs[i].dateTime + "," + logs[i].classSection + "," + logs[i].rollNumber + "," + logs[i].status + "\n";
    }
  }
  server.sendHeader("Content-Disposition", "attachment; filename=Attendance_" + requestedSlot + ".csv");
  server.send(200, "text/csv", csvContent);
}

String makeHtmlPage(String title, String content, bool enableAutoRefresh = false) {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>" + title + "</title>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"; 
  html += "<style>body{font-family:Arial,sans-serif; text-align:center; background:#f4f4f9; margin:0; padding:20px;} .container{max-width:600px; margin:auto; background:white; padding:20px; border-radius:8px; box-shadow:0 4px 8px rgba(0,0,0,0.1);} .chart-box{max-width:450px; margin:20px auto; padding:15px; border:1px solid #ddd; border-radius:6px; background:#fff;} table{border-collapse:collapse; width:100%; margin:20px 0;} th,td{padding:12px; border:1px solid #ddd; text-align:center;} th{background:#4CAF50; color:white;} .status{font-weight:bold; color:#2196F3;} input[type='text'], input[type='password'], input[type='number']{padding:10px; width:80%; margin:8px 0; border:1px solid #ccc; border-radius:4px; box-sizing:border-box;} .btn{display:inline-block; padding:10px 20px; background:#2196F3; color:white; text-decoration:none; border-radius:4px; font-weight:bold; margin:5px; border:none; cursor:pointer;} .btn-nav{background:#555;} .btn-del{background:#f44336; padding:5px 10px; font-size:12px;} .btn-admin{background:#6c757d;} .btn-excel{background:#1D7344;} .btn-reboot{background:#e67e22;} .badge{padding:5px 10px; border-radius:4px; color:white; font-weight:bold;} .inside{background:#4CAF50;} .outside{background:#ff9800;} .late{background:#b81d24;} .early{background:#e67e22;} .alert{padding:10px; border-radius:4px; margin:10px 0; font-weight:bold;}</style>";
  if (enableAutoRefresh) {
    html += "<script>var initialSyncId = " + String(stateID) + "; setInterval(function() { fetch('/sync-check').then(response => response.text()).then(currentSyncId => { if (parseInt(currentSyncId) !== initialSyncId) { window.location.reload(); } }); }, 1500);</script>";
  }
  html += "</head><body><div class='container'>" + content + "</div></body></html>";
  return html;
}

void handleRoot() {
  if (handleCaptivePortalRedirect()) return; 
  checkSlotReset();
  int dynamicPresentCount = 0;
  for (int i = 0; i < userCount; i++) if (activeSlotInsideState[i]) dynamicPresentCount++;

  String body = "<h2>Lab Attendance Portal</h2><p class='status'>System Time: " + getDateTimeString() + "</p>";
  
  String currentClass = trackedSlotName;
  if (currentClass == "LUNCH_BREAK") body += "<p style='color:#ff9800; font-weight:bold;'>Current Status: LUNCH BREAK</p>";
  else if (currentClass == "NO_CLASS") body += "<p style='color:#f44336; font-weight:bold;'>Current Status: LAB CLOSED</p>";
  else {
    body += "<p style='color:#2e7d32; font-weight:bold;'>Active Dynamic Slot: Class " + currentClass + "</p>";
    body += "<p>Students Inside Lab: <strong>" + String(dynamicPresentCount) + "</strong></p>";
  }
  
  if (lastScannedMessage != "") { body += lastScannedMessage; lastScannedMessage = ""; }
  if (statusMessage != "") { body += statusMessage; statusMessage = ""; }

  body += "<h3>Completed Slot Summary Report</h3><div class='chart-box'><canvas id='historicalSummaryChart'></canvas></div>";
  body += "<script>const ctxHist = document.getElementById('historicalSummaryChart').getContext('2d'); new Chart(ctxHist, { type: 'bar', data: { labels: ['10A (9AM)', '10B (10AM)', '11A (11AM)', '11B (12PM)', '12A (1PM)', '12B (3PM)', '12C (4PM)'], datasets: [{ label: 'Total Registered Attendance References', data: ["+String(calculateHistoricalPresentees("10A"))+","+String(calculateHistoricalPresentees("10B"))+","+String(calculateHistoricalPresentees("11A"))+","+String(calculateHistoricalPresentees("11B"))+","+String(calculateHistoricalPresentees("12A"))+","+String(calculateHistoricalPresentees("12B"))+","+String(calculateHistoricalPresentees("12C"))+"], backgroundColor: '#2196F3', borderWidth: 0 }] }, options: { scales: { y: { min: 0, max: 40, ticks: { stepSize: 5 } } }, plugins: { legend: { display: false } } } });</script>";
  body += "<h3>Shared Tag Live Map</h3><table><tr><th>Roll Assignment</th><th>UID Mapping</th><th>Position</th></tr>";
  for (int i = 0; i < userCount; i++) {
    body += "<tr><td><strong>Roll " + users[i].rollNumber + "</strong></td><td>" + users[i].uid + "</td><td>" + String(activeSlotInsideState[i] ? "<span class='badge inside'>INSIDE</span>" : "<span class='badge outside'>OUTSIDE</span>") + "</td></tr>";
  }
  body += "</table><br><a href='/' class='btn'>Refresh Dashboard</a><a href='/logs' class='btn btn-nav'>View Activity Log</a><a href='/admin-login' class='btn btn-admin'>Admin Panel</a>";
  server.send(200, "text/html", makeHtmlPage("RFID Status", body, true));
}

void handleSyncCheck() { server.send(200, "text/plain", String(stateID)); }

void handleAdminLogin() {
  if (isAdminLoggedIn) { server.sendHeader("Location", "/admin-panel"); server.send(303); return; }
  String body = "<h2>Admin Authentication</h2>";
  if (statusMessage != "") { body += statusMessage; statusMessage = ""; }
  body += "<div style='background:#f4f4f9; padding:20px; border-radius:5px;'><form action='/admin-login-submit' method='POST'><input type='text' name='username' placeholder='Username' required><br><input type='password' name='password' placeholder='Password' required><br><br><input type='submit' value='Login' class='btn' style='width:80%;'></form></div><br><a href='/' class='btn btn-nav'>Back to Dashboard</a>";
  server.send(200, "text/html", makeHtmlPage("Admin Login", body, false));
}

void handleAdminLoginSubmit() {
  if (server.hasArg("username") && server.hasArg("password") && server.arg("username") == "admin" && server.arg("password") == "123") {
    isAdminLoggedIn = true; capturedUID = ""; statusMessage = "<div class='alert' style='background:#d4edda; color:#155724;'>Admin Authenticated!</div>";
    server.sendHeader("Location", "/admin-panel"); server.send(303); return;
  }
  statusMessage = "<div class='alert' style='background:#f8d7da; color:#721c24;'>Error: Invalid Credentials!</div>";
  server.sendHeader("Location", "/admin-login"); server.send(303);
}

void handleAdminPanel() {
  if (!isAdminLoggedIn) { server.sendHeader("Location", "/admin-login"); server.send(303); return; }
  String body = "<h2>Secure Tag Administration</h2>";
  if (statusMessage != "") { body += statusMessage; statusMessage = ""; }

  body += "<h3>Infrastructure Wi-Fi Uplink Settings</h3><div style='background:#f1f5f9; padding:15px; border-radius:6px; text-align:left; border:1px solid #cbd5e1; margin-bottom:20px;'><form action='/admin-save-wifi' method='POST'><label><strong>Local Target SSID:</strong></label><br>";
  preferences.begin("attendance", true);
  String savedSSID = preferences.getString("wifi_ssid", "");
  preferences.end();
  body += "<input type='text' name='ssid' value='" + savedSSID + "' placeholder='Network SSID' required><br><label><strong>Password:</strong></label><br><input type='password' name='password' placeholder='Wi-Fi Password'><br><br><input type='submit' value='Save Credentials' class='btn' style='width:100%; background:#475569;'></form><hr style='border:0; border-top:1px dashed #cbd5e1; margin:15px 0;'><a href='/admin-reboot' class='btn btn-reboot' style='width:92%; text-align:center;' onclick='return confirm(\"Execute hardware reboot?\")'>Reboot ESP32</a></div>";
  body += "<h3>Download Excel Sheet Reports</h3><div style='background:#eef7f2; padding:12px; border-radius:6px; margin-bottom:20px; border:1px solid #1d7344;'><a href='/export-excel?slot=10A' class='btn btn-excel'>Excel 10A</a><a href='/export-excel?slot=10B' class='btn btn-excel'>Excel 10B</a><a href='/export-excel?slot=11A' class='btn btn-excel'>Excel 11A</a><br><a href='/export-excel?slot=11B' class='btn btn-excel'>Excel 11B</a><a href='/export-excel?slot=12A' class='btn btn-excel'>Excel 12A</a><a href='/export-excel?slot=12B' class='btn btn-excel'>Excel 12B</a><br><a href='/export-excel?slot=12C' class='btn btn-excel'>Excel 12C</a><a href='/export-excel?slot=ALL' class='btn' style='background:#333; margin-top:10px;'>Export Complete Local Log</a></div>";
  body += "<div class='scan-box'>" + String(capturedUID == "" ? "INSTRUCTION: Tap hardware tag on reader to register index..." : "Tag Captured! UID: " + capturedUID) + "</div>";
  body += "<div style='background:#e2ece9; padding:20px; border-radius:5px; text-align:left; margin-bottom:25px;'><h3 style='margin-top:0; color:#2e7d32;'>Link Tag to Shared Roll</h3><form action='/admin-add-user' method='POST'><input type='number' name='rollNumber' min='1' max='40' placeholder='Assign Roll (1-40)' required><br><br><input type='number' name='pin' min='100' max='999' placeholder='Set 3-Digit PIN' required><br><br><input type='text' name='uid' value='" + capturedUID + "' readonly style='background:#e9ecef;' required><br><br><input type='submit' value='Bind Tag' class='btn' style='background:#2e7d32; width:100%;'></form></div><h3>Configured Hardware Mappings</h3><table><tr><th>Roll</th><th>UID</th><th>PIN</th><th>Action</th></tr>";
  for (int i = 0; i < userCount; i++) {
    body += "<tr><td>Roll " + users[i].rollNumber + "</td><td>" + users[i].uid + "</td><td>" + users[i].pin + "</td><td><a href='/admin-delete-user?index=" + String(i) + "' class='btn btn-del' onclick='return confirm(\"Wipe reference?\")'>Remove</a></td></tr>";
  }
  body += "</table><br><a href='/' class='btn btn-nav'>Exit Admin Space</a><a href='/admin-logout' class='btn' style='background:#777;'>Logout</a>";
  server.send(200, "text/html", makeHtmlPage("Admin Panel", body, true));
}

void handleAdminSaveWiFi() {
  if (!isAdminLoggedIn) return;
  if (server.hasArg("ssid")) {
    preferences.begin("attendance", false);
    preferences.putString("wifi_ssid", server.arg("ssid"));
    preferences.putString("wifi_pass", server.hasArg("password") ? server.arg("password") : "");
    preferences.end();
    statusMessage = "<div class='alert' style='background:#d4edda; color:#155724;'>Credentials Saved! Click Reboot below to connect.</div>";
  }
  server.sendHeader("Location", "/admin-panel"); server.send(303);
}

void handleAdminReboot() {
  if (!isAdminLoggedIn) return;
  server.send(200, "text/html", makeHtmlPage("Rebooting", "<h2>System Rebooting...</h2>", false));
  delay(1000); ESP.restart();
}

void handleAdminAddUser() {
  if (!isAdminLoggedIn) return;
  if (server.hasArg("uid") && server.hasArg("rollNumber") && server.hasArg("pin")) {
    String newUid = server.arg("uid"); String rawRoll = server.arg("rollNumber"); String newPin = server.arg("pin");
    newUid.toUpperCase(); newUid.trim(); rawRoll.trim(); newPin.trim();
    bool conflict = false;
    for (int i = 0; i < userCount; i++) if (users[i].rollNumber == rawRoll || users[i].uid == newUid) { conflict = true; break; }
    if (conflict) statusMessage = "<div class='alert' style='background:#f8d7da; color:#721c24;'>Error: Identification profile already exists!</div>";
    else if (userCount < MAX_USERS) {
      users[userCount] = {newUid, rawRoll, newPin}; userCount++;
      statusMessage = "<div class='alert' style='background:#d4edda; color:#155724;'>Linked successfully to Roll " + rawRoll + "!</div>";
      capturedUID = ""; saveUsersToStorage(); stateID++;
    }
  }
  server.sendHeader("Location", "/admin-panel"); server.send(303);
}

void handleAdminDeleteUser() {
  if (!isAdminLoggedIn) return;
  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < userCount) {
      for (int i = index; i < userCount - 1; i++) users[i] = users[i + 1];
      userCount--; saveUsersToStorage(); stateID++;
    }
  }
  server.sendHeader("Location", "/admin-panel"); server.send(303);
}

void handleAdminLogout() { isAdminLoggedIn = false; capturedUID = ""; server.sendHeader("Location", "/"); server.send(303); }

void handleLogs() {
  String body = "<h2>Session Attendance Activity Logs</h2><table><tr><th>Timestamp</th><th>Applied Class Slot</th><th>Roll Number</th><th>Transaction</th></tr>";
  for (int i = logCount - 1; i >= 0; i--) {
    String badgeClass = (logs[i].status == "EXIT") ? "outside" : (logs[i].status.startsWith("LATE") ? "late" : (logs[i].status.startsWith("EARLY") ? "early" : "inside"));
    body += "<tr><td>" + logs[i].dateTime + "</td><td><strong style='color:#2196F3;'>" + logs[i].classSection + "</strong></td><td>Roll " + logs[i].rollNumber + "</td><td><span class='badge " + badgeClass + "'>" + logs[i].status + "</span></td></tr>";
  }
  body += "</table><br><a href='/' class='btn btn-nav'>Back to Dashboard</a>";
  server.send(200, "text/html", makeHtmlPage("Attendance Logs", body, false));
}

void setup() {
  Serial.begin(115200); delay(1000);
  Wire.begin(21, 22); lcd.init(); lcd.backlight();
  SPI.begin(18, 19, 23, 5); mfrc522.PCD_Init();
  loadUsersFromStorage();

  if (rtc.begin() && rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  preferences.begin("attendance", true);
  String storedSSID = preferences.getString("wifi_ssid", "");
  String storedPass = preferences.getString("wifi_pass", "");
  preferences.end();

  if (storedSSID != "") {
    updatePhysicalLCD("CONNECTING TO...", storedSSID);
    WiFi.mode(WIFI_STA); WiFi.begin(storedSSID.c_str(), storedPass.c_str());
    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < 30) { delay(500); Serial.print("."); counter++; }
    if (WiFi.status() == WL_CONNECTED) {
      isConnectedToStationWiFi = true; activeIPAddress = WiFi.localIP().toString();
      updatePhysicalLCD("CONNECTED IP:", activeIPAddress); delay(1500);
      syncRTCWithWorldTime();
    }
  }

  if (!isConnectedToStationWiFi) {
    updatePhysicalLCD("HOTSPOT ACTIVE", "192.168.4.1");
    WiFi.mode(WIFI_AP); IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0)); WiFi.softAP(ap_ssid, ap_password);
    dnsServer.start(DNS_PORT, "*", apIP); delay(3000);
  }

  displayDefaultMessage();

  server.on("/", handleRoot);
  server.on("/sync-check", handleSyncCheck);
  server.on("/logs", handleLogs);
  server.on("/export-excel", handleExportExcel); 
  server.on("/admin-login", handleAdminLogin);
  server.on("/admin-login-submit", HTTP_POST, handleAdminLoginSubmit);
  server.on("/admin-panel", handleAdminPanel);
  server.on("/admin-save-wifi", HTTP_POST, handleAdminSaveWiFi);
  server.on("/admin-reboot", handleAdminReboot);
  server.on("/admin-add-user", HTTP_POST, handleAdminAddUser);
  server.on("/admin-delete-user", handleAdminDeleteUser);
  server.on("/admin-logout", handleAdminLogout);
  server.onNotFound([]() { if (!handleCaptivePortalRedirect()) server.send(404, "text/plain", "Not Found"); });
  server.begin();
}

void loop() {
  if (!isConnectedToStationWiFi) dnsServer.processNextRequest(); 
  server.handleClient(); 

  char key = customKeypad.getKey();
  if (key) {
    if (key >= '0' && key <= '9') {
      keypadInputBuffer += key; updatePhysicalLCD("ENTERING PIN...", keypadInputBuffer);
      if (keypadInputBuffer.length() == 3) { processPhysicalPin(keypadInputBuffer); keypadInputBuffer = ""; }
    } else if (key == '*') { keypadInputBuffer = ""; displayDefaultMessage(); }
  }

  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

  checkSlotReset();
  String currentClass = trackedSlotName;
  if (currentClass == "LUNCH_BREAK" || currentClass == "NO_CLASS" || currentClass == "UNKNOWN") {
    updatePhysicalLCD("LAB CLOSED", "NO RUNNING SLOT");
    stateID++; delay(3000); displayDefaultMessage();
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1(); return;
  }

  String scannedUID = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
  bool cardFound = false;

  for (int i = 0; i < userCount; i++) {
    if (users[i].uid == scannedUID) {
      cardFound = true; activeSlotInsideState[i] = !activeSlotInsideState[i];
      String derivedStatus = computeAnalyticsStatus(activeSlotInsideState[i]);
      String shortLcdStatus = activeSlotInsideState[i] ? "ENTRY" : "EXIT";
      
      lastScannedMessage = "<div class='alert' style='background:#d4edda; color:#155724; font-size:1.2em;'>REGISTERED: Roll " + users[i].rollNumber + "</div>";
      updatePhysicalLCD("ROLL: " + users[i].rollNumber, shortLcdStatus + " - " + currentClass);
      addLog(users[i].rollNumber, currentClass, derivedStatus);
      
      stateID++; delay(3000); displayDefaultMessage(); break;
    }
  }

  if (!cardFound) {
    if (isAdminLoggedIn) {
      capturedUID = scannedUID;
      statusMessage = "<div class='alert' style='background:#d4edda; color:#155724;'>New physical tag caught!</div>";
    }
    lastScannedMessage = "<div class='alert' style='background:#f8d7da; color:#721c24; font-size:1.2em;'>ACCESS DENIED: Unmapped Tag!</div>";
    updatePhysicalLCD("ACCESS DENIED", "UNRESOLVED TAG");
    addLog("UNKNOWN", currentClass, "DENIED (BAD TAG)");
    stateID++; delay(3000); displayDefaultMessage();
  }

  mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1(); delay(100); 
}