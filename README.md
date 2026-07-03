# IoT Smart Lab Attendance System (ESP32 + RFID + Keypad + Google Sheets)

An automated, real-time student attendance tracking system built on the ESP32 platform. The system uses dual-factor authentication (RFID Smart Cards or a 3-Digit Security PIN via Keypad), cross-references logs using an external DS3231 Hardware Real-Time Clock (RTC), serves a rich Web Dashboard directly from the hardware chip, and mirrors all data live to a cloud-hosted Google Sheet.

## 🚀 Features

*   **Dual Authentication Methods:** Tap an RFID Card or type a customized 3-Digit PIN via the 4x4 matrix keypad.
*   **Dynamic Class Slot Allocation:** Tracks and sorts logs seamlessly based on actual time windows ($9\text{ AM}$ to $5\text{ PM}$).
*   **Automatic Entry/Exit Analytics:** Automatically determines whether a student is checking in (`ENTRY`), late (`LATE`), checking out (`EXIT`), or leaving early (`EARLY EXIT`).
*   **Google Sheets Cloud Sync:** Runs native background HTTP secure POST redirects to seamlessly store data on cloud spreadsheets.
*   **Captive Portal & Web Interface:** Operates as a Standalone Access Point Wi-Fi Hotspot with Captive Redirect (`192.168.4.1`) or connects directly to local institutional network infrastructures.
*   **Admin Control Panel:** Secured interface for assigning card IDs to specific roll numbers, updating structural local Wi-Fi configuration profiles, and downloading filtered CSV data directly.

---

## 📅 Scheduled Time Windows

The firmware tracks continuous hours to align with lab schedules. If a student checks in after the first 20 minutes of a slot, they are flagged as `LATE`. If they check out more than 10 minutes before a slot ends, they are flagged as `EARLY EXIT`.

| Time Window | Assigned Class Slot ID | Purpose / Description |
| :--- | :--- | :--- |
| **09:00 AM - 09:59 AM** | `10A` | Morning Session A |
| **10:00 AM - 10:59 AM** | `10B` | Morning Session B |
| **11:00 AM - 11:59 AM** | `11A` | Midday Session A |
| **12:00 PM - 12:59 PM** | `11B` | Midday Session B |
| **01:00 PM - 01:59 PM** | `12A` | Afternoon Session A |
| **02:00 PM - 02:59 PM** | `LUNCH_BREAK` | System Locked (Access Denied) |
| **03:00 PM - 03:59 PM** | `12B` | Late Afternoon Session B |
| **04:00 PM - 04:59 PM** | `12C` | Extension Session C *(New)* |
| *Any other time* | `NO_CLASS` | Lab Closed State (Access Denied) |

---

## 🛠️ Hardware Interfacing & PCB Routing Schematic

The system uses both **SPI** (high frequency for RFID) and **I2C** (shared multi-drop bus topology for the LCD Display & DS3231 RTC module). 

### Complete System Pin Map

| Component Module | Module Pin | ESP32 GPIO Pin | Connection Type / Implementation Notes |
| :--- | :--- | :--- | :--- |
| **MFRC522 RFID** | 3.3V | 3.3V | **Must be 3.3V.** Add a $10\mu\text{F}$ capacitor close to track for stability. |
| | GND | GND | Shared Reference Ground Plane |
| | RST | **GPIO 4** | Reset Pin Config |
| | SDA (SS) | **GPIO 5** | Dedicated SPI Slave Select (Hardware VSPI) |
| | MOSI | **GPIO 23** | Master Out Slave In (Hardware VSPI) |
| | MISO | **GPIO 19** | Master In Slave Out (Hardware VSPI) |
| | SCK | **GPIO 18** | Serial Clock Input (Hardware VSPI) |
| **DS3231 RTC** | VCC | 5V / 3.3V | Primary Logic Supply |
| | GND | GND | Shared Reference Ground Plane |
| | SDA | **GPIO 21** | Hardware I2C Data Line (Parallel Shared Bus Topology) |
| | SCL | **GPIO 22** | Hardware I2C Clock Line (Parallel Shared Bus Topology) |
| **I2C LCD (16x2)** | VCC | 5V | **Requires 5V** for uniform backlight and sharp trace text. |
| | GND | GND | Shared Reference Ground Plane |
| | SDA | **GPIO 21** | Hardware I2C Data Line (Parallel Shared Bus Topology) |
| | SCL | **GPIO 22** | Hardware I2C Clock Line (Parallel Shared Bus Topology) |
| **4x4 Keypad Matrix**| Row 1 | **GPIO 13** | Matrix Input Matrix Row |
| | Row 2 | **GPIO 12** | Matrix Input Matrix Row *(Bootstrapping Pin: Avoid keypresses during boot)* |
| | Row 3 | **GPIO 14** | Matrix Input Matrix Row |
| | Row 4 | **GPIO 27** | Matrix Input Matrix Row |
| | Col 1 | **GPIO 26** | Matrix Active Scan Output Column |
| | Col 2 | **GPIO 25** | Matrix Active Scan Output Column |
| | Col 3 | **GPIO 33** | Matrix Active Scan Output Column |
| | Col 4 | **GPIO 32** | Matrix Active Scan Output Column |

---

## 💻 Software Installation & Configuration

### 1. Required Arduino Libraries
Ensure you have the following libraries installed inside your Arduino IDE (`Sketch` -> `Include Library` -> `Manage Libraries`):
*   `MFRC522` (by GithubCommunity)
*   `RTClib` (by Adafruit)
*   `Keypad` (by Mark Stanley, Alexander Brevig)
*   `LiquidCrystal_I2C` (by Frank de Brabander)

### 2. Deployment of Google Apps Script
1. Open a Google Sheet and navigate to **Extensions** -> **Apps Script**.
2. Paste your backend deployment handling script (which receives JSON strings with keys `roll`, `classSlot`, `status`, and `timestamp`).
3. Deploy the script as a **Web App**, configuring access to **Anyone** (even anonymous).
4. Copy the deployment URL.
5. Paste this URL directly into your ESP32 source file code line:
   ```cpp
   const char* googleScriptUrl = "[https://script.google.com/macros/s/.../exec](https://script.google.com/macros/s/.../exec)";
