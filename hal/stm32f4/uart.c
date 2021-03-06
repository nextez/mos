/**
 * @file uart.c
 *
 * @brief implementation of the universal asynchronous receiver transmitter (serial port)
 *
 * @author OT
 *
 * @date Feb 2017
 *
 */

#include <stm32f4xx_conf.h>
#include "hal.h"
#include "uart_hw.h"
#include "gpio_hw.h"
#include "dma_hw.h"


#define MIN(a,b) ((a<b)?a:b)


static uart_t *uart_irq_list[5] = {NULL,};  ///< just store the uart handle so we can get it in the irq (then hw.c is more free form)
static uint8_t uart_irq(uart_t *uart)
{
	switch ((uint32_t)uart->channel)
	{
		case (uint32_t)USART1:
			uart_irq_list[0] = uart;
			return USART1_IRQn;
		case (uint32_t)USART2:
			uart_irq_list[1] = uart;
			return USART2_IRQn;
		case (uint32_t)USART3:
			uart_irq_list[2] = uart;
			return USART3_IRQn;
		case (uint32_t)UART4:
			uart_irq_list[3] = uart;
			return UART4_IRQn;
		case (uint32_t)UART5:
			uart_irq_list[4] = uart;
			return UART5_IRQn;
		default:
			///@todo error
			return 0x00;
	}
}


static void uart_clear_read(uart_t *uart)
{
	// disable the read isr's
	if (uart->rx_dma)
	{
		dma_cancel(uart->rx_dma);
		USART_DMACmd(uart->channel, USART_DMAReq_Rx, DISABLE);
	}
	USART_ITConfig(uart->channel, USART_IT_RXNE, DISABLE);

	// clear the buffers for next read
	uart->read_buf = NULL;
	uart->read_buf_len = 0;
	uart->read_count = 0;
	uart->read_complete_cb = NULL;
	uart->read_complete_param = NULL;
}


static void uart_clear_write(uart_t *uart)
{
	// disable the write isr
	if (uart->tx_dma)
	{
		dma_cancel(uart->tx_dma);
		USART_DMACmd(uart->channel, USART_DMAReq_Tx, DISABLE);
	}
	USART_ITConfig(uart->channel, USART_IT_TXE, DISABLE);

	// clear the buffers for next read
	uart->write_buf = NULL;
	uart->write_buf_len = 0;
	uart->write_count = 0;
	uart->write_complete_cb = NULL;
	uart->write_complete_param = NULL;
}


static void uart_rx_dma_complete(dma_request_t *req, void *param)
{
	uart_t *uart = (uart_t *)param;
	void *buf = uart->read_buf;
	int16_t len = uart->read_buf_len;
	uart_read_complete_cb read_complete_cb = uart->read_complete_cb;
	void *read_complete_param = uart->read_complete_param;

	uart_clear_read(uart);
	if (read_complete_cb != NULL)
		read_complete_cb(uart, buf, len, read_complete_param);
}


static void uart_tx_dma_complete(dma_request_t *req, void *param)
{
	uart_t *uart = (uart_t *)param;
	void *buf = uart->write_buf;
	int16_t len = uart->write_buf_len;
	uart_write_complete_cb write_complete_cb = uart->write_complete_cb;
	void *write_complete_param = uart->write_complete_param;

	uart_clear_write(uart);
	if (write_complete_cb != NULL)
		write_complete_cb(uart, buf, len, write_complete_param);
}


static void uart_irq_handler(uart_t *uart)
{
	// buffer the current transaction so we can clear the uart ready for a new
	// read/write before calling the completion routines (that was a complete
	// event can call another read/write)
	void *read_buf = uart->read_buf;
	int16_t read_count;
	uart_read_complete_cb read_complete_cb = NULL;
	void *read_complete_param = uart->read_complete_param;

	void *write_buf = uart->write_buf;
	int16_t write_count;
	uart_write_complete_cb write_complete_cb = NULL;
	void *write_complete_param = uart->write_complete_param;

	// sanity check that we setup this interrupt
	if (uart == NULL)
		return;

	// if the receive buffer is full copy it to read_buf
	if (USART_GetITStatus(uart->channel, USART_IT_RXNE) &&
		uart->read_buf != NULL && uart->read_count < uart->read_buf_len)
	{
		uint16_t rx = USART_ReceiveData(uart->channel);
		uint32_t word_len = uart->cfg.USART_WordLength;

		// copy received word into read_buf (if were using 8bits
		// then pack it as bytes, else use 16 bit words)
		if (word_len == USART_WordLength_9b)
			((uint16_t *)uart->read_buf)[uart->read_count++] = rx;
		else
			((uint8_t *)uart->read_buf)[uart->read_count++] = (uint8_t)(rx & 0xff);

		// if the read_buf is full then read is complete
		if (uart->read_count == uart->read_buf_len)
		{
			read_count = uart->read_count;
			read_complete_cb = uart->read_complete_cb;
			uart_clear_read(uart);
		}
	}

	// if the transmit buffer empty & do we have
	// more to send then populate it
	if (USART_GetITStatus(uart->channel, USART_IT_TXE) &&
		uart->write_buf != NULL && uart->write_count < uart->write_buf_len)
	{
		uint16_t tx;
		uint32_t word_len = uart->cfg.USART_WordLength;

		// transmit next word, assume the buffer is packed as 16bits
		// if word length is 9, otherwise just assume byte packing
		// as is normal
		if (word_len == USART_WordLength_9b)
			tx = ((uint16_t *)uart->write_buf)[uart->write_count++];
		else
			tx = ((uint8_t *)uart->write_buf)[uart->write_count++];
		if (uart->write_count == uart->write_buf_len)
			USART_ITConfig(uart->channel, USART_IT_TC, ENABLE);
		USART_SendData(uart->channel, tx);
	}

	// if the transmitter completed the last byte then the write is done
	if (USART_GetITStatus(uart->channel, USART_IT_TC) &&
		uart->write_buf != NULL && uart->write_count == uart->write_buf_len)
	{
		USART_ITConfig(uart->channel, USART_IT_TC, DISABLE);
		write_count = uart->write_count;
		write_complete_cb = uart->write_complete_cb;
		uart_clear_write(uart);
	}

	// run deferred read/write callbacks
	if (write_complete_cb != NULL)
		write_complete_cb(uart, write_buf, write_count, write_complete_param);
	if (read_complete_cb != NULL)
		read_complete_cb(uart, read_buf, read_count, read_complete_param);
}


void USART1_IRQHandler(void)
{
	uart_irq_handler(uart_irq_list[0]);
}


void USART2_IRQHandler(void)
{
	uart_irq_handler(uart_irq_list[1]);
}


void USART3_IRQHandler(void)
{
	uart_irq_handler(uart_irq_list[2]);
}

void UART4_IRQHandler(void)
{
	uart_irq_handler(uart_irq_list[3]);
}
void UART5_IRQHandler(void)
{
	uart_irq_handler(uart_irq_list[4]);
}

#define UART_DMA_DIR_RX 0
#define UART_DMA_DIR_TX 1
static uint32_t __NULL;
static void uart_dma_cfg(uart_t *uart, int dir, dma_request_t *dma_req, void *buf, uint16_t len)
{
	DMA_InitTypeDef *uart_cfg = &dma_req->st_dma_init;
	USART_TypeDef *channel = uart->channel;
	dma_t *dma = dma_req->dma;

	uart_cfg->DMA_Channel = dma->channel;
	uart_cfg->DMA_PeripheralBaseAddr = dir ? (uint32_t)&channel->DR: (uint32_t)&channel->DR;
	uart_cfg->DMA_Memory0BaseAddr = (uint32_t)buf;
	uart_cfg->DMA_MemoryInc = DMA_MemoryInc_Enable;
	if (buf == NULL)
	{
		// this NULL is a possible security hole as multiple io may read/write to
		// this, it is only really for dbg and a real buffer should really be used
		// at all times
		uart_cfg->DMA_Memory0BaseAddr = (uint32_t)&__NULL;
		uart_cfg->DMA_MemoryInc = DMA_MemoryInc_Disable;
	}
	uart_cfg->DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	uart_cfg->DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
	uart_cfg->DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
	uart_cfg->DMA_Mode = DMA_Mode_Normal;
	uart_cfg->DMA_Priority = DMA_Priority_High;
	uart_cfg->DMA_FIFOMode = DMA_FIFOMode_Disable;
	uart_cfg->DMA_MemoryBurst = DMA_MemoryBurst_Single;
	uart_cfg->DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
	uart_cfg->DMA_BufferSize = len;
	uart_cfg->DMA_DIR = dir ? DMA_DIR_MemoryToPeripheral: DMA_DIR_PeripheralToMemory;
}


void uart_read(uart_t *uart, void *buf, uint16_t len, uart_read_complete_cb cb, void *param)
{
	// sanity checks
	if (len < 1)
		///@todo invalid input parameters
		return;

	sys_enter_critical_section();   // lock while changing things so an isr does not find a half setup read

	if (uart->read_buf != NULL || uart->read_count != 0)
		///@todo read in progress already
		goto done;

	// load the read info
	uart->read_buf = buf;
	uart->read_buf_len = len;
	uart->read_count = 0;
	uart->read_complete_cb = cb;
	uart->read_complete_param = param;

	// enable rx dma or interrupt to kick off the read
	if (uart->rx_dma)
	{
		uart->rx_dma_req.complete = uart_rx_dma_complete;
		uart->rx_dma_req.complete_param = uart;
		uart->rx_dma_req.dma = uart->rx_dma;
		uart_dma_cfg(uart, UART_DMA_DIR_RX, &uart->rx_dma_req, uart->read_buf, uart->read_buf_len);
		USART_DMACmd(uart->channel, USART_DMAReq_Rx, ENABLE);
		dma_request(&uart->rx_dma_req);
	}
	else
	{
		USART_ITConfig(uart->channel, USART_IT_RXNE, ENABLE);
	}
done:
	sys_leave_critical_section();
	return;
}


int uart_read_count(uart_t *uart)
{
	if (uart->read_buf_len == 0)
		// no read in progress
		return -1;

	if (uart->rx_dma)
		return uart->read_buf_len - dma_remaining(&uart->rx_dma_req);
	else
		return uart->read_count;
}


void uart_cancel_read(uart_t *uart)
{
	sys_enter_critical_section();
	uart_clear_read(uart);
	sys_leave_critical_section();
}


void uart_write(uart_t *uart, void *buf, uint16_t len, uart_write_complete_cb cb, void *param)
{
	// sanity checks
	if (len < 1)
		///@todo invalid input parameters
		return;

	sys_enter_critical_section();   // lock while changing things so an isr does not find a half setup write

	if (uart->write_buf != NULL || uart->write_count != 0)
		///@todo write in progress already
		goto done;

	// load the write info
	uart->write_buf = buf;
	uart->write_buf_len = len;
	uart->write_count = 0;
	uart->write_complete_cb = cb;
	uart->write_complete_param = param;

	// enable tx dma or interrupts to kick off the write
	if (uart->tx_dma)
	{
		uart->tx_dma_req.complete = uart_tx_dma_complete;
		uart->tx_dma_req.complete_param = uart;
		uart->tx_dma_req.dma = uart->tx_dma;
		uart_dma_cfg(uart, UART_DMA_DIR_TX, &uart->tx_dma_req, uart->write_buf, uart->write_buf_len);
		USART_DMACmd(uart->channel, USART_DMAReq_Tx, ENABLE);
		dma_request(&uart->tx_dma_req);
	}
	else
		USART_ITConfig(uart->channel, USART_IT_TXE, ENABLE);

done:
	sys_leave_critical_section();
	return;
}


int uart_write_count(uart_t *uart)
{
	if (uart->write_buf_len == 0)
		// no write in progress
		return -1;

	if (uart->tx_dma)
		return uart->write_buf_len - dma_remaining(&uart->tx_dma_req);
	else
		return uart->write_count;
}


void uart_cancel_write(uart_t *uart)
{
	sys_enter_critical_section();
	uart_clear_write(uart);
	sys_leave_critical_section();
}

void uart_deinit(uart_t *uart) 
{
	// clear existing dma transfers
	uart_cancel_write(uart);
	uart_cancel_read(uart);
	
	NVIC_InitTypeDef nvic_init;
	USART_Cmd(uart->channel, DISABLE);

	// disable the uart clk
	switch ((uint32_t)uart->channel)
	{
		case (uint32_t)USART1:
			RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, DISABLE);
			break;
		case (uint32_t)USART2:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, DISABLE);
			break;
		case (uint32_t)USART3:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, DISABLE);
			break;
		case (uint32_t)UART4:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_UART4, DISABLE);
			break;
		case (uint32_t)UART5:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_UART5, DISABLE);
			break;
	}

	// set the uart isr disabled
	nvic_init.NVIC_IRQChannel = uart_irq(uart);
	nvic_init.NVIC_IRQChannelCmd = DISABLE;
	NVIC_Init(&nvic_init);

	// deinitialize uart
	USART_DeInit(uart->channel);
}

void uart_init(uart_t *uart)
{
	NVIC_InitTypeDef nvic_init;
	// init the uart gpio lines
	gpio_init_pin(uart->rx);
	gpio_init_pin(uart->tx);

	// init the uart clk
	switch ((uint32_t)uart->channel)
	{
		case (uint32_t)USART1:
			RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
			break;
		case (uint32_t)USART2:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
			break;
		case (uint32_t)USART3:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
			break;
		case (uint32_t)UART4:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_UART4, ENABLE);
			break;
		case (uint32_t)UART5:
			RCC_APB1PeriphClockCmd(RCC_APB1Periph_UART5, ENABLE);
			break;
	}

	// setup the uart isr
	nvic_init.NVIC_IRQChannelCmd = ENABLE;
	nvic_init.NVIC_IRQChannel = uart_irq(uart);
	nvic_init.NVIC_IRQChannelPreemptionPriority = uart->preemption_priority;
	nvic_init.NVIC_IRQChannelSubPriority = 0;
	nvic_init.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&nvic_init);

	// init dma
	if (uart->rx_dma)
		dma_init(uart->rx_dma);
	if (uart->tx_dma)
		dma_init(uart->tx_dma);

	// set uart and enable
	USART_Init(uart->channel, &uart->cfg);
	USART_Cmd(uart->channel, ENABLE);
}

void uart_set_baudrate(uart_t *uart, uint32_t baud)
{
	USART_TypeDef* USARTx = uart->channel;
	uint32_t tmpreg = 0x00, apbclock = 0x00;
	uint32_t integerdivider = 0x00;
	uint32_t fractionaldivider = 0x00;
	RCC_ClocksTypeDef RCC_ClocksStatus;
	
	/* Configure the USART Baud Rate */
	RCC_GetClocksFreq(&RCC_ClocksStatus);

	if ((USARTx == USART1) || (USARTx == USART6))
	{
		apbclock = RCC_ClocksStatus.PCLK2_Frequency;
	}
	else
	{
		apbclock = RCC_ClocksStatus.PCLK1_Frequency;
	}

	/* Determine the integer part */
	if ((USARTx->CR1 & USART_CR1_OVER8) != 0)
	{
		/* Integer part computing in case Oversampling mode is 8 Samples */
		integerdivider = ((25 * apbclock) / (2 * (baud)));    
	}
	else /* if ((USARTx->CR1 & USART_CR1_OVER8) == 0) */
	{
		/* Integer part computing in case Oversampling mode is 16 Samples */
		integerdivider = ((25 * apbclock) / (4 * (baud)));    
	}

	tmpreg = (integerdivider / 100) << 4;

	/* Determine the fractional part */
	fractionaldivider = integerdivider - (100 * (tmpreg >> 4));

	/* Implement the fractional part in the register */
	if ((USARTx->CR1 & USART_CR1_OVER8) != 0)
	{
		tmpreg |= ((((fractionaldivider * 8) + 50) / 100)) & ((uint8_t)0x07);
	}
	else /* if ((USARTx->CR1 & USART_CR1_OVER8) == 0) */
	{
		tmpreg |= ((((fractionaldivider * 16) + 50) / 100)) & ((uint8_t)0x0F);
	}
  
	/* Write to USART BRR register */
	USARTx->BRR = (uint16_t)tmpreg;
}
