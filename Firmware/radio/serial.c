// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
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
/// @file	serial.c
///
/// MCS51 Serial port driver with flow control and AT command
/// parser integration.
///

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "flash_layout.h"
#include "serial.h"
#include "packet.h"
#include "timer.h"
#include "i2c.h"
#include "sha3.h"
#include "csma.h"

void reinit(void);

// Serial rx/tx buffers.
//
// Note that the rx buffer is much larger than you might expect
// as we need the receive buffer to be many times larger than the
// largest possible air packet size for efficient TDM. Ideally it
// would be about 16x larger than the largest air packet if we have
// 8 TDM time slots
//

#define RX_BUFF_MAX 256
#define TX_BUFF_MAX 512

__xdata uint8_t rx_buf[RX_BUFF_MAX] = {0};
__xdata uint8_t tx_buf[TX_BUFF_MAX] = {0};

// TX gate / ! escape state
extern bool last_was_bang=0;
extern bool tx_buffered_data=0;

// FIFO insert/remove pointers
static volatile __pdata uint16_t				rx_insert, rx_remove;
static volatile __pdata uint16_t				tx_insert, tx_remove;

// count of number of bytes we are allowed to send due to a RTS low reading
static uint8_t rts_count;

// flag indicating the transmitter is idle
static volatile bool			tx_idle;

// FIFO status
#define BUF_NEXT_INSERT(_b)	((_b##_insert + 1) == sizeof(_b##_buf)?0:(_b##_insert + 1))
#define BUF_NEXT_REMOVE(_b)	((_b##_remove + 1) == sizeof(_b##_buf)?0:(_b##_remove + 1))
#define BUF_FULL(_b)	(BUF_NEXT_INSERT(_b) == (_b##_remove))
#define BUF_NOT_FULL(_b)	(BUF_NEXT_INSERT(_b) != (_b##_remove))
#define BUF_EMPTY(_b)	(_b##_insert == _b##_remove)
#define BUF_NOT_EMPTY(_b)	(_b##_insert != _b##_remove)
#define BUF_USED(_b)	((_b##_insert >= _b##_remove)?(_b##_insert - _b##_remove):(sizeof(_b##_buf) - _b##_remove) + _b##_insert)
#define BUF_FREE(_b)	((_b##_insert >= _b##_remove)?(sizeof(_b##_buf) + _b##_remove - _b##_insert):_b##_remove - _b##_insert)

// FIFO insert/remove operations
//
// Note that these are nominally interrupt-safe as only one of each
// buffer's end pointer is adjusted by either of interrupt or regular
// mode code.  This is violated if printing from interrupt context,
// which should generally be avoided when possible.
//
#define BUF_INSERT(_b, _c)	do { _b##_buf[_b##_insert] = (_c); \
		_b##_insert = BUF_NEXT_INSERT(_b); } while(0)
#define BUF_REMOVE(_b, _c)	do { (_c) = _b##_buf[_b##_remove]; \
		_b##_remove = BUF_NEXT_REMOVE(_b); } while(0)
#define BUF_PEEK(_b)	_b##_buf[_b##_remove]
#define BUF_PEEK2(_b)	_b##_buf[BUF_NEXT_REMOVE(_b)]
#define BUF_PEEKX(_b, offset)	_b##_buf[(_b##_remove+offset) % sizeof(_b##_buf)]

static void			_serial_write(register uint8_t c);
static void			serial_restart(void);

// save and restore serial interrupt. We use this rather than
// __critical to ensure we don't disturb the timer interrupt at all.
// minimal tick drift is critical for TDM
#define ES0_SAVE_DISABLE __bit ES_saved = ES0; ES0 = 0
#define ES0_RESTORE ES0 = ES_saved

// threshold for considering the rx buffer full
#define SERIAL_CTS_THRESHOLD_LOW  17
#define SERIAL_CTS_THRESHOLD_HIGH 34

uint8_t hex_decode(uint8_t c)
{
	if ((c>='0')&&(c<='9')) return c-'0';
	if ((c>='A')&&(c<='F')) return (c-'A')+10;
	if ((c>='a')&&(c<='f')) return (c-'a')+10;
	return 0;
}

__xdata unsigned char count;
__xdata char i;
__xdata short eeprom_address = 0;

__xdata uint8_t uboot_counter=0;
__xdata uint8_t last_byte=0;

void
serial_interrupt(void) __interrupt(INTERRUPT_UART0)
{
	register uint8_t	c;						

	// check for received byte first
	if (RI0) {
		// acknowledge interrupt and fetch the byte immediately
		RI0 = 0;
		c = SBUF0;

		// Mesh Extender specific:
		// Check for signs of a uboot banner at 115200 when we are at 230400
		// We will see a sequence of 86 98 for at least 80 bytes running when
		// the long sequence of asterisks is sent.
		if (((last_byte==0x86)&&c==0x98)||((last_byte==0x98)&&(c==0x86)))
			uboot_counter++; 
		else uboot_counter=0;
		if (uboot_counter>=80) {
			// uboot banner detected
			LED_BOOTLOADER = LED_ON;
			// Say nothing for twenty seconds, to give uboot and kernel
			// time to boot.
			uboot_silence_counter=20*100;
			uboot_counter=0;
			uboot_silence_mode=1;
#if 0
			// Issue boot command just to make sure
			serial_init(115);
			_serial_write('\r');
			_serial_write('b');
			_serial_write('o');
			_serial_write('o');
			_serial_write('t');
			_serial_write('\r');
			// wait long enough for those characters to get sent
			// 6x 115200 8N1 characters < ~1ms
			delay_msec(2);
			serial_init(param_get(PARAM_SERIAL_SPEED));
#endif
		}
		last_byte=c;
		
		// if AT mode is active, the AT processor owns the byte
		if (at_mode_active) {
			// If an AT command is ready/being processed, we would ignore this byte
			if (!at_cmd_ready) {
				at_input(c);
			}
		} else {
			// run the byte past the +++ detector
			at_plus_detector(c);

			// PGS: To enforce packet boundaries where we want them, we escape
			// '!'.  '!!' means send buffered serial data.  '!' followed by '.'
			// inserts a '!' into the serial buffer.
			if (c=='!') {
				// ! can only be received at 230400, not from a mangled
				// 115200 char from uboot or the linux kernel.
				// Thus when we see one, make silence mode expire
				// immediately.
				if (uboot_silence_mode) {
					uboot_silence_mode=0;
					uboot_silence_counter=0;
					LED_BOOTLOADER = LED_OFF;
				}
				
				if (last_was_bang) {
					tx_buffered_data=1;
					last_was_bang=0;
				} else {
					last_was_bang=1;
				}
			} else if ((c=='M') && last_was_bang ) {
				last_was_bang=0;
				radio_set_transmit_power(30);
			} else if ((c=='H') && last_was_bang ) {
				last_was_bang=0;
				radio_set_transmit_power(24);
			} else if ((c=='L') && last_was_bang ) {
				last_was_bang=0;
				radio_set_transmit_power(0);
			} else if ((c=='P') && last_was_bang ) {
				last_was_bang=0;
                                printfl("TXPOWER=%d\r\n",radio_get_transmit_power());
			} else if ((c=='B') && last_was_bang ) {

				// Drop to boot loader if !Cup!B is typed

				if (BUF_EMPTY(rx)||(serial_read()!='u')) c=1;
				if (BUF_EMPTY(rx)||(serial_read()!='p')) c=1;
				if (BUF_NOT_EMPTY(rx)) c=1;
				
				last_was_bang=0;

				if (c=='B') {
					// Erase Flash signature forcing it into reprogram mode next reset
					FLKEY = 0xa5;
					FLKEY = 0xf1;
					PSCTL = 0x03;	// set PSWE and PSEE
					*(uint8_t __xdata *)FLASH_SIGNATURE_BYTES = 0xff;	// do the page erase
					PSCTL = 0x00;	// disable PSWE/PSEE
					
					// Reset the device using sofware reset
					RSTSRC |= 0x10;
					
					for (;;)
						;
				}
			} else if ((c=='C') && last_was_bang ) {
				// clear TX buffer
				last_was_bang=0;
				while (BUF_NOT_EMPTY(rx)) {
					BUF_REMOVE(rx,c);
				}
			} else if (((c>='a') &&(c<='z') && last_was_bang )
				   || ((c>='0') &&(c<='9') && last_was_bang )) {
				last_was_bang=0;
#if PIN_MAX > 0
				// I2C debug functions
		        switch (c) {
			case '0': case '1': case '2': case '3': case '4': 
				pins_user_set_io(c-'0',PIN_OUTPUT);
				pins_user_set_value(c-'0',1);
				break;
			case '5': case '6': case '7': case '8': case '9': 
				pins_user_set_io(c-'5',PIN_OUTPUT);
				pins_user_set_value(c-'5',0);
				break;				
			case 'p': eeprom_poweron(); break;
			case 'o': eeprom_poweroff(); break;
			case 's': // i2c_clock_high(); break;
				// Drive line hard (for debugging)
				pins_user_set_io(4,PIN_OUTPUT);
				pins_user_set_value(4,1);
				break;
			case 'x': i2c_clock_low(); break;
			case 'd': // i2c_data_high(); break;
				// Drive line hard (for debugging)
				pins_user_set_io(3,PIN_OUTPUT);
				pins_user_set_value(3,1);
				break;
			case 'c': i2c_data_low(); break;
			case 'y':
				// Disable write-protect temporarily
				// (writing to EEPROM reasserts it automatically)
				eeprom_writeenable();
				break;
			case 'g':
				// Adjust where to read or write data in EEPROM
				// Allow things like 1a0!g to set EEPROM pointer to 0x1a0				
				eeprom_address=0;
				while (BUF_NOT_EMPTY(rx)) {
					eeprom_address=eeprom_address<<4;
					eeprom_address+=hex_decode(serial_read());
				}
				if (!eeprom_address) {
					// 0!g ends silent mode, so that Mesh Extenders
					// don't need to delay on power up.
					uboot_silence_mode = 0;
				}
				if (eeprom_address<0) eeprom_address+=0x800;
				if (eeprom_address>=0x800) eeprom_address-=0x800;
				printfl("EPRADDR=$%x\r\n",eeprom_address);
				break;
			case 'h': // Request heartbeat from radio
				heartbeat_requested=1;
				break;
			case 'w':
				// Write a page of data to EEPROM.
				// We copy the first 16 bytes from the serial buffer
				// to write.
				// XXX - Will read rubbish if less than 16 bytes are
				// available.
				
				eeprom_poweron();
				printfl("\r\n");
				{
					// Copy bytes from TX buffer
					char i;
					for(i=0;i<16;i++) {
						if (BUF_NOT_EMPTY(rx))
							eeprom_data[i]=serial_read();
						else eeprom_data[i]=0xbd;
					}
					if (eeprom_write_page(eeprom_address))
						printfl("WRITE ERROR\r\n");
					else {
						printfl("EEPROM WRITTEN @ $%x\r\nREAD BACK",
							eeprom_address);
						for(i=0;i<16;i++) eeprom_data[i]=0xEE;
						eeprom_read_page(eeprom_address);
						for(i=0;i<16;i++)
							printfl(" %x",eeprom_data[i]);
						printfl("\r\n");
					}
					
				}
				eeprom_poweroff();
				// Re-enable write-protect
				eeprom_writeprotect();
				break;
			case 'j':
				// Write a byte of data to EEPROM.
				
				eeprom_poweron();
				printfl("\r\n");
				{
					// Copy bytes from TX buffer
					eeprom_data[0]=serial_read();
					if (eeprom_write_byte(eeprom_address,eeprom_data[0]))
						printfl("WRITE ERROR\r\n");
					else {
						printfl("EEPROM WRITTEN %x -> $%x\r\n",
							eeprom_data[0],eeprom_address);
						eeprom_address++;
					}
					
				}
				eeprom_poweroff();
				// Re-enable write-protect
				eeprom_writeprotect();
				break;
			}
#endif
			} else if ((c=='D') && last_was_bang ) {
				eeprom_param_request=true;
				last_was_bang=0;
			} else if ((c=='E') && last_was_bang ) {
				// Dump EEPROM contents
				{
					count=0;
					eeprom_poweron();
					printfl("\r\n");
					while(1)
						{
							printfl("EPR:%x : ",eeprom_address);
							i=eeprom_read_page(eeprom_address);
							if (i) printfl("READ ERROR #%d",i);
							else {
								for(i=0;i<16;i++)
									printfl(" %x",eeprom_data[i]);
							}
							printfl("\r\n");
							eeprom_address+=16;
							if (eeprom_address>=0x800) eeprom_address=0;
							
							count+=16;
							if (count==0x80) break;
						}

					eeprom_poweroff();
					
					last_was_bang=0;
				}
			} else if ((c=='I') && last_was_bang ) {
				// Dump EEPROM contents in compat format
				{
					count=0;
					eeprom_poweron();
					while(1)
						{
							putchar_r(5); putchar_r(16);
							putchar_r(eeprom_address&0xff);
							putchar_r(eeprom_address>>8);
							i=eeprom_read_page(eeprom_address);
							if (i) printfl("READ ERROR #%d",i);
							else {
								// Use _serial_write to avoid CRLF conversion
								for(i=0;i<16;i++) _serial_write(eeprom_data[i]);
							}
							eeprom_address+=16;
							if (eeprom_address>=0x800) eeprom_address=0;
							
							count+=16;
							if (count==0x80) break;
						}

					eeprom_poweroff();
					
					last_was_bang=0;
				}

			} else if ((c=='F') && last_was_bang ) {
				// Identify radio firmware by series of checksums of flash
				last_was_bang=0;
				flash_report_summary();
			} else if ((c=='0') && last_was_bang ) {
				// Empty packet buffer
				last_was_bang=0;
				rx_insert=0; rx_remove=0;
			} else if ((c=='Y') && last_was_bang ) {
			     last_was_bang=0;
			        tx_buffered_data=0;
			        heartbeat_requested=0;

                                reinit();

                                printfl("REINITed\r\n");
			} else if ((c=='Z') && last_was_bang ) {
				// Trigger a reset of radio by software (like ATZ)
				last_was_bang=0;
                                printfl("Resetting...\n\r");
				RSTSRC |= (1 << 4);
	                        for (;;)
       	                         ;
			} else if ((c=='R') && last_was_bang ) {
				// Reset radio to default settings (like AT&F)
				last_was_bang=0;
				param_default();
			} else if ((c=='V') && last_was_bang ) {
				// Provide version info, to allow quick detection of CSMA
				// firmware
				last_was_bang=0;
				putchar_r('1');
			} else if ((c=='.') && last_was_bang ) {
				last_was_bang=0;
				// Insert escaped ! into serial RX buffer
				if (BUF_NOT_FULL(rx)) {
					BUF_INSERT(rx, '!');
				} else {
					if (errors.serial_rx_overflow != 0xFFFF) {
						errors.serial_rx_overflow++;
					}
				}
			} else if (last_was_bang) {
				// Unknown ! command
				last_was_bang=0;
				putchar_r('E');
			} else {
				// Character to put in TX buffer
				if (BUF_NOT_FULL(rx)) {
					BUF_INSERT(rx, c);
				} else {
					if (errors.serial_rx_overflow != 0xFFFF) {
						errors.serial_rx_overflow++;
					}
				}				
			}
#ifdef SERIAL_CTS
			if (BUF_FREE(rx) < SERIAL_CTS_THRESHOLD_LOW) {
				SERIAL_CTS = true;
			}
#endif
		}
	}

	// check for anything to transmit
	if (TI0) {
		// acknowledge the interrupt
		TI0 = 0;

		// look for another byte we can send
		if (BUF_NOT_EMPTY(tx)) {
#ifdef SERIAL_RTS
		if (feature_rtscts) {
				if (SERIAL_RTS && !at_mode_active) {
						if (rts_count == 0) {
								// the other end doesn't have room in
								// its serial buffer
								tx_idle = true;
								return;
						}
						rts_count--;
				} else {
								rts_count = 8;
				}
		}
#endif
			// fetch and send a byte
			BUF_REMOVE(tx, c);
			SBUF0 = c;
		} else {
			// note that the transmitter requires a kick to restart it
			tx_idle = true;
		}
	}
}


/// check if RTS allows us to send more data
///
void
serial_check_rts(void)
{
	if (BUF_NOT_EMPTY(tx) && tx_idle) {
		serial_restart();
	}
}

void
serial_init(register uint8_t speed)
{
	// disable UART interrupts
	ES0 = 0;

	// reset buffer state, discard all data
	rx_insert = 0;
	rx_remove = 0;
	tx_insert = 0;
	tx_remove = 0;
	tx_idle = true;

	// configure timer 1 for bit clock generation
	TR1 	= 0;				// timer off
	TMOD	= (TMOD & ~0xf0) | 0x20;	// 8-bit free-running auto-reload mode
	serial_device_set_speed(speed);		// device-specific clocking setup
	TR1	= 1;				// timer on

	// configure the serial port
	SCON0	= 0x10;				// enable receiver, clear interrupts

#ifdef SERIAL_CTS
	// setting SERIAL_CTS low tells the other end that we have
	// buffer space available
	SERIAL_CTS = false;
#endif

	// re-enable UART interrupts
	ES0 = 1;
}

bool
serial_write(register uint8_t c)
{
	if (uboot_silence_mode) return false;
	
	if (serial_write_space() < 1)
		return false;

	_serial_write(c);
	return true;
}

static void
_serial_write(register uint8_t c) __reentrant
{
	ES0_SAVE_DISABLE;

	// if we have space to store the character
	if (BUF_NOT_FULL(tx)) {

		// queue the character
		BUF_INSERT(tx, c);

		// if the transmitter is idle, restart it
		if (tx_idle)
			serial_restart();
	} else if (errors.serial_tx_overflow != 0xFFFF) {
		errors.serial_tx_overflow++;
	}

	ES0_RESTORE;
}

// write as many bytes as will fit into the serial transmit buffer
// if encryption turned on, decrypt the packet.
void
serial_write_buf(__xdata uint8_t * buf, __pdata uint8_t count)
{
	__pdata uint16_t space;
	__pdata uint8_t n1;

	if (count == 0) {
		return;
	}
  
	// discard any bytes that don't fit. We can't afford to
	// wait for the buffer to drain as we could miss a frequency
	// hopping transition
	space = serial_write_space();	
	if (count > space) {
		count = space;
		if (errors.serial_tx_overflow != 0xFFFF) {
			errors.serial_tx_overflow++;
		}
	}

	// write to the end of the ring buffer
	n1 = count;
	if (n1 > sizeof(tx_buf) - tx_insert) {
		n1 = sizeof(tx_buf) - tx_insert;
	}
	memcpy(&tx_buf[tx_insert], buf, n1);
	buf += n1;
	count -= n1;
	__critical {
		tx_insert += n1;
		if (tx_insert >= sizeof(tx_buf)) {
			tx_insert -= sizeof(tx_buf);
		}
	}

	// add any leftover bytes to the start of the ring buffer
	if (count != 0) {
		memcpy(&tx_buf[0], buf, count);
		__critical {
			tx_insert = count;
		}		
	}
	__critical {
		if (tx_idle) {
			serial_restart();
		}
	}
}

uint16_t
serial_write_space(void)
{
	register uint16_t ret;
	ES0_SAVE_DISABLE;
	ret = BUF_FREE(tx);
	ES0_RESTORE;
	return ret;
}

static void
serial_restart(void)
{
#ifdef SERIAL_RTS
	if (feature_rtscts && SERIAL_RTS && !at_mode_active) {
		// the line is blocked by hardware flow control
		return;
	}
#endif
	// generate a transmit-done interrupt to force the handler to send another byte
	tx_idle = false;
	TI0 = 1;
}

uint8_t
serial_read(void)
{
	register uint8_t	c;

	ES0_SAVE_DISABLE;

	if (BUF_NOT_EMPTY(rx)) {
		BUF_REMOVE(rx, c);
	} else {
		c = '\0';
	}

#ifdef SERIAL_CTS
	if (BUF_FREE(rx) > SERIAL_CTS_THRESHOLD_HIGH) {
		SERIAL_CTS = false;
	}
#endif

	ES0_RESTORE;

	return c;
}

uint8_t
serial_peek(void)
{
	register uint8_t c;

	ES0_SAVE_DISABLE;
	c = BUF_PEEK(rx);
	ES0_RESTORE;

	return c;
}

uint8_t
serial_peek2(void)
{
	register uint8_t c;

	ES0_SAVE_DISABLE;
	c = BUF_PEEK2(rx);
	ES0_RESTORE;

	return c;
}

uint8_t
serial_peekx(uint16_t offset)
{
	register uint8_t c;

	ES0_SAVE_DISABLE;
	c = BUF_PEEKX(rx, offset);
	ES0_RESTORE;

	return c;
}

// read count bytes from the serial buffer. This implementation
// tries to be as efficient as possible, while disabling interrupts
// for as short a time as possible
bool
serial_read_buf(__xdata uint8_t * buf, __pdata uint8_t count)
{
	__pdata uint16_t n1;
	// the caller should have already checked this, 
	// but lets be sure
	if (count > serial_read_available()) {
		return false;
	}
	// see how much we can copy from the tail of the buffer
	n1 = count;
	if (n1 > sizeof(rx_buf) - rx_remove) {
		n1 = sizeof(rx_buf) - rx_remove;
	}
	memcpy(buf, &rx_buf[rx_remove], n1);
	count -= n1;
	buf += n1;
	// update the remove marker with interrupts disabled
	__critical {
		rx_remove += n1;
		if (rx_remove >= sizeof(rx_buf)) {
			rx_remove -= sizeof(rx_buf);
		}
	}
	// any more bytes to do?
	if (count > 0) {
		memcpy(buf, &rx_buf[0], count);
		__critical {
			rx_remove = count;
		}		
	}

#ifdef SERIAL_CTS
	__critical {
		if (BUF_FREE(rx) > SERIAL_CTS_THRESHOLD_HIGH) {
			SERIAL_CTS = false;
		}
	}
#endif
	return true;
}

uint16_t
serial_read_available(void)
{
	register uint16_t ret;
	ES0_SAVE_DISABLE;
	ret = BUF_USED(rx);
	ES0_RESTORE;
	return ret;
}

// return available space in rx buffer as a percentage
uint8_t
serial_read_space(void)
{
	uint16_t space = sizeof(rx_buf) - serial_read_available();
	space = (100 * (space/8)) / (sizeof(rx_buf)/8);
	return space;
}

uint16_t
serial_read_space_bytes(void)
{
        return sizeof(rx_buf) - serial_read_available();
}


void putchar_r(char c) __reentrant
{
	if (c == '\n')
		_serial_write('\r');
	_serial_write(c);
}

void puts_r(char *s)
{
	while(s++) putchar_r(*s);
	putchar_r('\n');
}


///
/// Table of supported serial speed settings.
/// the table is looked up based on the 'one byte'
/// serial rate scheme that APM uses. If an unsupported
/// rate is chosen then 57600 is used
///
static const __code struct {
	uint8_t rate;
	uint8_t th1;
	uint8_t ckcon;
} serial_rates[] = {
	{1,   0x2C, 0x02}, // 1200
	{2,   0x96, 0x02}, // 2400
	{4,   0x2C, 0x00}, // 4800
	{9,   0x96, 0x00}, // 9600
	{19,  0x60, 0x01}, // 19200
	{38,  0xb0, 0x01}, // 38400
	{57,  0x2b, 0x08}, // 57600 - default
	{115, 0x96, 0x08}, // 115200
	{230, 0xcb, 0x08}, // 230400
};

//
// check if a serial speed is valid
//
bool 
serial_device_valid_speed(register uint8_t speed)
{
	uint8_t i;
	uint8_t num_rates = ARRAY_LENGTH(serial_rates);

	for (i = 0; i < num_rates; i++) {
		if (speed == serial_rates[i].rate) {
			return true;
		}
	}
	return false;
}

void 
serial_device_set_speed(register uint8_t speed)
{
	uint8_t i;
	uint8_t num_rates = ARRAY_LENGTH(serial_rates);

	for (i = 0; i < num_rates; i++) {
		if (speed == serial_rates[i].rate) {
			break;
		}
	}
	if (i == num_rates) {
		i = 6; // 57600 default
	}

	// set the rates in the UART
	TH1 = serial_rates[i].th1;
	CKCON = (CKCON & ~0x0b) | serial_rates[i].ckcon;

	// tell the packet layer how fast the serial link is. This is
	// needed for packet framing timeouts
	packet_set_serial_speed(speed*125UL);	
}

