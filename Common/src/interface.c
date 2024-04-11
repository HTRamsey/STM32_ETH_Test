#include <FreeRTOS_IP.h>
#include <stdlib.h>
#include <iperf_task.h>

#define TASK_PRIORITY 	( ipconfigIP_TASK_PRIORITY - 1 )
#define TASK_STACK 		( configMINIMAL_STACK_SIZE * 3 )
#define TASK_NAME 		"CommsTask"

void vApplicationPingReplyHook( ePingReplyStatus_t eStatus, uint16_t usIdentifier )
{
	( void ) eStatus; ( void ) usIdentifier;
}

BaseType_t xApplicationGetRandomNumber( uint32_t * pulNumber )
{
	*pulNumber = rand();
	return pdTRUE;
}

uint32_t ulApplicationGetNextSequenceNumber( uint32_t ulSourceAddress, uint16_t usSourcePort, uint32_t ulDestinationAddress, uint16_t usDestinationPort )
{
	uint32_t ulReturn;
	xApplicationGetRandomNumber( &( ulReturn ) );
	return ulReturn;
}

static void IP_vInitInterface()
{
	static NetworkInterface_t xInterfaces[ 1 ];
	extern NetworkInterface_t * pxSTM32_FillInterfaceDescriptor( BaseType_t xEMACIndex, NetworkInterface_t * pxInterface );
	pxSTM32_FillInterfaceDescriptor( 0, &( xInterfaces[ 0 ] ) );

	static NetworkEndPoint_t xEndPoints[ 1 ];

	const uint8_t ucIPAddress[ ipIP_ADDRESS_LENGTH_BYTES ] = { 192, 168, 1, 8 };
	const uint8_t ucNetMask[ ipIP_ADDRESS_LENGTH_BYTES ] = { 255, 255, 255, 0 };
	const uint8_t ucGatewayAddress[ ipIP_ADDRESS_LENGTH_BYTES ] = { 192, 168, 1, 1 };
	const uint8_t ucDNSServerAddress[ ipIP_ADDRESS_LENGTH_BYTES ] = { 192, 168, 1, 1 };
	const uint8_t ucMACAddress[ ipMAC_ADDRESS_LENGTH_BYTES ] = { 0x00, 0x80, 0xE1, 0x00, 0x00, 0x00 };

	FreeRTOS_FillEndPoint( &( xInterfaces[ 0 ] ), &( xEndPoints[ 0 ] ), ucIPAddress, ucNetMask, ucGatewayAddress, ucDNSServerAddress, ucMACAddress );

	const BaseType_t xResult = FreeRTOS_IPInit_Multi();
	configASSERT( xResult == pdTRUE );
	( void ) xResult;
}

static portTASK_FUNCTION( prv_vRunCommsTask, pvParameters )
{
	( void ) pvParameters;

	IP_vInitInterface();

	vIPerfInstall();

	vTaskDelete( NULL );
}

void CommsTask_vInit()
{
    static StackType_t uxTaskStack[ TASK_STACK ];
	static StaticTask_t xTaskTCB;
    ( void ) xTaskCreateStatic(
		prv_vRunCommsTask,
		TASK_NAME,
		TASK_STACK,
		NULL,
		TASK_PRIORITY,
		uxTaskStack,
		&xTaskTCB
	);
}
