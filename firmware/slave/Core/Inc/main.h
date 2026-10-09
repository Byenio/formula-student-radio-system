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
#include "stm32u3xx_hal.h"

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
#define E28_BUSY_Pin GPIO_PIN_2
#define E28_BUSY_GPIO_Port GPIOA
#define E28_DIO3_Pin GPIO_PIN_3
#define E28_DIO3_GPIO_Port GPIOA
#define E28_CS_Pin GPIO_PIN_4
#define E28_CS_GPIO_Port GPIOA
#define E28_DIO1_Pin GPIO_PIN_0
#define E28_DIO1_GPIO_Port GPIOB
#define E28_DIO1_EXTI_IRQn EXTI0_IRQn
#define E28_DIO2_Pin GPIO_PIN_1
#define E28_DIO2_GPIO_Port GPIOB
#define E28_TX_EN_Pin GPIO_PIN_14
#define E28_TX_EN_GPIO_Port GPIOB
#define E28_RX_EN_Pin GPIO_PIN_15
#define E28_RX_EN_GPIO_Port GPIOB
#define E28_NRESET_Pin GPIO_PIN_8
#define E28_NRESET_GPIO_Port GPIOA
#define EXTEND_ILIM_Pin GPIO_PIN_4
#define EXTEND_ILIM_GPIO_Port GPIOB
#define EEPROM_WP_Pin GPIO_PIN_8
#define EEPROM_WP_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
#define E28_SPI_HANDLE hspi1
#define E28_USE_RTOS   0
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
