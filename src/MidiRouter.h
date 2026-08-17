#ifndef MIDI_ROUTER_H
#define MIDI_ROUTER_H

#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "SpinlockGuard.h"

enum class MidiEvent : uint8_t {
    NONE = 0,
    EXPRESSION_UPDATE,
    KNOB_UPDATE,
    PREV_MODE,
    NEXT_MODE,
    LATENCY_CYCLE,
    PANIC_RESET,
    SR_TOGGLE,
    PB2_WIPER_TOGGLE,
    VOL_MODE_TOGGLE,
    TOGGLE_EFFECT,
    STEP_PARAM_UP,
    STEP_PARAM_DOWN
};

struct MidiAction {
    MidiEvent event = MidiEvent::NONE;
    uint8_t cc = 0;
    uint8_t val = 0;
    uint16_t rawValue = 0;
    int targetEffect = 0;
};

class MidiRouter {
public:
    // Corrected case: portMUX_INITIALIZER_UNLOCKED
    inline static portMUX_TYPE paramMux = portMUX_INITIALIZER_UNLOCKED;

    static inline MidiAction parseMessage(uint8_t data1, uint8_t data2) {
        MidiAction action;
        action.cc = data1;
        action.val = data2;

        switch (data1) {
            case 11: // Expression Pedal / Pitch Bend proxy
                action.event = MidiEvent::EXPRESSION_UPDATE;
                action.rawValue = (uint16_t)data2 * 128; // Map 7-bit to 14-bit equivalent
                break;

            case 14: action.event = MidiEvent::PREV_MODE; break;
            case 15: action.event = MidiEvent::NEXT_MODE; break;
            case 16: action.event = MidiEvent::LATENCY_CYCLE; break;
            case 17: action.event = MidiEvent::PANIC_RESET; break;
            case 18: action.event = MidiEvent::SR_TOGGLE; break;
            case 19: action.event = MidiEvent::VOL_MODE_TOGGLE; break;
            case 20: action.event = MidiEvent::PB2_WIPER_TOGGLE; break;

            case 21: action.event = MidiEvent::STEP_PARAM_DOWN; break;
            case 22: action.event = MidiEvent::STEP_PARAM_UP; break;

            // Direct Effect Toggles (CC 30-39 map to Effects 0-9)
            case 30: case 31: case 32: case 33: case 34:
            case 35: case 36: case 37: case 38: case 39:
                action.event = MidiEvent::TOGGLE_EFFECT;
                action.targetEffect = data1 - 30;
                break;

            // Real-time Knob Parameter Updates (CC 70-74 map to parameters 0-4)
            case 70: case 71: case 72: case 73: case 74:
                action.event = MidiEvent::KNOB_UPDATE;
                break;

            default:
                action.event = MidiEvent::NONE;
                break;
        }

        return action;
    }

    static inline void updateParameter(
        uint8_t cc, 
        uint8_t val, 
        int currentMode, 
        volatile float* effectMemory, 
        float fxParams[10][5], 
        volatile bool& lutNeedsUpdate, 
        volatile bool& dspNeedsCommit, 
        std::atomic<int>& feedbackIntervalIdx) 
    {
        int paramIdx = cc - 70;
        if (paramIdx < 0 || paramIdx >= 5) return;

        float normalizedVal = (float)val / 127.0f;

        {
            CriticalSectionGuard lock(paramMux);
            fxParams[currentMode][paramIdx] = normalizedVal;

            if (currentMode == 2 && paramIdx == 0) { // Feedback Interval Mode
                int fbIdx = (int)(normalizedVal * 4.99f);
                feedbackIntervalIdx.store(fbIdx, std::memory_order_release);
            }
        }

        lutNeedsUpdate = true;
        dspNeedsCommit = true;
    }
};

#endif // MIDI_ROUTER_H