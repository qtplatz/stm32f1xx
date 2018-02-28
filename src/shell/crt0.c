/*
 * taken from: adc.c, https://github.com/fcayci/stm32f1-bare-metal.git
 * modified for stm32fxx c++ trial project
 * by Toshinobu Hondo, Ph.D.  FEB 2018
 */

#include <stdint.h>
#include "stm32f103.hpp"

#define NVIC            ((NVIC_type  *)  NVIC_BASE)

uint64_t jiffies;

// Function declarations. Add your functions here
void enable_interrupt(IRQn_type IRQn);
void disable_interrupt(IRQn_type IRQn);
void set_system_clock_to_25Mhz(void);
void set_system_clock_to_72Mhz(void);
void __adc1_handler(void);
void __can1_tx_handler(void);
void __can1_rx0_handler(void);
void __can1_rx1_handler(void);
void __can1_sce_handler(void);
void __spi1_handler(void);
void __spi2_handler(void);
void __usart1_handler(void);
void __systick_handler(void);
void __main(void);

/*************************************************
* Vector Table
*************************************************/
// Attribute puts table in beginning of .vector section
//   which is the beginning of .text section in the linker script
// Add other vectors in order here
// Vector table can be found on page 197 in RM0008

void ( * const vector_table [] )() __attribute__ ((section(".vect"))) = {
	(void (*)()) STACKINIT,         /* 0x000 Stack Pointer                   */
	__main,                         /* 0x004 Reset                           */
	0,                              /* 0x008 Non maskable interrupt          */
	0,                              /* 0x00C HardFault                       */
	0,                              /* 0x010 Memory Management               */
	0,                              /* 0x014 BusFault                        */
	0,                              /* 0x018 UsageFault                      */
	0,                              /* 0x01C Reserved                        */
	0,                              /* 0x020 Reserved                        */
	0,                              /* 0x024 Reserved                        */
	0,                              /* 0x028 Reserved                        */
	0,                              /* 0x02C System service call             */
	0,                              /* 0x030 Debug Monitor                   */
	0,                              /* 0x034 Reserved                        */
	0,                              /* 0x038 PendSV                          */
	__systick_handler,              /* 0x03C System tick timer               */
	0,                              /* 0x040 Window watchdog                 */
	0,                              /* 0x044 PVD through EXTI Line detection */
	0,                              /* 0x048 Tamper                          */
	0,                              /* 0x04C RTC global                      */
	0,                              /* 0x050 FLASH global                    */
	0,                              /* 0x054 RCC global                      */
	0,                              /* 0x058 EXTI Line0                      */
	0,                              /* 0x05C EXTI Line1                      */
	0,                              /* 0x060 EXTI Line2                      */
	0,                              /* 0x064 EXTI Line3                      */
	0,                              /* 0x068 EXTI Line4                      */
	0,                              /* 0x06C DMA1_Ch1                        */
	0,                              /* 0x070 DMA1_Ch2                        */
	0,                              /* 0x074 DMA1_Ch3                        */
	0,                              /* 0x078 DMA1_Ch4                        */
	0,                              /* 0x07C DMA1_Ch5                        */
	0,                              /* 0x080 DMA1_Ch6                        */
	0,                              /* 0x084 DMA1_Ch7                        */
	__adc1_handler,                 /* 0x088 ADC1 and ADC2 global            */
	__can1_tx_handler,              /* 0x08C CAN1_TX                         */
	__can1_rx0_handler,             /* 0x090 CAN1_RX0                        */
	__can1_rx1_handler,             /* 0x094 CAN1_RX1                        */
	__can1_sce_handler,             /* 0x098 CAN1_SCE                        */
	0,                              /* 0x09C EXTI Lines 9:5                  */
	0,                              /* 0x0A0 TIM1 Break                      */
	0,                              /* 0x0A4 TIM1 Update                     */
	0,                              /* 0x0A8 TIM1 Trigger and Communication  */
	0,                              /* 0x0AC TIM1 Capture Compare            */
	0,                              /* 0x0B0 TIM2                            */
	0,                              /* 0x0B4 TIM3                            */
	0,                              /* 0x0B8 TIM4                            */
	0,                              /* 0x0BC I2C1 event                      */
	0,                              /* 0x0C0 I2C1 error                      */
	0,                              /* 0x0C4 I2C2 event                      */
	0,                              /* 0x0C8 I2C2 error                      */
	__spi1_handler,                 /* 0x0CC SPI1                            */
	__spi2_handler,                 /* 0x0D0 SPI2                            */
	__usart1_handler,               /* 0x0D4 USART1                          */
	0,                              /* 0x0D8 USART2                          */
	0,                              /* 0x0DC USART3                          */
	0,                              /* 0x0E0 EXTI Lines 15:10                */
	0,                              /* 0x0E4 RTC alarm through EXTI line     */
	0,                              /* 49  USB OTG FS Wakeup through EXTI  */
	0,                              /* -   Reserved                        */
	0,                              /* -   Reserved                        */
	0,                              /* -   Reserved                        */
	0,                              /* -   Reserved                        */
	0,                              /* -   Reserved                        */
	0,                              /* -   Reserved                        */
	0,                              /* -   Reserved                        */
	0,                              /* 57  TIM5                            */
	0,                              /* 58  SPI3                            */
	0,                              /* 59  USART4                          */
	0,                              /* 60  USART5                          */
	0,                              /* 61  TIM6                            */
	0,                              /* 62  TIM7                            */
	0,                              /* 63  DMA2_Ch1                        */
	0,                              /* 64  DMA2_Ch2                        */
	0,                              /* 65  DMA2_Ch3                        */
	0,                              /* 66  DMA2_Ch4                        */
	0,                              /* 67  DMA2_Ch5                        */
	0,                              /* 68  Ethernet                        */
	0,                              /* 69  Ethernet wakeup                 */
	0,                              /* 70  CAN2_TX                         */
	0,                              /* 71  CAN2_RX0                        */
	0,                              /* 72  CAN2_RX1                        */
	0,                              /* 73  CAN2_SCE                        */
	0,                              /* 74  USB OTG FS                      */
};

/*
 * Enable given interrupt
 *
 * Each ISER {0-7} holds 32 interrupts. Thus take mod32 of the given interrupt
 *   to choose the ISER number (ISER[0] for IRQn 0-31, and ISER[1] for IRQn 32-63 ..)
 *   Then, enable the given bit on that register based on the remainder of the mod.
 */
void
enable_interrupt(IRQn_type IRQn)
{
	NVIC->ISER[((uint32_t)(IRQn) >> 5)] = (1 << ((uint32_t)(IRQn) & 0x1f));
}

void
disable_interrupt(IRQn_type IRQn)
{
	NVIC->ICER[((uint32_t)(IRQn) >> 5)] = (1 << ((uint32_t)(IRQn) & 0x1f));
}

void
__adc1_handler(void)
{
    adc1_handler();
}

void
__can1_tx_handler(void)
{
    can1_tx_handler();
}

void
__can1_rx0_handler(void)
{
    can1_rx0_handler();
}

void
__can1_rx1_handler(void)
{
}
    
void
__can1_sce_handler(void)
{
}

void
__spi1_handler(void)
{
    spi1_handler();
}

void
__spi2_handler(void)
{
    spi2_handler();
}

void
__usart1_handler(void)
{
    uart1_handler();
}

void
__systick_handler( void )
{
    ++jiffies;
    systick_handler();
}

void
__main(void)
{
    jiffies = 0;
    main();
}
