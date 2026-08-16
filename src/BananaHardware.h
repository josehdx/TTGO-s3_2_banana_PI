// BananaHardware_v1.15
#ifndef BANANA_HARDWARE_H
#define BANANA_HARDWARE_H

#include <Arduino.h>
#include <driver/i2s_std.h>
#include "esp_adc/adc_continuous.h"
#include <atomic>

class BananaHardware {
public:
    // Centralized I2S Pin Config for Banana Leaf
    static i2s_std_config_t getI2SConfig(uint32_t sampleRate) {
        i2s_std_config_t stdConfig = { 
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate), 
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO), 
            .gpio_cfg = { 
                .mclk = GPIO_NUM_6, 
                .bclk = GPIO_NUM_7, 
                .ws   = GPIO_NUM_18, 
                .dout = GPIO_NUM_21, 
                .din  = GPIO_NUM_17 
            } 
        };
        stdConfig.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
        return stdConfig;
    }

    static int getBatteryPercentage(float voltage) {
        return 100; 
    }

    static void fetchADCDMA(adc_continuous_handle_t handle, volatile bool& isPaused, volatile int& pb1, volatile int& pb2, volatile int& pb3, std::atomic<int>& bat) {
        if(isPaused) return;
        uint8_t result[128] __attribute__((aligned(4)));
        uint32_t ret_num = 0; 
        esp_err_t err; 
        int loop_bound = 0;
        
        while(loop_bound++ < 4) {
            err = adc_continuous_read(handle, result, sizeof(result), &ret_num, 0);
            if(err == ESP_OK && ret_num > 0) {
                for(int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
                    adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
                    if(p->type2.channel == ADC_CHANNEL_0) pb1 = p->type2.data;
                    if(p->type2.channel == ADC_CHANNEL_1) pb2 = p->type2.data;
                    if(p->type2.channel == ADC_CHANNEL_9) pb3 = p->type2.data;
                    if(p->type2.channel == ADC_CHANNEL_3) bat.store(p->type2.data, std::memory_order_relaxed);
                }
            } else if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) { 
                adc_continuous_stop(handle); 
                vTaskDelay(pdMS_TO_TICKS(2)); 
                adc_continuous_start(handle); 
                break; 
            } else { 
                adc_continuous_stop(handle); 
                adc_continuous_start(handle); 
                break; 
            }
        }
    }
};

#endif