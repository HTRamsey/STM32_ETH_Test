/*
 * FreeRTOS+TCP <DEVELOPMENT BRANCH>
 * Copyright (C) 2022 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/*---------------------------------------------------------------------------*/

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_ARP.h"
#if ipconfigIS_ENABLED( ipconfigUSE_MDNS ) || ipconfigIS_ENABLED( ipconfigUSE_LLMNR )
    #include "FreeRTOS_DNS.h"
#endif
#if ipconfigIS_ENABLED( ipconfigUSE_IPv6 )
    #include "FreeRTOS_ND.h"
#endif
#include "FreeRTOS_Routing.h"
#if ipconfigIS_ENABLED( ipconfigETHERNET_DRIVER_FILTERS_PACKETS )
    #include "FreeRTOS_Sockets.h"
#endif
#include "NetworkBufferManagement.h"
#include "NetworkInterface.h"
#include "phyHandling.h"

/* ST includes. */
#if defined( STM32F4 )
    #include "stm32f4xx_hal.h"
#elif defined( STM32F7 )
    #include "stm32f7xx_hal.h"
#elif defined( STM32H7 )
    #include "stm32h7xx_hal.h"
#elif defined( STM32H5 )
    #include "stm32h5xx_hal.h"
#elif defined( STM32F2 )
    #error "This NetworkInterface is incompatible with STM32F2 - Use Legacy NetworkInterface"
#else
    #error "Unknown STM32 Family for NetworkInterface"
#endif

#if defined( STM32H5 ) || defined( STM32H7 )
    /* #ifdef LAN8742A_PHY_ADDRESS */
    #include "lan8742/lan8742.h"
    /* #endif */

    /* #ifdef DP83848_PHY_ADDRESS */
    #include "dp83848/dp83848.h"
    /* #endif */
#endif

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                                Config                                     */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

#if defined( STM32F7 ) || defined( STM32F4 )
    #define niEMAC_STM32FX
#elif defined( STM32H7 ) || defined( STM32H5 )
    #define niEMAC_STM32HX
#endif

#define niEMAC_TASK_NAME        "EMAC_STM32"
#define niEMAC_TASK_PRIORITY    ( configMAX_PRIORITIES - 1 )
#define niEMAC_TASK_STACK_SIZE  ( 4U * configMINIMAL_STACK_SIZE )

#define niEMAC_TX_DESC_SECTION    ".TxDescripSection"
#define niEMAC_RX_DESC_SECTION    ".RxDescripSection"
#define niEMAC_BUFFERS_SECTION    ".EthBuffersSection"

#define niEMAC_TASK_MAX_BLOCK_TIME_MS   100U
#define niEMAC_TX_MAX_BLOCK_TIME_MS     20U
#define niEMAC_RX_MAX_BLOCK_TIME_MS     20U
#define niEMAC_DESCRIPTOR_WAIT_TIME_MS  20U

#define niEMAC_TX_MUTEX_NAME    "EMAC_TxMutex"
#define niEMAC_TX_DESC_SEM_NAME "EMAC_TxDescSem"

#define niEMAC_AUTO_NEGOTIATION ipconfigENABLE
#define niEMAC_USE_100MB        ( ipconfigENABLE && ipconfigIS_DISABLED( niEMAC_AUTO_NEGOTIATION ) )
#define niEMAC_USE_FULL_DUPLEX  ( ipconfigENABLE && ipconfigIS_DISABLED( niEMAC_AUTO_NEGOTIATION ) )
#define niEMAC_AUTO_CROSS       ( ipconfigENABLE && ipconfigIS_ENABLED( niEMAC_AUTO_NEGOTIATION ) )
#define niEMAC_CROSSED_LINK     ( ipconfigENABLE && ipconfigIS_DISABLED( niEMAC_AUTO_CROSS ) )

#define niEMAC_USE_RMII ipconfigENABLE

#define niEMAC_USE_MPU ipconfigENABLE

#ifdef niEMAC_STM32HX
	#define niEMAC_TCP_SEGMENTATION ipconfigDISABLE
	#define niEMAC_ARP_OFFLOAD      ipconfigDISABLE
	#define niEMAC_USE_PHY_INT      ipconfigDISABLE
#endif

#define niEMAC_DEBUG_ERROR ipconfigDISABLE

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                             Config Checks                                 */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

#ifndef HAL_ETH_MODULE_ENABLED
    #error "HAL_ETH_MODULE_ENABLED must be enabled for NetworkInterface"
#endif

#if ipconfigIS_DISABLED( configUSE_TASK_NOTIFICATIONS )
    #error "Task Notifications must be enabled for NetworkInterface"
#endif

#if ipconfigIS_DISABLED( ipconfigZERO_COPY_TX_DRIVER )
    #error "ipconfigZERO_COPY_TX_DRIVER must be enabled for NetworkInterface"
#endif

#if ipconfigIS_DISABLED( ipconfigZERO_COPY_RX_DRIVER )
    #error "ipconfigZERO_COPY_RX_DRIVER must be enabled for NetworkInterface"
#endif

#if ( ipconfigNETWORK_MTU < ETH_MIN_PAYLOAD ) || ( ipconfigNETWORK_MTU > ETH_MAX_PAYLOAD )
    #error "Unsupported ipconfigNETWORK_MTU size for NetworkInterface"
#endif

#if defined( niEMAC_STM32HX )

    #if ( ETH_TX_DESC_CNT % 4 != 0 )
        #error "Invalid ETH_TX_DESC_CNT value for NetworkInterface, must be a multiple of 4"
    #endif

    #if ( ETH_RX_DESC_CNT % 4 != 0 )
        #error "Invalid ETH_RX_DESC_CNT value for NetworkInterface, must be a multiple of 4"
    #endif

#endif

#if ipconfigIS_DISABLED( ipconfigPORT_SUPPRESS_WARNING )

	#if defined( niEMAC_STM32FX ) && defined( ETH_RX_BUF_SIZE )
		#warning "As of F7 V1.17.1 && F4 V1.28.0, a bug exists in the ETH HAL Driver where ETH_RX_BUF_SIZE is used instead of RxBuffLen, so ETH_RX_BUF_SIZE must == niEMAC_DATA_BUFFER_SIZE"
    #endif

    #if ipconfigIS_DISABLED( ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM )
        #warning "Consider enabling ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM for NetworkInterface"
    #endif

    #if ipconfigIS_DISABLED( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM )
        #warning "Consider enabling ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM for NetworkInterface"
    #endif

    #if ipconfigIS_DISABLED( ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES )
        #warning "Consider enabling ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES for NetworkInterface"
    #endif

	/* TODO: There should be a universal check for use in network interfaces, similar to eConsiderFrameForProcessing.
	 * So, don't use this macro, and filter anyways in the mean time. */
	/* #if ipconfigIS_DISABLED( ipconfigETHERNET_DRIVER_FILTERS_PACKETS )
			#warning "Consider enabling ipconfigETHERNET_DRIVER_FILTERS_PACKETS for NetworkInterface"
	#endif */

    #if ipconfigIS_DISABLED( ipconfigUSE_LINKED_RX_MESSAGES )
        #warning "Consider enabling ipconfigUSE_LINKED_RX_MESSAGES for NetworkInterface"
    #endif

#endif

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                            Macros & Definitions                           */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

#define niEMAC_MPU ( defined( __MPU_PRESENT ) && ( __MPU_PRESENT == 1U ) )
#if ( niEMAC_MPU != 0 )
    #define niEMAC_MPU_ENABLED      ( _FLD2VAL( MPU_CTRL_ENABLE, MPU->CTRL ) != 0 )
#endif

#define niEMAC_CACHEABLE ( defined( __DCACHE_PRESENT ) && ( __DCACHE_PRESENT == 1U ) )
#if ( niEMAC_CACHEABLE != 0 )
    #define niEMAC_CACHE_ENABLED    ( _FLD2VAL( SCB_CCR_DC, SCB->CCR ) != 0 )
    #define niEMAC_CACHE_MAINTENANCE ( niEMAC_CACHE_ENABLED && ipconfigIS_DISABLED( niEMAC_USE_MPU ) )
    #ifdef __SCB_DCACHE_LINE_SIZE
        #define niEMAC_DATA_ALIGNMENT    __SCB_DCACHE_LINE_SIZE
    #else
        #define niEMAC_DATA_ALIGNMENT    32U
    #endif
#else
    #define niEMAC_DATA_ALIGNMENT   4U
#endif

#define niEMAC_DATA_ALIGNMENT_MASK   ( niEMAC_DATA_ALIGNMENT - 1U )
#define niEMAC_BUF_ALIGNMENT        32U
#define niEMAC_BUF_ALIGNMENT_MASK   ( niEMAC_BUF_ALIGNMENT - 1U )

#define niEMAC_DATA_BUFFER_SIZE     ( ( ipTOTAL_ETHERNET_FRAME_SIZE + niEMAC_DATA_ALIGNMENT_MASK ) & ~niEMAC_DATA_ALIGNMENT_MASK )
#define niEMAC_TOTAL_BUFFER_SIZE    ( ( ( niEMAC_DATA_BUFFER_SIZE + ipBUFFER_PADDING ) + niEMAC_BUF_ALIGNMENT_MASK ) & ~niEMAC_BUF_ALIGNMENT_MASK )

#if defined( niEMAC_STM32FX )

	#ifdef ETH_GIANT_PACKET
		#undef ETH_GIANT_PACKET
	#endif
	#define ETH_GIANT_PACKET ETH_DMARXDESC_IPV4HCE

	#ifdef ETH_DMA_RX_BUFFER_UNAVAILABLE_FLAG
		#undef ETH_DMA_RX_BUFFER_UNAVAILABLE_FLAG
	#endif
	#define ETH_DMA_RX_BUFFER_UNAVAILABLE_FLAG ETH_DMASR_RBUS

	#ifdef ETH_DMA_TX_BUFFER_UNAVAILABLE_FLAG
		#undef ETH_DMA_TX_BUFFER_UNAVAILABLE_FLAG
	#endif
	#define ETH_DMA_TX_BUFFER_UNAVAILABLE_FLAG ETH_DMASR_TBUS

	/* Note: ETH_CTRLPACKETS_BLOCK_ALL is incorrectly defined in HAL ETH Driver as of F7 V1.17.1 && F4 V1.28.0 */
	#ifdef ETH_CTRLPACKETS_BLOCK_ALL
		#undef ETH_CTRLPACKETS_BLOCK_ALL
	#endif
	#define ETH_CTRLPACKETS_BLOCK_ALL ETH_MACFFR_PCF_BlockAll

#elif defined( niEMAC_STM32HX )

	#ifdef ETH_DMA_TX_BUFFER_UNAVAILABLE_FLAG
		#undef ETH_DMA_TX_BUFFER_UNAVAILABLE_FLAG
	#endif
	#define ETH_DMA_TX_BUFFER_UNAVAILABLE_FLAG ETH_DMACSR_TBU

	#define ETH_DMA_TX_ERRORS_MASK ETH_DMACSR_TEB
	#define ETH_DMA_RX_ERRORS_MASK ETH_DMACSR_REB

#endif

#define INCR_TX_DESC_INDEX( inx, offset )\
    do {\
        ( inx ) += ( offset );\
        if( ( inx ) >= ETH_TX_DESC_CNT )\
        {\
            ( inx ) = ( ( inx ) - ETH_TX_DESC_CNT );\
        }\
    } while( 0 )

#define INCR_RX_DESC_INDEX( inx, offset )\
    do {\
        ( inx ) += ( offset );\
        if( ( inx ) >= ETH_RX_DESC_CNT )\
        {\
            ( inx ) = ( ( inx ) - ETH_RX_DESC_CNT );\
        }\
    } while( 0 )

#define niEMAC_BUFS_PER_DESC 2U

/* IEEE 802.3 CRC32 polynomial - 0x04C11DB7 */
#define niEMAC_CRC_POLY 0x04C11DB7
#define niEMAC_MAC_IS_MULTICAST( MAC )  ( ( MAC[ 0 ] & 1U ) != 0 )
#define niEMAC_MAC_IS_UNICAST( MAC )    ( ( MAC[ 0 ] & 1U ) == 0 )
#define niEMAC_ADDRESS_HASH_BITS    64U
#define niEMAC_MAC_MATCH_COUNT  3U

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                               typedefs                                    */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

/* Interrupt events to process: reception, transmission and error handling. */
typedef enum {
    eMacEventNone = 0,
    eMacEventRx = 1 << 0,
    eMacEventTx = 1 << 1,
    eMacEventErrRx = 1 << 2,
    eMacEventErrTx = 1 << 3,
    eMacEventErrDma = 1 << 4,
    eMacEventErrEth = 1 << 5,
    eMacEventErrMac = 1 << 6,
    eMacEventAll = ( 1 << 7 ) - 1,
} eMAC_IF_EVENT;

typedef enum
{
    eMacEthInit, /* Must initialise ETH. */
    eMacPhyInit, /* Must initialise PHY. */
    eMacPhyStart, /* Must start PHY. */
    eMacTaskStart, /* Must start deferred interrupt handler task. */
    eMacEthStart, /* Must start ETH. */
    eMacInitComplete /* Initialisation was successful. */
} eMAC_INIT_STATUS_TYPE;

/* typedef struct xPhyData
{
    EthernetPhy_t xPhyObject;
    #if defined( niEMAC_STM32HX ) && ipconfigIS_ENABLED( niEMAC_USE_PHY_INT )
		union {
			lan8742_Object_t xLan8742a;
			dp83848_Object_t xDp83848;
		} xPhyInstance;
	#endif
} PhyData_t;

typedef struct xMacSrcMatchData
{
    uint8_t ucSrcMatchCounters[ niEMAC_MAC_MATCH_COUNT ];
    uint8_t uxMACEntryIndex = 0;
} MacSrcMatchData_t;

typedef struct xMacHashData
{
    uint32_t ulHashTable[ niEMAC_ADDRESS_HASH_BITS / 32 ];
    uint8_t ucAddrHashCounters[ niEMAC_ADDRESS_HASH_BITS ];
} MacHashData_t;

typedef struct xMacFilteringData
{
    MacSrcMatchData_t xSrcMatch;
    MacHashData_t xHash;
} MacFilteringData_t;

typedef struct xEMACData
{
    ETH_HandleTypeDef xEthHandle;
    ETH_DMADescTypeDef xDMADescTx[ ETH_TX_DESC_CNT ];
    ETH_DMADescTypeDef xDMADescRx[ ETH_RX_DESC_CNT ];
    EthernetPhy_t xPhyObject;
    TaskHandle_t xEMACTaskHandle;
    SemaphoreHandle_t xTxMutex, xTxDescSem;
    ETH_BufferTypeDef xTxBuffers[ niEMAC_BUFS_PER_DESC * ETH_TX_DESC_CNT ];
    BaseType_t xSwitchRequired;
    eMAC_INIT_STATUS_TYPE xMacInitStatus;
    BaseType_t xEMACIndex;
    MacFilteringData_t xMacFilteringData;
} EMACData_t; */

/* TODO: need a data structure to assist in adding/removing allowed addresses */

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                      Static Function Declarations                         */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

/* Phy Hooks */
static BaseType_t prvPhyReadReg( BaseType_t xAddress, BaseType_t xRegister, uint32_t * pulValue );
static BaseType_t prvPhyWriteReg( BaseType_t xAddress, BaseType_t xRegister, uint32_t ulValue );

/* Network Interface Access Hooks */
static BaseType_t prvGetPhyLinkStatus( NetworkInterface_t * pxInterface );
static BaseType_t prvNetworkInterfaceInitialise( NetworkInterface_t * pxInterface );
static BaseType_t prvNetworkInterfaceOutput( NetworkInterface_t * pxInterface, NetworkBufferDescriptor_t * const pxDescriptor, BaseType_t xReleaseAfterSend );
static void prvAddAllowedMACAddress( NetworkInterface_t * pxInterface, const uint8_t * pucMacAddress );
static void prvRemoveAllowedMACAddress( NetworkInterface_t * pxInterface, const uint8_t * pucMacAddress );

/* EMAC Task */
static BaseType_t prvNetworkInterfaceInput( ETH_HandleTypeDef * pxEthHandle, NetworkInterface_t * pxInterface );
static __NO_RETURN portTASK_FUNCTION_PROTO( prvEMACHandlerTask, pvParameters );
static BaseType_t prvEMACTaskStart( NetworkInterface_t * pxInterface );

/* EMAC Init */
static BaseType_t prvEthConfigInit( ETH_HandleTypeDef * pxEthHandle, NetworkInterface_t * pxInterface );
static void prvInitMacAddresses( ETH_TypeDef * const pxEthHandle, NetworkInterface_t * pxInterface );
#ifdef niEMAC_STM32HX
    static void prvInitPacketFilter( ETH_TypeDef * const pxEthHandle, const NetworkInterface_t * const pxInterface );
#endif
static BaseType_t prvPhyInit( ETH_TypeDef * const pxEthInstance, EthernetPhy_t * pxPhyObject );
static BaseType_t prvPhyStart( ETH_HandleTypeDef * pxEthHandle, NetworkInterface_t * pxInterface, EthernetPhy_t * pxPhyObject );

/* MAC Filtering Helpers */
static uint32_t prvCalcCrc32( const uint8_t * const pucMACAddr );
static uint8_t prvGetMacHashIndex( const uint8_t * const pucMACAddr );
static void prvHAL_ETH_SetDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, uint8_t ucIndex, const uint8_t * const pucMACAddr );
static void prvHAL_ETH_ClearDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, uint8_t ucIndex );
static BaseType_t prvAddDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, const uint8_t * const pucMACAddr );
static BaseType_t prvRemoveDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, const uint8_t * const pucMACAddr );
static BaseType_t prvSetNewDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, uint8_t ucHashIndex, const uint8_t * const pucMACAddr );
static void prvAddDestMACAddrHash( ETH_TypeDef * const pxEthInstance, uint8_t ucHashIndex );
static void prvRemoveDestMACAddrHash( ETH_TypeDef * const pxEthInstance, const uint8_t * const pucMACAddr );

/* EMAC Helpers */
static void prvReleaseTxPacket( ETH_HandleTypeDef * pxEthHandle );
static BaseType_t prvMacUpdateConfig( ETH_HandleTypeDef * pxEthHandle, EthernetPhy_t * pxPhyObject );
static void prvReleaseNetworkBufferDescriptor( NetworkBufferDescriptor_t * const pxDescriptor );
static void prvSendRxEvent( NetworkBufferDescriptor_t * const pxDescriptor );
static BaseType_t prvAcceptPacket( const NetworkBufferDescriptor_t * const pxDescriptor, uint16_t usLength );

/* Network Interface Definition */
NetworkInterface_t * pxSTM32_FillInterfaceDescriptor( BaseType_t xEMACIndex, NetworkInterface_t * pxInterface );

/* Reimplemented HAL Functions */
static BaseType_t prvHAL_ETH_Start_IT( ETH_HandleTypeDef * const pxEthHandle );
static BaseType_t prvHAL_ETH_Stop_IT( ETH_HandleTypeDef * const pxEthHandle );
static BaseType_t prvHAL_ETH_ReadData( ETH_HandleTypeDef * const pxEthHandle, void ** ppvBuff );
static void prvHAL_ETH_ReleaseTxPacket( ETH_HandleTypeDef * const pxEthHandle );
static void prvETH_UpdateDescriptor( ETH_HandleTypeDef * const pxEthHandle );
static void prvHAL_ETH_SetMDIOClockRange( ETH_TypeDef * const pxEthInstance );
static void prvHAL_ETH_SetHashTable( ETH_TypeDef * const pxEthInstance );
static BaseType_t prvHAL_ETH_ReadPHYRegister( ETH_TypeDef * const pxEthInstance, uint32_t ulPhyAddr, uint32_t ulPhyReg, uint32_t * pulRegValue );
static BaseType_t prvHAL_ETH_WritePHYRegister( ETH_TypeDef * const pxEthInstance, uint32_t ulPhyAddr, uint32_t ulPhyReg, uint32_t ulRegValue );

/* Debugging */
#if ipconfigIS_ENABLED( niEMAC_DEBUG_ERROR )
	static void prvHAL_RX_ErrorCallback( ETH_HandleTypeDef * pxEthHandle );
	static void prvHAL_DMA_ErrorCallback( ETH_HandleTypeDef * pxEthHandle );
	static void prvHAL_MAC_ErrorCallback( ETH_HandleTypeDef * pxEthHandle );
#endif

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                      Static Variable Declarations                         */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

/* static EMACData_t xEMACData; */

static ETH_HandleTypeDef xEthHandle;

static EthernetPhy_t xPhyObject;

#if defined( niEMAC_STM32HX ) && ipconfigIS_ENABLED( niEMAC_USE_PHY_INT )
    static lan8742_Object_t xLan8742aObject;
    static dp83848_Object_t xDp83848Object;
#endif

static TaskHandle_t xEMACTaskHandle = NULL;
static SemaphoreHandle_t xTxMutex = NULL, xTxDescSem = NULL;

static BaseType_t xSwitchRequired = pdFALSE;

static eMAC_INIT_STATUS_TYPE xMacInitStatus = eMacEthInit;

/* Src Mac Matching */
static uint8_t ucSrcMatchCounters[ niEMAC_MAC_MATCH_COUNT ] = { 0U };
static uint8_t uxMACEntryIndex = 0;

/* Src Mac Hashing */
static uint32_t ulHashTable[ niEMAC_ADDRESS_HASH_BITS / 32 ];
static uint8_t ucAddrHashCounters[ niEMAC_ADDRESS_HASH_BITS ] = { 0U };

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                              Phy Hooks                                    */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

static BaseType_t prvPhyReadReg( BaseType_t xAddress, BaseType_t xRegister, uint32_t * pulValue )
{
    BaseType_t xResult = pdFALSE;

    if( prvHAL_ETH_ReadPHYRegister( xEthHandle.Instance, ( uint32_t ) xAddress, ( uint32_t ) xRegister, pulValue ) != pdPASS )
    {
        xResult = pdTRUE;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvPhyWriteReg( BaseType_t xAddress, BaseType_t xRegister, uint32_t ulValue )
{
    BaseType_t xResult = pdFALSE;

    if( prvHAL_ETH_WritePHYRegister( xEthHandle.Instance, ( uint32_t ) xAddress, ( uint32_t ) xRegister, ulValue ) != pdPASS )
    {
        xResult = pdTRUE;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                      Network Interface Access Hooks                       */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

static BaseType_t prvGetPhyLinkStatus( NetworkInterface_t * pxInterface )
{
    ( void ) pxInterface;

    BaseType_t xReturn = pdFALSE;

    /* const EMACData_t xEMACData = *( ( EMACData_t * ) pxInterface->pvArgument ); */

    if( xPhyObject.ulLinkStatusMask != 0U )
    {
        xReturn = pdTRUE;
    }

    return xReturn;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvNetworkInterfaceInitialise( NetworkInterface_t * pxInterface )
{
    BaseType_t xInitResult = pdFAIL;
    ETH_HandleTypeDef * pxEthHandle = &xEthHandle;
    EthernetPhy_t * pxPhyObject = &xPhyObject;

    switch( xMacInitStatus )
    {
        default:
            configASSERT( pdFALSE );
            break;

        case eMacEthInit:
            if( prvEthConfigInit( pxEthHandle, pxInterface ) == pdFALSE )
            {
                FreeRTOS_debug_printf( ( "prvNetworkInterfaceInitialise: eMacEthInit failed\n" ) );
                break;
            }

            xMacInitStatus = eMacPhyInit;
            /* fallthrough */

        case eMacPhyInit:
        	if( prvPhyInit( pxEthHandle->Instance, pxPhyObject ) == pdFALSE )
			{
				FreeRTOS_debug_printf( ( "prvNetworkInterfaceInitialise: eMacPhyInit failed\n" ) );
				break;
			}

            xMacInitStatus = eMacPhyStart;
            /* fallthrough */

        case eMacPhyStart:
            if( prvPhyStart( pxEthHandle, pxInterface, pxPhyObject ) == pdFALSE )
            {
                FreeRTOS_debug_printf( ( "prvNetworkInterfaceInitialise: eMacPhyStart failed\n" ) );
                break;
            }

            xMacInitStatus = eMacTaskStart;
            /* fallthrough */

        case eMacTaskStart:
            if( prvEMACTaskStart( pxInterface ) == pdFALSE )
            {
                FreeRTOS_debug_printf( ( "prvNetworkInterfaceInitialise: eMacTaskStart failed\n" ) );
                break;
            }

            xMacInitStatus = eMacEthStart;
            /* fallthrough */

        case eMacEthStart:
			if( prvHAL_ETH_Start_IT( pxEthHandle ) != pdPASS )
			{
				FreeRTOS_debug_printf( ( "prvNetworkInterfaceInitialise: eMacEthStart failed\n" ) );
				break;
			}

            xMacInitStatus = eMacInitComplete;
            /* fallthrough */

        case eMacInitComplete:
            if( prvGetPhyLinkStatus( pxInterface ) != pdTRUE )
            {
                FreeRTOS_debug_printf( ( "prvNetworkInterfaceInitialise: eMacInitComplete failed\n" ) );
                break;
            }

            xInitResult = pdPASS;
    }

    return xInitResult;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvNetworkInterfaceOutput( NetworkInterface_t * pxInterface, NetworkBufferDescriptor_t * const pxDescriptor, BaseType_t xReleaseAfterSend )
{
    BaseType_t xResult = pdFAIL;
    /* Zero-Copy Only */
    configASSERT( xReleaseAfterSend == pdTRUE );

    do
    {
        ETH_HandleTypeDef * pxEthHandle = &xEthHandle;

        if( ( pxDescriptor == NULL ) || ( pxDescriptor->pucEthernetBuffer == NULL ) || ( pxDescriptor->xDataLength > niEMAC_DATA_BUFFER_SIZE ) )
        {
            /* TODO: if xDataLength is greater than niEMAC_DATA_BUFFER_SIZE, you can link buffers */
            FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Invalid Descriptor\n" ) );
            break;
        }

        if( xCheckLoopback( pxDescriptor, xReleaseAfterSend ) != pdFALSE )
        {
            /* The packet has been sent back to the IP-task.
             * The IP-task will further handle it.
             * Do not release the descriptor. */
            xReleaseAfterSend = pdFALSE;
            FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Loopback\n" ) );
            break;
        }

        if( prvGetPhyLinkStatus( pxInterface ) == pdFALSE )
        {
            FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Link Down\n" ) );
            break;
        }

        if( ( xMacInitStatus != eMacInitComplete ) || ( pxEthHandle->gState != HAL_ETH_STATE_STARTED ) )
        {
            FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Interface Not Started\n" ) );
            break;
        }

        ETH_TxPacketConfig xTxConfig = {
            .CRCPadCtrl = ETH_CRC_PAD_INSERT,
            .Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD,
        };

        #if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM )
            xTxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
            xTxConfig.Attributes |= ETH_TX_PACKETS_FEATURES_CSUM;
        #else
            xTxConfig.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
        #endif

        const EthernetHeader_t * const pxEthHeader = ( const EthernetHeader_t * const ) pxDescriptor->pucEthernetBuffer;
        if( pxEthHeader->usFrameType == ipIPv4_FRAME_TYPE )
        {
            #if ipconfigIS_ENABLED( ipconfigUSE_IPv4 )
                const IPPacket_t * const pxIPPacket = ( const IPPacket_t * const ) pxDescriptor->pucEthernetBuffer;

                if( pxIPPacket->xIPHeader.ucProtocol == ipPROTOCOL_ICMP )
                {
                    #if ipconfigIS_ENABLED( ipconfigREPLY_TO_INCOMING_PINGS ) || ipconfigIS_ENABLED( ipconfigSUPPORT_OUTGOING_PINGS )
                        ICMPPacket_t * const pxICMPPacket = ( ICMPPacket_t * const ) pxDescriptor->pucEthernetBuffer;
                        #if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM )
                            pxICMPPacket->xICMPHeader.usChecksum = 0U;
                        #endif
                        ( void ) pxICMPPacket;
                    #else
                        FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Unsupported ICMP\n" ) );
                    #endif
                }
                else if( pxIPPacket->xIPHeader.ucProtocol == ipPROTOCOL_TCP )
                {
                    #if ipconfigIS_ENABLED( ipconfigUSE_TCP )
                        TCPPacket_t * const pxTCPPacket = ( TCPPacket_t * const ) pxDescriptor->pucEthernetBuffer;
                        /* #if defined( niEMAC_STM32HX ) && ipconfigIS_ENABLED( niEMAC_TCP_SEGMENTATION )
                            xTxConfig.MaxSegmentSize = ipconfigTCP_MSS;
                            xTxConfig.PayloadLen = pxDescriptor->xDataLength;
                            xTxConfig.TCPHeaderLen = ( pxTCPPacket->xIPHeader.ucVersionHeaderLength & ( uint8_t ) 0x0FU );
                            xTxConfig.Attributes |= ETH_TX_PACKETS_FEATURES_TSO;
                        #endif */
                        ( void ) pxTCPPacket;
                    #else
                        FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Unsupported TCP\n" ) );
                    #endif
                }
                else if( pxIPPacket->xIPHeader.ucProtocol == ipPROTOCOL_UDP )
                {
                    UDPPacket_t * const pxUDPPacket = ( UDPPacket_t * const ) pxDescriptor->pucEthernetBuffer;
                    ( void ) pxUDPPacket;
                }
            #else
                FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Unsupported IPv4\n" ) );
            #endif /* if ipconfigIS_ENABLED( ipconfigUSE_IPv4 ) */
        }
        else if( pxEthHeader->usFrameType == ipIPv6_FRAME_TYPE )
        {
            #if ipconfigIS_ENABLED( ipconfigUSE_IPv6 )
                const IPPacket_IPv6_t * pxIPPacket_IPv6 = ( IPPacket_IPv6_t * ) pxDescriptor->pucEthernetBuffer;

                if( pxIPPacket_IPv6->xIPHeader.ucNextHeader == ipPROTOCOL_ICMP_IPv6 )
                {
                    #if ipconfigIS_ENABLED( ipconfigREPLY_TO_INCOMING_PINGS ) || ipconfigIS_ENABLED( ipconfigSUPPORT_OUTGOING_PINGS )
                        ICMPPacket_IPv6_t * const pxICMPPacket_IPv6 = ( ICMPPacket_IPv6_t * const ) pxDescriptor->pucEthernetBuffer;
                        #if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM )
                            pxICMPPacket_IPv6->xICMPHeaderIPv6.usChecksum = 0U;
                        #endif
                        ( void ) pxICMPPacket_IPv6;
                    #else
                        FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Unsupported ICMP\n" ) );
                    #endif
                }
                else if( pxIPPacket_IPv6->xIPHeader.ucNextHeader == ipPROTOCOL_TCP )
                {
                    #if ipconfigIS_ENABLED( ipconfigUSE_TCP )
                        TCPPacket_IPv6_t * const pxTCPPacket_IPv6 = ( TCPPacket_IPv6_t * const ) pxDescriptor->pucEthernetBuffer;
                        /* #if defined( niEMAC_STM32HX ) && ipconfigIS_ENABLED( niEMAC_TCP_SEGMENTATION )
                            xTxConfig.PayloadLen = pxDescriptor->xDataLength;
                            xTxConfig.TCPHeaderLen = sizeof( pxTCPPacket_IPv6->xTCPHeader );
                            xTxConfig.Attributes |= ETH_TX_PACKETS_FEATURES_TSO;
                        #endif */
                        ( void ) pxTCPPacket_IPv6;
                    #else
                        FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Unsupported TCP\n" ) );
                    #endif
                }
                else if( pxIPPacket_IPv6->xIPHeader.ucNextHeader == ipPROTOCOL_UDP )
                {
                    UDPPacket_t * const pxUDPPacket_IPv6 = ( UDPPacket_t * const ) pxDescriptor->pucEthernetBuffer;
                    ( void ) pxUDPPacket_IPv6;
                }
            #else
                FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Unsupported IPv6\n" ) );
            #endif /* if ipconfigIS_ENABLED( ipconfigUSE_IPv6 ) */
        }
        else if( pxEthHeader->usFrameType == ipARP_FRAME_TYPE )
        {

        }

        ETH_BufferTypeDef xTxBuffer = {
            .buffer = pxDescriptor->pucEthernetBuffer,
            .len = pxDescriptor->xDataLength,
            .next = NULL
        };

        xTxConfig.pData = pxDescriptor;
        xTxConfig.TxBuffer = &xTxBuffer;
        xTxConfig.Length = xTxBuffer.len;

        /* TODO: Queue Tx Output? */
		/* if( xQueueSendToBack( xTxQueue, pxDescriptor, 0 ) != pdPASS )
		{
			xReleaseAfterSend = pdFALSE;
		} */

        if( xSemaphoreTake( xTxDescSem, pdMS_TO_TICKS( niEMAC_DESCRIPTOR_WAIT_TIME_MS ) ) == pdFALSE )
        {
            FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: No Descriptors Available\n" ) );
            break;
        }

        if( xSemaphoreTake( xTxMutex, pdMS_TO_TICKS( niEMAC_TX_MAX_BLOCK_TIME_MS ) ) == pdFALSE )
        {
            FreeRTOS_debug_printf( ( "xNetworkInterfaceOutput: Process Busy\n" ) );
            ( void ) xSemaphoreGive( xTxDescSem );
            break;
        }

        #if defined( niEMAC_CACHEABLE )
            if( niEMAC_CACHE_MAINTENANCE )
            {
                const uintptr_t uxDataStart = ( uintptr_t ) xTxBuffer.buffer;
                const uintptr_t uxLineStart = uxDataStart & ~niEMAC_DATA_ALIGNMENT_MASK;
                const ptrdiff_t uxDataOffset = uxDataStart - uxLineStart;
                const size_t uxLength = xTxBuffer.len + uxDataOffset;
                SCB_CleanDCache_by_Addr( ( uint32_t * ) uxLineStart, uxLength );
            }
        #endif

        if( HAL_ETH_Transmit_IT( pxEthHandle, &xTxConfig ) == HAL_OK )
        {
            /* Released later in deferred task by calling HAL_ETH_ReleaseTxPacket */
            xReleaseAfterSend = pdFALSE;
            xResult = pdPASS;
        }
        else
        {
            ( void ) xSemaphoreGive( xTxDescSem );
            configASSERT( pxEthHandle->gState == HAL_ETH_STATE_STARTED );
            /* Should be impossible if semaphores are correctly implemented */
            configASSERT( ( pxEthHandle->ErrorCode & HAL_ETH_ERROR_BUSY ) == 0 );

        }
        ( void ) xSemaphoreGive( xTxMutex );

    } while( pdFALSE );

    if( xReleaseAfterSend == pdTRUE )
    {
        prvReleaseNetworkBufferDescriptor( pxDescriptor );
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static void prvAddAllowedMACAddress( NetworkInterface_t * pxInterface, const uint8_t * pucMacAddress )
{
    ETH_HandleTypeDef * pxEthHandle = &xEthHandle;

    /* TODO: group address filtering with Mask Byte Control */
    BaseType_t xResult = prvAddDestMACAddrMatch( pxEthHandle->Instance, pucMacAddress );

    if( xResult == pdFALSE )
    {
        const uint8_t ucHashIndex = prvGetMacHashIndex( pucMacAddress );

        xResult = prvSetNewDestMACAddrMatch( pxEthHandle->Instance, ucHashIndex, pucMacAddress );

        if( xResult == pdFALSE )
        {
            prvAddDestMACAddrHash( pxEthHandle->Instance, ucHashIndex );
        }
    }
}

/*---------------------------------------------------------------------------*/

static void prvRemoveAllowedMACAddress( NetworkInterface_t * pxInterface, const uint8_t * pucMacAddress )
{
    ETH_HandleTypeDef * pxEthHandle = &xEthHandle;

    const BaseType_t xResult = prvRemoveDestMACAddrMatch( pxEthHandle->Instance, pucMacAddress );

    if( xResult == pdFALSE )
    {
        prvRemoveDestMACAddrHash( pxEthHandle->Instance, pucMacAddress );
    }
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                              EMAC Task                                    */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

static BaseType_t prvNetworkInterfaceInput( ETH_HandleTypeDef * pxEthHandle, NetworkInterface_t * pxInterface )
{
    BaseType_t xResult = pdFALSE;
    UBaseType_t uxCount = 0;
    #if ipconfigIS_ENABLED( ipconfigUSE_LINKED_RX_MESSAGES )
        NetworkBufferDescriptor_t * pxStartDescriptor = NULL;
        NetworkBufferDescriptor_t * pxEndDescriptor = NULL;
    #endif
    NetworkBufferDescriptor_t * pxCurDescriptor = NULL;
    if( ( xMacInitStatus == eMacInitComplete ) && ( pxEthHandle->gState == HAL_ETH_STATE_STARTED ) )
    {
        while( prvHAL_ETH_ReadData( pxEthHandle, ( void ** ) &pxCurDescriptor ) == pdPASS )
        {
            ++uxCount;
            if( pxCurDescriptor == NULL )
            {
                /* Buffer was dropped, ignore packet */
                continue;
            }
            configASSERT( pxEthHandle->RxDescList.RxDataLength <= niEMAC_DATA_BUFFER_SIZE );

            pxCurDescriptor->pxInterface = pxInterface;
            pxCurDescriptor->pxEndPoint = FreeRTOS_MatchingEndpoint( pxCurDescriptor->pxInterface, pxCurDescriptor->pucEthernetBuffer );
            #if ipconfigIS_ENABLED( ipconfigUSE_LINKED_RX_MESSAGES )
                if( pxStartDescriptor == NULL )
                {
                    pxStartDescriptor = pxCurDescriptor;
                }
                else if( pxEndDescriptor != NULL )
                {
                    pxEndDescriptor->pxNextBuffer = pxCurDescriptor;
                }
                pxEndDescriptor = pxCurDescriptor;
            #else
                prvSendRxEvent( pxCurDescriptor );
            #endif
        }
    }

    if( uxCount > 0 )
    {
        #if ipconfigIS_ENABLED( ipconfigUSE_LINKED_RX_MESSAGES )
            prvSendRxEvent( pxStartDescriptor );
        #endif
        xResult = pdTRUE;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static portTASK_FUNCTION( prvEMACHandlerTask, pvParameters )
{
    NetworkInterface_t * pxInterface = ( NetworkInterface_t * ) pvParameters;
    ETH_HandleTypeDef * pxEthHandle = &xEthHandle;
    EthernetPhy_t * pxPhyObject = &xPhyObject;

    /* iptraceEMAC_TASK_STARTING(); */

    for( ;; )
    {
        BaseType_t xResult = pdFALSE;
        uint32_t ulISREvents = 0U;

        if( xTaskNotifyWait( 0U, eMacEventAll, &ulISREvents, pdMS_TO_TICKS( niEMAC_TASK_MAX_BLOCK_TIME_MS ) ) == pdTRUE )
        {
            if( ( ulISREvents & eMacEventRx ) != 0 )
            {
                xResult = prvNetworkInterfaceInput( pxEthHandle, pxInterface );
            }

            if( ( ulISREvents & eMacEventTx ) != 0 )
            {
                prvReleaseTxPacket( pxEthHandle );
            }

            if( ( ulISREvents & eMacEventErrRx ) != 0 )
            {
                xResult = prvNetworkInterfaceInput( pxEthHandle, pxInterface );
            }

            if( ( ulISREvents & eMacEventErrTx ) != 0 )
            {
                prvReleaseTxPacket( pxEthHandle );
            }

            if( ( ulISREvents & eMacEventErrEth ) != 0 )
            {
                configASSERT( ( pxEthHandle->ErrorCode & HAL_ETH_ERROR_PARAM ) == 0 );
                if( pxEthHandle->gState == HAL_ETH_STATE_ERROR )
                {
                    /* Recover from critical error */
                    ( void ) HAL_ETH_Init( pxEthHandle );
                    ( void ) prvHAL_ETH_Start_IT( pxEthHandle );
                    xResult = prvNetworkInterfaceInput( pxEthHandle, pxInterface );
                }
            }

            /* if( ( ulISREvents & eMacEventErrMac ) != 0 ) */
            /* if( ( ulISREvents & eMacEventErrDma ) != 0 ) */
        }

        if( xPhyCheckLinkStatus( pxPhyObject, xResult ) != pdFALSE )
        {
            if( prvGetPhyLinkStatus( pxInterface ) != pdFALSE )
            {
                if( pxEthHandle->gState == HAL_ETH_STATE_ERROR )
                {
                    /* Recover from critical error */
                    ( void ) HAL_ETH_Init( pxEthHandle );
                }
                if( pxEthHandle->gState == HAL_ETH_STATE_READY )
                {
                    /* Link was down or critical error occurred */
                    if( prvMacUpdateConfig( pxEthHandle, pxPhyObject ) != pdFALSE )
                    {
                        ( void ) prvHAL_ETH_Start_IT( pxEthHandle );
                    }
                }
            }
            else
            {
                ( void ) prvHAL_ETH_Stop_IT( pxEthHandle );
                prvReleaseTxPacket( pxEthHandle );
                #if ( ipconfigIS_ENABLED( ipconfigSUPPORT_NETWORK_DOWN_EVENT ) )
                    FreeRTOS_NetworkDown( pxInterface );
                #endif
            }
        }
    }
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvEMACTaskStart( NetworkInterface_t * pxInterface )
{
    BaseType_t xResult = pdFALSE;

    if( xTxMutex == NULL )
    {
        #if ipconfigIS_ENABLED( configSUPPORT_STATIC_ALLOCATION )
            static StaticSemaphore_t xTxMutexBuf;
            xTxMutex = xSemaphoreCreateMutexStatic( &xTxMutexBuf );
        #else
            xTxMutex = xSemaphoreCreateMutex();
        #endif
        configASSERT( xTxMutex != NULL );
        vQueueAddToRegistry( xTxMutex, niEMAC_TX_MUTEX_NAME );
    }

    if( xTxDescSem == NULL )
    {
        #if ( ipconfigIS_ENABLED( configSUPPORT_STATIC_ALLOCATION ) )
            static StaticSemaphore_t xTxDescSemBuf;
            xTxDescSem = xSemaphoreCreateCountingStatic(
                ( UBaseType_t ) ETH_TX_DESC_CNT,
                ( UBaseType_t ) ETH_TX_DESC_CNT,
                &xTxDescSemBuf
            );
        #else
            xTxDescSem = xSemaphoreCreateCounting(
                ( UBaseType_t ) ETH_TX_DESC_CNT,
                ( UBaseType_t ) ETH_TX_DESC_CNT
            );
        #endif
        configASSERT( xTxDescSem != NULL );
        vQueueAddToRegistry( xTxDescSem, niEMAC_TX_DESC_SEM_NAME );
    }

    if( xEMACTaskHandle == NULL && ( xTxMutex != NULL ) && ( xTxDescSem != NULL ) )
    {
        #if ipconfigIS_ENABLED( configSUPPORT_STATIC_ALLOCATION )
            static StackType_t uxEMACTaskStack[ niEMAC_TASK_STACK_SIZE ];
            static StaticTask_t xEMACTaskTCB;
            xEMACTaskHandle = xTaskCreateStatic(
                prvEMACHandlerTask,
                niEMAC_TASK_NAME,
                niEMAC_TASK_STACK_SIZE,
                ( void * ) pxInterface,
                niEMAC_TASK_PRIORITY,
                uxEMACTaskStack,
                &xEMACTaskTCB
            );
        #else
            ( void ) xTaskCreate(
                prvEMACHandlerTask,
                niEMAC_TASK_NAME,
                niEMAC_TASK_STACK_SIZE,
                ( void * ) pxInterface,
                niEMAC_TASK_PRIORITY,
                &xEMACTaskHandle
            );
        #endif
    }

    if( xEMACTaskHandle != NULL )
    {
        xResult = pdTRUE;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                               EMAC Init                                   */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

static BaseType_t prvEthConfigInit( ETH_HandleTypeDef * pxEthHandle, NetworkInterface_t * pxInterface )
{
    BaseType_t xResult = pdFALSE;

    pxEthHandle->Instance = ETH;

    pxEthHandle->Init.MediaInterface = ipconfigIS_ENABLED( niEMAC_USE_RMII ) ? HAL_ETH_RMII_MODE : HAL_ETH_MII_MODE;

    pxEthHandle->Init.RxBuffLen = niEMAC_DATA_BUFFER_SIZE;
    /* configASSERT( pxEthHandle->Init.RxBuffLen <= ETH_MAX_PACKET_SIZE ); */
    configASSERT( pxEthHandle->Init.RxBuffLen % 4U == 0 );
    #if ( defined( niEMAC_STM32FX ) && defined( ETH_RX_BUF_SIZE ) )
        configASSERT( pxEthHandle->Init.RxBuffLen == ETH_RX_BUF_SIZE );
    #endif

    #ifdef niEMAC_STM32HX
        configASSERT( ETH_TX_DESC_CNT % 4 == 0 );
        configASSERT( ETH_RX_DESC_CNT % 4 == 0 );
    #endif
    static ETH_DMADescTypeDef xDMADescTx[ ETH_TX_DESC_CNT ] __ALIGNED( portBYTE_ALIGNMENT ) __attribute__( ( section( niEMAC_TX_DESC_SECTION ) ) );
    static ETH_DMADescTypeDef xDMADescRx[ ETH_RX_DESC_CNT ] __ALIGNED( portBYTE_ALIGNMENT ) __attribute__( ( section( niEMAC_RX_DESC_SECTION ) ) );
    pxEthHandle->Init.TxDesc = xDMADescTx;
    pxEthHandle->Init.RxDesc = xDMADescRx;
    ( void ) memset( &xDMADescTx, 0, sizeof( xDMADescTx ) );
    ( void ) memset( &xDMADescRx, 0, sizeof( xDMADescRx ) );

    const NetworkEndPoint_t * const pxEndPoint = FreeRTOS_FirstEndPoint( pxInterface );
    if( pxEndPoint != NULL )
    {
        /* ipLOCAL_MAC_ADDRESS */
        pxEthHandle->Init.MACAddr = ( uint8_t * ) pxEndPoint->xMACAddress.ucBytes;

        if( HAL_ETH_Init( pxEthHandle ) == HAL_OK )
        {
			#if defined( niEMAC_STM32FX )
				/* This function doesn't get called in Fxx driver */
				prvHAL_ETH_SetMDIOClockRange( pxEthHandle->Instance );
			#endif

            uint32_t ulReg = READ_REG( pxEthHandle->Instance->MACCR );
            #ifdef niEMAC_STM32FX
                #if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM )
                    SET_BIT( ulReg, ETH_MACCR_IPCO );
                #else
                    CLEAR_BIT( ulReg, ETH_MACCR_IPCO );
                #endif
                #ifdef ETH_MACCR_CSTF
                    CLEAR_BIT( ulReg, ETH_MACCR_CSTF );
                #endif
                SET_BIT( ulReg, ETH_MACCR_APCS );
                CLEAR_BIT( ulReg, ETH_MACCR_RD );
            #elif defined( niEMAC_STM32HX )
                #if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM )
                    SET_BIT( ulReg, ETH_MACCR_IPC );
                #else
                    CLEAR_BIT( ulReg, ETH_MACCR_IPC );
                #endif
                CLEAR_BIT( ulReg, ETH_MACCR_CST );
                SET_BIT( ulReg, ETH_MACCR_ACS );
                CLEAR_BIT( ulReg, ETH_MACCR_DR );
            #endif
            WRITE_REG( pxEthHandle->Instance->MACCR, ulReg );

            #if defined( niEMAC_STM32FX )
                #if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM ) || ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM )
                    SET_BIT( pxEthHandle->Instance->DMABMR, ETH_DMABMR_EDE );
                #else
                    CLEAR_BIT( pxEthHandle->Instance->DMABMR, ETH_DMABMR_EDE );
                #endif
                SET_BIT( pxEthHandle->Instance->DMAOMR, ETH_DMAOMR_OSF );
            #elif defined( niEMAC_STM32HX )
                SET_BIT( pxEthHandle->Instance->DMACTCR, ETH_DMACTCR_OSP );
                /* #if ipconfigIS_ENABLED( ipconfigUSE_TCP ) && ipconfigIS_ENABLED( niEMAC_TCP_SEGMENTATION )
                    SET_BIT( pxEthHandle->Instance->DMACTCR, ETH_DMACTCR_TSE );
                    MODIFY_REG( pxEthHandle->Instance->DMACCR, ETH_DMACCR_MSS, ipconfigTCP_MSS );
                #endif */
            #endif

            #if defined( niEMAC_STM32HX )
                prvInitPacketFilter( pxEthHandle->Instance, pxInterface );

                /* CLEAR_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_ARP;
                WRITE_REG( pxEthHandle->Instance->MACARPAR, ulSourceIPAddress );
                SET_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_ARP ); */
            #endif

            prvInitMacAddresses( pxEthHandle->Instance, pxInterface );

            xResult = pdTRUE;
        }
    }

    if( xResult == pdTRUE )
    {
        #if ( niEMAC_CACHEABLE != 0 )
            if( niEMAC_CACHE_ENABLED )
            {
                #if ( niEMAC_MPU != 0 )
                    configASSERT( _FLD2VAL( MPU_CTRL_ENABLE, MPU->CTRL ) != 0 );
                #else
                    configASSERT( pdFALSE );
                #endif
                /* _FLD2VAL( SCB_CCSIDR_LINESIZE, SCB->CCSIDR ) */
            }
        #endif

		#ifdef configPRIO_BITS
            const uint32_t ulPrioBits = configPRIO_BITS;
		#else
            const uint32_t ulPrioBits = __NVIC_PRIO_BITS;
		#endif
		const uint32_t ulPriority = NVIC_GetPriority( ETH_IRQn ) << ( 8U - ulPrioBits );
        if( ulPriority < configMAX_SYSCALL_INTERRUPT_PRIORITY )
        {
            FreeRTOS_debug_printf( ( "prvEthConfigInit: Incorrectly set ETH_IRQn priority\n" ) );
            NVIC_SetPriority( ETH_IRQn, configMAX_SYSCALL_INTERRUPT_PRIORITY >> ( 8U - ulPrioBits ) );
        }
        if( NVIC_GetEnableIRQ( ETH_IRQn ) == 0 )
        {
            FreeRTOS_debug_printf( ( "prvEthConfigInit: ETH_IRQn was not enabled by application\n" ) );
            HAL_NVIC_EnableIRQ( ETH_IRQn );
        }
    }
    else
    {
        #ifdef niEMAC_STM32FX
            configASSERT( __HAL_RCC_ETH_IS_CLK_ENABLED() != 0 );
        #elif defined( STM32H5 )
            configASSERT( __HAL_RCC_ETH_IS_CLK_ENABLED() != 0 );
            configASSERT( __HAL_RCC_ETHTX_IS_CLK_ENABLED() != 0 );
            configASSERT( __HAL_RCC_ETHRX_IS_CLK_ENABLED() != 0 );
        #elif defined( STM32H7)
            configASSERT( __HAL_RCC_ETH1MAC_IS_CLK_ENABLED() != 0 );
            configASSERT( __HAL_RCC_ETH1TX_IS_CLK_ENABLED() != 0 );
            configASSERT( __HAL_RCC_ETH1RX_IS_CLK_ENABLED() != 0 );
        #endif
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static void prvInitMacAddresses( ETH_TypeDef * const pxEthInstance, NetworkInterface_t * pxInterface )
{
    #ifdef niEMAC_STM32FX
        uint32_t ulReg = READ_REG( pxEthInstance->MACFFR );
        CLEAR_BIT( ulReg, ETH_MACFFR_RA );
        SET_BIT( ulReg, ETH_MACFFR_HPF );
        CLEAR_BIT( ulReg, ETH_MACFFR_SAF );
        CLEAR_BIT( ulReg, ETH_MACFFR_SAIF );
        CLEAR_BIT( ulReg, ETH_MACFFR_PCF );
        SET_BIT( ulReg, ETH_MACFFR_PCF_BlockAll );
        CLEAR_BIT( ulReg, ETH_MACFFR_BFD );
        CLEAR_BIT( ulReg, ETH_MACFFR_PAM );
        CLEAR_BIT( ulReg, ETH_MACFFR_DAIF );
        SET_BIT( ulReg, ETH_MACFFR_HM );
        SET_BIT( ulReg, ETH_MACFFR_HU );
        CLEAR_BIT( ulReg, ETH_MACFFR_PM );
        WRITE_REG( pxEthInstance->MACFFR, ulReg );
    #elif defined( niEMAC_STM32HX )
        uint32_t ulReg = READ_REG( pxEthInstance->MACPFR );
        CLEAR_BIT( ulReg, ETH_MACPFR_RA );
        CLEAR_BIT( ulReg, ETH_MACPFR_DNTU );
        SET_BIT( ulReg, ETH_MACPFR_HPF );
        CLEAR_BIT( ulReg, ETH_MACPFR_SAF );
        CLEAR_BIT( ulReg, ETH_MACPFR_SAIF );
        MODIFY_REG( ulReg, ETH_MACPFR_PCF, ETH_MACPFR_PCF_BLOCKALL );
        CLEAR_BIT( ulReg, ETH_MACPFR_DBF );
        CLEAR_BIT( ulReg, ETH_MACPFR_PM );
        CLEAR_BIT( ulReg, ETH_MACPFR_DAIF );
        SET_BIT( ulReg, ETH_MACPFR_HMC );
        SET_BIT( ulReg, ETH_MACPFR_HUC );
        CLEAR_BIT( ulReg, ETH_MACPFR_PR );
        WRITE_REG( pxEthInstance->MACPFR, ulReg );
    #endif

    NetworkEndPoint_t * pxEndPoint;
    for( pxEndPoint = FreeRTOS_FirstEndPoint( pxInterface ); pxEndPoint != NULL; pxEndPoint = FreeRTOS_NextEndPoint( pxInterface, pxEndPoint ) )
    {
        prvAddAllowedMACAddress( pxInterface, pxEndPoint->xMACAddress.ucBytes );
    }

    #if ipconfigIS_ENABLED( ipconfigUSE_IPv4 )
        #if ipconfigIS_ENABLED( ipconfigUSE_MDNS )
            prvAddAllowedMACAddress( xMDNS_MacAddress.ucBytes );
        #endif
        #if ipconfigIS_ENABLED( ipconfigUSE_LLMNR )
            prvAddAllowedMACAddress( xLLMNR_MacAddress.ucBytes );
        #endif
    #endif

    #if ipconfigIS_ENABLED( ipconfigUSE_IPv6 )
        prvAddAllowedMACAddress( pcLOCAL_ALL_NODES_MULTICAST_MAC.ucBytes );
        #if ipconfigIS_ENABLED( ipconfigUSE_MDNS )
            prvAddAllowedMACAddress( xMDNS_MACAddressIPv6.ucBytes );
        #endif
        #if ipconfigIS_ENABLED( ipconfigUSE_LLMNR )
            prvAddAllowedMACAddress( xLLMNR_MacAddressIPv6.ucBytes );
        #endif
    #endif
}

/*---------------------------------------------------------------------------*/

#ifdef niEMAC_STM32HX

static void prvInitPacketFilter( ETH_TypeDef * const pxEthInstance, const NetworkInterface_t * const pxInterface )
{
    CLEAR_BIT( pxEthInstance->MACPFR, ETH_MACPFR_IPFE );

    #if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM )
    {
        const uint8_t ucFilterCount = _FLD2VAL( ETH_MACHWF1R_L3L4FNUM, pxEthInstance->MACHWF1R );
        if( ucFilterCount > 0 )
        {
            /* "The Layer 3 and Layer 4 Packet Filter feature automatically selects the IPC Full Checksum
             *  Offload Engine on the Receive side. When this feature is enabled, you must set the IPC bit." */
            if( READ_BIT( pxEthInstance->MACCR, ETH_MACCR_IPC ) == 0 )
            {
                SET_BIT( pxEthInstance->MACCR, ETH_MACCR_IPC );
            }

            #if ipconfigIS_ENABLED( ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES )
            {
                /* Filter out all possibilities if frame type is disabled */
                #if ipconfigIS_DISABLED( ipconfigUSE_IPv4 )
                {
                    /* Block IPv4 if it is disabled */
                    uint32_t ulReg = READ_REG( pxEthInstance->MACL3L4C0R );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3PEN );
                    SET_BIT( ulReg, ETH_MACL3L4CR_L3SAM );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3SAIM );
                    SET_BIT( ulReg, ETH_MACL3L4CR_L3DAM );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3DAIM );
                    MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HSBM, 0x1FU );
                    MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HDBM, 0x1FU );
                    MODIFY_REG( pxEthInstance->MACL3A0R0R, ETH_MACL3A0R_L3A0, ipBROADCAST_IP_ADDRESS );
                    MODIFY_REG( pxEthInstance->MACL3A1R0R, ETH_MACL3A1R_L3A1, ipBROADCAST_IP_ADDRESS );
                    WRITE_REG( pxEthInstance->MACL3L4C0R, ulReg );
                }
                #endif /* if ipconfigIS_DISABLED( ipconfigUSE_IPv4 ) */

                #if ipconfigIS_DISABLED( ipconfigUSE_IPv6 )
                {
                    /* Block IPv6 if it is disabled */
                    uint32_t ulReg = READ_REG( pxEthInstance->MACL3L4C1R );
                    SET_BIT( ulReg, ETH_MACL3L4CR_L3PEN );
                    SET_BIT( ulReg, ETH_MACL3L4CR_L3SAM );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3SAIM );
                    SET_BIT( ulReg, ETH_MACL3L4CR_L3DAM );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3DAIM );
                    MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HSBM, 0x1FU );
                    MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HDBM, 0x1FU );
                    MODIFY_REG( pxEthInstance->MACL3A0R1R, ETH_MACL3A0R_L3A0, ipBROADCAST_IP_ADDRESS );
                    MODIFY_REG( pxEthInstance->MACL3A1R1R, ETH_MACL3A1R_L3A1, ipBROADCAST_IP_ADDRESS );
                    MODIFY_REG( pxEthInstance->MACL3A2R1R, ETH_MACL3A2R_L3A2, ipBROADCAST_IP_ADDRESS );
                    MODIFY_REG( pxEthInstance->MACL3A3R1R, ETH_MACL3A3R_L3A3, ipBROADCAST_IP_ADDRESS );
                    WRITE_REG( pxEthInstance->MACL3L4C1R, ulReg );
                }
                #endif /* if ipconfigIS_DISABLED( ipconfigUSE_IPv6 ) */

                for( NetworkEndPoint_t * pxEndPoint = FreeRTOS_FirstEndPoint( pxInterface ); pxEndPoint != NULL; pxEndPoint = FreeRTOS_NextEndPoint( pxInterface, pxEndPoint ) )
                {
                    #if ipconfigIS_ENABLED( ipconfigUSE_IPv4 )
                        if( ENDPOINT_IS_IPv4( pxEndPoint ) )
                        {
                            uint32_t ulReg = READ_REG( pxEthInstance->MACL3L4C0R );
                            CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3PEN );
                            SET_BIT( ulReg, ETH_MACL3L4CR_L3SAM );
                            CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3SAIM );
                            SET_BIT( ulReg, ETH_MACL3L4CR_L3DAM );
                            CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3DAIM );
                            MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HSBM, 0x1FU );
                            MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HDBM, 0x1FU );
                            MODIFY_REG( pxEthInstance->MACL3A0R0R, ETH_MACL3A0R_L3A0, ipBROADCAST_IP_ADDRESS );
                            MODIFY_REG( pxEthInstance->MACL3A1R0R, ETH_MACL3A1R_L3A1, pxEndPoint->ipv4_settings.ulIPAddress );
                            WRITE_REG( pxEthInstance->MACL3L4C0R, ulReg );
                        }
                    #endif /* if ipconfigIS_ENABLED( ipconfigUSE_IPv4 ) */
                    #if ipconfigIS_ENABLED( ipconfigUSE_IPv6 )
                        if( ENDPOINT_IS_IPv6( pxEndPoint ) )
                        {
                            uint32_t ulReg = READ_REG( pxEthInstance->MACL3L4C1R );
                            SET_BIT( ulReg, ETH_MACL3L4CR_L3PEN );
                            SET_BIT( ulReg, ETH_MACL3L4CR_L3SAM );
                            CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3SAIM );
                            SET_BIT( ulReg, ETH_MACL3L4CR_L3DAM );
                            CLEAR_BIT( ulReg, ETH_MACL3L4CR_L3DAIM );
                            MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HSBM, 0x1FU );
                            MODIFY_REG( ulReg, ETH_MACL3L4CR_L3HDBM, 0x1FU );
                            MODIFY_REG( pxEthInstance->MACL3A0R1R, ETH_MACL3A0R_L3A0, ipBROADCAST_IP_ADDRESS );
                            MODIFY_REG( pxEthInstance->MACL3A1R1R, ETH_MACL3A1R_L3A1, ipBROADCAST_IP_ADDRESS );
                            MODIFY_REG( pxEthInstance->MACL3A2R1R, ETH_MACL3A2R_L3A2, ipBROADCAST_IP_ADDRESS );
                            MODIFY_REG( pxEthInstance->MACL3A3R1R, ETH_MACL3A3R_L3A3, ipBROADCAST_IP_ADDRESS );
                            WRITE_REG( pxEthInstance->MACL3L4C1R, ulReg );
                        }
                    #endif /* if ipconfigIS_ENABLED( ipconfigUSE_IPv6 ) */
                }
            }
            #endif /* if ipconfigIS_ENABLED( ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES ) */

            #if ipconfigIS_ENABLED( ipconfigETHERNET_DRIVER_FILTERS_PACKETS )
            {
                /* TODO: Let user to block certain port numbers */
                /* TODO: Live updated in task based on active sockets? */

                /* Always allow all UDP */
                uint32_t ulReg = READ_REG( pxEthInstance->MACL4A0R );
                SET_BIT( ulReg, ETH_MACL3L4CR_L4PEN );
                CLEAR_BIT( ulReg, ETH_MACL3L4CR_L4SPM );
                CLEAR_BIT( ulReg, ETH_MACL3L4CR_L4SPIM );
                CLEAR_BIT( ulReg, ETH_MACL3L4CR_L4DPM );
                CLEAR_BIT( ulReg, ETH_MACL3L4CR_L4DPIM );
                MODIFY_REG( ulReg, ETH_MACL4AR_L4DP, 0U );
                MODIFY_REG( ulReg, ETH_MACL4AR_L4SP, 0U );
                WRITE_REG( pxEthInstance->MACL4A0R, ulReg );

                #if ipconfigIS_DISABLED( ipconfigUSE_TCP )
                    /* Block TCP if it is disabled */
                    uint32_t ulReg = READ_REG( pxEthInstance->MACL4A1R );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L4PEN );
                    SET_BIT( ulReg, ETH_MACL3L4CR_L4SPM );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L4SPIM );
                    SET_BIT( ulReg, ETH_MACL3L4CR_L4DPM );
                    CLEAR_BIT( ulReg, ETH_MACL3L4CR_L4DPIM );
                    MODIFY_REG( ulReg, ETH_MACL4AR_L4DP, 0xFFFFU );
                    MODIFY_REG( ulReg, ETH_MACL4AR_L4SP, 0xFFFFU );
                    WRITE_REG( pxEthInstance->MACL4A1R, ulReg );
                #endif /* if ipconfigIS_DISABLED( ipconfigUSE_TCP ) */
            }
            #endif /* if ipconfigIS_ENABLED( ipconfigETHERNET_DRIVER_FILTERS_PACKETS ) */

            SET_BIT( pxEthInstance->MACPFR, ETH_MACPFR_IPFE );
        }
    }
    #endif /* if ipconfigIS_ENABLED( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM ) */
}

#endif /* ifdef niEMAC_STM32HX */

/*---------------------------------------------------------------------------*/

static BaseType_t prvPhyInit( ETH_TypeDef * const pxEthInstance, EthernetPhy_t * pxPhyObject )
{
	BaseType_t xResult = pdFAIL;

	vPhyInitialise( pxPhyObject, ( xApplicationPhyReadHook_t ) prvPhyReadReg, ( xApplicationPhyWriteHook_t ) prvPhyWriteReg );

	if( xPhyDiscover( pxPhyObject ) != 0 )
	{
        #if defined( niEMAC_STM32HX ) && ipconfigIS_ENABLED( niEMAC_USE_PHY_INT )
    		for( size_t uxCount = 0; uxCount < pxPhyObject->xPortCount; ++uxCount )
    		{
    			if( pxPhyObject->ulPhyIDs[ uxCount ] == PHY_ID_LAN8742A )
    			{
    				xLan8742aObject.DevAddr = pxPhyObject->ucPhyIndexes[ uxCount ];
    				lan8742_IOCtx_t xLan8742aControl = {
    					.Init = NULL,
    					.DeInit = NULL,
    					.WriteReg = ( lan8742_WriteReg_Func ) &prvPhyWriteReg,
    					.ReadReg = ( lan8742_ReadReg_Func ) &prvPhyReadReg,
                        #if ( INCLUDE_vTaskDelay == 1 )
    					   .GetTick = ( lan8742_GetTick_Func ) &vTaskDelay,
                        #else
                           .GetTick = ( lan8742_GetTick_Func ) &HAL_GetTick,
                        #endif
    				};
    				( void ) LAN8742_RegisterBusIO( &xLan8742aObject, &xLan8742aControl );
                    xLan8742aObject.Is_Initialized = 1U;
    				if( LAN8742_EnableIT( &xLan8742aObject, LAN8742_LINK_DOWN_IT ) == LAN8742_STATUS_OK )
    				{
                        pxEthInstance->MACIER |= ETH_MACIER_PHYIE;
    				}
    				break;
    			}
    			else if( pxPhyObject->ulPhyIDs[ uxCount ] == PHY_ID_DP83848I )
    			{
    				xDp83848Object.DevAddr = pxPhyObject->ucPhyIndexes[ uxCount ];
    				dp83848_IOCtx_t xDp83848Control = {
    					.Init = NULL,
    					.DeInit = NULL,
    					.WriteReg = ( dp83848_WriteReg_Func ) &prvPhyWriteReg,
    					.ReadReg = ( dp83848_ReadReg_Func ) &prvPhyReadReg,
                        #if ( INCLUDE_vTaskDelay == 1 )
    					   .GetTick = ( dp83848_GetTick_Func ) &vTaskDelay,
                        #else
                           .GetTick = ( dp83848_GetTick_Func ) &HAL_GetTick,
                        #endif
    				};
    				( void ) DP83848_RegisterBusIO( &xDp83848Object, &xDp83848Control );
                    xDp83848Object.Is_Initialized = 1U;
    				if( DP83848_EnableIT( &xDp83848Object, DP83848_LINK_DOWN_IT ) == DP83848_STATUS_OK )
    				{
                        pxEthInstance->MACIER |= ETH_MACIER_PHYIE;
    				}
    				break;
    			}
    		}
        #endif /* if defined( niEMAC_STM32HX ) && ipconfigIS_ENABLED( niEMAC_USE_PHY_INT ) */

		xResult = pdPASS;
	}

	return xResult;
}

static BaseType_t prvPhyStart( ETH_HandleTypeDef * pxEthHandle, NetworkInterface_t * pxInterface, EthernetPhy_t * pxPhyObject )
{
    BaseType_t xResult = pdFALSE;

    if( prvGetPhyLinkStatus( pxInterface ) == pdFALSE )
    {
        const PhyProperties_t xPhyProperties = {
            #if ipconfigIS_ENABLED( niEMAC_AUTO_NEGOTIATION )
                .ucSpeed = PHY_SPEED_AUTO,
                .ucDuplex = PHY_DUPLEX_AUTO,
            #else
                .ucSpeed = ipconfigIS_ENABLED( niEMAC_USE_100MB ) ? PHY_SPEED_100 : PHY_SPEED_10,
                .ucDuplex = ipconfigIS_ENABLED( niEMAC_USE_FULL_DUPLEX ) ? PHY_DUPLEX_FULL : PHY_DUPLEX_HALF,
            #endif

            #if ipconfigIS_ENABLED( niEMAC_AUTO_CROSS )
                .ucMDI_X = PHY_MDIX_AUTO,
            #elif ipconfigIS_ENABLED( niEMAC_CROSSED_LINK )
                .ucMDI_X = PHY_MDIX_CROSSED,
            #else
                .ucMDI_X = PHY_MDIX_DIRECT,
            #endif
        };

        #if ipconfigIS_DISABLED( niEMAC_AUTO_NEGOTIATION )
            pxPhyObject->xPhyPreferences.ucSpeed = xPhyProperties.ucSpeed;
            pxPhyObject->xPhyPreferences.ucDuplex = xPhyProperties.ucDuplex;
        #endif

        if( xPhyConfigure( pxPhyObject, &xPhyProperties ) == 0 )
        {
            if( prvMacUpdateConfig( pxEthHandle, pxPhyObject ) != pdFALSE )
            {
                xResult = pdTRUE;
            }
        }
    }
    else
    {
        xResult = pdTRUE;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                           MAC Filtering Helpers                           */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

/* Compute the CRC32 of the given MAC address as per IEEE 802.3 CRC32 */
static uint32_t prvCalcCrc32( const uint8_t * const pucMACAddr )
{
    uint32_t ulCRC32 = 0xFFFFFFFFU;

    uint32_t ucIndex;
    for( ucIndex = ipMAC_ADDRESS_LENGTH_BYTES; ucIndex > 0; --ucIndex )
    {
        ulCRC32 ^= __RBIT( pucMACAddr[ ipMAC_ADDRESS_LENGTH_BYTES - ucIndex ] );

        uint8_t ucJndex;
        for( ucJndex = 8; ucJndex > 0; --ucJndex )
        {
            if( ulCRC32 & 0x80000000U )
            {
                ulCRC32 <<= 1;
                ulCRC32 ^= niEMAC_CRC_POLY;
            }
            else
            {
                ulCRC32 <<= 1;
            }
        }
    }

    return ~ulCRC32;
}

/*---------------------------------------------------------------------------*/

static uint8_t prvGetMacHashIndex( const uint8_t * const pucMACAddr )
{
    const uint32_t ulHash = prvCalcCrc32( pucMACAddr );
    const uint8_t ucHashIndex = ( ulHash >> 26 ) & 0x3FU;

    return ucHashIndex;
}

/*---------------------------------------------------------------------------*/

/* Needed since HAL Driver only provides source matching */
static void prvHAL_ETH_SetDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, uint8_t ucIndex, const uint8_t * const pucMACAddr )
{
    configASSERT( ucIndex < niEMAC_MAC_MATCH_COUNT );
    const uint32_t ulMacAddrHigh = ( pucMACAddr[ 5 ] << 8 ) | ( pucMACAddr[ 4 ] );
    const uint32_t ulMacAddrLow = ( pucMACAddr[ 3 ] << 24 ) | ( pucMACAddr[ 2 ] << 16 ) | ( pucMACAddr[ 1 ] << 8 ) | ( pucMACAddr[ 0 ] );

    /* MACA0HR/MACA0LR reserved for the primary MAC-address. */
    const uint32_t ulMacRegHigh = ( ( uint32_t ) &( pxEthInstance->MACA1HR ) + ( 8 * ucIndex ) );
    const uint32_t ulMacRegLow = ( ( uint32_t ) &( pxEthInstance->MACA1LR ) + ( 8 * ucIndex ) );
    WRITE_REG( * ( __IO uint32_t * ) ulMacRegHigh , ETH_MACA1HR_AE | ulMacAddrHigh );
    WRITE_REG( * ( __IO uint32_t * ) ulMacRegLow, ulMacAddrLow );
}

/*---------------------------------------------------------------------------*/

static void prvHAL_ETH_ClearDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, uint8_t ucIndex )
{
    configASSERT( ucIndex < niEMAC_MAC_MATCH_COUNT );
    const uint32_t ulMacRegHigh = ( ( uint32_t ) &( pxEthInstance->MACA1HR ) + ( 8 * ucIndex ) );
    const uint32_t ulMacRegLow = ( ( uint32_t ) &( pxEthInstance->MACA1LR ) + ( 8 * ucIndex ) );
    CLEAR_REG( * ( __IO uint32_t * ) ulMacRegHigh );
    CLEAR_REG( * ( __IO uint32_t * ) ulMacRegLow );
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvAddDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, const uint8_t * const pucMACAddr )
{
    BaseType_t xResult = pdFALSE;

    uint8_t ucIndex;
    for( ucIndex = 0; ucIndex < niEMAC_MAC_MATCH_COUNT; ++ucIndex )
    {
        if( ucSrcMatchCounters[ ucIndex ] > 0U )
        {
            /* ETH_MACA1HR_MBC - Group Address Filtering */
            const uint32_t ulMacRegHigh = READ_REG( ( uint32_t ) &( pxEthInstance->MACA1HR ) + ( 8 * ucIndex ) );
            const uint32_t ulMacRegLow = READ_REG( ( uint32_t ) &( pxEthInstance->MACA1LR ) + ( 8 * ucIndex ) );

            const uint32_t ulMacAddrHigh = ( pucMACAddr[ 5 ] << 8 ) | ( pucMACAddr[ 4 ] );
            const uint32_t ulMacAddrLow = ( pucMACAddr[ 3 ] << 24 ) | ( pucMACAddr[ 2 ] << 16 ) | ( pucMACAddr[ 1 ] << 8 ) | ( pucMACAddr[ 0 ] );

            if( ( ulMacRegHigh == ulMacAddrHigh ) && ( ulMacRegLow == ulMacAddrLow ) )
            {
                if( ucSrcMatchCounters[ ucIndex ] < UINT8_MAX )
                {
                    ++( ucSrcMatchCounters[ ucIndex ] );
                }
                xResult = pdTRUE;
                break;
            }
        }
        else if( uxMACEntryIndex > niEMAC_MAC_MATCH_COUNT )
        {
            uxMACEntryIndex = niEMAC_MAC_MATCH_COUNT;
        }
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvRemoveDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, const uint8_t * const pucMACAddr )
{
    BaseType_t xResult = pdFALSE;

    uint8_t ucIndex;
    for( ucIndex = 0; ucIndex < niEMAC_MAC_MATCH_COUNT; ++ucIndex )
    {
        if( ucSrcMatchCounters[ ucIndex ] > 0U )
        {
            /* ETH_MACA1HR_MBC - Group Address Filtering */
            const uint32_t ulMacRegHigh = READ_REG( ( uint32_t ) &( pxEthInstance->MACA1HR ) + ( 8 * ucIndex ) );
            const uint32_t ulMacRegLow = READ_REG( ( uint32_t ) &( pxEthInstance->MACA1LR ) + ( 8 * ucIndex ) );

            const uint32_t ulMacAddrHigh = ( pucMACAddr[ 5 ] << 8 ) | ( pucMACAddr[ 4 ] );
            const uint32_t ulMacAddrLow = ( pucMACAddr[ 3 ] << 24 ) | ( pucMACAddr[ 2 ] << 16 ) | ( pucMACAddr[ 1 ] << 8 ) | ( pucMACAddr[ 0 ] );

            if( ( ulMacRegHigh == ulMacAddrHigh ) && ( ulMacRegLow == ulMacAddrLow ) )
            {
                if( ucSrcMatchCounters[ ucIndex ] < UINT8_MAX )
                {
                    if( --( ucSrcMatchCounters[ ucIndex ] ) == 0 )
                    {
                        prvHAL_ETH_ClearDestMACAddrMatch( pxEthInstance, ucIndex );
                    }
                }

                xResult = pdTRUE;
                break;
            }
        }
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvSetNewDestMACAddrMatch( ETH_TypeDef * const pxEthInstance, uint8_t ucHashIndex, const uint8_t * const pucMACAddr )
{
    BaseType_t xResult = pdFALSE;

    if( uxMACEntryIndex < niEMAC_MAC_MATCH_COUNT )
    {
        if( ucAddrHashCounters[ ucHashIndex ] == 0U )
        {
            prvHAL_ETH_SetDestMACAddrMatch( pxEthInstance, uxMACEntryIndex, pucMACAddr );
            ucSrcMatchCounters[ uxMACEntryIndex ] = 1U;
            xResult = pdTRUE;
        }
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static void prvAddDestMACAddrHash( ETH_TypeDef * const pxEthInstance, uint8_t ucHashIndex )
{
    if( ucAddrHashCounters[ ucHashIndex ] == 0 )
    {
        if( ucHashIndex & 0x20U )
        {
            ulHashTable[ 1 ] |= ( 1U << ( ucHashIndex & 0x1FU ) );
        }
        else
        {
            ulHashTable[ 0 ] |= ( 1U << ucHashIndex );
        }

        prvHAL_ETH_SetHashTable( pxEthInstance );
    }

    if( ucAddrHashCounters[ ucHashIndex ] < UINT8_MAX )
    {
        ++( ucAddrHashCounters[ ucHashIndex ] );
    }
}

/*---------------------------------------------------------------------------*/

static void prvRemoveDestMACAddrHash( ETH_TypeDef * const pxEthInstance, const uint8_t * const pucMACAddr )
{
    const uint8_t ucHashIndex = prvGetMacHashIndex( pucMACAddr );

    if( ucAddrHashCounters[ ucHashIndex ] > 0U )
    {
        if( ucAddrHashCounters[ ucHashIndex ] < UINT8_MAX )
        {
            if( --( ucAddrHashCounters[ ucHashIndex ] ) == 0 )
            {
                if( ucHashIndex & 0x20U )
                {
                    ulHashTable[ 1 ] &= ~( 1U << ( ucHashIndex & 0x1FU ) );
                }
                else
                {
                    ulHashTable[ 0 ] &= ~( 1U << ucHashIndex );
                }

                prvHAL_ETH_SetHashTable( pxEthInstance );
            }
        }
    }
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                              EMAC Helpers                                 */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

static void prvReleaseTxPacket( ETH_HandleTypeDef * pxEthHandle )
{
    if( xSemaphoreTake( xTxMutex, pdMS_TO_TICKS( niEMAC_TX_MAX_BLOCK_TIME_MS ) ) != pdFALSE )
    {
        prvHAL_ETH_ReleaseTxPacket( pxEthHandle );
        ( void ) xSemaphoreGive( xTxMutex );
    }
    else
    {
        FreeRTOS_debug_printf( ( "prvReleaseTxPacket: Failed\n" ) );
    }

    /* TODO: Is it possible for the semaphore and BuffersInUse to get out of sync? */
    /* while( ETH_TX_DESC_CNT - uxQueueMessagesWaiting( ( QueueHandle_t ) xTxDescSem ) > pxEthHandle->TxDescList.BuffersInUse )
    {
        ( void ) xSemaphoreGive( xTxDescSem );
    } */
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvMacUpdateConfig( ETH_HandleTypeDef * pxEthHandle, EthernetPhy_t * pxPhyObject )
{
    BaseType_t xResult = pdFALSE;

    if( pxEthHandle->gState == HAL_ETH_STATE_STARTED )
    {
        ( void ) prvHAL_ETH_Stop_IT( pxEthHandle );
    }

    #if ipconfigIS_ENABLED( niEMAC_AUTO_NEGOTIATION )
        ( void ) xPhyStartAutoNegotiation( pxPhyObject, xPhyGetMask( pxPhyObject ) );
    #else
        ( void ) xPhyFixedValue( pxPhyObject, xPhyGetMask( pxPhyObject ) );
    #endif

    if( pxPhyObject->xPhyProperties.ucDuplex == PHY_DUPLEX_FULL )
    {
        SET_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_DM );
    }
    else
    {
        CLEAR_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_DM );
    }

    if( pxPhyObject->xPhyProperties.ucSpeed == PHY_SPEED_100 )
    {
        SET_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_FES );
    }
    else
    {
        CLEAR_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_FES );
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static void prvReleaseNetworkBufferDescriptor( NetworkBufferDescriptor_t * const pxDescriptor )
{
    NetworkBufferDescriptor_t * pxDescriptorToClear = pxDescriptor;
    while( pxDescriptorToClear != NULL )
    {
        #if ipconfigIS_ENABLED( ipconfigUSE_LINKED_RX_MESSAGES )
            NetworkBufferDescriptor_t * const pxNext = pxDescriptorToClear->pxNextBuffer;
        #else
            NetworkBufferDescriptor_t * const pxNext = NULL;
        #endif
        vReleaseNetworkBufferAndDescriptor( pxDescriptorToClear );
        pxDescriptorToClear = pxNext;
    };
}

/*---------------------------------------------------------------------------*/

static void prvSendRxEvent( NetworkBufferDescriptor_t * const pxDescriptor )
{
    const IPStackEvent_t xRxEvent = {
        .eEventType = eNetworkRxEvent,
        .pvData = ( void * ) pxDescriptor
    };
    if( xSendEventStructToIPTask( &xRxEvent, pdMS_TO_TICKS( niEMAC_RX_MAX_BLOCK_TIME_MS ) ) != pdPASS )
    {
        iptraceETHERNET_RX_EVENT_LOST();
        FreeRTOS_debug_printf( ( "prvSendRxEvent: xSendEventStructToIPTask failed\n" ) );
        prvReleaseNetworkBufferDescriptor( pxDescriptor );
    }
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvAcceptPacket( const NetworkBufferDescriptor_t * const pxDescriptor, uint16_t usLength )
{
    BaseType_t xResult = pdFALSE;
    do
    {
        if( pxDescriptor == NULL )
        {
            iptraceETHERNET_RX_EVENT_LOST();
            FreeRTOS_debug_printf( ( "prvAcceptPacket: Null Descriptor\n" ) );
            break;
        }

        if( usLength > pxDescriptor->xDataLength )
		{
			iptraceETHERNET_RX_EVENT_LOST();
			FreeRTOS_debug_printf( ( "prvAcceptPacket: Packet size overflow\n" ) );
			break;
		}

        ETH_HandleTypeDef * pxEthHandle = &xEthHandle;

        const ETH_DMADescTypeDef * const ulRxDesc = ( const ETH_DMADescTypeDef * const ) pxEthHandle->RxDescList.RxDesc[ pxEthHandle->RxDescList.RxDescIdx ];

        #ifdef niEMAC_STM32FX
            if( READ_BIT( ulRxDesc->DESC0, ETH_DMARXDESC_ES ) != 0 )
        #elif defined( niEMAC_STM32HX )
            if( READ_BIT( ulRxDesc->DESC3, ETH_DMARXNDESCWBF_ES ) != 0 )
        #endif
        {
			#if ipconfigIS_ENABLED( niEMAC_DEBUG_ERROR )
        		prvHAL_RX_ErrorCallback( pxEthHandle );
			#endif
            iptraceETHERNET_RX_EVENT_LOST();
            FreeRTOS_debug_printf( ( "prvAcceptPacket: Rx Data Error\n" ) );
            break;
        }

		#if ipconfigIS_ENABLED( ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES )
			if( eConsiderFrameForProcessing( pxDescriptor->pucEthernetBuffer ) != eProcessBuffer )
			{
				iptraceETHERNET_RX_EVENT_LOST();
				FreeRTOS_debug_printf( ( "prvAcceptPacket: Frame discarded\n" ) );
				break;
			}
		#endif

		#if ipconfigIS_ENABLED( ipconfigETHERNET_DRIVER_FILTERS_PACKETS )
			#ifdef niEMAC_STM32HX
				const uint32_t ulRxDesc1 = ulRxDesc->DESC1;

                if( ( ulRxDesc1 & ETH_CHECKSUM_IP_PAYLOAD_ERROR ) != 0 )
                {
                    iptraceETHERNET_RX_EVENT_LOST();
                    break;
                }

                if( ( ulRxDesc1 & ETH_CHECKSUM_IP_HEADER_ERROR ) != 0 )
                {
                    iptraceETHERNET_RX_EVENT_LOST();
                    break;
                }

				if( ( ulRxDesc1 & ETH_IP_HEADER_IPV4 ) != 0 )
				{
					/* Should be impossible if hardware filtering is implemented correctly */
					configASSERT( ipconfigIS_ENABLED( ipconfigUSE_IPv4 ) );
					#if ipconfigIS_ENABLED( ipconfigUSE_IPv4 )
					   /* prvAllowIPPacketIPv4(); */
					#endif
				}
				else if( ( ulRxDesc1 & ETH_IP_HEADER_IPV6 ) != 0 )
				{
					/* Should be impossible if hardware filtering is implemented correctly */
					configASSERT( ipconfigIS_ENABLED( ipconfigUSE_IPv6 ) );
					#if ipconfigIS_ENABLED( ipconfigUSE_IPv6 )
						/* prvAllowIPPacketIPv6(); */
					#endif
				}

				const uint32_t ulPacketType = ulRxDesc1 & ETH_DMARXNDESCWBF_PT;
				if( ulPacketType == ETH_IP_PAYLOAD_UNKNOWN )
				{
					iptraceETHERNET_RX_EVENT_LOST();
					break;
				}
				else if( ulPacketType == ETH_IP_PAYLOAD_UDP )
				{
					/* prvProcessUDPPacket(); */
				}
				else if( ulPacketType == ETH_IP_PAYLOAD_ICMPN )
				{
					#if ipconfigIS_DISABLED( ipconfigREPLY_TO_INCOMING_PINGS ) && ipconfigIS_DISABLED( ipconfigSUPPORT_OUTGOING_PINGS )
						iptraceETHERNET_RX_EVENT_LOST();
						break;
					#else
						/* ProcessICMPPacket(); */
					#endif
				}
				else if( ulPacketType == ETH_IP_PAYLOAD_TCP )
				{
					/* Should be impossible if hardware filtering is implemented correctly */
					configASSERT( ipconfigIS_ENABLED( ipconfigUSE_TCP ) );
					#if ipconfigIS_ENABLED( ipconfigUSE_TCP )
						/* xProcessReceivedTCPPacket() */
					#endif
				}
			#endif

			/* TODO: Create a eConsiderPacketForProcessing */
            if( eConsiderPacketForProcessing( pxDescriptor->pucEthernetBuffer ) != eProcessBuffer )
            {
            	iptraceETHERNET_RX_EVENT_LOST();
                FreeRTOS_debug_printf( ( "prvAcceptPacket: Packet discarded\n" ) );
                break;
            }
        #endif

        xResult = pdTRUE;

    } while( pdFALSE );

    return xResult;
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                              IRQ Handlers                                 */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

void ETH_IRQHandler( void )
{
    traceISR_ENTER();

    ETH_HandleTypeDef * pxEthHandle = &xEthHandle;

    xSwitchRequired = pdFALSE;

    #ifdef niEMAC_STM32FX
    	if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_NIS ) )
		{
            if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_NISE ) )
            {
    			if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_RS ) )
    			{
    				if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_RIE ) )
    				{
    					__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_RS );
    					HAL_ETH_RxCpltCallback( pxEthHandle );
    				}
    			}

    			if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_TS ) )
    			{
    				if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_TIE ) )
    				{
    					__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_TS );
    					HAL_ETH_TxCpltCallback( pxEthHandle );
    				}
    			}

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_ERS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_ERIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_ERS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_TBUS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_TBUIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_TBUS );
                    }
                }

    			__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_NIS );
            }
		}

    	if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_AIS ) )
		{
			if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_AISE ) )
			{
				if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_FBES ) )
				{
					if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_FBEIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_FBES );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_TPS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_TPSIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_TPS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_RPS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_RPSIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_RPS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_RBUS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_RBUIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_RBUS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_TUS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_TUIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_TUS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_RWTS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_RWTIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_RWTS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_ETS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_ETIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_ETS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_ROS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_ROIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_ROS );
                    }
                }

                if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMASR_TJTS ) )
                {
                    if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMAIER_TJTIE ) )
                    {
                        __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_TJTS );
                    }
                }

				__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMASR_AIS );
			}
		}
    #elif defined( niEMAC_STM32HX )
        if( READ_BIT( pxEthHandle->Instance->DMAISR, ETH_DMAISR_MTLIS ) != 0 )
        {

        }

        if( READ_BIT( pxEthHandle->Instance->DMAISR, ETH_DMAISR_DMACIS ) != 0 )
        {
            /* ETH_DMACSR_REB */
            /* ETH_DMACSR_TEB */
            if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_NIS ) )
            {
                if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_NIE ) )
                {
                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_RI ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_RIE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_RI );
                            HAL_ETH_RxCpltCallback( pxEthHandle );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_TI ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_TIE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_TI );
                            HAL_ETH_TxCpltCallback( pxEthHandle );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_ERI ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_ERIE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_ERI );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_TBU ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_TBUE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_TBU );
                        }
                    }

                    __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_NIS );
                }
            }

            if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_AIS ) )
            {
                if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_AIE ) )
                {
                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_FBE ) )
                    {
                    	if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_FBEE ) )
						{
							__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_FBE );
							/* prvHAL_MAC_ErrorCallback( pxEthHandle ); */
						}
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_RWT ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_RWTE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_RWT );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_ETI ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_ETIE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_ETI );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_TPS ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_TXSE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_TPS );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_RPS ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_RSE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_RPS );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_RBU ) )
                    {
                        if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_RBUE ) )
                        {
                            __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_RBU );
                        }
                    }

                    if( __HAL_ETH_DMA_GET_IT( pxEthHandle, ETH_DMACSR_CDE ) )
					{
						if( __HAL_ETH_DMA_GET_IT_SOURCE( pxEthHandle, ETH_DMACIER_CDEE ) )
						{
							__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_CDE );
						}
					}

                    __HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_DMACSR_AIS );
                }
            }
        }

        if( ( pxEthHandle->Instance->DMAISR & ETH_DMAISR_MACIS ) != 0 )
        {
            if( __HAL_ETH_MAC_GET_IT( pxEthHandle, ETH_MACISR_RXSTSIS ) )
            {
                if( READ_BIT( pxEthHandle->Instance->MACIER, ETH_MACIER_RXSTSIE ) != 0 )
                {
                	__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_MACISR_RXSTSIS );
                    /* prvHAL_MAC_ErrorCallback( pxEthHandle ); */
                }
            }

            if( __HAL_ETH_MAC_GET_IT( pxEthHandle, ETH_MACISR_TXSTSIS ) )
            {
                if( READ_BIT( pxEthHandle->Instance->MACIER, ETH_MACIER_TXSTSIE ) != 0 )
                {
                	__HAL_ETH_DMA_CLEAR_IT( pxEthHandle, ETH_MACISR_TXSTSIS );
                    /* prvHAL_MAC_ErrorCallback( pxEthHandle ); */
                }
            }

            #if ipconfigIS_ENABLED( niEMAC_USE_PHY_INT )
                if( __HAL_ETH_MAC_GET_IT( pxEthHandle, ETH_MACISR_PHYIS ) )
                {
                    if( READ_BIT( pxEthHandle->Instance->MACIER, ETH_MACIER_PHYIE ) != 0 )
                    {
                        if( xLan8742aObject.Is_Initialized != 0 )
                        {
                            if( LAN8742_GetITStatus( &xLan8742aObject, LAN8742_LINK_DOWN_IT ) )
                            {
                                LAN8742_ClearIT( &xLan8742aObject, LAN8742_LINK_DOWN_IT );
                            }
                        }
                        if( xDp83848Object.Is_Initialized != 0 )
                        {
                            if( DP83848_GetITStatus( &xDp83848Object, DP83848_LINK_DOWN_IT ) )
                            {
                                DP83848_ClearIT( &xDp83848Object, DP83848_LINK_DOWN_IT );
                            }
                        }
                    }
                }
            #endif /* if defined( niEMAC_STM32HX ) && ipconfigIS_ENABLED( niEMAC_USE_PHY_INT ) */
        }
    #endif

    portYIELD_FROM_ISR( xSwitchRequired );
}

/*---------------------------------------------------------------------------*/

void HAL_ETH_ErrorCallback( ETH_HandleTypeDef * pxEthHandle )
{
    eMAC_IF_EVENT eErrorEvents = eMacEventNone;

    if( pxEthHandle->gState == HAL_ETH_STATE_ERROR )
    {
        /* Fatal bus error occurred */
        eErrorEvents |= eMacEventErrEth;
    }

    if( ( pxEthHandle->ErrorCode & HAL_ETH_ERROR_DMA ) != 0 )
    {
        eErrorEvents |= eMacEventErrDma;
        const uint32_t ulDmaError = pxEthHandle->DMAErrorCode;
		#if ipconfigIS_ENABLED( niEMAC_DEBUG_ERROR )
        	prvHAL_DMA_ErrorCallback( pxEthHandle );
		#endif
		if( ( ulDmaError & ETH_DMA_TX_BUFFER_UNAVAILABLE_FLAG ) != 0 )
		{
			eErrorEvents |= eMacEventErrTx;
		}

		if( ( ulDmaError & ETH_DMA_RX_BUFFER_UNAVAILABLE_FLAG ) != 0 )
		{
			eErrorEvents |= eMacEventErrRx;
		}
    }

    if( ( pxEthHandle->ErrorCode & HAL_ETH_ERROR_MAC ) != 0 )
    {
        eErrorEvents |= eMacEventErrMac;
		#if ipconfigIS_ENABLED( niEMAC_DEBUG_ERROR )
        	prvHAL_MAC_ErrorCallback( pxEthHandle );
		#endif
    }

    if( ( xEMACTaskHandle != NULL ) && ( eErrorEvents != eMacEventNone ) )
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        ( void ) xTaskNotifyFromISR( xEMACTaskHandle, eErrorEvents, eSetBits, &xHigherPriorityTaskWoken );
        xSwitchRequired |= xHigherPriorityTaskWoken;
    }
}

/*---------------------------------------------------------------------------*/

void HAL_ETH_RxCpltCallback( ETH_HandleTypeDef * pxEthHandle )
{
    static size_t uxMostRXDescsUsed = 0U;

    const size_t uxRxUsed = pxEthHandle->RxDescList.RxDescCnt;

    if( uxMostRXDescsUsed < uxRxUsed )
    {
        uxMostRXDescsUsed = uxRxUsed;
    }

    iptraceNETWORK_INTERFACE_RECEIVE();

    if( xEMACTaskHandle != NULL )
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        ( void ) xTaskNotifyFromISR( xEMACTaskHandle, eMacEventRx, eSetBits, &xHigherPriorityTaskWoken );
        xSwitchRequired |= xHigherPriorityTaskWoken;
    }
}

/*---------------------------------------------------------------------------*/

void HAL_ETH_TxCpltCallback( ETH_HandleTypeDef * pxEthHandle )
{
    static size_t uxMostTXDescsUsed = 0U;

    const size_t uxTxUsed = pxEthHandle->TxDescList.BuffersInUse;

    if( uxMostTXDescsUsed < uxTxUsed )
    {
        uxMostTXDescsUsed = uxTxUsed;
    }

    iptraceNETWORK_INTERFACE_TRANSMIT();

    if( xEMACTaskHandle != NULL )
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        ( void ) xTaskNotifyFromISR( xEMACTaskHandle, eMacEventTx, eSetBits, &xHigherPriorityTaskWoken );
        xSwitchRequired |= xHigherPriorityTaskWoken;
    }
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                            HAL Tx/Rx Callbacks                            */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

void HAL_ETH_RxAllocateCallback( uint8_t ** ppucBuff )
{
    const NetworkBufferDescriptor_t * pxBufferDescriptor = pxGetNetworkBufferWithDescriptor( niEMAC_DATA_BUFFER_SIZE, pdMS_TO_TICKS( niEMAC_DESCRIPTOR_WAIT_TIME_MS ) );
    if( pxBufferDescriptor != NULL )
    {
        #ifdef niEMAC_CACHEABLE
            if( niEMAC_CACHE_MAINTENANCE )
            {
                SCB_InvalidateDCache_by_Addr( ( uint32_t * ) pxBufferDescriptor->pucEthernetBuffer, pxBufferDescriptor->xDataLength );
            }
        #endif
        *ppucBuff = pxBufferDescriptor->pucEthernetBuffer;
    }
    else
    {
        FreeRTOS_debug_printf( ( "HAL_ETH_RxAllocateCallback: failed\n" ) );
    }
}

/*---------------------------------------------------------------------------*/

void HAL_ETH_RxLinkCallback( void ** ppvStart, void ** ppvEnd, uint8_t * pucBuff, uint16_t usLength )
{
    NetworkBufferDescriptor_t ** const ppxStartDescriptor = ( NetworkBufferDescriptor_t ** ) ppvStart;
    NetworkBufferDescriptor_t ** const ppxEndDescriptor = ( NetworkBufferDescriptor_t ** ) ppvEnd;
    NetworkBufferDescriptor_t * const pxCurDescriptor = pxPacketBuffer_to_NetworkBuffer( ( const void * ) pucBuff );
    if( prvAcceptPacket( pxCurDescriptor, usLength ) == pdTRUE )
    {
        pxCurDescriptor->xDataLength = usLength;
        #if ipconfigIS_ENABLED( ipconfigUSE_LINKED_RX_MESSAGES )
            pxCurDescriptor->pxNextBuffer = NULL;
        #endif
        if( *ppxStartDescriptor == NULL )
        {
            *ppxStartDescriptor = pxCurDescriptor;
        }
        #if ipconfigIS_ENABLED( ipconfigUSE_LINKED_RX_MESSAGES )
            else if( ppxEndDescriptor != NULL )
            {
                ( *ppxEndDescriptor )->pxNextBuffer = pxCurDescriptor;
            }
        #endif
        *ppxEndDescriptor = pxCurDescriptor;
        /* Only single buffer packets are supported */
        configASSERT( *ppxStartDescriptor == *ppxEndDescriptor );
        #ifdef niEMAC_CACHEABLE
            if( niEMAC_CACHE_MAINTENANCE )
            {
                SCB_InvalidateDCache_by_Addr( ( uint32_t * ) pucBuff, usLength );
            }
        #endif
    }
    else
    {
        FreeRTOS_debug_printf( ( "HAL_ETH_RxLinkCallback: Buffer Dropped\n" ) );
        prvReleaseNetworkBufferDescriptor( pxCurDescriptor );
    }
}

/*---------------------------------------------------------------------------*/

void HAL_ETH_TxFreeCallback( uint32_t * pulBuff )
{
    NetworkBufferDescriptor_t * const pxNetworkBuffer = ( NetworkBufferDescriptor_t * ) pulBuff;
    prvReleaseNetworkBufferDescriptor( pxNetworkBuffer );
    ( void ) xSemaphoreGive( xTxDescSem );
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                           Buffer Allocation                               */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

void vNetworkInterfaceAllocateRAMToBuffers( NetworkBufferDescriptor_t pxNetworkBuffers[ ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS ] )
{
    static uint8_t ucNetworkPackets[ ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS ][ niEMAC_TOTAL_BUFFER_SIZE ] __ALIGNED( niEMAC_BUF_ALIGNMENT ) __attribute__( ( section( niEMAC_BUFFERS_SECTION ) ) );

    configASSERT( xBufferAllocFixedSize == pdTRUE );

    size_t uxIndex;
    for( uxIndex = 0; uxIndex < ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS; ++uxIndex )
    {
        pxNetworkBuffers[ uxIndex ].pucEthernetBuffer = &( ucNetworkPackets[ uxIndex ][ ipBUFFER_PADDING ] );
        *( ( uint32_t * ) &( ucNetworkPackets[ uxIndex ][ 0 ] ) ) = ( uint32_t ) ( &( pxNetworkBuffers[ uxIndex ] ) );
    }
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                      Network Interface Definition                         */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

NetworkInterface_t * pxSTM32_FillInterfaceDescriptor( BaseType_t xEMACIndex, NetworkInterface_t * pxInterface )
{
    static char pcName[ 17 ];

    ( void ) snprintf( pcName, sizeof( pcName ), "eth%u", ( unsigned ) xEMACIndex );

    ( void ) memset( pxInterface, '\0', sizeof( *pxInterface ) );
    pxInterface->pcName = pcName;
    /* TODO: use pvArgument to get xEMACData? */
    /* xEMACData.xEMACIndex = xEMACIndex; */
    /* pxInterface->pvArgument = ( void * ) &xEMACData; */
    /* pxInterface->pvArgument = pvPortMalloc( sizeof( EMACData_t ) ); */
    pxInterface->pvArgument = ( void * ) xEMACIndex;
    pxInterface->pfInitialise = prvNetworkInterfaceInitialise;
    pxInterface->pfOutput = prvNetworkInterfaceOutput;
    pxInterface->pfGetPhyLinkStatus = prvGetPhyLinkStatus;
    /* pxInterface->pfAddAllowedMAC = prvAddAllowedMACAddress;
    pxInterface->pfRemoveAllowedMAC = prvRemoveAllowedMACAddress; */

    return FreeRTOS_AddNetworkInterface( pxInterface );
}

/*---------------------------------------------------------------------------*/

#if ipconfigIS_ENABLED( ipconfigIPv4_BACKWARD_COMPATIBLE )

/* Do not call the following function directly. It is there for downward compatibility.
 * The function FreeRTOS_IPInit() will call it to initialice the interface and end-point
 * objects.  See the description in FreeRTOS_Routing.h. */
    NetworkInterface_t * pxFillInterfaceDescriptor( BaseType_t xEMACIndex, NetworkInterface_t * pxInterface )
    {
        pxSTM32_FillInterfaceDescriptor( xEMACIndex, pxInterface );
    }

#endif

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                        Reimplemented HAL Functions                        */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

static BaseType_t prvHAL_ETH_Start_IT( ETH_HandleTypeDef * const pxEthHandle )
{
    BaseType_t xResult = pdFAIL;

    if( pxEthHandle->gState == HAL_ETH_STATE_READY )
    {
        pxEthHandle->gState = HAL_ETH_STATE_BUSY;

        pxEthHandle->RxDescList.ItMode = 1U;
        #ifdef niEMAC_STM32FX
            SET_BIT( pxEthHandle->Instance->MACIMR, ETH_MACIMR_TSTIM | ETH_MACIMR_PMTIM );
            SET_BIT( pxEthHandle->Instance->MMCRIMR, ETH_MMCRIMR_RGUFM | ETH_MMCRIMR_RFAEM | ETH_MMCRIMR_RFCEM );
            SET_BIT( pxEthHandle->Instance->MMCTIMR, ETH_MMCTIMR_TGFM | ETH_MMCTIMR_TGFMSCM | ETH_MMCTIMR_TGFSCM );
        #elif defined( niEMAC_STM32HX )
            SET_BIT( pxEthHandle->Instance->MMCRIMR, ETH_MMCRIMR_RXLPITRCIM | ETH_MMCRIMR_RXLPIUSCIM | ETH_MMCRIMR_RXUCGPIM | ETH_MMCRIMR_RXALGNERPIM | ETH_MMCRIMR_RXCRCERPIM );
            SET_BIT( pxEthHandle->Instance->MMCTIMR, ETH_MMCTIMR_TXLPITRCIM | ETH_MMCTIMR_TXLPIUSCIM | ETH_MMCTIMR_TXGPKTIM | ETH_MMCTIMR_TXMCOLGPIM | ETH_MMCTIMR_TXSCOLGPIM );
        #endif

        pxEthHandle->RxDescList.RxBuildDescCnt = ETH_RX_DESC_CNT;

        prvETH_UpdateDescriptor( pxEthHandle );

        SET_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_TE | ETH_MACCR_RE );

        #ifdef niEMAC_STM32FX
            SET_BIT( pxEthHandle->Instance->DMAOMR, ETH_DMAOMR_FTF | ETH_DMAOMR_ST | ETH_DMAOMR_SR );
            CLEAR_BIT( pxEthHandle->Instance->DMASR, ( ETH_DMASR_TPS | ETH_DMASR_RPS ) );
            __HAL_ETH_DMA_ENABLE_IT( pxEthHandle, ( ETH_DMAIER_NISE | ETH_DMAIER_RIE | ETH_DMAIER_TIE | ETH_DMAIER_FBEIE | ETH_DMAIER_AISE | ETH_DMAIER_RBUIE ) );
        #elif defined( niEMAC_STM32HX )
            SET_BIT( pxEthHandle->Instance->MTLTQOMR, ETH_MTLTQOMR_FTQ );
            SET_BIT( pxEthHandle->Instance->DMACTCR, ETH_DMACTCR_ST );
            SET_BIT( pxEthHandle->Instance->DMACRCR, ETH_DMACRCR_SR );
            CLEAR_BIT( pxEthHandle->Instance->DMACSR, ( ETH_DMACSR_TPS | ETH_DMACSR_RPS ) );
            __HAL_ETH_DMA_ENABLE_IT( pxEthHandle, ( ETH_DMACIER_NIE | ETH_DMACIER_RIE | ETH_DMACIER_TIE | ETH_DMACIER_FBEE | ETH_DMACIER_AIE | ETH_DMACIER_RBUE ) );
        #endif

        pxEthHandle->gState = HAL_ETH_STATE_STARTED;
    }

    if( pxEthHandle->gState == HAL_ETH_STATE_STARTED )
    {
    	xResult = pdPASS;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvHAL_ETH_Stop_IT( ETH_HandleTypeDef * const pxEthHandle )
{
    BaseType_t xResult = pdFAIL;

    if( pxEthHandle->gState == HAL_ETH_STATE_STARTED )
    {
        pxEthHandle->gState = HAL_ETH_STATE_BUSY;

        #ifdef niEMAC_STM32FX
            __HAL_ETH_DMA_DISABLE_IT( pxEthHandle, ( ETH_DMAIER_NISE | ETH_DMAIER_RIE | ETH_DMAIER_TIE | ETH_DMAIER_FBEIE | ETH_DMAIER_AISE | ETH_DMAIER_RBUIE ) );
            CLEAR_BIT( pxEthHandle->Instance->DMAOMR, ETH_DMAOMR_ST );
            CLEAR_BIT( pxEthHandle->Instance->DMAOMR, ETH_DMAOMR_SR );
            CLEAR_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_RE );
            SET_BIT( pxEthHandle->Instance->DMAOMR, ETH_DMAOMR_FTF );
            CLEAR_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_TE );
        #elif defined( niEMAC_STM32HX )
            __HAL_ETH_DMA_DISABLE_IT( pxEthHandle, ( ETH_DMACIER_NIE | ETH_DMACIER_RIE | ETH_DMACIER_TIE | ETH_DMACIER_FBEE | ETH_DMACIER_AIE | ETH_DMACIER_RBUE ) );
            CLEAR_BIT( pxEthHandle->Instance->DMACTCR, ETH_DMACTCR_ST );
            CLEAR_BIT( pxEthHandle->Instance->DMACRCR, ETH_DMACRCR_SR );
            CLEAR_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_RE );
            SET_BIT( pxEthHandle->Instance->MTLTQOMR, ETH_MTLTQOMR_FTQ );
            CLEAR_BIT( pxEthHandle->Instance->MACCR, ETH_MACCR_TE );
        #endif

        uint8_t descindex;
        for( descindex = 0; descindex < ETH_RX_DESC_CNT; ++descindex )
        {
            ETH_DMADescTypeDef * const dmarxdesc = ( ETH_DMADescTypeDef * const ) pxEthHandle->RxDescList.RxDesc[ descindex ];
            #ifdef niEMAC_STM32FX
                dmarxdesc->DESC1 |= ETH_DMARXDESC_DIC;
            #elif defined( niEMAC_STM32HX )
                dmarxdesc->DESC3 &= ~ETH_DMARXNDESCRF_IOC;
            #endif
        }

        pxEthHandle->RxDescList.ItMode = 0U;

        pxEthHandle->gState = HAL_ETH_STATE_READY;

        xResult = pdPASS;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvHAL_ETH_ReadData( ETH_HandleTypeDef * const pxEthHandle, void ** ppvBuff )
{
    configASSERT( ppvBuff != NULL );
    configASSERT( pxEthHandle->gState == HAL_ETH_STATE_STARTED );
    
    BaseType_t xResult = pdFAIL;

    uint8_t rxdataready = 0U;
    ETH_RxDescListTypeDef * const dmarxdesclist = &( pxEthHandle->RxDescList );
    uint32_t descidx = dmarxdesclist->RxDescIdx;
    ETH_DMADescTypeDef * dmarxdesc = ( ETH_DMADescTypeDef * ) dmarxdesclist->RxDesc[ descidx ];
    const uint32_t desccntmax = ETH_RX_DESC_CNT - dmarxdesclist->RxBuildDescCnt;
    uint32_t desccnt = 0U;

    #ifdef niEMAC_STM32FX
        while( ( READ_BIT( dmarxdesc->DESC0, ETH_DMARXDESC_OWN ) == 0U ) && ( desccnt < desccntmax ) && ( rxdataready == 0U ) )
    #elif defined( niEMAC_STM32HX )
        while( ( READ_BIT( dmarxdesc->DESC3, ETH_DMARXNDESCWBF_OWN ) == 0U ) && ( desccnt < desccntmax ) && ( rxdataready == 0U ) )
    #endif
    {
        #ifdef niEMAC_STM32FX
            const BaseType_t xFirstDescriptor = ( READ_BIT( dmarxdesc->DESC0, ETH_DMARXDESC_FS ) != 0U );
        #elif defined( niEMAC_STM32HX )
            const BaseType_t xFirstDescriptor = ( READ_BIT( dmarxdesc->DESC3, ETH_DMARXNDESCWBF_FD ) != 0U );
        #endif
        if( ( xFirstDescriptor == pdTRUE ) || ( dmarxdesclist->pRxStart != NULL ) )
        {
            if( xFirstDescriptor == pdTRUE )
            {
                dmarxdesclist->RxDescCnt = 0;
                dmarxdesclist->RxDataLength = 0;
            }

            uint32_t bufflength = pxEthHandle->Init.RxBuffLen;
            #ifdef niEMAC_STM32FX
                if( READ_BIT( dmarxdesc->DESC0, ETH_DMARXDESC_LS ) != 0U )
                {
                    bufflength = ( READ_BIT( dmarxdesc->DESC0, ETH_DMARXDESC_FL ) >> 16U ) - 4U;
                    dmarxdesclist->pRxLastRxDesc = dmarxdesc->DESC0;
                    rxdataready = 1;
                }
            #elif defined( niEMAC_STM32HX )
                if( READ_BIT( dmarxdesc->DESC3, ETH_DMARXNDESCWBF_LD ) != 0U )
                {
                    bufflength = READ_BIT( dmarxdesc->DESC3, ETH_DMARXNDESCWBF_PL ) - dmarxdesclist->RxDataLength;
                    dmarxdesclist->pRxLastRxDesc = dmarxdesc->DESC3;
                    rxdataready = 1;
                }
            #endif

            #ifdef niEMAC_STM32FX
                WRITE_REG( dmarxdesc->BackupAddr0, dmarxdesc->DESC2 );
            #endif
            HAL_ETH_RxLinkCallback( &dmarxdesclist->pRxStart, &dmarxdesclist->pRxEnd, ( uint8_t * ) dmarxdesc->BackupAddr0, ( uint16_t ) bufflength );
            ++( dmarxdesclist->RxDescCnt );
            dmarxdesclist->RxDataLength += bufflength;

            dmarxdesc->BackupAddr0 = 0;
        }

        INCR_RX_DESC_INDEX( descidx, 1U );
        dmarxdesc = ( ETH_DMADescTypeDef * ) dmarxdesclist->RxDesc[ descidx ];
        ++desccnt;
    }

    dmarxdesclist->RxBuildDescCnt += desccnt;
    if( dmarxdesclist->RxBuildDescCnt != 0U )
    {
        prvETH_UpdateDescriptor( pxEthHandle );
    }

    dmarxdesclist->RxDescIdx = descidx;

    if( rxdataready == 1U )
    {
        *ppvBuff = dmarxdesclist->pRxStart;
        dmarxdesclist->pRxStart = NULL;
        xResult = pdPASS;
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static void prvHAL_ETH_ReleaseTxPacket( ETH_HandleTypeDef * const pxEthHandle )
{
    ETH_TxDescListTypeDef * const dmatxdesclist = &( pxEthHandle->TxDescList );
    uint32_t numOfBuf = dmatxdesclist->BuffersInUse;
    uint32_t idx = dmatxdesclist->releaseIndex;
    uint8_t pktTxStatus = 1U;

    while( ( numOfBuf != 0U ) && ( pktTxStatus != 0U ) )
    {
        uint8_t pktInUse = 1U;
        --numOfBuf;
        if( dmatxdesclist->PacketAddress[ idx ] == NULL )
        {
            idx = ( idx + 1U ) & ( ETH_TX_DESC_CNT - 1U );
            pktInUse = 0U;
        }

        if( pktInUse != 0U )
        {
            #ifdef niEMAC_STM32FX
                if( ( pxEthHandle->Init.TxDesc[ idx ].DESC0 & ETH_DMATXDESC_OWN ) == 0U )
            #elif defined( niEMAC_STM32HX )
                 if( ( pxEthHandle->Init.TxDesc[ idx ].DESC3 & ETH_DMATXNDESCRF_OWN ) == 0U )
            #endif
            {
                HAL_ETH_TxFreeCallback( dmatxdesclist->PacketAddress[ idx ] );
                dmatxdesclist->PacketAddress[ idx ] = NULL;
                idx = ( idx + 1U ) & ( ETH_TX_DESC_CNT - 1U );
                dmatxdesclist->BuffersInUse = numOfBuf;
                dmatxdesclist->releaseIndex = idx;
            }
            else
            {
                pktTxStatus = 0U;
            }
        }
    }
}

/*---------------------------------------------------------------------------*/

static void prvETH_UpdateDescriptor( ETH_HandleTypeDef * const pxEthHandle )
{
    uint8_t allocStatus = 1U;
    ETH_RxDescListTypeDef * pxRxDescList = &pxEthHandle->RxDescList;
    uint32_t descidx = pxRxDescList->RxBuildDescIdx;
    uint32_t desccount = pxRxDescList->RxBuildDescCnt;
    ETH_DMADescTypeDef * dmarxdesc = ( ETH_DMADescTypeDef * ) pxRxDescList->RxDesc[ descidx ];

    while( ( desccount > 0U ) && ( allocStatus != 0U ) )
    {
        if( READ_REG( dmarxdesc->BackupAddr0 ) == 0U )
        {
            uint8_t * buff = NULL;
            HAL_ETH_RxAllocateCallback( &buff );
            if( buff == NULL )
            {
                allocStatus = 0U;
            }
            else
            {
                WRITE_REG( dmarxdesc->BackupAddr0, ( uint32_t ) buff );
                #ifdef niEMAC_STM32FX
                    WRITE_REG( dmarxdesc->DESC2, ( uint32_t ) buff );
                #elif defined( niEMAC_STM32HX )
                    WRITE_REG( dmarxdesc->DESC0, ( uint32_t ) buff );
                #endif
            }
        }

        if( allocStatus != 0U )
        {
            if( pxRxDescList->ItMode == 0U )
            {
                #ifdef niEMAC_STM32FX
                    WRITE_REG( dmarxdesc->DESC1, ETH_DMARXDESC_DIC | pxEthHandle->Init.RxBuffLen | ETH_DMARXDESC_RCH );
                #elif defined( niEMAC_STM32HX )
                    WRITE_REG( dmarxdesc->DESC3, ETH_DMARXNDESCRF_OWN | ETH_DMARXNDESCRF_BUF1V );
                #endif
            }
            else
            {
                #ifdef niEMAC_STM32FX
                    WRITE_REG( dmarxdesc->DESC1, pxEthHandle->Init.RxBuffLen | ETH_DMARXDESC_RCH );
                #elif defined( niEMAC_STM32HX )
                    WRITE_REG( dmarxdesc->DESC3, ETH_DMARXNDESCRF_OWN | ETH_DMARXNDESCRF_BUF1V | ETH_DMARXNDESCRF_IOC );
                #endif
            }

            #ifdef niEMAC_STM32FX
                /* __DMB(); */
                SET_BIT( dmarxdesc->DESC0, ETH_DMARXDESC_OWN );
            #endif

            INCR_RX_DESC_INDEX( descidx, 1U );
            dmarxdesc = ( ETH_DMADescTypeDef * ) pxRxDescList->RxDesc[ descidx ];
            --desccount;
        }
    }

    if( pxRxDescList->RxBuildDescCnt != desccount )
    {
        #ifdef niEMAC_STM32FX
            CLEAR_REG( pxEthHandle->Instance->DMARPDR );
        #elif defined( niEMAC_STM32HX )
            CLEAR_REG( pxEthHandle->Instance->DMACRDTPR );
        #endif
        pxRxDescList->RxBuildDescIdx = descidx;
        pxRxDescList->RxBuildDescCnt = desccount;
    }
}

/*---------------------------------------------------------------------------*/

static void prvHAL_ETH_SetMDIOClockRange( ETH_TypeDef * const pxEthInstance )
{
    const uint32_t ulHClk = HAL_RCC_GetHCLKFreq();

    if( ( ulHClk >= 20000000U ) && ( ulHClk < 35000000U ) )
    {
        #ifdef niEMAC_STM32FX
            MODIFY_REG( pxEthInstance->MACMIIAR, ETH_MACMIIAR_CR, ETH_MACMIIAR_CR_Div16 );
        #elif defined( niEMAC_STM32HX )
            MODIFY_REG( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_CR, ETH_MACMDIOAR_CR_DIV16 );
        #endif
    }
    else if( ( ulHClk >= 35000000U ) && ( ulHClk < 60000000U ) )
    {
        #ifdef niEMAC_STM32FX
            MODIFY_REG( pxEthInstance->MACMIIAR, ETH_MACMIIAR_CR, ETH_MACMIIAR_CR_Div26 );
        #elif defined( niEMAC_STM32HX )
            MODIFY_REG( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_CR, ETH_MACMDIOAR_CR_DIV26 );
        #endif
    }
    else if( ( ulHClk >= 60000000U ) && ( ulHClk < 100000000U ) )
    {
        #ifdef niEMAC_STM32FX
            MODIFY_REG( pxEthInstance->MACMIIAR, ETH_MACMIIAR_CR, ETH_MACMIIAR_CR_Div42 );
        #elif defined( niEMAC_STM32HX )
            MODIFY_REG( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_CR, ETH_MACMDIOAR_CR_DIV42 );
        #endif
    }
    else if( ( ulHClk >= 100000000U ) && ( ulHClk < 150000000U ) )
    {
        #ifdef niEMAC_STM32FX
            MODIFY_REG( pxEthInstance->MACMIIAR, ETH_MACMIIAR_CR, ETH_MACMIIAR_CR_Div62 );
        #elif defined( niEMAC_STM32HX )
            MODIFY_REG( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_CR, ETH_MACMDIOAR_CR_DIV62 );
        #endif
    }
    else if(  ( ulHClk >= 150000000U ) && ( ulHClk <= 250000000U ) )
    {
        #ifdef niEMAC_STM32FX
            MODIFY_REG( pxEthInstance->MACMIIAR, ETH_MACMIIAR_CR, ETH_MACMIIAR_CR_Div102 );
        #elif defined( niEMAC_STM32HX )
            MODIFY_REG( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_CR, ETH_MACMDIOAR_CR_DIV102 );
        #endif
    }
    else
    {
        #ifdef niEMAC_STM32HX
            MODIFY_REG( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_CR, ETH_MACMDIOAR_CR_DIV124 );
        #endif
    }
}

/*---------------------------------------------------------------------------*/

static void prvHAL_ETH_SetHashTable( ETH_TypeDef * const pxEthInstance )
{
    #ifdef niEMAC_STM32FX
        WRITE_REG( pxEthInstance->MACHTHR, ulHashTable[ 0 ] );
        WRITE_REG( pxEthInstance->MACHTLR, ulHashTable[ 1 ] );
    #elif defined( niEMAC_STM32HX )
        WRITE_REG( pxEthInstance->MACHT0R, ulHashTable[ 0 ] );
        WRITE_REG( pxEthInstance->MACHT1R, ulHashTable[ 1 ] );
    #endif
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvHAL_ETH_ReadPHYRegister( ETH_TypeDef * const pxEthInstance, uint32_t ulPhyAddr, uint32_t ulPhyReg, uint32_t * pulRegValue )
{
    BaseType_t xResult = pdPASS;

    #ifdef niEMAC_STM32FX
        uint32_t ulReg = READ_REG( pxEthInstance->MACMIIAR );
    #elif defined( niEMAC_STM32HX )
        uint32_t ulReg = READ_REG( pxEthInstance->MACMDIOAR );
    #endif

    #ifdef niEMAC_STM32FX
        if( READ_BIT( ulReg, ETH_MACMIIAR_MB ) != 0 )
    #elif defined( niEMAC_STM32HX )
        if( READ_BIT( ulReg, ETH_MACMDIOAR_MB ) != 0 )
    #endif
    {
        xResult = pdFAIL;
    }
    else
    {
        #ifdef niEMAC_STM32FX
            MODIFY_REG( ulReg, ETH_MACMIIAR_PA, _VAL2FLD( ETH_MACMIIAR_PA, ulPhyAddr ) );
            MODIFY_REG( ulReg, ETH_MACMIIAR_MR, _VAL2FLD( ETH_MACMIIAR_MR, ulPhyReg ) );
            CLEAR_BIT( ulReg, ETH_MACMIIAR_MW );
            SET_BIT( ulReg, ETH_MACMIIAR_MB );
            WRITE_REG( pxEthInstance->MACMIIAR, ulReg);
        #elif defined( niEMAC_STM32HX )
            MODIFY_REG( ulReg, ETH_MACMDIOAR_PA, _VAL2FLD( ETH_MACMDIOAR_PA, ulPhyAddr ) );
            MODIFY_REG( ulReg, ETH_MACMDIOAR_RDA, _VAL2FLD( ETH_MACMDIOAR_RDA, ulPhyReg ) );
            MODIFY_REG( ulReg, ETH_MACMDIOAR_MOC, ETH_MACMDIOAR_MOC_WR );
            SET_BIT( ulReg, ETH_MACMDIOAR_MB);
            WRITE_REG( pxEthInstance->MACMDIOAR, ulReg );
        #endif

        TimeOut_t xPhyWriteTimer;
        vTaskSetTimeOutState( &xPhyWriteTimer );
        TickType_t xPhyWriteRemaining = pdMS_TO_TICKS( 2U );
        #ifdef niEMAC_STM32FX
            while( READ_BIT( pxEthInstance->MACMIIAR, ETH_MACMIIAR_MB ) != 0 )
        #elif defined( niEMAC_STM32HX )
            while( READ_BIT( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_MB ) != 0 )
        #endif
        {
            if( xTaskCheckForTimeOut( &xPhyWriteTimer, &xPhyWriteRemaining ) != pdFALSE )
            {
                xResult = pdFAIL;
                break;
            }
            else
            {
                vTaskDelay( pdMS_TO_TICKS( 1 ) );
            }
        }

        #ifdef niEMAC_STM32FX
            WRITE_REG( *pulRegValue, _FLD2VAL( ETH_MACMIIDR_MD, pxEthInstance->MACMIIDR ) );
        #elif defined( niEMAC_STM32HX )
            WRITE_REG( *pulRegValue, _FLD2VAL( ETH_MACMDIODR_MD, pxEthInstance->MACMDIODR ) );
        #endif
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/

static BaseType_t prvHAL_ETH_WritePHYRegister( ETH_TypeDef * const pxEthInstance, uint32_t ulPhyAddr, uint32_t ulPhyReg, uint32_t ulRegValue )
{
    BaseType_t xResult = pdPASS;

    #ifdef niEMAC_STM32FX
        uint32_t ulReg = READ_REG( pxEthInstance->MACMIIAR );
    #elif defined( niEMAC_STM32HX )
        uint32_t ulReg = READ_REG( pxEthInstance->MACMDIOAR );
    #endif

    #ifdef niEMAC_STM32FX
        if( READ_BIT( ulReg, ETH_MACMIIAR_MB ) != 0 )
    #elif defined( niEMAC_STM32HX )
        if( READ_BIT( ulReg, ETH_MACMDIOAR_MB ) != 0 )
    #endif
    {
        xResult = pdFAIL;
    }
    else
    {
        #ifdef niEMAC_STM32FX
            MODIFY_REG( ulReg, ETH_MACMIIAR_PA, _VAL2FLD( ETH_MACMIIAR_PA, ulPhyAddr ) );
            MODIFY_REG( ulReg, ETH_MACMIIAR_MR, _VAL2FLD( ETH_MACMIIAR_MR, ulPhyReg ) );
            SET_BIT( ulReg, ETH_MACMIIAR_MW );
            SET_BIT( ulReg, ETH_MACMIIAR_MB );
            WRITE_REG( pxEthInstance->MACMIIDR, _VAL2FLD( ETH_MACMIIDR_MD, ulRegValue ) );
            WRITE_REG( pxEthInstance->MACMIIAR, ulReg);
        #elif defined( niEMAC_STM32HX )
            MODIFY_REG( ulReg, ETH_MACMDIOAR_PA, _VAL2FLD( ETH_MACMDIOAR_PA, ulPhyAddr ) );
            MODIFY_REG( ulReg, ETH_MACMDIOAR_RDA, _VAL2FLD( ETH_MACMDIOAR_RDA, ulPhyReg ) );
            MODIFY_REG( ulReg, ETH_MACMDIOAR_MOC, ETH_MACMDIOAR_MOC_WR );
            SET_BIT( ulReg, ETH_MACMDIOAR_MB);
            WRITE_REG( pxEthInstance->MACMDIODR, _VAL2FLD( ETH_MACMDIODR_MD, ulRegValue ) );
            WRITE_REG( pxEthInstance->MACMDIOAR, ulReg );
        #endif

        TimeOut_t xPhyWriteTimer;
        vTaskSetTimeOutState( &xPhyWriteTimer );
        TickType_t xPhyWriteRemaining = pdMS_TO_TICKS( 2U );
        #ifdef niEMAC_STM32FX
            while( READ_BIT( pxEthInstance->MACMIIAR, ETH_MACMIIAR_MB ) != 0 )
        #elif defined( niEMAC_STM32HX )
            while( READ_BIT( pxEthInstance->MACMDIOAR, ETH_MACMDIOAR_MB ) != 0 )
        #endif
        {
            if( xTaskCheckForTimeOut( &xPhyWriteTimer, &xPhyWriteRemaining ) != pdFALSE )
            {
                xResult = pdFAIL;
                break;
            }
            else
            {
                vTaskDelay( pdMS_TO_TICKS( 1 ) );
            }
        }
    }

    return xResult;
}

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                      		Debugging                         			 */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

#if ipconfigIS_ENABLED( niEMAC_DEBUG_ERROR )

static void prvHAL_RX_ErrorCallback( ETH_HandleTypeDef * pxEthHandle )
{
	uint32_t ulErrorCode = 0;
	( void ) HAL_ETH_GetRxDataErrorCode( pxEthHandle, &ulErrorCode );
    if( ( pxEthHandle->Init.MediaInterface == HAL_ETH_MII_MODE ) && ( ( ulErrorCode & ETH_DRIBBLE_BIT_ERROR ) != 0 ) )
    {
    	static size_t uxRxDBError = 0;
    	++uxRxDBError;
    }
    if( ( ulErrorCode & ETH_RECEIVE_ERROR ) != 0 )
    {
    	static size_t uxRxRcvError = 0;
		++uxRxRcvError;
    }
    if( ( ulErrorCode & ETH_RECEIVE_OVERFLOW ) != 0 )
    {
    	static size_t uxRxROError = 0;
		++uxRxROError;
    }
    if( ( ulErrorCode & ETH_WATCHDOG_TIMEOUT ) != 0 )
    {
    	static size_t uxRxWDTError = 0;
		++uxRxWDTError;
    }
    if( ( ulErrorCode & ETH_GIANT_PACKET ) != 0 )
    {
    	static size_t uxRxGPError = 0;
		++uxRxGPError;
    }
    if( ( ulErrorCode & ETH_CRC_ERROR ) != 0 )
    {
    	static size_t uxRxCRCError = 0;
		++uxRxCRCError;
    }
}

/*---------------------------------------------------------------------------*/

static void prvHAL_DMA_ErrorCallback( ETH_HandleTypeDef * pxEthHandle )
{
	#ifdef niEMAC_STM32HX
	{
		const uint32_t ulDmaError = pxEthHandle->DMAErrorCode;
		if( ( ulDmaError & ETH_DMA_RX_ERRORS_MASK ) != ETH_DMA_RX_NO_ERROR_FLAG )
		{
			/* if( ( ulDmaError & ETH_DMA_RX_ERRORS_MASK ) == ETH_DMA_RX_DESC_READ_ERROR_FLAG )
			{
				static size_t uxDmaRDRError = 0;
				++uxDmaRDRError;
			}
			else if( ( ulDmaError & ETH_DMA_RX_ERRORS_MASK ) == ETH_DMA_RX_DESC_WRITE_ERROR_FLAG )
			{
				static size_t uxDmaRDWError = 0;
				++uxDmaRDWError;
			}
			else if( ( ulDmaError & ETH_DMA_RX_ERRORS_MASK ) == ETH_DMA_RX_BUFFER_READ_ERROR_FLAG )
			{
				static size_t uxDmaRBRError = 0;
				++uxDmaRBRError;
			}
			else if( ( ulDmaError & ETH_DMA_RX_ERRORS_MASK ) == ETH_DMA_RX_BUFFER_WRITE_ERROR_FLAG )
			{
				static size_t uxDmaRBWError = 0;
				++uxDmaRBWError;
			} */
		}
		if( ( ulDmaError & ETH_DMA_TX_ERRORS_MASK ) != ETH_DMA_TX_NO_ERROR_FLAG )
		{
			/* if( ( ulDmaError & ETH_DMA_TX_ERRORS_MASK ) == ETH_DMA_TX_DESC_READ_ERROR_FLAG )
			{
				static size_t uxDmaTDRError = 0;
				++uxDmaTDRError;
			}
			else if( ( ulDmaError & ETH_DMA_TX_ERRORS_MASK ) == ETH_DMA_TX_DESC_WRITE_ERROR_FLAG )
			{
				static size_t uxDmaTDWError = 0;
				++uxDmaTDWError;
			}
			else if( ( ulDmaError & ETH_DMA_TX_ERRORS_MASK ) == ETH_DMA_TX_BUFFER_READ_ERROR_FLAG )
			{
				static size_t uxDmaTBRError = 0;
				++uxDmaTBRError;
			}
			else if( ( ulDmaError & ETH_DMA_TX_ERRORS_MASK ) == ETH_DMA_TX_BUFFER_WRITE_ERROR_FLAG )
			{
				static size_t uxDmaTBWError = 0;
				++uxDmaTBWError;
			} */
		}
		if( ( ulDmaError & ETH_DMA_CONTEXT_DESC_ERROR_FLAG ) != 0 )
		{
			static size_t uxDmaCDError = 0;
			++uxDmaCDError;
		}
		if( ( ulDmaError & ETH_DMA_FATAL_BUS_ERROR_FLAG ) != 0 )
		{
			static size_t uxDmaFBEError = 0;
			++uxDmaFBEError;
		}
		if( ( ulDmaError & ETH_DMA_EARLY_TX_IT_FLAG ) != 0 )
		{
			static size_t uxDmaETIError = 0;
			++uxDmaETIError;
		}
		if( ( ulDmaError & ETH_DMA_RX_WATCHDOG_TIMEOUT_FLAG ) != 0 )
		{
			static size_t uxDmaRWTError = 0;
			++uxDmaRWTError;
		}
		if( ( ulDmaError & ETH_DMA_RX_PROCESS_STOPPED_FLAG ) != 0 )
		{
			static size_t uxDmaRPSError = 0;
			++uxDmaRPSError;
		}
		if( ( ulDmaError & ETH_DMA_RX_BUFFER_UNAVAILABLE_FLAG ) != 0 )
		{
			static size_t uxDmaRBUError = 0;
			++uxDmaRBUError;
		}
		if( ( ulDmaError & ETH_DMA_TX_PROCESS_STOPPED_FLAG ) != 0 )
		{
			static size_t uxDmaTPSError = 0;
			++uxDmaTPSError;
		}
	}
	#endif /* ifdef niEMAC_STM32HX */
}

/*---------------------------------------------------------------------------*/

static void prvHAL_MAC_ErrorCallback( ETH_HandleTypeDef * pxEthHandle )
{
	#ifdef niEMAC_STM32HX
	{
		const uint32_t ulMacError = pxEthHandle->MACErrorCode;
		if( ( ulMacError & ETH_RECEIVE_WATCHDOG_TIMEOUT ) != 0 )
		{
			static size_t uxMacRWTError = 0;
			++uxMacRWTError;
		}
		if( ( ulMacError & ETH_EXECESSIVE_COLLISIONS ) != 0 )
		{
			static size_t uxMacECError = 0;
			++uxMacECError;
		}
		if( ( ulMacError & ETH_LATE_COLLISIONS ) != 0 )
		{
			static size_t uxMacLCError = 0;
			++uxMacLCError;
		}
		if( ( ulMacError & ETH_EXECESSIVE_DEFERRAL ) != 0 )
		{
			static size_t uxMacEDError = 0;
			++uxMacEDError;
		}
		if( ( ulMacError & ETH_LOSS_OF_CARRIER ) != 0 )
		{
			static size_t uxMacLOCError = 0;
			++uxMacLOCError;
		}
		if( ( ulMacError & ETH_NO_CARRIER ) != 0 )
		{
			static size_t uxMacNCError = 0;
			++uxMacNCError;
		}
		if( ( ulMacError & ETH_TRANSMIT_JABBR_TIMEOUT ) != 0 )
		{
			static size_t uxMacTJTError = 0;
			++uxMacTJTError;
		}
	}
	#endif /* ifdef niEMAC_STM32HX */
}

#endif /* ipconfigIS_ENABLED( niEMAC_DEBUG_ERROR ) */

/*---------------------------------------------------------------------------*/
/*===========================================================================*/
/*                          Sample HAL User Functions                        */
/*===========================================================================*/
/*---------------------------------------------------------------------------*/

#if 0

/**
  * @brief  Initializes the ETH MSP.
  * @param  pxEthHandle: ETH handle
  * @retval None
*/
void HAL_ETH_MspInit( ETH_HandleTypeDef * pxEthHandle )
{
    if( pxEthHandle->Instance == ETH )
    {
        /* Enable ETHERNET clock */
        #ifdef niEMAC_STM32FX
            __HAL_RCC_ETH_CLK_ENABLE();
        #elif defined( STM32H5 )
            __HAL_RCC_ETH_CLK_ENABLE();
            __HAL_RCC_ETHTX_CLK_ENABLE();
            __HAL_RCC_ETHRX_CLK_ENABLE();
        #elif defined( STM32H7)
            __HAL_RCC_ETH1MAC_CLK_ENABLE();
            __HAL_RCC_ETH1TX_CLK_ENABLE();
            __HAL_RCC_ETH1RX_CLK_ENABLE();
        #endif

        /* Enable GPIOs clocks */
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();
        __HAL_RCC_GPIOE_CLK_ENABLE();
        __HAL_RCC_GPIOF_CLK_ENABLE();
        __HAL_RCC_GPIOG_CLK_ENABLE();
        __HAL_RCC_GPIOH_CLK_ENABLE();

        /* Ethernet pins configuration ************************************************/
        /*
            Common Pins
            ETH_MDC ----------------------> ETH_MDC_Port, ETH_MDC_Pin
            ETH_MDIO --------------------->
            ETH_RXD0 --------------------->
            ETH_RXD1 --------------------->
            ETH_TX_EN -------------------->
            ETH_TXD0 --------------------->
            ETH_TXD1 --------------------->

            RMII Specific Pins
            ETH_REF_CLK ------------------>
            ETH_CRS_DV ------------------->

            MII Specific Pins
            ETH_RX_CLK ------------------->
            ETH_RX_ER -------------------->
            ETH_RX_DV -------------------->
            ETH_RXD2 --------------------->
            ETH_RXD3 --------------------->
            ETH_TX_CLK ------------------->
            ETH_TXD2 --------------------->
            ETH_TXD3 --------------------->
            ETH_CRS ---------------------->
            ETH_COL ---------------------->
        */

        GPIO_InitTypeDef GPIO_InitStructure = { 0 };
        GPIO_InitStructure.Speed = GPIO_SPEED_HIGH;
        GPIO_InitStructure.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStructure.Pull = GPIO_NOPULL;
        GPIO_InitStructure.Alternate = GPIO_AF11_ETH;

        GPIO_InitStructure.Pin = ETH_MDC_Pin;
        GPIO_InitStructure.Speed = GPIO_SPEED_MEDIUM;
        HAL_GPIO_Init( ETH_MDC_Port, &GPIO_InitStructure );
        GPIO_InitStructure.Speed = GPIO_SPEED_HIGH;

        GPIO_InitStructure.Pin = ETH_MDIO_Pin;
        HAL_GPIO_Init( ETH_MDIO_Port, &GPIO_InitStructure );

        GPIO_InitStructure.Pin = ETH_RXD0_Pin;
        HAL_GPIO_Init( ETH_RXD0_Port, &GPIO_InitStructure );

        GPIO_InitStructure.Pin = ETH_RXD1_Pin;
        HAL_GPIO_Init( ETH_RXD1_Port, &GPIO_InitStructure );

        GPIO_InitStructure.Pin = ETH_TX_EN_Pin;
        HAL_GPIO_Init( ETH_TX_EN_Port, &GPIO_InitStructure );

        GPIO_InitStructure.Pin = ETH_TXD0_Pin;
        HAL_GPIO_Init( ETH_TXD0_Port, &GPIO_InitStructure );

        GPIO_InitStructure.Pin = ETH_TXD1_Pin;
        HAL_GPIO_Init( ETH_TXD1_Port, &GPIO_InitStructure );

        if( pxEthHandle->Init.MediaInterface == HAL_ETH_RMII_MODE )
        {
            GPIO_InitStructure.Pin = ETH_REF_CLK_Pin;
            HAL_GPIO_Init( ETH_REF_CLK_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_CRS_DV_Pin;
            HAL_GPIO_Init( ETH_CRS_DV_Port, &GPIO_InitStructure );
        }
        else if( pxEthHandle->Init.MediaInterface == HAL_ETH_MII_MODE )
        {
            GPIO_InitStructure.Pin = ETH_RX_CLK_Pin;
            HAL_GPIO_Init( ETH_RX_CLK_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_RX_ER_Pin;
            HAL_GPIO_Init( ETH_RX_ER_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_RX_DV_Pin;
            HAL_GPIO_Init( ETH_RX_DV_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_RXD2_Pin;
            HAL_GPIO_Init( ETH_RXD2_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_RXD3_Pin;
            HAL_GPIO_Init( ETH_RXD3_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_TX_CLK_Pin;
            HAL_GPIO_Init( ETH_TX_CLK_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_TXD2_Pin;
            HAL_GPIO_Init( ETH_TXD2_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_TXD3_Pin;
            HAL_GPIO_Init( ETH_TXD3_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_COL_Pin;
            HAL_GPIO_Init( ETH_COL_Port, &GPIO_InitStructure );

            GPIO_InitStructure.Pin = ETH_CRS_Pin;
            HAL_GPIO_Init( ETH_CRS_Port, &GPIO_InitStructure );
        }

        /* Enable the Ethernet global Interrupt */
        HAL_NVIC_SetPriority( ETH_IRQn, ( uint32_t ) configMAX_SYSCALL_INTERRUPT_PRIORITY, 0 );
        HAL_NVIC_EnableIRQ( ETH_IRQn );
    }
}

/*---------------------------------------------------------------------------*/

void HAL_ETH_MspDeInit( ETH_HandleTypeDef * pxEthHandle )
{
	if( pxEthHandle->Instance == ETH )
	{
		/* Peripheral clock disable */
		#ifdef niEMAC_STM32FX
			__HAL_RCC_ETH_CLK_DISABLE();
		#elif defined( STM32H5 )
			__HAL_RCC_ETH_CLK_DISABLE();
			__HAL_RCC_ETHTX_CLK_DISABLE();
			__HAL_RCC_ETHRX_CLK_DISABLE();
		#elif defined( STM32H7)
			__HAL_RCC_ETH1MAC_CLK_DISABLE();
			__HAL_RCC_ETH1TX_CLK_DISABLE();
			__HAL_RCC_ETH1RX_CLK_DISABLE();
		#endif

		/**ETH GPIO Configuration
		Common Pins
		ETH_MDC ----------------------> ETH_MDC_Port, ETH_MDC_Pin
		ETH_MDIO --------------------->
		ETH_RXD0 --------------------->
		ETH_RXD1 --------------------->
		ETH_TX_EN -------------------->
		ETH_TXD0 --------------------->
		ETH_TXD1 --------------------->

		RMII Specific Pins
		ETH_REF_CLK ------------------>
		ETH_CRS_DV ------------------->

		MII Specific Pins
		ETH_RX_CLK ------------------->
		ETH_RX_ER -------------------->
		ETH_RX_DV -------------------->
		ETH_RXD2 --------------------->
		ETH_RXD3 --------------------->
		ETH_TX_CLK ------------------->
		ETH_TXD2 --------------------->
		ETH_TXD3 --------------------->
		ETH_CRS ---------------------->
		ETH_COL ---------------------->
		*/

		HAL_GPIO_DeInit( ETH_MDC_Port, ETH_MDC_Pin );
		HAL_GPIO_DeInit( ETH_MDIO_Port, ETH_MDIO_Pin );
		HAL_GPIO_DeInit( ETH_RXD0_Port, ETH_RXD0_Pin );
		HAL_GPIO_DeInit( ETH_RXD1_Port, ETH_RXD1_Pin );
		HAL_GPIO_DeInit( ETH_TX_EN_Port, ETH_TX_EN_Pin );
		HAL_GPIO_DeInit( ETH_TXD0_Port, ETH_TXD0_Pin );
		HAL_GPIO_DeInit( ETH_TXD1_Port, ETH_TXD1_Pin );

		if( pxEthHandle->Init.MediaInterface == HAL_ETH_RMII_MODE )
		{
			HAL_GPIO_DeInit( ETH_REF_CLK_Port, ETH_REF_CLK_Pin );
			HAL_GPIO_DeInit( ETH_CRS_DV_Port, ETH_CRS_DV_Pin );
		}
		else if( pxEthHandle->Init.MediaInterface == HAL_ETH_MII_MODE )
		{
			HAL_GPIO_DeInit( ETH_RX_CLK_Port, ETH_RX_CLK_Pin );
			HAL_GPIO_DeInit( ETH_RX_ER_Port, ETH_RX_ER_Pin );
			HAL_GPIO_DeInit( ETH_RX_DV_Port, ETH_RX_DV_Pin );
			HAL_GPIO_DeInit( ETH_RXD2_Port, ETH_RXD2_Pin );
			HAL_GPIO_DeInit( ETH_RXD3_Port, ETH_RXD3_Pin );
			HAL_GPIO_DeInit( ETH_TX_CLK_Port, ETH_TX_CLK_Pin );
			HAL_GPIO_DeInit( ETH_TXD2_Port, ETH_TXD2_Pin );
			HAL_GPIO_DeInit( ETH_TXD3_Port, ETH_TXD3_Pin );
			HAL_GPIO_DeInit( ETH_COL_Port, ETH_COL_Pin );
			HAL_GPIO_DeInit( ETH_CRS_Port, ETH_CRS_Pin );
		}

		/* ETH interrupt Deinit */
		HAL_NVIC_DisableIRQ(ETH_IRQn);
	}
}

/*---------------------------------------------------------------------------*/

#if ( niEMAC_MPU != 0 )

void MPU_Config(void)
{
    MPU_Region_InitTypeDef MPU_InitStruct = {0};

    HAL_MPU_Disable();

    extern uint8_t __ETH_BUFFERS_START;

    MPU_InitStruct.Enable = ipconfigIS_ENABLED( niEMAC_USE_MPU ) ? ENABLE : DISABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER0;
    MPU_InitStruct.BaseAddress = ( uint32_t ) &__ETH_BUFFERS_START;
    MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
    MPU_InitStruct.SubRegionDisable = 0x0;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

    HAL_MPU_ConfigRegion(&MPU_InitStruct);


    extern uint8_t __ETH_DESCRIPTORS_START;

    MPU_InitStruct.Enable = MPU_REGION_ENABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER1;
    MPU_InitStruct.BaseAddress = ( uint32_t ) &__ETH_DESCRIPTORS_START;
    MPU_InitStruct.Size = MPU_REGION_SIZE_1KB;
    MPU_InitStruct.SubRegionDisable = 0x0;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;

    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

#endif /* if ( niEMAC_MPU != 0 ) */

#endif /* if 0 */

/*---------------------------------------------------------------------------*/
