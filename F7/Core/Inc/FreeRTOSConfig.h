#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stm32f7xx.h>

// #define FREERTOS_TASKS_C_ADDITIONS_INIT
// freertos_tasks_c_additions_init.h
// freertos_tasks_c_additions_init();

#define configUSE_PREEMPTION                    1
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
extern uint32_t SystemCoreClock;
#define configCPU_CLOCK_HZ                      SystemCoreClock
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)256)
#define configTOTAL_HEAP_SIZE                   ((size_t)(352U * 1024U))
#define configAPPLICATION_ALLOCATED_HEAP 		0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 1
#define configUSE_16_BIT_TICKS                  0

// #define configMAX_TASK_NAME_LEN
#define configUSE_RECURSIVE_MUTEXES				1
#define configUSE_MUTEXES                       1
#define configUSE_APPLICATION_TASK_TAG          0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               5
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configUSE_NEWLIB_REENTRANT              1
#define configUSE_MPU_WRAPPERS_V1               0
#define configUSE_PICOLIBC_TLS                  0
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
// #define configMESSAGE_BUFFER_LENGTH_TYPE

#define configCHECK_FOR_STACK_OVERFLOW          1
#define configUSE_SB_COMPLETED_CALLBACK         0

#define configGENERATE_RUN_TIME_STATS           1
extern void vConfigureTimerForRunTimeStats( void );
extern uint32_t vGetRunTimeCounterValue( void );
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()   vConfigureTimerForRunTimeStats()
#define portGET_RUN_TIME_COUNTER_VALUE()           vGetRunTimeCounterValue()
#define configUSE_TRACE_FACILITY                1

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               3
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            (configMINIMAL_STACK_SIZE * 2)

#define INCLUDE_xQueueGetMutexHolder            1
#define INCLUDE_xSemaphoreGetMutexHolder        1
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_uxTaskGetStackHighWaterMark2    1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1

#define configMAX_SYSCALL_INTERRUPT_PRIORITY ( 0xF << ( 8 - __NVIC_PRIO_BITS ) )

#define configASSERT( x ) assert_param( x )

#define vPortSVCHandler       SVC_Handler
#define xPortPendSVHandler    PendSV_Handler
#define xPortSysTickHandler   SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
