/**

stm32_buffered_uart
provides functionality to receive and send data via UART and DMA by the use of ringbuffers


MIT License

Copyright (c) 2022 Jonas Rahlf

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "stm32_buffered_uart.h"
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include <string.h>

#if MAX_NUMBER_BUFFERED_UARTS == 0
	#error "MAX_NUMBER_BUFFERED_UARTS must be defined and > 0"
#endif

#ifdef BUFFERED_UART_PROVIDE_HAL_UART_TxCpltCallback
	#ifdef USE_HAL_UART_REGISTER_CALLBACKS
		#if(USE_HAL_UART_REGISTER_CALLBACKS == 0)
			#define BufferedUart_TxCpltCallback HAL_UART_TxCpltCallback
		#endif
	#endif
#endif

#ifdef BUFFERED_UART_PROVIDE_HAL_UARTEx_RxEventCallback
	#ifdef USE_HAL_UART_REGISTER_CALLBACKS
		#if(USE_HAL_UART_REGISTER_CALLBACKS == 0)
			#define BufferedUart_RxEventCallback HAL_UARTEx_RxEventCallback
		#endif
	#endif
#endif

#ifdef BUFFERED_UART_PROVIDE_HAL_UART_ErrorCallback
	#ifdef USE_HAL_UART_REGISTER_CALLBACKS
		#if(USE_HAL_UART_REGISTER_CALLBACKS == 0)
			#define BufferedUart_UART_ErrorCallback HAL_UART_ErrorCallback
		#endif
	#endif
#endif


static struct BufferedUart * s_uarts[MAX_NUMBER_BUFFERED_UARTS];
static int s_numberUartsInUse;

static void BufferedUart_TryStartTransmission(struct BufferedUart * uart);
static bool BufferedUart_TXQueue_Enqueue(struct BufferedUart * uart, const void * data, unsigned int length);
static const void* BufferedUart_TXQueue_Dequeue(const struct BufferedUart * uart, unsigned int * length);
void BufferedUart_TxCpltCallback(UART_HandleTypeDef *huart);
void BufferedUart_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size);
void BufferedUart_UART_ErrorCallback(UART_HandleTypeDef *huart);
static struct BufferedUart * ContainerOf(const UART_HandleTypeDef * huart);

static inline unsigned int min(unsigned int a, unsigned int b) {
    if (a > b) {
        return b;
    }
    return a;
}

static inline void disableHalfCompleteInterrupt(DMA_HandleTypeDef *dmaHandle)
{
	__HAL_DMA_DISABLE_IT(dmaHandle, DMA_IT_HT);
}

static void registerBufferedUart(struct BufferedUart * uart)
{
	s_uarts[s_numberUartsInUse] = uart;
	s_numberUartsInUse++;
}

void BlockRingbuffer_Init(struct BlockRingbuffer * buffer, void * underlying, unsigned int length)
{
	buffer->buf = underlying;
	buffer->length = length;
	buffer->head = 0;
	buffer->tail = 0;
}

static bool BlockRingbuffer_IsValid(const struct BlockRingbuffer * buffer)
{
	return buffer != NULL && buffer->buf != NULL && buffer->length > 0 && buffer->length <= 0xFFFF;
}

static inline unsigned int BlockRingbuffer_GetReadAvailable(const struct BlockRingbuffer * buffer)
{
	atomic_signal_fence(memory_order_acquire);
	return buffer->head - buffer->tail;
}

static inline unsigned int BlockRingbuffer_GetWriteAvailable(const struct BlockRingbuffer * buffer)
{
	return buffer->length - BlockRingbuffer_GetReadAvailable(buffer);
}

static inline unsigned int BlockRingbuffer_GetLength(const struct BlockRingbuffer * buffer)
{
#ifdef BUFFERED_UART_FIXED_BUFFER_SIZE
	return BUFFERED_UART_FIXED_BUFFER_SIZE;
#endif
	return buffer->length;
}

HAL_StatusTypeDef BufferedUart_Init(struct BufferedUart * bufferedUart, UART_HandleTypeDef * uart, enum BufferedUartMode mode, void *txBuffer, unsigned int txSize, void *rxBuffer, unsigned int rxSize)
{
	if (s_numberUartsInUse == MAX_NUMBER_BUFFERED_UARTS) {
		return HAL_ERROR;
	}

	if (bufferedUart == NULL || uart == NULL) {
		return HAL_ERROR;
	}

	if (mode == BUFFERED_UART_TX || mode == BUFFERED_UART_TX_RX) {
		struct BlockRingbuffer buffer;
		BlockRingbuffer_Init(&buffer, txBuffer, txSize);
		bufferedUart->txqueue = buffer;
		if (!BlockRingbuffer_IsValid(&bufferedUart->txqueue)) {
			return HAL_ERROR;
		}

#if (USE_HAL_UART_REGISTER_CALLBACKS == 1)
		HAL_StatusTypeDef status = HAL_UART_RegisterCallback(uart, HAL_UART_TX_COMPLETE_CB_ID, BufferedUart_TxCpltCallback);
		if (status != HAL_OK) {
			return status;
		}
#endif

	}

	if (mode == BUFFERED_UART_RX || mode == BUFFERED_UART_TX_RX) {
		struct BlockRingbuffer buffer;
		BlockRingbuffer_Init(&buffer, rxBuffer, rxSize);
		bufferedUart->rxqueue = buffer;
		if (!BlockRingbuffer_IsValid(&bufferedUart->rxqueue)) {
			return HAL_ERROR;
		}

#if (USE_HAL_UART_REGISTER_CALLBACKS == 1)
		HAL_StatusTypeDef status = HAL_UART_RegisterRxEventCallback(uart, BufferedUart_RxEventCallback);
		if (status != HAL_OK) {
			return status;
		}
#endif

	}

#if (USE_HAL_UART_REGISTER_CALLBACKS == 1)
	HAL_StatusTypeDef status = HAL_UART_RegisterCallback(uart, HAL_UART_ERROR_CB_ID, BufferedUart_UART_ErrorCallback);
	if (status != HAL_OK) {
		return status;
	}
#endif

	registerBufferedUart(bufferedUart);
	bufferedUart->uart = uart;
	bufferedUart->lastSendBlockSize = 0;

	return HAL_OK;
}

HAL_StatusTypeDef BufferedUart_StartReception(struct BufferedUart *uart)
{
	if (!BlockRingbuffer_IsValid(&uart->rxqueue)) {
		return HAL_ERROR;
	}

	return HAL_UARTEx_ReceiveToIdle_DMA(uart->uart, (uint8_t*)uart->rxqueue.buf, uart->rxqueue.length);
}

HAL_StatusTypeDef BufferedUart_StopReception(struct BufferedUart *uart)
{
	return HAL_UART_DMAStop(uart->uart);
}

struct BufferedUart * ContainerOf(const UART_HandleTypeDef * huart)
{
#if MAX_NUMBER_BUFFERED_UARTS == 1
	return s_uarts[0];
#else
	for (int i = 0; i < s_numberUartsInUse; i++) {
		if (s_uarts[i]->uart == huart) {
			return s_uarts[i];
		}
	}

	return NULL;
#endif
}

void BufferedUart_TxCpltCallback(UART_HandleTypeDef *huart)
{
	struct BufferedUart * bufferedUart = ContainerOf(huart);
	if (bufferedUart == NULL) {
		// error: the given uart handle was not registered before via BufferedUart_Init
		Error_Handler();
	}

	atomic_signal_fence(memory_order_acquire);
	bufferedUart->txqueue.tail += bufferedUart->lastSendBlockSize;
	atomic_signal_fence(memory_order_release);

	BufferedUart_TryStartTransmission(bufferedUart);
}

void BufferedUart_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	struct BufferedUart * bufferedUart = ContainerOf(huart);
	if (bufferedUart == NULL) {
		// error: the given uart handle was not registered before via buffered_uart_init
		Error_Handler();
	}

	atomic_signal_fence(memory_order_acquire);
	bufferedUart->rxqueue.head = Size;
	atomic_signal_fence(memory_order_release);

	if (Size < bufferedUart->rxqueue.tail) {
		// this seems like a bug in ST HAL driver, where
		// HAL_UARTEx_RxEventCallback(huart, (huart->RxXferSize - huart->RxXferCount)); is called and
		// RxXferCount > RxXferSize
		// this can happen in conjunction with restarts of DMA reception due to UART errors
		// e.g. other device sends with wrong baud rate
		// current strategy is to ignore this data, usually it fixes itself (e.g. other device sends at correct baud rate)
		return;
	}

	enum DataHandledResult handled = BUFFERED_UART_DATA_NOT_HANDLED;
	if (bufferedUart->DataReceivedHandler != NULL) {
		unsigned int length = BlockRingbuffer_GetReadAvailable(&bufferedUart->rxqueue);
		// length would be 0 if a UART IDLE event happens exactly after (HALF) DMA COMPLETE interrupt
		if (length > 0) {
			handled = bufferedUart->DataReceivedHandler(bufferedUart->rxqueue.buf + bufferedUart->rxqueue.tail, length);
		}
	}

	if (handled == BUFFERED_UART_DATA_HANDLED) {
		atomic_signal_fence(memory_order_acquire);
		bufferedUart->rxqueue.tail = bufferedUart->rxqueue.head;
		atomic_signal_fence(memory_order_release);
	}

	if (bufferedUart->rxqueue.tail == bufferedUart->rxqueue.length) {
		atomic_signal_fence(memory_order_acquire);
		bufferedUart->rxqueue.tail = 0;
		atomic_signal_fence(memory_order_release);
	}
}

/// the current strategy is to just restart the reception on error
/// transmission will start automatically the next time something is enqueued
void BufferedUart_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	struct BufferedUart * bufferedUart = ContainerOf(huart);
	if (bufferedUart == NULL) {
		return;
	}

	if (BlockRingbuffer_IsValid(&bufferedUart->rxqueue)) {
		HAL_StatusTypeDef result = BufferedUart_StartReception(bufferedUart);
		if (result != HAL_OK) {
			Error_Handler();
		}
	}
}

/**
 * Transmit data
 * The data is first copied into an internal buffer and then immediately send as soon as the
 * uart peripheral becomes available
 * @note if this is called from interrupt and normal context (reentrant), BUFFERED_UART_REENTRANT must be defined
 * 		 if BUFFERED_UART_REENTRANT is defined, all interrupts are disabled in this function (default reentrant strategy)
 * @param[in]	uart
 * @param[in]	data
 * @param[in]	length
 * @return		HAL_StatusTypeDef
 */
HAL_StatusTypeDef BufferedUart_Transmit(struct BufferedUart *uart, const void * data, unsigned int length)
{
	BUFFERED_UART_REENTRANT_ENTER_CRITICAL_SECTION();

	HAL_StatusTypeDef result = HAL_OK;
	bool ok = BufferedUart_TXQueue_Enqueue(uart, data, length);
	if (!ok) {
		result = HAL_BUSY;
	}

	BufferedUart_TryStartTransmission(uart);

	BUFFERED_UART_REENTRANT_EXIT_CRITICAL_SECTION();

	return result;
}

/**
 * Transmit data with a maximum timeout
 * The data is first copied into an internal buffer and then immediately send as soon as the
 * uart peripheral becomes available
 * @note the timeout parameter allows to call this function with a data length greater than the internal buffer size.
 *       Note that in this case the sent data can be incomplete if the timeout occurs before all data is sent
 * @note if this is called from interrupt and normal context (reentrant), BUFFERED_UART_REENTRANT must be defined
 * 		 if BUFFERED_UART_REENTRANT is defined, all interrupts are disabled in this function (default reentrant strategy)
 * @param[in]	uart
 * @param[in]	data
 * @param[in]	length
 * @return		HAL_StatusTypeDef
 */
HAL_StatusTypeDef BufferedUart_TransmitTimed(struct BufferedUart *uart, const void * data, unsigned int length, unsigned int timeoutMs)
{
    if (timeoutMs == 0) {
        return BufferedUart_Transmit(uart, data, length);
    }

    uint32_t start = HAL_GetTick();
    HAL_StatusTypeDef result = HAL_OK;
    while (length > 0) {
        if (HAL_GetTick() - start > timeoutMs) {
            result = HAL_TIMEOUT;
            break;
        }

        unsigned int enqueueSize = min(length, uart->txqueue.length);
        result = BufferedUart_Transmit(uart, data, enqueueSize);
        if (result == HAL_OK) {
            length -= enqueueSize;
            data += length;
        }

    }

    return result;
}

bool BufferedUart_TXQueue_Enqueue(struct BufferedUart * uart, const void * data, unsigned int length)
{
	if (length > BlockRingbuffer_GetWriteAvailable(&uart->txqueue)) {
		return false;
	}

	atomic_signal_fence(memory_order_acquire);
	unsigned int head = uart->txqueue.head;
	unsigned int queueMaxSize = BlockRingbuffer_GetLength(&uart->txqueue);
	unsigned int sizeTillWrapAround = queueMaxSize - (head % queueMaxSize);
	unsigned int firstLength = min(length, sizeTillWrapAround);
	unsigned int secondLength = length - firstLength;

	// first part
	unsigned int insertIndex = head % queueMaxSize;
	memcpy(uart->txqueue.buf + insertIndex, data, firstLength);
	head += firstLength;

	// second part after wrap around
	memcpy(uart->txqueue.buf, data + firstLength, secondLength);
	head += secondLength;

	atomic_signal_fence(memory_order_acquire);
	uart->txqueue.head = head;
	atomic_signal_fence(memory_order_release);

	return true;
}

const void * BufferedUart_TXQueue_Dequeue(const struct BufferedUart *uart, unsigned int * length)
{
	unsigned int txAvailable = BlockRingbuffer_GetReadAvailable(&uart->txqueue);
	if (txAvailable == 0) {
		*length = 0;
		return NULL;
	}

	atomic_signal_fence(memory_order_acquire);

	unsigned int tail = uart->txqueue.tail;
	unsigned int queueMaxSize = BlockRingbuffer_GetLength(&uart->txqueue);
	unsigned int sizeTillWrapAround = queueMaxSize - (tail % queueMaxSize);
	unsigned int dequeueLength = min(txAvailable, sizeTillWrapAround);

	*length = dequeueLength;
	const void * dequeueData = uart->txqueue.buf + (tail % queueMaxSize);

	return dequeueData;
}

void BufferedUart_TryStartTransmission(struct BufferedUart *uart)
{
	if (BufferedUart_IsTXBusy(uart)) {
		return;
	}

	unsigned int length;
	const void * data = BufferedUart_TXQueue_Dequeue(uart, &length);
	if (length > 0) {
		uart->lastSendBlockSize = length;
		HAL_StatusTypeDef result = HAL_UART_Transmit_DMA(uart->uart, (uint8_t*)data, length);
		disableHalfCompleteInterrupt(uart->uart->hdmatx);	// small optimization, disable unused interrupt
		if (result != HAL_OK) {
			Error_Handler();
		}
	}
}

unsigned int BufferedUart_Dequeue(struct BufferedUart *uart, void * buffer, unsigned int maximumLength)
{
    uint32_t queueSize = BlockRingbuffer_GetReadAvailable(&uart->rxqueue);
    if (queueSize == 0 || maximumLength == 0) {
        return 0;
    }

    atomic_signal_fence(memory_order_acquire);
    unsigned int tail = uart->rxqueue.tail;
    unsigned int dequeueLength = min(queueSize, maximumLength);
    unsigned int queueMaxSize = BlockRingbuffer_GetLength(&uart->rxqueue);
    unsigned int sizeTillWrapAround = queueMaxSize - (tail % queueMaxSize);
    unsigned int firstLength = min(dequeueLength, sizeTillWrapAround);
    unsigned int secondLength = dequeueLength - firstLength;

    // first part
    const char *dequeueData = uart->rxqueue.buf + (tail % queueMaxSize);
    memcpy(buffer, dequeueData, firstLength);
    tail += firstLength;

    // second part after wrap around
    dequeueData = uart->rxqueue.buf;
    char * dstPtr = ((char*)buffer) + firstLength;
    memcpy(dstPtr, dequeueData, secondLength);
    tail += secondLength;

    atomic_signal_fence(memory_order_acquire);
    uart->rxqueue.tail = tail;
    atomic_signal_fence(memory_order_release);

    return dequeueLength;
}


