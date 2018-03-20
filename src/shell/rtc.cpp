// Copyright (C) 2018 MS-Cheminformatics LLC
// Licence: CC BY-NC
// Author: Toshinobu Hondo, Ph.D.
// Contact: toshi.hondo@qtplatz.com
//

#include "bitset.hpp"
#include "condition_wait.hpp"
#include "debug_print.hpp"
#include "rcc.hpp"
#include "rtc.hpp"
#include "stream.hpp"
#include "stm32f103.hpp"
#include <bitset>
#include <cstdint>

extern "C" {
    void __rtc_handler();
    void enable_interrupt( stm32f103::IRQn_type IRQn );
}

extern std::atomic< uint32_t > atomic_jiffies;          //  100us  (4.97 days)
extern std::atomic< uint32_t > atomic_seconds;

namespace stm32f103 {
    struct RTC {
        uint32_t CRH;   // 0x00
        uint32_t CRL;   // 0x04
        uint32_t PRLH;  // 0x08
        uint32_t PRLL;  // 0x0c
        uint32_t DIVH;  // 0x10
        uint32_t DIVL;  // 0x14
        uint32_t CNTH;  // 0x18
        uint32_t CNTL;  // 0x1c
        uint32_t ALRH;  // 0x20
        uint32_t ALRL;  // 0x24
    };

    constexpr static const char * rtc_register_names [] = {
        "CRH", "CRL", "PRLH", "PRLL", "DIVH", "DIVL", "CNTH", "CNTL", "ALRH", "ALRL"
    };

    // Table 15, PWR register map, p79
    struct PWR {
        uint32_t CR;
        uint32_t CSR;
    };

    /*----------------------------------------------------------------------------
      RTC
      *----------------------------------------------------------------------------*/
    enum RTC_CRL {
        RTC_CRL_SECF          	= 0x00000001
        , RTC_CRL_ALRF          	= 1 << 1 //0x00000002
        , RTC_CRL_OWF           	= 1 << 2 //0x00000004
        , RTC_CRL_RSF           	= 1 << 3 //0x00000008
        , RTC_CRL_CNF           	= 1 << 4 //0x00000010
        , RTC_CRL_RTOFF         	= 1 << 5 //0x00000020
    };

    // Reference: http://embedded-lab.com/blog/stm32s-internal-rtc/
    
    enum rtc_clock_source {
        rtc_clock_source_none  = 0x000
        , rtc_clock_source_lsi = 0x100
        , rtc_clock_source_lse = 0x200 
        , rtc_clock_source_hse = 0x300
    };

    template< rtc_clock_source > struct rtc_clock { constexpr static uint32_t clk = 0; };
    template<> struct rtc_clock< rtc_clock_source_lsi > { constexpr static uint32_t clk = 32000; }; // 32kHz
    template<> struct rtc_clock< rtc_clock_source_lse > { constexpr static uint32_t clk = 32768; }; // 32.768kHz
    template<> struct rtc_clock< rtc_clock_source_hse > { constexpr static uint32_t clk = 8000000/128; }; // 8MHz/128

    // it seems that HSE and LSE clock not working on STM32F103C8 Blue Pills
    //constexpr rtc_clock_source clock_source = rtc_clock_source_hse;
    //constexpr rtc_clock_source clock_source = rtc_clock_source_lse;
    constexpr rtc_clock_source clock_source = rtc_clock_source_lsi;
    
    template< rtc_clock_source > struct rtc_clock_enabler {
        static bool enable() {}
    };

    template<> struct rtc_clock_enabler< rtc_clock_source_lsi > {
        static bool enable() {
            // clock source (LSI)
            auto RCC = reinterpret_cast< volatile stm32f103::RCC * >( stm32f103::RCC_BASE );
            stm32f103::bitset::set( RCC->CSR, stm32f103::RCC_CSR_LSION );
            if ( !condition_wait()( [&](){ return RCC->CSR & stm32f103::RCC_CSR_LSIRDY; } ) )
                stream(__FILE__,__LINE__,__FUNCTION__) << "\t##### time out #####\n";
        }
    };    

    template<> struct rtc_clock_enabler< rtc_clock_source_lse > {
        static bool enable() {
            // clock source (LSE)            
            auto RCC = reinterpret_cast< volatile stm32f103::RCC * >( stm32f103::RCC_BASE );
            stm32f103::bitset::set( RCC->BDCR, stm32f103::RCC_BDCR_LSEON );
            if ( ! condition_wait()( [&](){ return RCC->BDCR & stm32f103::RCC_BDCR_LSERDY; } ) )
                stream(__FILE__,__LINE__,__FUNCTION__) << "\t##### time out #####\n";
        }
    };    
    

    constexpr uint32_t __rtc_period = 1000;
    // constexpr uint32_t __rtc_time_h = 12;
    // constexpr uint32_t __rtc_time_m = 0;
    // constexpr uint32_t __rtc_time_s = 0;
    // constexpr uint32_t __rtc_cnt_tics = __rtc_time_h * 3600ul + __rtc_time_m * 60ul + __rtc_time_s;
    // constexpr uint32_t __rtc_cnt = __rtc_cnt_tics * 1000ul/__rtc_period;
    // constexpr uint32_t __rtc_alarm_h = 12;
    // constexpr uint32_t __rtc_alarm_m = 0;
    // constexpr uint32_t __rtc_alarm_s = 0;
    // constexpr uint32_t __rtc_alr_tics = __rtc_alarm_h * 3600ul + __rtc_alarm_m * 60ul + __rtc_alarm_s;
    // constexpr uint32_t __rtc_alr = __rtc_alr_tics * 1000ul/__rtc_period;
}

using namespace stm32f103;

rtc::rtc()
{
}

// static
void
rtc::init()
{
}

void
rtc::reset()
{
    if ( auto RCC = reinterpret_cast< volatile stm32f103::RCC * >( stm32f103::RCC_BASE ) ) {
        stm32f103::bitset::set( RCC->BDCR, (1 << 16) );
        stm32f103::bitset::reset( RCC->BDCR, (1 << 16) );
    }
}

bool
rtc::enable()
{
    if ( auto RCC = reinterpret_cast< volatile stm32f103::RCC * >( stm32f103::RCC_BASE ) ) {
        using namespace stm32f103;
        RCC->APB1ENR |= RCC_APB1ENR_PWREN;

        // p541, section 5.4
        if ( auto PWR = reinterpret_cast< volatile stm32f103::PWR * >( stm32f103::PWR_BASE ) )
            PWR->CR |= 0x100; // enable access to RTC, BDC registers

        rtc_clock_enabler< clock_source >::enable();
        
        // rtcsel [9:8], p149
        // 00 no clock, 01 := LSE, 10 := LSI, 11 := HSE / 128
        stm32f103::bitset::set( RCC->BDCR, clock_source | RCC_BDCR_RTCEN );  // set RTC clock source, enable RTC clock
    }

    if ( auto RTC = reinterpret_cast< volatile stm32f103::RTC * >( stm32f103::RTC_BASE ) ) {
        using namespace stm32f103;

        stm32f103::bitset::set( RTC->CRL, RTC_CRL_CNF );      // set configuration mode

        RTC->PRLH  = ( (rtc_clock< clock_source >::clk - 1) >> 16) & 0x00ff;  // set prescaler load register high
        RTC->PRLL  =   (rtc_clock< clock_source >::clk - 1)        & 0xffff;  // set prescaler load register low

        RTC->CRH = 3;                                         // enable RTC alarm, second interrupts
        enable_interrupt( RTC_IRQn );                         // enable interrupt
        
        stm32f103::bitset::reset( RTC->CRL, RTC_CRL_CNF );    // exit configuration mode

        if ( ! condition_wait()( [&](){ return ( RTC->CRL & RTC_CRL_RTOFF); } ) )
            stream(__FILE__,__LINE__,__FUNCTION__) << "\t##### time out #####\n";

        if ( auto PWR = reinterpret_cast< volatile stm32f103::PWR * >( stm32f103::PWR_BASE ) )
            stm32f103::bitset::reset( PWR->CR, 0x100 ); // disable access to RTC registers
    }

    return true;
}

int64_t
rtc::now() const
{
    auto RTC = reinterpret_cast< volatile stm32f103::RTC * >( stm32f103::RTC_BASE );
    return int64_t( RTC->CNTH ) << 26 | RTC->CNTL << 10 | ( RTC->DIVL * 1024 / rtc_clock< clock_source >::clk );
}

void
rtc::handle_interrupt() const
{
    if ( auto RTC = reinterpret_cast< volatile stm32f103::RTC * >( stm32f103::RTC_BASE ) ) {
        // stream(__FILE__,__LINE__,__FUNCTION__) << "rtc: " << int( RTC->CNTL ) << ":" << int( RTC->DIVL )
        //                                        << "\t" << now() << std::endl;
    }
}

rtc *
rtc::instance()
{
    static std::atomic_flag __once_flag;
    static rtc __instance;
    if ( !__once_flag.test_and_set() )
        __instance.init();
    return &__instance;
}

void
__rtc_handler()
{
    if ( auto RTC = reinterpret_cast< volatile stm32f103::RTC * >( stm32f103::RTC_BASE ) )
        stm32f103::bitset::reset( RTC->CRL, (RTC->CRL & 0x0003) );

    rtc::instance()->handle_interrupt();
}

void
rtc::print_registers()
{
    stream() << "See section 18.4, p486- on RM0008\n";
    auto * reg = reinterpret_cast< volatile uint32_t * >( stm32f103::RTC_BASE );
    for ( auto& name: rtc_register_names ) {
        std::bitset<16> bits( *reg );
        print()( stream(), bits, name ) << "\t" << *reg << std::endl;
        ++reg;
    }
}

