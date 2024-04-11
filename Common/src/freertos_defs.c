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
