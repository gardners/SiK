// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
// Copyright (c) 2011 Andrew Tridgell, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

///
/// @file	_start.c
///
/// Early startup code.
/// This file *must* be linked first for interrupt vector generation and main() to work.
/// XXX this may no longer be the case - it may be sufficient for the interrupt vectors
/// to be located in the same file as main()
///

#include <stdarg.h>
#include <stdbool.h>
#include "radio.h"
#include "csma.h"
#include "timer.h"
#include "serial.h"
#include "i2c.h"

////////////////////////////////////////////////////////////////////////////////
/// @name	Interrupt vector prototypes
///
/// @note these *must* be placed in this file for SDCC to generate the
/// interrupt vector table correctly.
//@{

/// Serial rx/tx interrupt handler.
///
extern void	serial_interrupt(void)	__interrupt(INTERRUPT_UART0);

/// Radio event interrupt handler.
///
extern void	Receiver_ISR(void)	__interrupt(INTERRUPT_INT0);

/// Timer2 tick interrupt handler
///
extern void    T2_ISR(void)     __interrupt(INTERRUPT_TIMER2);

/// Timer3 tick interrupt handler
///
/// @todo switch this and everything it calls to use another register bank?
///
extern void    T3_ISR(void)     __interrupt(INTERRUPT_TIMER3);

//@}

__code const char g_banner_string[] = "RFD SiK CSMA " stringify(APP_VERSION_HIGH) "." stringify(APP_VERSION_LOW) " on " BOARD_NAME;
__code const char g_version_string[] = stringify(APP_VERSION_HIGH) "." stringify(APP_VERSION_LOW);

__pdata enum BoardFrequency	g_board_frequency;	///< board info from the bootloader
__pdata uint8_t			g_board_bl_version;	///< from the bootloader

/// Configure the Si1000 for operation.
///
static void hardware_init(void);

/// Initialise the radio and bring it online.
///
static void radio_init(void);

/// statistics for radio and serial errors
__pdata struct error_counts errors;
__pdata struct statistics statistics, remote_statistics;

/// optional features
bool feature_golay;
bool feature_opportunistic_resend;
bool feature_rtscts;

void
main(void)
{
#ifdef CPU_SI1030
	PSBANK = 0x33;
#endif
	
	// Stash board info from the bootloader before we let anything touch
	// the SFRs.
	//
	g_board_frequency = BOARD_FREQUENCY_REG;
	g_board_bl_version = BOARD_BL_VERSION_REG;

	// Load parameters from flash or defaults
	// this is done before hardware_init() to get the serial speed
	if (!param_load())
		param_default();

	// setup boolean features
	feature_opportunistic_resend = param_get(PARAM_OPPRESEND)?true:false;
	feature_golay = param_get(PARAM_ECC)?true:false;
	feature_rtscts = param_get(PARAM_RTSCTS)?true:false;

	// Reset our serial processing state machine before any serial characters
	// are received.
	last_was_bang=0;
	tx_buffered_data=0;
	
	// Do hardware initialisation.
	hardware_init();

	// do radio initialisation
	radio_init();

	// turn on the receiver
	if (!radio_receiver_on()) {
		panic("failed to enable receiver");
	}

	// Init user pins
#if PIN_MAX > 0
	pins_user_init();
#endif

	// Power on with uboot detection briefly enforced
	uboot_silence_mode=1;
	uboot_silence_counter=1*100;
	
	// Calculate hashes of flash memory now, so that we can respond quickly later
	// when requested.
	flash_calculate_hash();

	// Load radio parameters from I2C EEPROM, if present.
	eeprom_load_parameters();
	
	csma_serial_loop();
}

void
panic(char *fmt, ...)
{
	va_list ap;

	puts_r("\n**PANIC**");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	puts_r("");

	EA = 1;
	ES0 = 1;
	
	delay_msec(1000);

	// generate a software reset
	RSTSRC |= (1 << 4);
	for (;;)
		;
}

static void
hardware_init(void)
{
	__xdata uint16_t	i;

	// Disable the watchdog timer
	PCA0MD	&= ~0x40;

	// Select the internal oscillator, prescale by 1
#ifdef CPU_SI1030
	OSCICN	|=  0x80;
#else
	OSCICN	 =  0x8F;
#endif
	FLSCL	 =  0x40;
	CLKSEL	 =  0x00;

	// Configure the VDD brown out detector
	VDM0CN	 =  0x80;
	for (i = 0; i < 350; i++);	// Wait 100us for initialization
	RSTSRC	 =  0x06;		// enable brown out and missing clock reset sources

#if defined CPU_SI1030
	P0SKIP  =  0xCF;
	P1SKIP  =  0xFF;
	P2SKIP  =  0x28;
#elif defined BOARD_rfd900a		// Redefine port skips to override bootloader defs
	P0SKIP  =  0xCF;				// P0 UART avail on XBAR
	P1SKIP  =  0xF8;				// P1 SPI1 avail on XBAR
	P2SKIP  =  0xCF;				// P2 CEX0 avail on XBAR P2.4, rest GPIO
#endif

	// Configure crossbar for UART
	P0MDOUT   =  0x10;		// UART Tx push-pull
	SFRPAGE   =  CONFIG_PAGE;
	P0DRV     =  0x10;		// UART TX
	SFRPAGE   =  LEGACY_PAGE;
	XBR0      =  0x01;		// UART enable

	// SPI1
#if defined CPU_SI1030
	XBR1    |= 0x41;	// Enable SPI1 (3 wire mode) + CEX0
	P2MDOUT |= 0xFD;	// SCK1, MOSI1, & NSS1,push-pull
#elif defined BOARD_rfd900a		// Redefine port skips to override bootloader defs
	XBR1    |= 0x41;	// enable SPI in 3-wire mode + CEX0
	P1MDOUT |= 0xF5;	// SCK1, MOSI1, MISO1 push-pull
	P2MDOUT |= 0xFF;	// SCK1, MOSI1, MISO1 push-pull
#else
	XBR1    |= 0x40;	// enable SPI in 3-wire mode
	P1MDOUT |= 0xF5;	// SCK1, MOSI1, MISO1 push-pull
#endif	
	
	/* ------------ Config Parameters ------------ */
	SFRPAGE	 = CONFIG_PAGE;
	P1DRV	|= 0xF5;	// SPI signals use high-current mode, LEDs and PAEN High current drive
	
#ifdef CPU_SI1030
	P2DRV	 = 0xFD; // MOSI1, SCK1, NSS1, high-drive mode
	
	P3MDOUT |= 0xC0;		/* Leds */
	P3DRV   |= 0xC0;		/* Leds */
#else
	P2DRV	|= 0xFF;
#endif
	
	/* ------------ Change to radio page ------------ */
	RADIO_PAGE();
	SPI1CFG  = 0x40;  // master mode
	SPI1CN   = 0x00;  // 3 wire master mode
	SPI1CKR  = 0x00;  // Initialise SPI prescaler to divide-by-2 (12.25MHz, technically out of spec)
	SPI1CN  |= 0x01;  // enable SPI
	NSS1     = 1;     // set NSS high

	/* ------------ END of Config Parameters ------------ */
	SFRPAGE	 = LEGACY_PAGE;
	
	// Clear the radio interrupt state
	IE0	 = 0;

	// initialise timers
	timer_init();

	// UART - set the configured speed
	serial_init(param_get(PARAM_SERIAL_SPEED));

	// set all interrupts to the same priority level
	IP = 0;

	// global interrupt enable
	EA = 1;
	
	// Turn off the 'radio running' LED and turn off the bootloader LED
	LED_RADIO = LED_OFF;
	LED_BOOTLOADER = LED_OFF;

	// ADC system initialise for temp sensor
	AD0EN = 1;	// Enable ADC0
	ADC0CF = 0xF9;  // Set amp0gn=1 (1:1)
	ADC0AC = 0x00;
	ADC0MX = 0x1B;	// Set ADC0MX to temp sensor
	REF0CN = 0x07;	// Define reference and enable temp sensor

#if defined BOARD_rfd900a || defined BOARD_rfd900p
	// PCA0, CEX0 setup and enable.
	PCA0MD = 0x88;
	PCA0PWM = 0x00;
	PCA0CPM0 = 0x42;
	PCA0CPH0 = 0x80;
	PCA0CN = 0x40;
#endif
	XBR2	 =  0x40;		// Crossbar (GPIO) enable
}

static void
radio_init(void)
{
	__xdata uint32_t freq;
	__xdata uint8_t txpower;

	// Do generic PHY initialisation
	if (!radio_initialise()) {
		panic("radio_initialise failed");
	}

	switch (g_board_frequency) {
	case FREQ_433:
		freq = 434000000UL;
		txpower = 10;
		break;
	case FREQ_470:
		freq = 470500000UL;
		txpower = 10;
		break;
	case FREQ_868:
		freq = 869000000UL;
		if (param_get(PARAM_FREQ)>898000)
		    // If we are using the 900 MHz default frequency, set it to the 868 band
                    param_set(PARAM_FREQ,868000);
		txpower = 10;
		break;
	case FREQ_915:
		freq = 923000000UL;
		txpower = 20;
		break;
	default:
		freq = 0;
		txpower = 0;
		panic("bad board frequency %d", g_board_frequency);
		break;
	}

	if (param_get(PARAM_FREQ) != 0) {
		freq        = param_get(PARAM_FREQ) * 1000UL;
	}
	if (param_get(PARAM_TXPOWER) != 0) {
		txpower = param_get(PARAM_TXPOWER);
	}

	// constrain power and channels
	txpower = constrain(txpower, BOARD_MINTXPOWER, BOARD_MAXTXPOWER);

	// double check ranges the board can do
	switch (g_board_frequency) {
	case FREQ_433:
		freq = constrain(freq, 414000000UL, 460000000UL);
		break;
	case FREQ_470:
		freq = constrain(freq, 450000000UL, 490000000UL);
		break;
	case FREQ_868:
		freq = constrain(freq, 849000000UL, 889000000UL);
		break;
	case FREQ_915:
		freq = constrain(freq, 868000000UL, 935000000UL);
		break;
	default:
		panic("bad board frequency %d", g_board_frequency);
		break;
	}

	// get the duty cycle we will use
	duty_cycle = param_get(PARAM_DUTY_CYCLE);
	duty_cycle = constrain(duty_cycle, 0, 100);
	param_set(PARAM_DUTY_CYCLE, duty_cycle);

	// get the LBT threshold we will use
	lbt_rssi = param_get(PARAM_LBT_RSSI);
	if (lbt_rssi != 0) {
		// limit to the RSSI valid range
		lbt_rssi = constrain(lbt_rssi, 25, 220);
	}
	param_set(PARAM_LBT_RSSI, lbt_rssi);

	// sanity checks
	param_set(PARAM_FREQ, freq/1000);

	// set the frequency and channel spacing
	// change base freq based on netid
	radio_set_frequency(freq);

	// start on a channel chosen by network ID
	radio_set_channel(0);

	// And intilise the radio with them.
	if (!radio_configure(param_get(PARAM_AIR_SPEED)) &&
	    !radio_configure(param_get(PARAM_AIR_SPEED)) &&
	    !radio_configure(param_get(PARAM_AIR_SPEED))) {
		panic("radio_configure failed");
	}

	// report the real air data rate in parameters
	param_set(PARAM_AIR_SPEED, radio_air_rate());

	// setup network ID
	radio_set_network_id(param_get(PARAM_NETID));

	// setup transmit power
	radio_set_transmit_power(txpower);
	
	// report the real transmit power in settings
	param_set(PARAM_TXPOWER, radio_get_transmit_power());

#ifdef USE_RTC
	// initialise real time clock
	rtc_init();
#endif

	// initialise CSMA system
        csma_init();
}

