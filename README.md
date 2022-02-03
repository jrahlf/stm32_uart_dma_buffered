# stm32_uart_dma_buffered
Provides asynchronous transmission and reception via UART by using DMA channels.
Inspired by [MaJerle/stm32-usart-uart-dma-rx-tx](https://github.com/MaJerle/stm32-usart-uart-dma-rx-tx)

It provides ring buffers for the user, incoming data should be polled via `BufferedUart_Dequeue` or by providing a `DataReceivedHandler` and  returning  `BUFFERED_UART_DATA_HANDLED`. The user must provided the underlying buffers to initialize the ringbuffers in `BufferedUart_Init`.

The (maximum) number of uarts to be used must be set via `MAX_NUMBER_BUFFERED_UARTS` (default = 1). This is because the HAL code is not object oriented and this driver must keep track of which underlying `UART_HandleTypeDef` belongs to which `BufferedUart`.

It is based on the ST HAL and requires that the UART peripheral and the DMA channels are configured beforehand.
See [doc/dma_rx.png](doc/dma_rx.png) and [doc/dma_tx.png](doc/dma_tx.png). This can be done entirely via STM32CubeIDE in the IOC editor.

### Optimizations
The driver disables the transfer half complete interrupt, as it not necessary. However, the HAL automatically enables it every time, so this driver disables it every time a transfer is started. This shall reduce interrupt workload.
