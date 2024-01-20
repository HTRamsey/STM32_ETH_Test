
/* Please add this configuration to you FreeRTOSIPConfig.h file: */

/* sudo iperf3 -c 192.168.1.8 -4 --port 5001 --bytes 100M --set-mss 1460 --window 57K */

#define USE_IPERF								1

#define ipconfigIPERF_PRIORITY_IPERF_TASK		( tskIDLE_PRIORITY + 1 )

#define ipconfigIPERF_HAS_UDP					0
#define ipconfigIPERF_DOES_ECHO_UDP				0

#define ipconfigIPERF_VERSION					3
#define ipconfigIPERF_STACK_SIZE_IPERF_TASK		680

/* When there is a lot of RAM, try these values or higher: */

#ifdef STM32F7
	#define ipconfigIPERF_TX_WINSIZE				( 57 )
#else
	#define ipconfigIPERF_TX_WINSIZE				( 73 )
#endif
#define ipconfigIPERF_TX_BUFSIZE				( ipconfigIPERF_TX_WINSIZE * ipconfigTCP_MSS )
#define ipconfigIPERF_RX_WINSIZE				ipconfigIPERF_TX_WINSIZE
#define ipconfigIPERF_RX_BUFSIZE				ipconfigIPERF_TX_BUFSIZE


/* The iperf module declares a character buffer to store its send data. */
#define ipconfigIPERF_RECV_BUFFER_SIZE			( 1 * ipconfigTCP_MSS )
