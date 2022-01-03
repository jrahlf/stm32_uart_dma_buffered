/*
 *
 *  Created on: Dec 2, 2021
 *  Author: Jonas Rahlf
 */

#pragma once

#include <stdbool.h>
#include <string.h>
#include "main.h"		// pull in ST definitions like UART_HandleTypeDef

#ifdef __cplusplus
extern "C" {
#endif


/// ==== configuration options ====

// define BUFFERED_UART_REENTRANT if you use the buffered uart API from interrupt and normal context (reentrant)
// default off
//#define BUFFERED_UART_REENTRANT

// define BUFFERED_UART_PROVIDE_HAL_UART_TxCpltCallback if the API should provide the definition for the function
// otherwise, if USE_HAL_UART_REGISTER_CALLBACKS==0 you have to call BufferedUart_TxCpltCallback from external glue code
// default on
#define BUFFERED_UART_PROVIDE_HAL_UART_TxCpltCallback

// define BUFFERED_UART_PROVIDE_HAL_UARTEx_RxEventCallback if the API should provide the definition for the function
// otherwise, if USE_HAL_UART_REGISTER_CALLBACKS==0 you have to call BufferedUart_RxEventCallback from external glue code
// default on
#define BUFFERED_UART_PROVIDE_HAL_UARTEx_RxEventCallback

// set MAX_NUMBER_BUFFERED_UARTS to the number of required number of buffered uarts
// default 1
#define MAX_NUMBER_BUFFERED_UARTS (1)

// if ALL ring buffer queue sizes are equal and known at compile time, you can specify the size here
// to enable optimization for internal modulo operations (especially if size is power of 2)
// default off
//#define BUFFERED_UART_FIXED_BUFFER_SIZE (128) //set this accordingly

/// ==== end configuration options ====

enum BufferedUartMode {
	BUFFERED_UART_TX_RX,
	BUFFERED_UART_TX,
	BUFFERED_UART_RX
};

enum DataHandledResult {
	BUFFERED_UART_DATA_NOT_HANDLED,
	BUFFERED_UART_DATA_HANDLED
};

struct BlockRingbuffer {
	char * buf;
	unsigned int head;
	unsigned int tail;
	unsigned int length;
};

struct BufferedUart {
	UART_HandleTypeDef * uart;
	struct BlockRingbuffer txqueue;
	struct BlockRingbuffer rxqueue;
	unsigned int lastSendBlockSize;
	enum DataHandledResult (*DataReceivedHandler)(const char * data, unsigned int length);
};

#ifdef BUFFERED_UART_REENTRANT
    #define BUFFERED_UART_REENTRANT_ENTER_CRITICAL_SECTION()   \
       uint32_t PriMsk;                    						\
       PriMsk = __get_PRIMASK();           						\
       __set_PRIMASK(1);

    #define BUFFERED_UART_REENTRANT_EXIT_CRITICAL_SECTION()    \
       __set_PRIMASK(PriMsk);
#else
    #define BUFFERED_UART_REENTRANT_ENTER_CRITICAL_SECTION()
    #define BUFFERED_UART_REENTRANT_EXIT_CRITICAL_SECTION()
#endif





HAL_StatusTypeDef BufferedUart_StartReception(struct BufferedUart *uart);
HAL_StatusTypeDef BufferedUart_StopReception(struct BufferedUart *uart);
HAL_StatusTypeDef BufferedUart_Init(struct BufferedUart * buffered_uart, UART_HandleTypeDef * uart, enum BufferedUartMode mode, void *txBuffer, unsigned int txSize, void *rxBuffer, unsigned int rxSize);
HAL_StatusTypeDef BufferedUart_Transmit(struct BufferedUart *uart, const void * data, unsigned int length);
HAL_StatusTypeDef BufferedUart_TransmitTimed(struct BufferedUart *uart, const void * data, unsigned int length, unsigned int timeoutMs);
unsigned int BufferedUart_Dequeue(struct BufferedUart *uart, void * buffer, unsigned int maximumLength);

#ifndef BUFFERED_UART_PROVIDE_HAL_UART_TxCpltCallback
	/// this function must be called if USE_HAL_UART_REGISTER_CALLBACKS==0
	void BufferedUart_TxCpltCallback(UART_HandleTypeDef *huart);
#endif

#ifndef BUFFERED_UART_PROVIDE_HAL_UARTEx_RxEventCallback
	/// this function must be called if USE_HAL_UART_REGISTER_CALLBACKS==0
	void BufferedUart_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size);
#endif

/**
 * Check if the buffered uart is busy sending data over the wire
 * @note while the buffered uart is busy, one can still enqueue data via the transmit functions
 * @param[in]	uart	buffered uart
 * @return		bool	true if sending data
 */
static inline bool BufferedUart_IsTXBusy(const struct BufferedUart *uart)
{
	return uart->uart->gState == HAL_UART_STATE_BUSY_TX;
}

/**
 * Similar to @ref BufferedUart_Transmit but accepts a zero terminated string as data
 */
static inline HAL_StatusTypeDef BufferedUart_TransmitString(struct BufferedUart *uart, const char * string)
{
	return BufferedUart_Transmit(uart, string, strlen(string));
}

/**
 * Similar to @ref BufferedUart_TransmitTimed but accepts a zero terminated string as data
 */
static inline HAL_StatusTypeDef BufferedUart_TransmitStringTimed(struct BufferedUart *uart, const char * string, unsigned int timeoutMs)
{
	return BufferedUart_TransmitTimed(uart, string, strlen(string), timeoutMs);
}


#ifdef __cplusplus
}
#endif


