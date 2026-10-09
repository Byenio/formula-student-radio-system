/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : app_freertos.c
  * Description        : Code for freertos applications
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

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "safety.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
  /* Called by the kernel when a task overruns its stack. configCHECK_FOR_
     STACK_OVERFLOW is 2, so the check is both a bounds test on the stack
     pointer and a pattern check on the far end of the stack.

     Returning from here resumes a task whose locals are already corrupt, so
     we do not: Safety_Fault latches the task name and stops the board. */
  (void)xTask;
  Safety_Fault(SAFETY_FAULT_STACK_OVERFLOW, (const char *)pcTaskName);
}
/* USER CODE END 4 */

/* USER CODE BEGIN 5 */
void vApplicationMallocFailedHook(void)
{
  /* pvPortMalloc returned NULL. Every allocation on this board happens during
     start-up -- tasks, queues and semaphores -- so in practice this means
     configTOTAL_HEAP_SIZE is too small for the current task stacks, which is
     exactly the failure mode to expect while sizing a codec's scratch needs.

     Named "heap" rather than a task name because the caller may be the kernel
     itself, before any task exists to name. */
  Safety_Fault(SAFETY_FAULT_MALLOC_FAILED, "heap");
}
/* USER CODE END 5 */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */