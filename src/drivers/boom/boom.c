#include "config.h"

// The sensor boom measurement circuit only exists on RS41 hardware
#if defined(RS41) && SENSOR_BOOM_ENABLE

#include "boom.h"
#include "gpio.h"
#include "drivers/hal/delay.h"

#ifndef RS41_RSM4x4
#include <stm32f1xx_hal.h>
#else
#include <stm32l4xx_hal.h>
#endif

// Number of consecutive oscillator periods accumulated per measurement
#define BOOM_CAPTURE_PERIODS 2400
// Poll-loop iterations to wait for a single capture edge before declaring the
// channel dead. Live channels produce an edge every ~10-20 us; this is a few
// tens of milliseconds.
#define BOOM_EDGE_POLL_TIMEOUT 200000
// Oscillator settling time after switching a channel in
#define BOOM_SETTLE_TIME_MS 18

void boom_power_down(void)
{
    HAL_GPIO_WritePin(BANK_PULLUP_TM, PIN_PULLUP_TM, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_PULLUP_HYG, PIN_PULLUP_HYG, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_SPST1, PIN_SPST1, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_SPST2, PIN_SPST2, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_SPST3, PIN_SPST3, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_SPST4, PIN_SPST4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_SPDT1, PIN_SPDT1, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_SPDT2, PIN_SPDT2, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BANK_SPDT3, PIN_SPDT3, GPIO_PIN_RESET);
}

/**
 * Connect a single channel to the oscillator: power down everything first,
 * then raise the channel's rail bias pin and analog switch pin.
 */
static void boom_select(boom_channel channel)
{
    boom_power_down();

    switch (channel) {
        case BOOM_CHANNEL_REF_750R:
            HAL_GPIO_WritePin(BANK_SPST1, PIN_SPST1, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BANK_PULLUP_TM, PIN_PULLUP_TM, GPIO_PIN_SET);
            break;
        case BOOM_CHANNEL_REF_1100R:
            HAL_GPIO_WritePin(BANK_SPST2, PIN_SPST2, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BANK_PULLUP_TM, PIN_PULLUP_TM, GPIO_PIN_SET);
            break;
        case BOOM_CHANNEL_TEMP_HUMIDITY_SENSOR:
            HAL_GPIO_WritePin(BANK_SPST3, PIN_SPST3, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BANK_PULLUP_TM, PIN_PULLUP_TM, GPIO_PIN_SET);
            break;
        case BOOM_CHANNEL_TEMP_MAIN:
            HAL_GPIO_WritePin(BANK_SPST4, PIN_SPST4, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BANK_PULLUP_TM, PIN_PULLUP_TM, GPIO_PIN_SET);
            break;
        case BOOM_CHANNEL_HUMIDITY:
            HAL_GPIO_WritePin(BANK_SPDT1, PIN_SPDT1, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BANK_PULLUP_HYG, PIN_PULLUP_HYG, GPIO_PIN_SET);
            break;
        case BOOM_CHANNEL_REF_CAP_HIGH:
            HAL_GPIO_WritePin(BANK_SPDT2, PIN_SPDT2, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BANK_PULLUP_HYG, PIN_PULLUP_HYG, GPIO_PIN_SET);
            break;
        case BOOM_CHANNEL_REF_CAP_LOW:
            HAL_GPIO_WritePin(BANK_SPDT3, PIN_SPDT3, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BANK_PULLUP_HYG, PIN_PULLUP_HYG, GPIO_PIN_SET);
            break;
        default:
            break;
    }
}

static uint32_t boom_timer_clock_hz(void)
{
    uint32_t clock = HAL_RCC_GetPCLK1Freq();
    // Timer clock is 2x PCLK1 when the APB1 prescaler is not 1
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1) {
        clock *= 2;
    }
    return clock;
}

float boom_measure_frequency(boom_channel channel)
{
    uint64_t total_ticks = 0;
    uint32_t poll_count;
    bool success = true;

    boom_select(channel);
    delay_ms(BOOM_SETTLE_TIME_MS);

    // Route the oscillator output (PA1) to TIM2 channel 2
    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.Pin = PIN_BOOM_MEAS;
    gpio_init.Pull = GPIO_NOPULL;
#ifdef RS41_RSM4x4
    gpio_init.Mode = GPIO_MODE_AF_PP;
    gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
    gpio_init.Alternate = GPIO_AF1_TIM2;
#else
    gpio_init.Mode = GPIO_MODE_AF_INPUT;
    gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
#endif
    HAL_GPIO_Init(BANK_BOOM_MEAS, &gpio_init);

    // Register-level TIM2 setup for input capture on channel 2. No state is
    // preserved: the radio data timer performs its own full reinitialization.
    __HAL_RCC_TIM2_CLK_ENABLE();
    TIM2->CR1 = 0;
    TIM2->PSC = 0;
    TIM2->ARR = 0xFFFFFFFFU; // Truncates to 16 bits on the F100, 32 bits on the L412
    TIM2->CCMR1 = TIM_CCMR1_CC2S_0 | TIM_CCMR1_IC2F_2; // TI2 input, glitch filter (fDTS/2, N=6)
    TIM2->CCER = TIM_CCER_CC2E; // Capture on rising edge
    TIM2->SR = 0;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CR1 |= TIM_CR1_CEN;

    // The capture loop runs with interrupts disabled: an ISR between edges
    // would not corrupt the hardware capture itself, but a delayed CCR2 read
    // can miss an edge entirely, silently merging two periods into one delta.
    __disable_irq();

    // Wait for the first edge to establish a reference capture
    poll_count = 0;
    while (!(TIM2->SR & TIM_SR_CC2IF)) {
        if (++poll_count > BOOM_EDGE_POLL_TIMEOUT) {
            success = false;
            break;
        }
    }
    uint32_t previous_capture = TIM2->CCR2; // Reading CCR2 clears CC2IF

    for (uint32_t i = 0; success && i < BOOM_CAPTURE_PERIODS; i++) {
        poll_count = 0;
        while (!(TIM2->SR & TIM_SR_CC2IF)) {
            if (++poll_count > BOOM_EDGE_POLL_TIMEOUT) {
                success = false;
                break;
            }
        }
        if (!success) {
            break;
        }

        uint32_t capture = TIM2->CCR2;
        TIM2->SR = ~TIM_SR_CC2IF;
#ifdef RS41_RSM4x4
        total_ticks += (uint32_t) (capture - previous_capture);
#else
        // TIM2 is 16-bit on the F100; wraparound arithmetic keeps deltas valid
        total_ticks += (uint16_t) (capture - previous_capture);
#endif
        previous_capture = capture;
    }

    __enable_irq();

    TIM2->CR1 &= ~TIM_CR1_CEN;
    boom_power_down();

    if (!success || total_ticks == 0) {
        return 0.0f;
    }

    return (float) ((double) boom_timer_clock_hz() * (double) BOOM_CAPTURE_PERIODS / (double) total_ticks);
}

#endif
