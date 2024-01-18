#ifndef FREERTOS_IP_CONFIG_H
#define FREERTOS_IP_CONFIG_H


#define ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM        	1
#define ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM        	1
#define ipconfigZERO_COPY_RX_DRIVER                   	1
#define ipconfigZERO_COPY_TX_DRIVER                   	1
#define ipconfigUSE_LINKED_RX_MESSAGES                	1
#define ipconfigUSE_NETWORK_EVENT_HOOK 					0
#define ipconfigUSE_DHCP								0
#define ipconfigUSE_TCP									1
#define ipconfigUSE_DNS									0
#define ipconfigBYTE_ORDER 								pdFREERTOS_LITTLE_ENDIAN
#define ipconfigSUPPORT_OUTGOING_PINGS 					1
#define ipconfigSUPPORT_SIGNALS 						1
#define ipconfigENABLE_BACKWARD_COMPATIBILITY 			0
#define ipconfigUSE_CALLBACKS							1
#define ipconfigCHECK_IP_QUEUE_SPACE					1
#define ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES     1
#define ipconfigETHERNET_DRIVER_FILTERS_PACKETS			0
#define ipconfigALLOW_SOCKET_SEND_WITHOUT_BIND          0
#define ipconfigSOCKET_HAS_USER_WAKE_CALLBACK    		1
#define ipconfigSUPPORT_SELECT_FUNCTION 				1
// ipSIZE_OF_IPv4_HEADER + ipSIZE_OF_UDP_HEADER + MAVLINK_MAX_PACKET_LEN
// 20 					 + 8 					+ 280
#define ipconfigNETWORK_MTU                             1500U
// #define ipconfigETHERNET_MINIMUM_PACKET_BYTES           60U
#define ipconfigUDP_MAX_RX_PACKETS                      50U
#define ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS          40U
#define ipconfigIP_TASK_STACK_SIZE_WORDS                ( configMINIMAL_STACK_SIZE * 4 )
#define ipconfigSOCKET_HAS_USER_SEMAPHORE               0
#define ipconfigWATCHDOG_TIMER()
#define ipconfigHAS_DEBUG_PRINTF                        0
#define ipconfigHAS_PRINTF                              0

#define ipconfigUSE_IPv4 								1
#define ipconfigUSE_IPv6 								0
#define ipconfigUSE_RA 									0
#define ipconfigSUPPORT_NETWORK_DOWN_EVENT              0

#endif /* INC_FREERTOSIPCONFIG_H_ */
