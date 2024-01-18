#include "stm32f7xx_hal.h"
#include <FreeRTOS.h>
#include <task.h>

static volatile uint32_t ulRunTimeCounterTicks;

// TODO: this must be at least 10x more than tick
void vConfigureTimerForRunTimeStats( void )
{
	ulRunTimeCounterTicks = HAL_GetTick();
}

uint32_t vGetRunTimeCounterValue( void )
{
	return ( HAL_GetTick() - ulRunTimeCounterTicks );
}

void vApplicationMallocFailedHook( void )
{
	configASSERT(pdFALSE);
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
	(void) (xTask); (void) (pcTaskName);
	configASSERT(pdFALSE);
}

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
	static StaticTask_t xIdleTaskTCB;
	static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

	*ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
	*ppxIdleTaskStackBuffer = uxIdleTaskStack;
	*pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize )
{
	static StaticTask_t xTimerTaskTCB;
	static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

	*ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
	*ppxTimerTaskStackBuffer = uxTimerTaskStack;
	*pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

