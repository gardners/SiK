#include "board.h"
#include "radio.h"
#include "timer.h"
#include "golay.h"
#include "crc.h"
#include "pins_user.h"

#if PIN_MAX>0

void i2c_delay(void)
{
  // 10 microsecond delay

  // Will this be enough?
  for(unsigned char i=0;i<255;i++) continue;
}

void i2c_clock_high(void)
{
  pins_user_set_io(3,false);
}

void i2c_clock_low(void)
{
  pins_user_set_io(3,true);
  pins_user_set_value(3,false);
}

void i2c_data_high(void)
{
  pins_user_set_io(4,false);
}

void i2c_data_low(void)
{
  pins_user_set_io(4,true);
  pins_user_set_value(4,false);
}

unsigned char i2c_clock_value(void)
{
  if (pins_user_get_adc(3)) return 1; else return 0;
}

unsigned char i2c_data_value(void)
{
  if (pins_user_get_adc(4)) return 1; else return 0;
}


void i2c_stop(void)
{
    i2c_clock_high();
  i2c_data_high();

}

void i2c_start(void)
{
  i2c_stop();
  
  i2c_data_low();
  i2c_clock_low();
}

unsigned char i2c_rx(char ack)
{
  char x=0;
  char d=0;

  i2c_data_high();
  i2c_delay();

  // Receive bits
  for(x=0;x<8;x++) {
    d <<= 1;
    i2c_clock_high();
    i2c_delay();

    // Wait for any clock stretching
    while (i2c_clock_value()) i2c_delay();

    if (i2c_data_value()) d|=1;

    i2c_clock_low();
    i2c_delay();
  }

  // Send ack
  if (ack) i2c_data_low(); else i2c_data_high();

  i2c_delay();
  i2c_clock_high();
  i2c_delay();

  // send ACK/NACK
  i2c_clock_low(); i2c_delay();
  i2c_data_high(); i2c_delay();
  return d;
}

unsigned char i2c_tx(unsigned char d)
{
  unsigned char x;

  i2c_clock_low(); i2c_delay();
  
  for(x=8;x;x--) {
    if (d&0x80) i2c_data_high(); else i2c_data_low();
    d<<=1;
    i2c_clock_high(); i2c_delay();
    i2c_clock_low(); i2c_delay();
  }

  return i2c_data_value();
}

char eeprom_write_byte(unsigned short address, unsigned char value)
{
  i2c_start();
  if (i2c_tx(0xa0+((address>>7)&0xe))) return -1;
  if (i2c_tx(address&0xff)) return -1;
  while (i2c_tx(value)) i2c_delay();
  i2c_stop();
  return 0;
}

char eeprom_read_byte(unsigned short address, char *byte)
{
  // Setup for a write, then abort it, to set memory pointer
  i2c_start();
  if (i2c_tx(0xa0+((address>>7)&0xe))) return -1;
  if (i2c_tx(address&0xff)) return -1;
  i2c_stop();

  i2c_start();
  if (i2c_tx(0xa1+((address>>7)&0xe))) return -1;

  *byte=i2c_rx(1);
  i2c_stop();
  return 0;
}

#else
// No GPIOs, so ignore
char eeprom_write_byte(unsigned short address, unsigned char value)
{
  address=value;  // suppress compiler warnings
  return -1;
}

char eeprom_read_byte(unsigned short address, char *byte)
{  
  *byte="NOEPROM."[address&7];
  return 0;
}

#endif