// Copyright (C) 2018 MS-Cheminformatics LLC
// Licence: CC BY-NC
// Author: Toshinobu Hondo, Ph.D.
// Contact: toshi.hondo@qtplatz.com
//

#include "dma.hpp"
#include "dma_channel.hpp"
#include "i2c.hpp"
#include "i2cdebug.hpp"
#include "stream.hpp"
#include "stm32f103.hpp"
#include <array>
#include <atomic>
#include <mutex>

extern uint32_t __pclk1, __pclk2;
extern stm32f103::i2c __i2c0;
extern void mdelay( uint32_t );
extern std::atomic< uint32_t > atomic_jiffies;

extern "C" {
    void i2c1_handler();
    void enable_interrupt( stm32f103::IRQn_type IRQn );
}

// bits in the status register
namespace stm32f103 {
    enum I2C_CR1_MASK {
        SWRST          = 1 << 15  // Software reset (0 := not under reset, 0 := under reset state
        , RES0         = 1 << 14  //
        , ALERT        = 1 << 13  //
        , PEC          = 1 << 12  //
        , POS          = 1 << 11  //
        , ACK          = 1 << 10  //
        , STOP         = 1 <<  9  //
        , START        = 1 <<  8  //
        , NOSTRETCH    = 1 <<  7  //
        , ENGC         = 1 <<  6  //
        , ENPEC        = 1 <<  5  //
        , ENARP        = 1 <<  4  //
        , SMBTYPE      = 1 <<  3  //
        , RES1         = 1 <<  2  //
        , SMBUS        = 1 <<  1  //
        , PE           = 1 <<  0  // Peripheral enable
    };

    enum I2C_CR2_MASK {
        LAST          = 1 << 12
        , DMAEN       = 1 << 11    // DMA requests enable
        , ITBUFFN     = 1 << 10    // Buffer interrupt enable
        , ITEVTEN     = 1 <<  9    // Event interrupt enable
        , ITERREN     = 1 <<  8    // Error interrupt enable
        , FREQ        = 0x3f       // Peirpheral clock frequence ( 0x02(2MHz) .. 0x32(50MHz) )
    };

   // p782
    enum I2C_STATUS {
        // SR2, p782
        ST_PEC          = 0xff << 8  // Packet error checking rigister
        , DUALF         = 1 << 7     // Dual flag (slave mode)
        , SMBHOST       = 1 << 6     // SMBus host header (slave mode)
        , SMBDEFAULT    = 1 << 5     // SMB deault, SMBus device default address (slave mode)
        , GENCALL       = 1 << 4     // General call address (slave mode)
        , TRA           = 1 << 2     // Transmitter/receiver (0: data bytes received, 1: data bytes transmitted)
        , BUSY          = 1 << 1     // Bus busy
        , MSL           = 1          // Master/slave
        // SR1, p778
        , SMB_ALART     = 1 << 15    // SMBus alert
        , TIME_OUT      = 1 << 14    // Timeout or Tlow error
        , PEC_ERR       = 1 << 12    // PEC Error in reception
        , OVR           = 1 << 11    // Overrun/Underrun
        , AF            = 1 << 10    // Acknowledge failure
        , ARLO          = 1 << 9     // arbitration lost
        , BERR          = 1 << 8     // Bus error
        , TxE           = 1 << 7     // Data register empty (transmitter)
        , RxNE          = 1 << 6     // Date register not empty (receiver)
        , STOPF         = 1 << 4     // Stop detection (slave)
        , ADD10         = 1 << 3     // 10-bit header sent
        , BTF           = 1 << 2     // Byte transfer finished
        , ADDR          = 1 << 1     // Address sent (master), matched(slave)
        , SB            = 1          // Start bit (master mode); 1: start condition generated
    };

    struct i2c_status {
        volatile I2C& i2c;
        i2c_status( volatile I2C& t ) : i2c( t ) {}
        inline bool busy() const { return i2c.SR2 & BUSY; }

        // p783, Reading SR2 after reading SR1 clears ADDR flag.
        inline uint32_t status() const { return ( (i2c.SR1 & STOPF) == 0
                                                  || ((i2c.SR1 & ADDR) == ADDR ) ) ? ( i2c.SR2 << 16 ) | i2c.SR1 : i2c.SR1; }

        inline uint32_t operator ()() const { return status(); }

        inline bool is_equal( uint32_t flags ) const {
            return ( status() & flags ) == flags;
        }

        inline bool is_equal( uint32_t flags, uint32_t mask ) const {
            return ( status() & mask ) == flags;
        }
    };

    struct i2c_stop {
        inline bool operator()(volatile I2C& i2c ) const { i2c.CR1 |= STOP;  return true;  }
    };

    // master start
    struct i2c_start {
        volatile I2C& i2c;
        i2c_start( volatile I2C& t ) : i2c( t ) {}

        constexpr static uint32_t master_mode_selected = ( ( BUSY | MSL ) << 16 ) | SB;

        inline bool operator()() const {
            size_t count = 3;
            i2c_status st( i2c );

            while ( st.busy() && --count )
                mdelay(1);

            i2c.CR1 |= START;
                
            count = 3;
            while( !st.is_equal( master_mode_selected ) && --count )
                mdelay(1);

            return st.is_equal( master_mode_selected );
        }
    };

    enum I2C_DIRECTION { Transmitter, Receiver };

    template< I2C_DIRECTION >    
    struct i2c_address {
        inline bool operator()( volatile I2C& i2c, uint8_t address ) {
            
            i2c.DR = ( address << 1 );
            
            constexpr uint32_t byte_transmitting = (( TRA | BUSY | MSL) << 16 ) | TxE;
            i2c_status st( i2c );
            size_t count = 0x7fff;
            while( !st.is_equal( byte_transmitting ) && --count )
                ;
            return st.is_equal( byte_transmitting );
        }
    };

    template<> inline bool i2c_address<Receiver>::operator()( volatile I2C& i2c, uint8_t address ) {
        i2c.DR = (address << 1) | 1;
        return true;
    }

    // master transmit
    struct i2c_transmit {
        inline bool operator()( volatile I2C& i2c, uint8_t data ) {
            constexpr size_t byte_transferred = (( TRA | BUSY | MSL ) << 16) | TxE | BTF; // /*0x00070084*/

            i2c.DR = data;

            size_t count = 0x7fff;
            i2c_status st( i2c );
            while ( !st.is_equal( byte_transferred ) && --count )
                ;
            return st.is_equal( byte_transferred );
        }
    };

    // master receive
    struct i2c_receive {
        inline bool operator()( volatile I2C& i2c, uint8_t& data ) {
            constexpr size_t byte_received = (( BUSY | MSL ) << 16) | RxNE;
            size_t count = 100;
            i2c_status st( i2c );

            while ( !st.is_equal( byte_received ) && --count )
                mdelay(1);

            if ( st.is_equal( byte_received ) ) {
                data = i2c.DR;
                return true;
            }
            return false;
        }
    };
    
    template< bool > struct i2c_enable { inline bool operator()( volatile I2C& i2c ) {  i2c.CR1 |= PE;  return true;  } };
    template<> bool inline i2c_enable< false >::operator()( volatile I2C& i2c )  {  i2c.CR1 &= ~PE; return true;  }

    template< bool > struct i2c_dma_enable { inline bool operator()( volatile I2C& i2c ) {  i2c.CR2 |= DMAEN;  return true;  } };
    template<> bool inline i2c_dma_enable< false >::operator()( volatile I2C& i2c )  {  i2c.CR2 &= ~DMAEN; return true;  }    
}

using namespace stm32f103;
using namespace stm32f103::i2cdebug;

static uint8_t __i2c1_tx_dma[ sizeof( dma_channel_t< DMA_I2C1_TX > ) ];
static uint8_t __i2c1_rx_dma[ sizeof( dma_channel_t< DMA_I2C1_RX > ) ];
static uint8_t __i2c2_tx_dma[ sizeof( dma_channel_t< DMA_I2C2_TX > ) ];
static uint8_t __i2c2_rx_dma[ sizeof( dma_channel_t< DMA_I2C2_RX > ) ];

static dma_channel_t< DMA_I2C1_TX > * __dma_i2c1_tx;
static dma_channel_t< DMA_I2C1_RX > * __dma_i2c1_rx;
static dma_channel_t< DMA_I2C2_TX > * __dma_i2c2_tx;
static dma_channel_t< DMA_I2C2_RX > * __dma_i2c2_rx;

extern stm32f103::dma __dma0;

constexpr uint32_t i2c_clock_speed = 100'000; //'; //100kHz

i2c::i2c() : i2c_( 0 )
{
}

void
i2c::attach( dma& dma, DMA_Direction dir )
{
    uint32_t addr = reinterpret_cast< uint32_t >(const_cast< I2C * >(i2c_));

    stream() << "*********************** attach " << addr
             << " with dma (" << (dir == DMA_Tx ? "Tx" : ( dir == DMA_Rx ? "Rx" : "Both"))
             << ") **********************"
             << std::endl;

    if ( addr == I2C1_BASE ) {
        if ( dir == DMA_Rx || dir == DMA_Both )
            __dma_i2c1_rx = new (&__i2c1_rx_dma) dma_channel_t< DMA_I2C1_RX >( dma );            
        if ( dir == DMA_Tx || dir == DMA_Both )
            __dma_i2c1_tx = new (&__i2c1_tx_dma) dma_channel_t< DMA_I2C1_TX >( dma );
    } else if ( addr == I2C2_BASE ) {
        if ( dir == DMA_Rx || dir == DMA_Both )        
            __dma_i2c2_rx = new (&__i2c2_rx_dma) dma_channel_t< DMA_I2C2_RX >( dma );
        if ( dir == DMA_Tx || dir == DMA_Both )
            __dma_i2c2_tx = new (&__i2c2_tx_dma) dma_channel_t< DMA_I2C2_TX >( dma );            
    }
}

void
i2c::init( stm32f103::I2C_BASE addr )
{
    lock_.clear();
    rxd_ = 0;
    own_addr_ = ( addr == I2C1_BASE ) ? 0x03 : 0x04;

    if ( auto I2C = reinterpret_cast< volatile stm32f103::I2C * >( addr ) ) {
        i2c_ = I2C;
        
        reset();
        
        switch ( addr ) {
        case I2C1_BASE:
            enable_interrupt( I2C1_EV_IRQn );
            enable_interrupt( I2C1_ER_IRQn );
            break;
        case I2C2_BASE:
            enable_interrupt( I2C2_EV_IRQn );
            enable_interrupt( I2C2_ER_IRQn );            
            break;
        }
    }

}

void
i2c::setOwnAddr( uint8_t addr )
{
    own_addr_ = addr;
    i2c_->OAR1 = own_addr_ << 1;
}

uint8_t
i2c::ownAddr() const
{
    return uint8_t( i2c_->OAR1 >> 1 );
}

void
i2c::reset()
{
    i2c_enable< false >()( *i2c_ );
    i2c_->CR1 |= SWRST;
    while ( i2c_->SR1 && i2c_->SR2 )
        ;
    i2c_->CR1 &= ~SWRST;
    
    i2c_enable< false >()( *i2c_ );

    uint16_t freqrange = uint16_t( __pclk1 / 1000'000 /*'*/ ); // souce clk in MHz
    //uint16_t cr2 = freqrange; // | ITBUFFN |  ITEVTEN | ITERREN;
    uint16_t cr2 = freqrange; //| ITERREN;// | ITEVTEN;
    i2c_->CR2 |= cr2;
    
    // p784, 
    i2c_->TRISE = freqrange + 1;
    i2c_->OAR1 = own_addr_ << 1;
    i2c_->OAR2 = 0;
    
    uint16_t ccr = __pclk1 / (i2c_clock_speed * 2);  // ratio make it 5us for 10us interval clock
    i2c_->CCR = ccr;  // Sm mode
}

bool
i2c::start()
{
    if ( ( i2c_->CR1 & PE ) == 0 )
        i2c_enable<false>( *i2c_ )();
    return i2c_start( *i2c_ )();
}

bool
i2c::stop()
{
    return i2c_stop()( *i2c_ );
}

bool
i2c::disable()
{
    return i2c_enable<false>()( *i2c_ );
}

bool
i2c::enable()
{
    return i2c_enable<true>()( *i2c_ );
}

bool
i2c::dmaEnable( bool enable )
{
    if ( enable ) {
        i2c_->CR2 |= DMAEN;
    } else {
        i2c_->CR2 &= ~DMAEN;
    }
}

bool
i2c::hasDMA( bool receiving ) const
{
    if ( reinterpret_cast< uint32_t >(const_cast< I2C * >(i2c_)) == I2C1_BASE ) {
        if ( receiving )
            return __dma_i2c1_rx != nullptr;
        else
            return __dma_i2c1_tx != nullptr;
    } else if ( reinterpret_cast< uint32_t >(const_cast< I2C * >(i2c_)) == I2C2_BASE ) {
        if ( receiving )
            return __dma_i2c2_rx != nullptr;
        else
            return __dma_i2c2_tx != nullptr;
    }
    return false;
}

void
i2c::print_status() const
{
    stream() << "OAR1 : 0x" << i2c_->OAR1 << "\tOwn address: " << int(i2c_->OAR1 >> 1) << std::endl;
    stream() << "CCR  : {" << int( i2c_->CCR & 0x7ff ) << "}\t";
    stream() << "TRISE: {" << int( i2c_->TRISE ) << "(" << ((i2c_->TRISE&0x3f)-1)/(i2c_->CR2&0x3f) << "us)}" << std::endl;
    
    stream() << "CR1,2: [" << i2c_->CR1 << "," << i2c_->CR2 << "]\t" << CR1_to_string( i2c_->CR1 ) << "\t" << CR2_to_string( i2c_->CR2 ) << std::endl;
    stream() << "SR1,2: " << status32_to_string( i2c_status( *i2c_ )() ) << std::endl;

    int count = 10;
    while( --count && ( i2c_->SR2 & BUSY ) )
        mdelay( 1 );

    if ( count == 0 )
        stream() << "I2C keep busy -- check SDA line, it must be high" << std::endl;
}

    
bool
i2c::read( uint8_t address, uint8_t& data )
{
    i2c_status st( *i2c_ );
    i2c_enable< true >()( *i2c_ );

    constexpr uint32_t error_condition = SMB_ALART | TIME_OUT | PEC_ERR | OVR | AF | ARLO | BERR;
    
    size_t retry = 3;

    do {
        size_t count = 30;
        while ( ( st.busy() || ( st() & error_condition ) ) && --count ) {
            if ( ( i2c_->SR1 & error_condition ) && ( i2c_->SR2 & (BUSY| MSL) ) ) {
                reset();
            }
        }
    } while ( st.busy() && --retry );

    if ( st.busy() ) {
        stream() << "I2C is busy -- check SDA line, must be high" << std::endl;
        return false;
    }

    // Direction Transmitter = 0, Receiver = 1
    if ( i2c_start( *i2c_ )() ) {

        if ( i2c_address< Receiver >()( *i2c_, address ) ) {
            
            if ( i2c_receive()( *i2c_, data ) ) {
                if ( i2c_stop()( *i2c_ ) ) {
                    return true;
                } else {
                    stream() << "i2c::stop failed. " << status32_to_string( st.status() ) << std::endl;
                }
            } else {
                if ( i2c_->SR1 & (error_condition | BUSY | MSL) ) {
                    reset();
                    return false;
                } else 
                    stream() << "i2c::receive -- failed. " << status32_to_string( st.status() ) << std::endl;
            }
        } else {
            stream() << "i2c::address -- failed. " << status32_to_string( st.status() ) << std::endl;
        }
    } else {
        stream() << "i2c::start -- command failed. " << status32_to_string( st.status() ) << std::endl;
    }
    
    return false;
}

bool
i2c::write( uint8_t address, uint8_t data )
{
    i2c_status st( *i2c_ );

    i2c_->CR1 |= PE;

    size_t count = 100;
    while ( st.busy() && --count )
        ;

    if ( st.busy() ) {
        stream() << "I2C is busy -- check SDA line, must be high" << std::endl;
        return false;
    }

    if ( i2c_start( *i2c_ )() ) {

        if ( i2c_address< Transmitter >()( *i2c_, address ) ) {
            
            if ( i2c_transmit()( *i2c_, data ) ) {

                if ( i2c_stop()( *i2c_ ) ) {
                    return true;
                } else {
                    stream() << "i2c::stop failed. " << status32_to_string( st.status() ) << std::endl;
                }

            } else {
                stream() << "i2c::transmit -- failed. " << status32_to_string( st.status() ) << std::endl;
            }
        } else {
            stream() << "i2c::address -- failed. " << status32_to_string( st.status() ) << std::endl;
        }

    } else {
        stream() << "i2c::start -- command failed. " << status32_to_string( st.status() ) << std::endl;
    }
    
    return false;
}

namespace stm32f103 {

    struct dma_master_transfer {
        volatile I2C& i2c;
        dma_master_transfer( volatile I2C& _i2c ) : i2c( _i2c ) {}

        template< typename T >
        bool operator()( T& dma_channel, uint8_t address, const uint8_t * data, size_t size ) const {

            for ( size_t i = 0; i < size && i < sizeof( dma_channel.buffer.data ); ++i )
                dma_channel.buffer.data[ i ] = data[ i ];
            
            if ( i2c_start( i2c )() ) { // generate start condition (master start)
                stream() << "i2c_start - success: " << status32_to_string( i2c_status( i2c )() ) << std::endl;
                
                if ( i2c_address< Transmitter >()( i2c, address ) ) {
                    i2c_dma_enable< true >()( i2c );
                    dma_channel.enable( true );
                    size_t count = 0x7fff;
                    while ( --count && !dma_channel.transfer_complete() )
                        ;
                    return count != 0;
                } else {
                    stream() << "i2c::dma_master_transfer -- address phase failed: " << status32_to_string( i2c_status( i2c )() ) << std::endl;
                }
            } else {
                stream() << "i2c::dma_master_transfer() -- can't generate start condition" << std::endl;
            }
            return false;
        }
    };

    struct dma_slave_receive {
        volatile I2C& i2c;
        dma_slave_receive( volatile I2C& _i2c ) : i2c( _i2c ) {}
        template< typename T>
        size_t operator()( T& dma_channel, const uint8_t *& data ) const {
            data = dma_channel.buffer.data;
            i2c_dma_enable< true >()( i2c );
            dma_channel.enable( true );
            return sizeof( dma_channel.buffer.data );
        }
    };
        
}

bool
i2c::dma_transfer( uint8_t address, const uint8_t * data, size_t size )
{
    stream() << "i2c::dma_transfer(" << uint32_t( i2c_ ) << ")" << std::endl;

    i2c_status st( *i2c_ );
    i2c_->CR1 |= PE;
    size_t count = 100;
    while ( st.busy() && --count )
        ;
    if ( st.busy() ) {
        stream() << "I2C is busy" << std::endl;
        return false;
    }
    
    auto base_addr = reinterpret_cast< uint32_t >( const_cast< I2C * >(i2c_) );

    if ( base_addr == I2C1_BASE && __dma_i2c1_tx != nullptr ) {

        return dma_master_transfer( *i2c_ )( *__dma_i2c1_tx, address, data, size );
        
    } else if ( base_addr == I2C2_BASE && __dma_i2c2_tx != nullptr ) {

        return dma_master_transfer( *i2c_ )( *__dma_i2c2_tx, address, data, size );
    }

    return false;
}

size_t
i2c::dma_receive( uint8_t address, const uint8_t *& data )
{
    auto base_addr = reinterpret_cast< uint32_t >( const_cast< I2C * >(i2c_) );

    stream() << "i2c::dma_receiver address=" << base_addr
             << "\ti2c1 rx dma: " << (uint32_t)__dma_i2c1_rx
             << "\ti2c2 rx dma: " << (uint32_t)__dma_i2c2_rx 
             << std::endl;

    data = nullptr;

    i2c_status st( *i2c_ );
    i2c_->CR1 |= PE;

    // slave receive
    size_t count = 100;
    while ( --count && !st.is_equal( (BUSY << 16)|ADDR ) )  // 0x00020002, busy and addr (slave receiver address matched)
        mdelay(1);
    stream() << "i2c::dma_receive: " << status32_to_string(st()) << std::endl;
    count = 100;
    while ( --count && !st.is_equal( (BUSY|MSL|TRA) << 16 | TxE ) )
        ;
    stream() << "i2c::dma_receive: " << status32_to_string(st()) << std::endl;    
    
    if ( base_addr == I2C1_BASE && __dma_i2c1_rx != nullptr ) {

        return dma_slave_receive( *i2c_ )( *__dma_i2c1_rx, data );
        
    } else if ( base_addr == I2C2_BASE && __dma_i2c2_rx != nullptr ) {

        return dma_slave_receive( *i2c_ )( *__dma_i2c2_rx, data );
    }
    return 0;
}

void
i2c::handle_event_interrupt()
{
    stream() << "EVENT: " << status32_to_string( i2c_status( *i2c_ )() ) << std::endl;
}

void
i2c::handle_error_interrupt()
{
    stream() << "ERROR irq: " << status32_to_string( i2c_status( *i2c_ )() ) << std::endl;

    constexpr uint32_t error_condition = SMB_ALART | TIME_OUT | PEC_ERR | OVR | AF | ARLO | BERR;
    i2c_->SR1 &= ~error_condition;
    //disable(); mdelay(1);
    //enable(); mdelay(1);
}

//static
void
i2c::interrupt_event_handler( i2c * _this )
{
    _this->handle_event_interrupt();
}

// static
void
i2c::interrupt_error_handler( i2c * _this )
{
    _this->handle_error_interrupt();
}

