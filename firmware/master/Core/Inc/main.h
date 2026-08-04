/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define PTT_BTN_Pin GPIO_PIN_13
#define PTT_BTN_GPIO_Port GPIOC
#define RS485_RE_DE_Pin GPIO_PIN_14
#define RS485_RE_DE_GPIO_Port GPIOC
#define E28_BUSY_Pin GPIO_PIN_5
#define E28_BUSY_GPIO_Port GPIOA
#define CAN_STB_Pin GPIO_PIN_6
#define CAN_STB_GPIO_Port GPIOA
#define CAN_SHDN_Pin GPIO_PIN_7
#define CAN_SHDN_GPIO_Port GPIOA
#define E28_RX_EN_Pin GPIO_PIN_10
#define E28_RX_EN_GPIO_Port GPIOB
#define E28_NRESET_Pin GPIO_PIN_11
#define E28_NRESET_GPIO_Port GPIOB
#define E28_CS_Pin GPIO_PIN_12
#define E28_CS_GPIO_Port GPIOB
#define STATUS_LED1_Pin GPIO_PIN_8
#define STATUS_LED1_GPIO_Port GPIOA
#define E28_DIO3_Pin GPIO_PIN_4
#define E28_DIO3_GPIO_Port GPIOB
#define E28_DIO1_Pin GPIO_PIN_5
#define E28_DIO1_GPIO_Port GPIOB
#define E28_DIO1_EXTI_IRQn EXTI9_5_IRQn
#define E28_DIO2_Pin GPIO_PIN_6
#define E28_DIO2_GPIO_Port GPIOB
#define STATUS_LED2_Pin GPIO_PIN_7
#define STATUS_LED2_GPIO_Port GPIOB
#define E28_TX_EN_Pin GPIO_PIN_9
#define E28_TX_EN_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
#define STATUS_LED1_BLINK_PERIOD_MS 500U
#define E28_SPI_HANDLE hspi2
#define E28_ROLE_TRANSMITTER 1
#define AUDIO_PATH 0
#define RS485_BENCH_TEST 1
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
