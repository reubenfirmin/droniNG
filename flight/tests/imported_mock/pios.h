/* SPDX-License-Identifier: GPL-3.0-or-later; host simulation only. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PIOS_INCLUDE_RE1_FPGA
#define PIOS_Assert assert
#include <pios_spi.h>
typedef struct { uint16_t GPIO_Pin; } GPIO_InitTypeDef;
struct stm32_gpio { void *gpio; GPIO_InitTypeDef init; };
#define GPIO_AF_MCO 0
#define RCC_MCO1Source_HSE 1
#define RCC_MCO1Div_1 1
#define ENABLE 1
void *PIOS_malloc(size_t size);
void GPIO_Init(void *port, GPIO_InitTypeDef *init);
void GPIO_ResetBits(void *port, uint16_t pin);
void GPIO_SetBits(void *port, uint16_t pin);
bool GPIO_ReadInputDataBit(void *port, uint16_t pin);
void GPIO_PinAFConfig(void *port, unsigned pin, unsigned af);
void RCC_MCO1Config(unsigned source, unsigned divider);
void RCC_MCO1Cmd(unsigned enabled);
void PIOS_DELAY_WaitmS(unsigned ms);
