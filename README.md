# stm32_uart_dma_buffered
Provides asynchronous transmission and reception via UART by using DMA channels.
Inspired by [MaJerle/stm32-usart-uart-dma-rx-tx](https://github.com/MaJerle/stm32-usart-uart-dma-rx-tx)

It provides ring buffers for the user, incoming data should be polled via `BufferedUart_Dequeue` or by providing a `DataReceivedHandler` and  returning  `BUFFERED_UART_DATA_HANDLED`. The user must provided the underlying buffers to initialize the ringbuffers in `BufferedUart_Init`.

The (maximum) number of uarts to be used must be set via `MAX_NUMBER_BUFFERED_UARTS` (default = 1). This is because the HAL code is not object oriented and this driver must keep track of which underlying `UART_HandleTypeDef` belongs to which `BufferedUart`.

It is based on the ST HAL and requires that the UART peripheral and the DMA channels are configured beforehand.
See [doc/dma_rx.png](doc/dma_rx.png) and [doc/dma_tx.png](doc/dma_tx.png). This can be done entirely via STM32CubeIDE in the IOC editor.

### Reentrancy
The driver is not reentrant safe by default (usually this means do not use it inside interrupts). However, one can `#define BUFFERED_UART_REENTRANT` which causes interrupts to be disabled when enqueuing data and then the driver can be used in a reentrant way.

### Optimizations
The driver disables the transfer half complete interrupt, as it not necessary. However, the HAL automatically enables it every time, so this driver disables it every time a transfer is started. This shall reduce interrupt workload. **If** all ring buffer sizes are equal and known at compile time, it can be configured by defining `BUFFERED_UART_FIXED_BUFFER_SIZE` appropiately. The compiler can then replace modulo operations with bitmasking, **if** the size is a power of 2. Should not be used by default.

### Error Handling
If a UART error occurs, the ST HAL aborts the DMA transmission. The driver then automatically restarts the reception. This can happen for example if the other device sends at a different baud rate.

### Example

```c
extern UART_HandleTypeDef huart2; //! must be configured beforehand, e.g. via STM32CubeIDE and corresponding init functions

enum DataHandledResult DataReceivedHandler(const char * data, unsigned int length)
{
  // copy incoming data into another processing buffer, note this is called from interrupt context!
  return BUFFERED_UART_DATA_HANDLED;
}

static char txbuffer[110];
static char rxbuffer[30];
struct BlockRingbuffer ringbuffer;
buffered_uart.DataReceivedHandler = DataReceivedHandler;
BufferedUart_Init(&buffered_uart, &huart2, BUFFERED_UART_TX_RX, txbuffer, sizeof(txbuffer), rxbuffer, sizeof(rxbuffer));
BufferedUart_StartReception(&buffered_uart);
BufferedUart_TransmitString(&buffered_uart, "hello world\n");
```
