// Copyright (C) 2018 MS-Cheminformatics LLC

#include <atomic>
#include <cstdint>

namespace stm32f103 {

    template< typename T > class gpio;
    
    enum SPI_BASE : uint32_t;
    struct SPI;

    class dma;

    class spi {
        volatile SPI * spi_;
        std::atomic_flag lock_;
        std::atomic< uint32_t > rxd_;
        std::atomic< uint32_t > txd_;
        
        // workaround -- Initially, I thought GPIO and SPI controls are fully independent,
        // bit this peripheral seems exepecting a ~ss line control by software using GPIO.
        // I've designed GPIO control based on c++ type dispatch, but not for SPI.
        uint8_t gpio_;  // A|B|none
        uint32_t ss_n_;  // PA4|PB
        uint32_t cr1_;
        dma * dma_;
        uint32_t dma_channel_;
    public:
        void init( SPI_BASE, uint8_t gpio = 0, uint32_t ss_n = 0 );
        void slave_init( SPI_BASE );

        void init( SPI_BASE, dma& );

        inline operator bool () const { return spi_; };
        
        spi& operator << ( uint16_t );
        spi& operator >> ( uint16_t& );
        
        void operator = ( bool flag ); // SS control

        void handle_interrupt();
        static void interrupt_handler( spi * );
    };

    template< typename GPIO_PIN_type >
    class spi_t : public spi {
        GPIO_PIN_type ss_n_;
    public:
        template< GPIO_PIN_type > void init( SPI_BASE, GPIO_PIN_type ss_n );
    };
}

