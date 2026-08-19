#define FW_VERSION "v4.5"

#include <Arduino.h>
#include <Update.h>
#include "USB.h"
#include "USBMSC.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_vfs_fat.h"
#include "esp_app_format.h" // Required to read the internal binary metadata

// --- BANANA SPECIFIC HARDWARE & MONITORING ---
#include "BananaHardware.h"
#include "SerialMonitor.h"

// --- MODULAR SYSTEM MANAGERS ---
#include "SystemState.h"
#include "SettingsManager.h"
#include "DSPEngine.h"
#include "MidiRouter.h"
#include "PedalManager.h"
#include "BluetoothManager.h"
#include "LUTManager.h"
#include "I2SManager.h"
#include "StressTester.h"
#include "SpinlockGuard.h"

// --- DECOUPLED TASKS & ACTIONS ---
#include "SystemActions.h"
#include "MidiHandler.h"
#include "LUTTask.h"
#include "AudioTask.h"
#include "InputManager.h"
#include "AudioBufferManager.h"

#if defined(TARGET_LILYGO)
#include "DisplayManager.h"
DisplayManager displayManager;
#endif

#if defined(TARGET_BANANA)
    #define ENABLE_STRESS_TESTER true
#else
    #define ENABLE_STRESS_TESTER false 
#endif

SerialMonitor serialMonitor;

// ============================================================================
// NATIVE USB MSC BLOCK CALLBACKS 
// ============================================================================
USBMSC MSC;
const esp_partition_t* ffat_partition = NULL;

static uint8_t msc_cache[4096];
static uint32_t msc_cache_sector = 0xFFFFFFFF;
static bool msc_cache_dirty = false;
static unsigned long msc_last_write_time = 0; 

static void my_msc_flush_cb() {
    if (msc_cache_dirty && msc_cache_sector != 0xFFFFFFFF && ffat_partition) {
        esp_partition_erase_range(ffat_partition, msc_cache_sector * 4096, 4096);
        esp_partition_write(ffat_partition, msc_cache_sector * 4096, msc_cache, 4096);
        msc_cache_dirty = false;
    }
}

static int32_t my_msc_read_cb(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!ffat_partition) return -1;
    uint32_t abs_offset = (lba * 4096) + offset;
    
    esp_partition_read(ffat_partition, abs_offset, buffer, bufsize);

    if (msc_cache_dirty && msc_cache_sector != 0xFFFFFFFF) {
        uint32_t cache_start = msc_cache_sector * 4096;
        uint32_t cache_end = cache_start + 4096;
        uint32_t read_start = abs_offset;
        uint32_t read_end = abs_offset + bufsize;

        if (read_start < cache_end && read_end > cache_start) {
            uint32_t overlap_start = max(read_start, cache_start);
            uint32_t overlap_end = min(read_end, cache_end);
            uint32_t overlap_len = overlap_end - overlap_start;
            uint32_t buffer_offset = overlap_start - read_start;
            uint32_t cache_offset = overlap_start - cache_start;
            memcpy((uint8_t*)buffer + buffer_offset, msc_cache + cache_offset, overlap_len);
        }
    }
    return bufsize;
}

static int32_t my_msc_write_cb(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (!ffat_partition) return -1;
    uint32_t abs_offset = (lba * 4096) + offset;
    
    for (uint32_t i = 0; i < bufsize; ) {
        uint32_t current_addr = abs_offset + i;
        uint32_t sector = current_addr / 4096;
        uint32_t sec_offset = current_addr % 4096;
        uint32_t bytes_to_write = min(bufsize - i, 4096 - sec_offset);
        
        if (sector != msc_cache_sector) {
            my_msc_flush_cb(); 
            msc_cache_sector = sector;
            esp_partition_read(ffat_partition, sector * 4096, msc_cache, 4096); 
        }
        memcpy(msc_cache + sec_offset, buffer + i, bytes_to_write);
        msc_cache_dirty = true;
        i += bytes_to_write;
    }
    msc_last_write_time = millis(); 
    return bufsize;
}

static bool my_msc_start_stop_cb(uint8_t power_condition, bool start, bool load_eject) {
    my_msc_flush_cb(); 
    return true;
}

// ============================================================================
// FIRMWARE UPDATE TRIGGER LOGIC (RAW READ-ONLY MOUNT + METADATA EXTRACTION)
// ============================================================================
void checkAndApplyFirmwareUpdate() {
    Serial0.println("[SYSTEM] Mounting RAW FAT file system (Bypassing Wear-Levelling)...");
    
    // Using standard variable assignment avoids C++ designated initializer order errors
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
            
            // 1. Verify Magic Byte
            uint8_t magic = 0;
            fread(&magic, 1, 1, updateFile);
            
            if (magic != ESP_IMAGE_HEADER_MAGIC) {
                Serial0.printf("[OTA] ERROR: Invalid Magic Byte (0x%02X). File is corrupted.\n", magic);
            } else {
                // 2. Extract Incoming Firmware Metadata
                esp_app_desc_t new_app_info;
                fseek(updateFile, sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), SEEK_SET);
                fread(&new_app_info, 1, sizeof(esp_app_desc_t), updateFile);
                
                Serial0.println("--- INCOMING FIRMWARE DETECTED ---");
                Serial0.printf("Compile Date: %s %s\n", new_app_info.date, new_app_info.time);
                Serial0.printf("Project Name: %s\n", new_app_info.project_name);
                Serial0.printf("IDF Version:  %s\n", new_app_info.idf_ver);
                Serial0.println("----------------------------------");

                // 3. Extract Current Running Metadata
                const esp_partition_t* running = esp_ota_get_running_partition();
                esp_app_desc_t running_app_info;
                esp_ota_get_partition_description(running, &running_app_info);
                
                // Compare Secure SHA256 Hashes to prevent boot loops
                if (memcmp(new_app_info.app_elf_sha256, running_app_info.app_elf_sha256, sizeof(new_app_info.app_elf_sha256)) == 0) {
                    Serial0.println("[OTA] Binary matches currently running image. Skipping flash.");
                } else {
                    Serial0.println("[OTA] New firmware verified! Flashing to partition...");
                    
                    // Reset file pointer to the beginning for the actual flash read
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
    
    // Unmount RO partition so the USB MSC task can grab exclusive raw control
    esp_vfs_fat_spiflash_unmount_ro("/ffat", "ffat");
}

// ============================================================================
// MAIN SETUP
// ============================================================================
void setup() {
    Serial.begin(115200); 

    BoardHAL::init();             
    Serial0.println("\n--- HARDWARE UART0 INITIALIZED ---");
    Serial0.print("Firmware Version: ");
    Serial0.println(FW_VERSION);

    checkAndApplyFirmwareUpdate();

    ffat_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
    if (ffat_partition) {
        uint32_t block_count = ffat_partition->size / 4096;
        MSC.vendorID("MultiFX");
        MSC.productID("Storage");
        MSC.productRevision("1.0");
        MSC.onRead(my_msc_read_cb);
        MSC.onWrite(my_msc_write_cb);
        MSC.onStartStop(my_msc_start_stop_cb);
        MSC.mediaPresent(true);
        MSC.isWritable(true);
        MSC.begin(block_count, 4096);
        Serial0.println("[USB] Mass Storage Class (MSC) initialized with 4K Sectors.");
    }
         
    USB.begin(); 
    vTaskDelay(pdMS_TO_TICKS(100));

    Serial0.println("[INIT 1/8] Allocating System Tables, Audio Buffers & I2S DMA...");
    LUTManager::allocateAll(); 
    LUTManager::generateStaticTables();
    AudioBufferManager::allocate(); 
    monoPolyEngine.allocateBuffers(); 

    padVectorFilter.setLPF(1200.0f, (float)currentSampleRate.load(std::memory_order_acquire));
    I2SManager::initChannels(BoardHAL::getI2SConfig(currentSampleRate.load(std::memory_order_acquire)), HOP_SIZE);

    Serial0.println("[INIT 2/8] Initializing NVS Flash...");
    esp_err_t err = nvs_flash_init(); 
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { 
        ESP_ERROR_CHECK(nvs_flash_erase()); 
        err = nvs_flash_init(); 
    } 
    ESP_ERROR_CHECK(err);

    settingsMgr.init(preferences);
    isKnobEditMode = preferences.getBool("knobEditMode", false);

#if defined(FW_MODE_KNOBS_ONLY)
    isKnobEditMode = true;
#endif

    Serial0.println("[INIT 3/8] Setting up Control Surface MIDI & Bluetooth...");
#if !defined(FW_MODE_KNOBS_ONLY)
    if (!isKnobEditMode) {
        BluetoothManager::initHCI();
        btmidi = new BluetoothMIDI_Interface(); 
        btmidi->setName("Whammy_S3"); 
        Control_Surface >> pipes >> *btmidi;
        Control_Surface >> pipes >> usbmidi;
        usbmidi >> pipes >> Control_Surface;
        *btmidi >> pipes >> Control_Surface;
        Control_Surface.setMIDIInputCallbacks(channelMessageCallback, nullptr, nullptr, nullptr);
        Control_Surface.begin(); 
        BluetoothManager::configurePowerAndMac();
    } else {
        Control_Surface >> pipes >> usbmidi;
        usbmidi >> pipes >> Control_Surface;
        Control_Surface.setMIDIInputCallbacks(channelMessageCallback, nullptr, nullptr, nullptr);
        Control_Surface.begin();
    }
#else
    Control_Surface >> pipes >> usbmidi;
    usbmidi >> pipes >> Control_Surface;
    Control_Surface.setMIDIInputCallbacks(channelMessageCallback, nullptr, nullptr, nullptr);
    Control_Surface.begin();
#endif

    Serial0.println("[INIT 4/8] Configuring Power Lock...");
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "DSP_Max_CPU", &dsp_cpu_lock); 
    if(dsp_cpu_lock != NULL) esp_pm_lock_acquire(dsp_cpu_lock);
    
    Serial0.println("[INIT 5/8] Configuring ADC Continuous DMA...");
    adc_continuous_handle_cfg_t adc_config={}; 
    adc_config.max_store_buf_size=16384; 
    adc_config.conv_frame_size=128; 
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &multifx_adc_handle));
    
    adc_continuous_config_t dig_cfg={}; 
    dig_cfg.sample_freq_hz = 2 * 1000; 
    dig_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1; 
    dig_cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    
    adc_digi_pattern_config_t adc_pattern[5]={ 
        {.atten=ADC_ATTEN_DB_12,.channel=ADC_CHANNEL_0,.unit=ADC_UNIT_1,.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH}, 
        {.atten=ADC_ATTEN_DB_12,.channel=ADC_CHANNEL_1,.unit=ADC_UNIT_1,.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH}, 
        {.atten=ADC_ATTEN_DB_12,.channel=ADC_CHANNEL_9,.unit=ADC_UNIT_1,.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH}, 
        {.atten=ADC_ATTEN_DB_12,.channel=ADC_CHANNEL_3,.unit=ADC_UNIT_1,.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH},
        {.atten=ADC_ATTEN_DB_12,.channel=ADC_CHANNEL_2,.unit=ADC_UNIT_1,.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH} 
    };
    dig_cfg.pattern_num=5; 
    dig_cfg.adc_pattern=adc_pattern; 
    ESP_ERROR_CHECK(adc_continuous_config(multifx_adc_handle, &dig_cfg)); 
    ESP_ERROR_CHECK(adc_continuous_start(multifx_adc_handle));

    activeEffectMode.store(0, std::memory_order_release);
    latencyMode.store(constrain(preferences.getInt("latMode", 0), 0, 3), std::memory_order_release); 
    isPB2WiperMode=preferences.getBool("pb2Wiper", false); 
    isVolumeMode=false; 
    currentSampleRate.store(96000, std::memory_order_release); 
    freezeLength = 96000; 
    feedbackIntervalIdx.store(constrain(preferences.getInt("fbIdx", 0), 0, 4), std::memory_order_release);
    
    AppSettings savedSettings; 
    size_t len=preferences.getBytes("dspData", &savedSettings, sizeof(AppSettings));
    if(len==sizeof(AppSettings)) { 
        for(int i=0; i<10; i++) { 
            effectMemory[i]=savedSettings.fxMem[i]; 
            for(int p=0; p<5; p++) fxParams[i][p]=savedSettings.params[i][p]; 
        } 
    }
    
    commitDSPState();
    pedals.resetToCenter(); 
    pinMode(BOOT_SENSE_PIN, INPUT_PULLUP); 
    pinMode(BLE_TOGGLE_PIN, INPUT_PULLUP); 
    lastActivityTime=millis();

    Serial0.println("[INIT 6/8] Calibrating Expression Pedals...");
    calibratePBs(); 
    
    DSPCoreState* activeDSP = dspActiveState.load(std::memory_order_acquire);
    LUTManager::updateDynamicLUT(activeDSP->cp, activeDSP->w, activeDSP->activeMode, effectMemory, activeDSP->fbIdx, currentSampleRate.load(std::memory_order_acquire)); 
    
    float* currLut = LUTManager::pitchShiftLUT.load(std::memory_order_acquire); 
    if (currLut) pitchShiftFactor.store(currLut[8192], std::memory_order_release); 

    Serial0.println("[INIT 7/8] Spawning Core 0 (Audio) and Core 1 (LUT) FreeRTOS Tasks...");
    dspTaskStack = (StackType_t*)heap_caps_aligned_alloc(16, 8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); 
    dspTaskTCB = (StaticTask_t*)heap_caps_aligned_alloc(16, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dspTaskStack != nullptr && dspTaskTCB != nullptr) { 
        audioTaskHandle = xTaskCreateStaticPinnedToCore(AudioDSPTask, "DSP", 8192, NULL, configMAX_PRIORITIES - 1, dspTaskStack, dspTaskTCB, 0); 
    } else { 
        while(1) vTaskDelay(100); 
    }
    
    lutTaskStack = (StackType_t*)heap_caps_aligned_alloc(16, 3072, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); 
    lutTaskTCB = (StaticTask_t*)heap_caps_aligned_alloc(16, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (lutTaskStack != nullptr && lutTaskTCB != nullptr) { 
        lutTaskHandle = xTaskCreateStaticPinnedToCore(LUTUpdateTask, "LUT_Task", 3072, NULL, 1, lutTaskStack, lutTaskTCB, 1); 
    } else { 
        while(1) vTaskDelay(100); 
    }

    I2SManager::enableChannels(); 

    Serial0.println("[INIT 8/8] Spawning Selected Stress Tester Task...");
    if (ENABLE_STRESS_TESTER) { 
        static StressParams stressParams = { &latestPB1, &latestPB2, &latestPB3, switchEffectMode, triggerPanicReset, &sampleRateToggleRequested };
#if (SELECTED_STRESS_TEST == 2)
        xTaskCreatePinnedToCore(StressTester::MonoPolyStressTask, "MonoStressTask", 4096, &stressParams, 1, NULL, 1);
#else
        xTaskCreatePinnedToCore(StressTester::StressTask, "PolyStressTask", 4096, &stressParams, 1, NULL, 1);
#endif
    }
    Serial0.println("--- SETUP COMPLETE: ENTERING MAIN SYSTEM LOOP ---");
}

void loop() {
    unsigned long loop_start_time = micros();                
    static unsigned long lastLoopMicro = micros();                
    static bool lastBtState = false;                          

    // --- CRITICAL: AUTO-FLUSH USB CACHE ---
    if (msc_cache_dirty && (millis() - msc_last_write_time > 500)) {
        my_msc_flush_cb();
    }

    if (bleEnabled.load(std::memory_order_relaxed)) { 
        Control_Surface.loop(); 
    } else { 
        Control_Surface.updateMidiInput(); 
    }                          

    bool currentBtState = false;
#if !defined(FW_MODE_KNOBS_ONLY)
    if (!isKnobEditMode && btmidi != nullptr) { currentBtState = btmidi->isConnected(); }
#endif                     

    if (currentBtState != lastBtState) { lastBtState = currentBtState; }                
    InputManager::processInputs(currentBtState);                     

    if (!ENABLE_STRESS_TESTER) {
        BoardHAL::fetchADC(multifx_adc_handle, isAdcPaused, latestPB1, latestPB2, latestPB3, latestPar1, latestBat);
    }                
    BoardHAL::updateExtraControls(activeEffectMode.load(std::memory_order_acquire), effectMemory, fxParams, lutNeedsUpdate, dspNeedsCommit, feedbackIntervalIdx, isKnobEditMode, latestPar1);                          

    InputManager::processPedals();                          

    static unsigned long lastLutUpdate = 0;                
    if (lutNeedsUpdate && (millis() - lastLutUpdate > (ENABLE_STRESS_TESTER ? 250 : 40))) {                         
        lutNeedsUpdate = false;                
        asyncLutUpdateRequested.store(true, std::memory_order_release);                
        lastLutUpdate = millis();                 
    }                          

    if (sampleRateToggleRequested) { sampleRateToggleRequested = false; toggleSampleRate(); }        
    if (pb2ToggleRequested) { pb2ToggleRequested = false; calibratePBs(); }                
    if (dspNeedsCommit) { if (commitDSPState()) dspNeedsCommit = false; }                          

    unsigned long loopBusyTime = micros() - loop_start_time;                
    unsigned long totalLoopTime = micros() - lastLoopMicro;                
    lastLoopMicro = micros();                
    if (totalLoopTime > 0) {
        core1_ctrl_load.store(__builtin_fmaf(core1_ctrl_load.load(std::memory_order_relaxed), 0.95f, __builtin_fminf(100.0f, (((float)loopBusyTime / (float)totalLoopTime) * 100.0f)) * 0.05f), std::memory_order_relaxed);                     
    }

    uint32_t iter_latency = (micros() - loop_start_time) / 1000;                 
    if (iter_latency > max_loop_latency_ms.load(std::memory_order_relaxed)) {
        max_loop_latency_ms.store(iter_latency, std::memory_order_relaxed);                 
    }

#ifdef ENABLE_ADVANCED_TELEMETRY                
    if (audioTaskHandle != NULL) dsp_stack_watermark.store(uxTaskGetStackHighWaterMark(audioTaskHandle) * sizeof(StackType_t), std::memory_order_relaxed);                
    if (lutTaskHandle != NULL) lut_stack_watermark.store(uxTaskGetStackHighWaterMark(lutTaskHandle) * sizeof(StackType_t), std::memory_order_relaxed);
#endif                     

#if defined(TARGET_LILYGO)
    DSPCoreState* activeDSP_UI = dspActiveState.load(std::memory_order_acquire);                
    uint32_t watermarkVal = 0;
    #ifdef ENABLE_ADVANCED_TELEMETRY                
        watermarkVal = lut_stack_watermark.load(std::memory_order_relaxed);
    #endif
    BoardHAL::updateUI(activeDSP_UI, currentPB1, currentPB2, currentPB3, currentCC11, ui_audio_level.load(std::memory_order_acquire), ui_output_level.load(std::memory_order_acquire), core0_dsp_load.load(std::memory_order_relaxed), core1_ctrl_load.load(std::memory_order_relaxed), currentSampleRate.load(std::memory_order_acquire), max_loop_latency_ms.exchange(0, std::memory_order_relaxed), audio_underflow_count.load(std::memory_order_relaxed), watermarkVal, currentBtState, latestBat, isMonoPolyActive.load(std::memory_order_acquire));                          
#endif

    DSPCoreState* activeDSP = dspActiveState.load(std::memory_order_acquire);
    TelemetryData tData = {}; 
    tData.dspCoreLoad = core0_dsp_load.load(std::memory_order_relaxed); 
    tData.ctrlCoreLoad = core1_ctrl_load.load(std::memory_order_relaxed);
    tData.sampleRate = currentSampleRate.load(std::memory_order_acquire);
    tData.latencyMode = latencyMode.load(std::memory_order_relaxed);
    tData.batVoltage = 4.00f; tData.batPercent = 100; tData.isCharging = false;
    
#if !defined(FW_MODE_KNOBS_ONLY)
    tData.bleConnected = btmidi != nullptr ? btmidi->isConnected() : false; 
#else
    tData.bleConnected = false;
#endif

    tData.activeMode = activeDSP->activeMode;
    tData.isMonoPolyActive = isMonoPolyActive.load(std::memory_order_acquire);
    tData.monoPolyAlgo = constrain((int)(fxParams[activeDSP->activeMode][0] * 4.99f), 0, 4); 

    tData.fxStates[0] = activeDSP->w;  tData.fxStates[1] = activeDSP->fz; tData.fxStates[2] = activeDSP->fb; tData.fxStates[3] = activeDSP->hr; tData.fxStates[4] = activeDSP->cp; tData.fxStates[5] = activeDSP->sy; tData.fxStates[6] = activeDSP->pd; tData.fxStates[7] = activeDSP->ch; tData.fxStates[8] = activeDSP->sw; tData.fxStates[9] = activeDSP->vb;
    tData.pb1 = currentPB1; tData.pb2 = currentPB2; tData.pb3 = currentPB3; tData.cc11 = currentCC11;
    tData.inMeter = ui_audio_level.load(std::memory_order_acquire); tData.outMeter = ui_output_level.load(std::memory_order_acquire);
    
#ifdef ENABLE_ADVANCED_TELEMETRY
    tData.audioUnderflows = audio_underflow_count.load(std::memory_order_relaxed);
    tData.dmaTransfers = 0;
    tData.dspStackWatermark = lut_stack_watermark.load(std::memory_order_relaxed); 
#else
    tData.audioUnderflows = 0; tData.dmaTransfers = 0; tData.dspStackWatermark = 0;
#endif
    
    tData.peakLoopLatency = max_loop_latency_ms.exchange(0, std::memory_order_relaxed);

    static unsigned long lastTelemetryPrint = 0;
    if (millis() - lastTelemetryPrint >= 7000) {
        lastTelemetryPrint = millis(); 
        serialMonitor.printMetrics(tData); 
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}