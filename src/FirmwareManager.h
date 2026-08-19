#pragma once
#include <Arduino.h>
#include <Update.h>
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_vfs_fat.h"
#include "esp_app_format.h"

class FirmwareManager {
public:
    static void checkAndApplyUpdate() {
        Serial0.println("[SYSTEM] Mounting RAW FAT file system (Bypassing Wear-Levelling)...");
        
        esp_vfs_fat_mount_config_t mount_config = {};
        mount_config.max_files = 4;
        mount_config.format_if_mount_failed = false;
        mount_config.allocation_unit_size = 4096;
        
        esp_err_t err = esp_vfs_fat_spiflash_mount_ro("/ffat", "ffat", &mount_config);
        if (err != ESP_OK) {
            Serial0.printf("[SYSTEM] Raw Mount Failed: %s\n", esp_err_to_name(err));
            return;
        }

        const char* targetPath = "/ffat/firmware.bin";
        FILE* updateFile = fopen(targetPath, "rb");
        if (!updateFile) {
            targetPath = "/ffat/firmware.bin.bin";
            updateFile = fopen(targetPath, "rb");
        }

        if (updateFile) {
            fseek(updateFile, 0, SEEK_END);
            size_t fileSize = ftell(updateFile);
            fseek(updateFile, 0, SEEK_SET);

            Serial0.printf("[OTA] Found %s (%d bytes).\n", targetPath, fileSize);

            if (fileSize > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                uint8_t magic = 0;
                fread(&magic, 1, 1, updateFile);
                
                if (magic != ESP_IMAGE_HEADER_MAGIC) {
                    Serial0.printf("[OTA] ERROR: Invalid Magic Byte (0x%02X). File is corrupted.\n", magic);
                } else {
                    esp_app_desc_t new_app_info;
                    fseek(updateFile, sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), SEEK_SET);
                    fread(&new_app_info, 1, sizeof(esp_app_desc_t), updateFile);
                    
                    Serial0.println("--- INCOMING FIRMWARE DETECTED ---");
                    Serial0.printf("Compile Date: %s %s\n", new_app_info.date, new_app_info.time);
                    Serial0.printf("Project Name: %s\n", new_app_info.project_name);
                    Serial0.printf("IDF Version:  %s\n", new_app_info.idf_ver);
                    Serial0.println("----------------------------------");

                    const esp_partition_t* running = esp_ota_get_running_partition();
                    esp_app_desc_t running_app_info;
                    esp_ota_get_partition_description(running, &running_app_info);
                    
                    if (memcmp(new_app_info.app_elf_sha256, running_app_info.app_elf_sha256, sizeof(new_app_info.app_elf_sha256)) == 0) {
                        Serial0.println("[OTA] Binary matches currently running image. Skipping flash.");
                    } else {
                        Serial0.println("[OTA] New firmware verified! Flashing to partition...");
                        fseek(updateFile, 0, SEEK_SET);
                        
                        if (Update.begin(fileSize, U_FLASH)) {
                            uint8_t* buf = (uint8_t*)malloc(4096);
                            size_t written = 0;
                            bool writeError = false;

                            while (written < fileSize) {
                                size_t bytesToRead = min((size_t)4096, fileSize - written);
                                size_t len = fread(buf, 1, bytesToRead, updateFile);
                                if (len == 0) break;

                                size_t w = Update.write(buf, len);
                                if (w != len) {
                                    Serial0.println("[OTA] ERROR: Flash write failed mid-stream!");
                                    writeError = true;
                                    break;
                                }
                                written += len;
                                
                                if (written % 131072 == 0) {
                                    Serial0.printf("[OTA] Flashed %d bytes...\n", written);
                                }
                            }
                            free(buf);

                            if (!writeError && written == fileSize && Update.end(true)) {
                                Serial0.printf("[OTA] Update complete! Wrote %d bytes. Rebooting...\n", written);
                                fclose(updateFile);
                                esp_vfs_fat_spiflash_unmount_ro("/ffat", "ffat");
                                delay(1000);
                                ESP.restart();
                            } else {
                                Serial0.printf("[OTA] Update Failed: %s\n", Update.errorString());
                            }
                        } else {
                            Serial0.printf("[OTA] Not enough space: %s\n", Update.errorString());
                        }
                    }
                }
            } else {
                Serial0.println("[OTA] File is too small to be a valid firmware image.");
            }
            fclose(updateFile);
        } else {
            Serial0.println("[OTA] No firmware.bin found. Booting normally.");
        }
        
        esp_vfs_fat_spiflash_unmount_ro("/ffat", "ffat");
    }
};