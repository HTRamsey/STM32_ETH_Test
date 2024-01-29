/* Standard includes. */
#include <stdint.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_IP_Private.h"
#include "NetworkInterface.h"
#include "NetworkBufferManagement.h"

#define baINTERRUPT_BUFFER_GET_THRESHOLD    ( 3 )

#if !defined( ipconfigBUFFER_ALLOC_LOCK )
    #define ipconfigBUFFER_ALLOC_INIT()             do {} while( ipFALSE_BOOL )
    #define ipconfigBUFFER_ALLOC_LOCK_FROM_ISR()    UBaseType_t uxSavedInterruptStatus = ( UBaseType_t ) portSET_INTERRUPT_MASK_FROM_ISR(); {
    #define ipconfigBUFFER_ALLOC_UNLOCK_FROM_ISR()  portCLEAR_INTERRUPT_MASK_FROM_ISR( uxSavedInterruptStatus ); }
    #define ipconfigBUFFER_ALLOC_LOCK()             taskENTER_CRITICAL()
    #define ipconfigBUFFER_ALLOC_UNLOCK()           taskEXIT_CRITICAL()
#endif

const BaseType_t xBufferAllocFixedSize = pdTRUE;

static NetworkBufferDescriptor_t xNetworkBuffers[ ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS ];

static List_t xFreeBuffersList;

static UBaseType_t uxMinimumFreeNetworkBuffers = 0U;

static SemaphoreHandle_t xNetworkBufferSemaphore = NULL;

/*-----------------------------------------------------------*/

static BaseType_t xIsValidNetworkDescriptor( const NetworkBufferDescriptor_t * pxDesc )
{
    BaseType_t xReturn;

    const uint32_t offset = ( uint32_t ) ( ( ( const char * ) pxDesc ) - ( ( const char * ) xNetworkBuffers ) );
    const int32_t index = pxDesc - xNetworkBuffers;

    if( ( offset >= sizeof( xNetworkBuffers ) ) || ( ( offset % sizeof( xNetworkBuffers[ 0 ] ) ) != 0 ) )
    {
        xReturn = pdFALSE;
    }
    else if( ( index < 0 ) || ( ( uint32_t ) index > ( ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS - 1 ) ) )
    {
    	xReturn = pdFALSE;
    }
    else
    {
    	xReturn = pdTRUE;
    }

    return xReturn;
}

/*-----------------------------------------------------------*/

static BaseType_t xIsFreeBuffer( const NetworkBufferDescriptor_t * pxDesc )
{
    return ( xIsValidNetworkDescriptor( pxDesc ) != pdFALSE ) && ( listIS_CONTAINED_WITHIN( &xFreeBuffersList, &( pxDesc->xBufferListItem ) ) != pdFALSE );
}

/*-----------------------------------------------------------*/

UBaseType_t uxGetMinimumFreeNetworkBuffers( void )
{
    return uxMinimumFreeNetworkBuffers;
}

/*-----------------------------------------------------------*/

UBaseType_t uxGetNumberOfFreeNetworkBuffers( void )
{
    return listCURRENT_LIST_LENGTH( &xFreeBuffersList );
}

/*-----------------------------------------------------------*/

NetworkBufferDescriptor_t * pxResizeNetworkBufferWithDescriptor( NetworkBufferDescriptor_t * pxNetworkBuffer, size_t xNewSizeBytes )
{
    pxNetworkBuffer->xDataLength = xNewSizeBytes;
    return pxNetworkBuffer;
}

/*-----------------------------------------------------------*/

BaseType_t xNetworkBuffersInitialise( void )
{
    BaseType_t xReturn;
    size_t i;

    if( xNetworkBufferSemaphore == NULL )
    {
        ipconfigBUFFER_ALLOC_INIT();

        #if ( configSUPPORT_STATIC_ALLOCATION != 0 )
            static StaticSemaphore_t xNetworkBufferSemaphoreBuffer;
            xNetworkBufferSemaphore = xSemaphoreCreateCountingStatic( ( UBaseType_t ) ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS, ( UBaseType_t ) ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS, &xNetworkBufferSemaphoreBuffer );
        #else
            xNetworkBufferSemaphore = xSemaphoreCreateCounting( ( UBaseType_t ) ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS, ( UBaseType_t ) ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS );
        #endif

        configASSERT( xNetworkBufferSemaphore != NULL );

        if( xNetworkBufferSemaphore != NULL )
        {
            #if ( configQUEUE_REGISTRY_SIZE > 0 )
                vQueueAddToRegistry( xNetworkBufferSemaphore, "NetBufSem" );
            #endif

            vListInitialise( &xFreeBuffersList );

            vNetworkInterfaceAllocateRAMToBuffers( xNetworkBuffers );

            for( i = 0U; i < ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS; ++i )
            {
                vListInitialiseItem( &( xNetworkBuffers[ i ].xBufferListItem ) );
                listSET_LIST_ITEM_OWNER( &( xNetworkBuffers[ i ].xBufferListItem ), &xNetworkBuffers[ i ] );
                vListInsert( &xFreeBuffersList, &( xNetworkBuffers[ i ].xBufferListItem ) );
            }

            uxMinimumFreeNetworkBuffers = ( UBaseType_t ) ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS;
        }
    }

    if( xNetworkBufferSemaphore == NULL )
    {
        xReturn = pdFAIL;
    }
    else
    {
        xReturn = pdPASS;
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

NetworkBufferDescriptor_t * pxGetNetworkBufferWithDescriptor( size_t xRequestedSizeBytes, TickType_t xBlockTimeTicks )
{
    NetworkBufferDescriptor_t * pxReturn = NULL;
    UBaseType_t uxCount;

    if( xNetworkBufferSemaphore != NULL )
    {
        if( xSemaphoreTake( xNetworkBufferSemaphore, xBlockTimeTicks ) == pdPASS )
        {
            ipconfigBUFFER_ALLOC_LOCK();
            {
                pxReturn = ( NetworkBufferDescriptor_t * ) listGET_OWNER_OF_HEAD_ENTRY( &xFreeBuffersList );

                if( xIsFreeBuffer( pxReturn ) != pdFALSE )
                {
                    ( void ) uxListRemove( &( pxReturn->xBufferListItem ) );
                }
                else
                {
                    pxReturn = NULL;
                }
            }
            ipconfigBUFFER_ALLOC_UNLOCK();

            if( pxReturn != NULL )
            {
                uxCount = uxGetNumberOfFreeNetworkBuffers();

                if( uxMinimumFreeNetworkBuffers > uxCount )
                {
                    uxMinimumFreeNetworkBuffers = uxCount;
                }

                pxReturn->xDataLength = xRequestedSizeBytes;
                pxReturn->pxInterface = NULL;
                pxReturn->pxEndPoint = NULL;

                #if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 )
                    pxReturn->pxNextBuffer = NULL;
                #endif
            }
        }
    }

    if( pxReturn == NULL )
    {
        iptraceFAILED_TO_OBTAIN_NETWORK_BUFFER();
    }
    else
    {
        iptraceNETWORK_BUFFER_OBTAINED( pxReturn );
    }

    return pxReturn;
}
/*-----------------------------------------------------------*/

void vReleaseNetworkBufferAndDescriptor( NetworkBufferDescriptor_t * const pxNetworkBuffer )
{
    BaseType_t xListItemAlreadyInFreeList;

    if( xIsValidNetworkDescriptor( pxNetworkBuffer ) != pdFALSE )
    {
        ipconfigBUFFER_ALLOC_LOCK();
        {
            xListItemAlreadyInFreeList = listIS_CONTAINED_WITHIN( &xFreeBuffersList, &( pxNetworkBuffer->xBufferListItem ) );

            if( xListItemAlreadyInFreeList == pdFALSE )
            {
                vListInsertEnd( &xFreeBuffersList, &( pxNetworkBuffer->xBufferListItem ) );
            }
        }
        ipconfigBUFFER_ALLOC_UNLOCK();

        if( xListItemAlreadyInFreeList == pdFALSE )
        {
            if( xSemaphoreGive( xNetworkBufferSemaphore ) == pdPASS )
            {
                iptraceNETWORK_BUFFER_RELEASED( pxNetworkBuffer );
            }

            pxNetworkBuffer->xDataLength = 0U;
        }
    }
}
/*-----------------------------------------------------------*/

NetworkBufferDescriptor_t * pxNetworkBufferGetFromISR( size_t xRequestedSizeBytes )
{
    NetworkBufferDescriptor_t * pxReturn = NULL;
    UBaseType_t uxCount;

    if( xNetworkBufferSemaphore != NULL )
    {
        if( uxQueueMessagesWaitingFromISR( ( QueueHandle_t ) xNetworkBufferSemaphore ) > ( UBaseType_t ) baINTERRUPT_BUFFER_GET_THRESHOLD )
        {
            if( xSemaphoreTakeFromISR( xNetworkBufferSemaphore, NULL ) == pdPASS )
            {
                ipconfigBUFFER_ALLOC_LOCK_FROM_ISR();
                {
                    pxReturn = ( NetworkBufferDescriptor_t * ) listGET_OWNER_OF_HEAD_ENTRY( &xFreeBuffersList );

                    if( xIsFreeBuffer( pxReturn ) != pdFALSE )
                    {
                        ( void ) uxListRemove( &( pxReturn->xBufferListItem ) );
                    }
                    else
                    {
                        pxReturn = NULL;
                    }
                }
                ipconfigBUFFER_ALLOC_UNLOCK_FROM_ISR();

                if( pxReturn != NULL )
                {
                    uxCount = uxGetNumberOfFreeNetworkBuffers();

                    if( uxMinimumFreeNetworkBuffers > uxCount )
                    {
                        uxMinimumFreeNetworkBuffers = uxCount;
                    }

                    pxReturn->xDataLength = xRequestedSizeBytes;
                    pxReturn->pxInterface = NULL;
                    pxReturn->pxEndPoint = NULL;

                    #if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 )
                        pxReturn->pxNextBuffer = NULL;
                    #endif
                }
            }
        }
    }

    if( pxReturn == NULL )
    {
        iptraceFAILED_TO_OBTAIN_NETWORK_BUFFER_FROM_ISR();
    }
    else
    {
        iptraceNETWORK_BUFFER_OBTAINED_FROM_ISR( pxReturn );
    }

    return pxReturn;
}
/*-----------------------------------------------------------*/

BaseType_t xReleaseNetworkBufferFromISR( NetworkBufferDescriptor_t * const pxNetworkBuffer )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t xListItemAlreadyInFreeList;

    if( xIsValidNetworkDescriptor( pxNetworkBuffer ) != pdFALSE )
    {
        ipconfigBUFFER_ALLOC_LOCK_FROM_ISR();
        {
            xListItemAlreadyInFreeList = listIS_CONTAINED_WITHIN( &xFreeBuffersList, &( pxNetworkBuffer->xBufferListItem ) );

            if( xListItemAlreadyInFreeList == pdFALSE )
            {
                vListInsertEnd( &xFreeBuffersList, &( pxNetworkBuffer->xBufferListItem ) );
            }
        }
        ipconfigBUFFER_ALLOC_UNLOCK_FROM_ISR();

        if( xListItemAlreadyInFreeList == pdFALSE )
        {
            if( xSemaphoreGiveFromISR( xNetworkBufferSemaphore, &xHigherPriorityTaskWoken ) == pdPASS )
            {
                iptraceNETWORK_BUFFER_RELEASED( pxNetworkBuffer );
            }
        }

        pxNetworkBuffer->xDataLength = 0U;
    }

    return xHigherPriorityTaskWoken;
}

/*-----------------------------------------------------------*/
