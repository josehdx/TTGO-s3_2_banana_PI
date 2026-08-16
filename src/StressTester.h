#ifndef STRESS_TESTER_H
#define STRESS_TESTER_H

#include <Arduino.h>

typedef void (*ModeSwitchFn)(int);
typedef void (*PanicResetFn)();

struct StressParams {
    volatile int* pb1;
    volatile int* pb2;
    volatile int* pb3;
    ModeSwitchFn switchMode;
    PanicResetFn triggerPanic;
    volatile bool* srToggleReq;
};

class StressTester {
public:
    static void StressTask(void* pvParameters) {
        StressParams* p = (StressParams*)pvParameters;
        int currentFxCount = 1;

        unsigned long lastPedalRandomize = 0;
        unsigned long lastSrToggle = millis();
        unsigned long lastFxStep = millis();
        bool isHoldingMaxLoad = false;
        unsigned long maxLoadStartTime = 0;

        for (;;) {
            unsigned long now = millis();

            // 1. Continuous Pedal Randomization (Every 40 ms)
            if (now - lastPedalRandomize >= 40) {
                *p->pb1 = random(0, 4095);
                *p->pb2 = random(0, 4095);
                *p->pb3 = random(0, 4095);
                lastPedalRandomize = now;
            }

            // 2. Sample Rate Swap Request (Every 5000 ms / 5 Seconds)
            if (now - lastSrToggle >= 5000) {
                if (p->srToggleReq != nullptr) {
                    *p->srToggleReq = true;
                    Serial.println("[STRESS] 5s TIMER -> SAMPLE RATE TOGGLE REQUESTED (48k <-> 96k)");
                }
                lastSrToggle = now;
            }

            // 3. FX Stacking & Panic Reset State Machine
            if (!isHoldingMaxLoad) {
                if (now - lastFxStep >= 2500) {
                    if (currentFxCount < 10) {
                        Serial.printf("[STRESS] Mode Switch to %d (Stacking FX #%d)\n", currentFxCount, currentFxCount + 1);
                        p->switchMode(currentFxCount);
                        currentFxCount++;
                    } else {
                        isHoldingMaxLoad = true;
                        maxLoadStartTime = now;
                        Serial.println("==================================================================");
                        Serial.println("[STRESS] ALL 10 FX STACKED! HOLDING MAX LOAD FOR 20 SECONDS...");
                        Serial.println("==================================================================");
                    }
                    lastFxStep = now;
                }
            } else {
                // Hold max load state for 20 seconds before panic resetting
                if (now - maxLoadStartTime >= 20000) {
                    Serial.println("[STRESS] 20s HOLD COMPLETE -> FIRING STAGE 2 PANIC RESET");
                    p->triggerPanic();
                    
                    currentFxCount = 1;
                    isHoldingMaxLoad = false;
                    lastFxStep = now + 3000; // 3s grace period post-reset before restarting cycle
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10)); // Yield 10 ms
        }
    }
};

#endif