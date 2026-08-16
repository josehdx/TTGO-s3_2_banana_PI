#ifndef BLUETOOTH_MANAGER_H
#define BLUETOOTH_MANAGER_H

#include <Arduino.h>
#include <esp_nimble_hci.h> 
#include <esp_bt.h>
#include <esp_mac.h>

class BluetoothManager {
public:
    // 1. Manually pre-initialize the HCI controller before the Control Surface boots
    static void initHCI() {
        Serial.println("[BLE] Pre-initializing NimBLE HCI Controller...");
        esp_err_t err_hci = esp_nimble_hci_init();
        if (err_hci != ESP_OK) {
            Serial.printf("[BLE] HCI Init Failed! Error: %d\n", err_hci);
        } else {
            Serial.println("[BLE] HCI Controller Successfully Initialized.");
        }
    }

    // 2. Configure maximum transmission power (+9dBm) after baseband is up
    static void configurePowerAndMac() {
        Serial.println("[BLE] Setting TX Power to Maximum (+9dBm)...");
        
        // ESP_PWR_LVL_P9 = +9dBm (Maximum output power for ESP32-S3)
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
        
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        Serial.printf("[BLE] NimBLE baseband up! Hardware MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", 
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
};

#endif