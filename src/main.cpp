#define FW_VERSION "v4.6"

#include <Arduino.h>
#include "USB.h"

// --- EXTRACTED SUBSYSTEMS ---
#include "USBMscManager.h"
#include "FirmwareManager.h"
#include "SystemBootloader.h"
#include "TelemetryManager.h"

// --- BANANA SPECIFIC HARDWARE & MONITORING ---
#include "BananaHardware.h"
#include "SerialMonitor.h"
#include "SystemState.h"
#include "InputManager.h"

#if defined(TARGET_LILYGO)
#include "DisplayManager.h"
DisplayManager displayManager;
#endif

SerialMonitor serialMonitor;

// ============================================================================
// MAIN SETUP
// ============================================================================
void setup() {
    Serial.begin(115200); 
    BoardHAL::init();             
    
    Serial0.println("\n--- HARDWARE UART0 INITIALIZED ---");
    Serial0.print("Firmware Version: ");
    Serial0.println(FW_VERSION);

    // 1. Check for OTA Updates
    FirmwareManager::checkAndApplyUpdate();

    // 2. Initialize USB Storage
    USBMscManager::init();
    USB.begin(); 
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. Run the massive initialization sequence
    SystemBootloader::run();
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    unsigned long loop_start_time = micros();                
    static unsigned long lastLoopMicro = micros();                
    static bool lastBtState = false;                          

    // --- CRITICAL: AUTO-FLUSH USB CACHE ---
    USBMscManager::handleCacheFlush();

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

    // --- METRICS CALCULATION ---
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

    DSPCoreState* activeDSP = dspActiveState.load(std::memory_order_acquire);

#if defined(TARGET_LILYGO)
    uint32_t watermarkVal = 0;
    #ifdef ENABLE_ADVANCED_TELEMETRY                
        watermarkVal = lut_stack_watermark.load(std::memory_order_relaxed);
    #endif
    BoardHAL::updateUI(activeDSP, currentPB1, currentPB2, currentPB3, currentCC11, ui_audio_level.load(std::memory_order_acquire), ui_output_level.load(std::memory_order_acquire), core0_dsp_load.load(std::memory_order_relaxed), core1_ctrl_load.load(std::memory_order_relaxed), currentSampleRate.load(std::memory_order_acquire), max_loop_latency_ms.exchange(0, std::memory_order_relaxed), audio_underflow_count.load(std::memory_order_relaxed), watermarkVal, currentBtState, latestBat, isMonoPolyActive.load(std::memory_order_acquire));                          
#endif

    // --- REPORT TELEMETRY ---
    TelemetryManager::report(activeDSP);
    
    vTaskDelay(pdMS_TO_TICKS(5));
}