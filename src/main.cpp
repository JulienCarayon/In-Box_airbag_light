/*
 * BLE Scanner -- ESP32-C3 + 0.42" OLED (72 x 40 px, SSD1306 I2C)
 *
 * BLE scanning runs as a FreeRTOS background task on core 0.
 * The display + LED update on core 1 (Arduino loop) without
 * ever blocking on a scan -- the screen is always live.
 *
 * - Only named devices are shown, sorted A-Z.
 * - Target device tracked by MAC address:
 *     LED SOLID ON        = target seen within DEVICE_TTL_MS
 *     LED FAST BLINK 50ms = target not seen
 * - Devices not seen for DEVICE_TTL_MS are automatically removed.
 *
 * Wiring:
 *   OLED VCC -> 3V3        OLED SDA -> GPIO 5
 *   OLED GND -> GND        OLED SCL -> GPIO 6
 *   Built-in LED -> GPIO 8 (active LOW on ESP32-C3 SuperMini)
 *   BOOT button  -> GPIO 9 (active LOW) -- manual scroll
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ============================================================
// Configuration
// ============================================================

#define OLED_SDA 5
#define OLED_SCL 6
#define OLED_I2C_ADDR 0x3C  // try 0x3D if screen stays blank
#define BTN_PIN 9           // BOOT button (active LOW)
#define LED_PIN 8           // Built-in LED
#define LED_ACTIVE_LOW true // set false if your LED is active HIGH

// MAC address of the device to track (lowercase, colon-separated)
#define TARGET_MAC "cb:d0:7c:59:0c:0f"

#define DEVICE_TTL_MS 500    // ms before an unseen device is removed
#define DISPLAY_EVERY_MS 200 // how often the screen refreshes
#define AUTO_SCROLL_MS 1500  // ms between auto-scroll steps
#define BLINK_FAST_MS 100    // LED blink half-period when target absent
#define MAX_DEVICES 24

// BLE scan window -- continuous, no gaps
#define BLE_SCAN_INTERVAL 100
#define BLE_SCAN_WINDOW 99

// ============================================================
// OLED
// ============================================================

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ============================================================
// Shared device list  (guarded by devicesMutex)
// ============================================================

struct Device
{
    char name[32];
    char mac[18];
    int rssi;
    unsigned long lastSeen; // millis() timestamp
};

Device devices[MAX_DEVICES];
int deviceCount = 0;
SemaphoreHandle_t devicesMutex;

// Named-only view rebuilt each display cycle (main core only, no mutex needed)
int namedIdx[MAX_DEVICES];
int namedCount = 0;
bool targetFound = false;
int scrollOffset = 0;

// ============================================================
// LED helpers
// ============================================================

inline void ledOn() { digitalWrite(LED_PIN, LED_ACTIVE_LOW ? LOW : HIGH); }
inline void ledOff() { digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW); }

void ledBlinkTick(unsigned long period_ms)
{
    bool on = ((millis() % period_ms) < (period_ms / 2));
    if (on)
        ledOn();
    else
        ledOff();
}

// ============================================================
// BLE scan callback  (runs on core 0 scan task)
// ============================================================

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks
{
public:
    void onResult(BLEAdvertisedDevice dev) override
    {
        const char *mac = dev.getAddress().toString().c_str();
        int rssi = dev.getRSSI();
        const char *name = dev.haveName() ? dev.getName().c_str() : "";
        unsigned long now = millis();

        if (xSemaphoreTake(devicesMutex, pdMS_TO_TICKS(10)) != pdTRUE)
            return;

        // Update existing entry
        for (int i = 0; i < deviceCount; i++)
        {
            if (strcmp(devices[i].mac, mac) == 0)
            {
                devices[i].rssi = rssi;
                devices[i].lastSeen = now;
                if (name[0])
                {
                    strncpy(devices[i].name, name, 31);
                    devices[i].name[31] = '\0';
                }
                xSemaphoreGive(devicesMutex);
                return;
            }
        }

        // New entry
        if (deviceCount < MAX_DEVICES)
        {
            Device &d = devices[deviceCount++];
            strncpy(d.mac, mac, 17);
            d.mac[17] = '\0';
            strncpy(d.name, name, 31);
            d.name[31] = '\0';
            d.rssi = rssi;
            d.lastSeen = now;
        }

        xSemaphoreGive(devicesMutex);
    }
};

ScanCallbacks scanCallbacks;

// ============================================================
// Background scan task  (core 0)
// ============================================================

void scanTask(void * /*param*/)
{
    BLEDevice::init("ESP32C3-BLEScan");
    BLEScan *pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&scanCallbacks, /*wantDuplicates=*/true);
    pScan->setActiveScan(true);
    pScan->setInterval(BLE_SCAN_INTERVAL);
    pScan->setWindow(BLE_SCAN_WINDOW);

    // Start continuous scan -- never stops, callbacks fire in real time
    pScan->start(0, /*async=*/false); // duration=0 means indefinite

    // Should never reach here, but yield if it does
    vTaskDelete(NULL);
}

// ============================================================
// Expire stale devices + rebuild named index  (main core)
// ============================================================

int nameCmp(const void *a, const void *b)
{
    return strcasecmp(
        devices[*(const int *)a].name,
        devices[*(const int *)b].name);
}

void rebuildView()
{
    unsigned long now = millis();

    if (xSemaphoreTake(devicesMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return;

    // Remove devices not seen recently
    for (int i = 0; i < deviceCount;)
    {
        if (now - devices[i].lastSeen > DEVICE_TTL_MS)
        {
            // Overwrite with last entry
            devices[i] = devices[--deviceCount];
        }
        else
        {
            i++;
        }
    }

    // Snapshot into named index
    namedCount = 0;
    targetFound = false;
    for (int i = 0; i < deviceCount; i++)
    {
        if (devices[i].name[0] != '\0')
        {
            namedIdx[namedCount++] = i;
            if (strcasecmp(devices[i].mac, TARGET_MAC) == 0)
                targetFound = true;
        }
    }

    xSemaphoreGive(devicesMutex);

    qsort(namedIdx, namedCount, sizeof(int), nameCmp);

    // Clamp scroll so it never points past the list
    int maxOff = namedCount - 3;
    if (maxOff < 0)
        maxOff = 0;
    if (scrollOffset > maxOff)
        scrollOffset = maxOff;
}

// ============================================================
// OLED helpers
// ============================================================

void oledSplash()
{
    u8g2.clearBuffer();
    u8g2.drawCircle(36, 17, 13, U8G2_DRAW_ALL);
    u8g2.setFont(u8g2_font_7x14B_tr);
    u8g2.drawStr(29, 22, "BT");
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(9, 36, "BLE  Scanner  v2");
    u8g2.sendBuffer();
    delay(1400);
}

void oledDeviceList()
{
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_4x6_tr);

    const int VISIBLE = 3;
    const int ROW_H = 10;

    // Inverted header
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 72, 8);
    u8g2.setDrawColor(0);
    char hdr[22];
    snprintf(hdr, sizeof(hdr), "BLE  %d named", namedCount);
    u8g2.drawStr(2, 6, hdr);
    u8g2.setDrawColor(1);

    if (namedCount == 0)
    {
        u8g2.drawStr(4, 22, "Listening...");
        u8g2.sendBuffer();
        return;
    }

    for (int row = 0; row < VISIBLE; row++)
    {
        int slot = scrollOffset + row;
        if (slot >= namedCount)
            break;

        int devI = namedIdx[slot];
        int y = 8 + 2 + row * ROW_H + 6;
        bool target = (strcasecmp(devices[devI].mac, TARGET_MAC) == 0);

        const char *sig;
        if (devices[devI].rssi > -60)
            sig = "|||";
        else if (devices[devI].rssi > -75)
            sig = "|| ";
        else
            sig = "|  ";

        char label[16];
        if (target)
        {
            label[0] = '*';
            strncpy(label + 1, devices[devI].name, 14);
            label[15] = '\0';
        }
        else
        {
            strncpy(label, devices[devI].name, 15);
            label[15] = '\0';
        }

        char row_buf[22];
        snprintf(row_buf, sizeof(row_buf), "%s %s", sig, label);
        u8g2.drawStr(0, y, row_buf);
    }

    // Scroll bar
    if (namedCount > VISIBLE)
    {
        int trackH = 30, trackY = 8;
        int maxOff = namedCount - VISIBLE;
        int thumbH = (trackH * VISIBLE) / namedCount;
        if (thumbH < 4)
            thumbH = 4;
        int thumbY = trackY + scrollOffset * (trackH - thumbH) / maxOff;
        u8g2.drawFrame(70, trackY, 2, trackH);
        u8g2.drawBox(70, thumbY, 2, thumbH);
    }

    u8g2.sendBuffer();
}

// ============================================================
// Arduino entry points  (core 1)
// ============================================================

void setup()
{
    Serial.begin(115200);
    pinMode(BTN_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    ledOff();

    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.setI2CAddress(OLED_I2C_ADDR << 1);
    u8g2.begin();
    u8g2.setContrast(200);
    oledSplash();

    // Mutex protecting the shared device list
    devicesMutex = xSemaphoreCreateMutex();

    // Launch BLE scan on core 0, stack 8 kB, priority 1
    xTaskCreatePinnedToCore(
        scanTask,      // function
        "BLEScanTask", // name
        8192,          // stack bytes
        NULL,          // param
        1,             // priority
        NULL,          // handle (unused)
        0              // core 0
    );

    Serial.println("[BLE] Background scan task started");
}

void loop()
{
    static unsigned long lastDisplay = 0;
    static unsigned long lastScroll = 0;
    static bool btnWasLow = false;

    unsigned long now = millis();

    // Rebuild view and refresh display every DISPLAY_EVERY_MS
    if (now - lastDisplay >= DISPLAY_EVERY_MS)
    {
        rebuildView();
        oledDeviceList();
        lastDisplay = now;

        // Serial heartbeat
        Serial.printf("[BLE] %d device(s) total, %d named, target %s\n",
                      deviceCount, namedCount, targetFound ? "FOUND" : "absent");
    }

    // LED
    if (targetFound)
        ledOn();
    else
        ledBlinkTick(BLINK_FAST_MS * 2);

    // Auto-scroll
    if (now - lastScroll >= AUTO_SCROLL_MS)
    {
        int maxOff = namedCount - 3;
        if (maxOff < 0)
            maxOff = 0;
        scrollOffset = (scrollOffset >= maxOff) ? 0 : scrollOffset + 1;
        lastScroll = now;
    }

    // Manual scroll via BOOT button
    bool btnLow = (digitalRead(BTN_PIN) == LOW);
    if (btnLow && !btnWasLow)
    {
        int maxOff = namedCount - 3;
        if (maxOff < 0)
            maxOff = 0;
        scrollOffset = (scrollOffset >= maxOff) ? 0 : scrollOffset + 1;
        oledDeviceList();
        delay(200);
    }
    btnWasLow = btnLow;

    delay(10);
}