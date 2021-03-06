/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Utility functions
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler library
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>. 
 */

#include "utils.h"

#if defined(ARDUINO)
#include <avr/eeprom.h>

#else
void nvm_read_block(void *dst, const void *src, int len) {
  FILE *fp = fopen(NVM_FILENAME, "rb");
  if(fp) {
    fseek(fp, (unsigned int)src, SEEK_SET);
    fread(dst, 1, len, fp);
    fclose(fp);
  }
}

void nvm_write_block(const void *src, void *dst, int len) {
  FILE *fp = fopen(NVM_FILENAME, "rb+");
  if(!fp) {
    fp = fopen(NVM_FILENAME, "wb");
  }
  if(fp) {
    fseek(fp, (unsigned int)dst, SEEK_SET);
    fwrite(src, 1, len, fp);
    fclose(fp);
  } else {
    // file does not exist
  }
}

byte nvm_read_byte(const byte *p) {
  FILE *fp = fopen(NVM_FILENAME, "rb");
  byte v = 0;
  if(fp) {
    fseek(fp, (unsigned int)p, SEEK_SET);
    fread(&v, 1, 1, fp);
    fclose(fp);
  } else {
   // file does not exist
  }
  return v;
}

void nvm_write_byte(const byte *p, byte v) {
  FILE *fp = fopen(NVM_FILENAME, "rb+");
  if(!fp) {
    fp = fopen(NVM_FILENAME, "wb");
  }
  if(fp) {
    fseek(fp, (unsigned int)p, SEEK_SET);
    fwrite(&v, 1, 1, fp);
    fclose(fp);
  } else {
    // file does not exist
  }
}
#endif

// compare a string to nvm
byte strcmp_to_nvm(const char* src, int _addr) {
  byte i=0;
  byte c1, c2;
  byte *addr = (byte*)_addr;
  while(1) {
    c1 = nvm_read_byte(addr++);
    c2 = *src++;
    if (c1==0 || c2==0)
      break;      
    if (c1!=c2)  return 1;
  }
  return (c1==c2) ? 0 : 1;
}

// ================================================
// ====== Data Encoding / Decoding Functions ======
// ================================================
// encode a 16-bit unsigned water time to 8-bit byte
/* encoding scheme:
   byte value : water time
     [0.. 59]  : [0..59]  (seconds)
    [60..238]  : [1..179] (minutes), or 60 to 10740 seconds
   [239..254]  : [3..18]  (hours),   or 10800 to 64800 seconds
*/
byte water_time_encode(uint16_t i) {
  if (i<60) {
    return (byte)(i);
  } else if (i<10800) {
    return (byte)(i/60+59);
  } else if (i<64800) {
    return (byte)(i/3600+236);
  } else {
    return 254;
  }
}

// encode a 16-bit signed water time to 8-bit unsigned byte (leading bit is the sign)
byte water_time_encode_signed(int16_t i) {
  byte ret = water_time_encode(i>=0?i : -i);
  return ((i>=0) ? (128+ret) : (128-ret));
}

// decode a 8-bit unsigned byte (leading bit is the sign) into a 16-bit signed water time
int16_t water_time_decode_signed(byte i) {
  int16_t ret = i;
  ret -= 128;
  ret = water_time_decode(ret>=0?ret:-ret);
  return (i>=128 ? ret : -ret);
}

// decode a 8-bit byte to a 16-bit unsigned water time
uint16_t water_time_decode(byte i) {
  uint16_t ii = i;
  if (i<60) {
    return ii;
  } else if (i<239) {
    return (ii-59)*60;
  } else {
    return (ii-236)*3600;
  }  
}




