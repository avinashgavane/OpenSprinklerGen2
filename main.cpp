/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Main loop
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler Firmware
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

#include <limits.h>

#include "OpenSprinkler.h"
#include "program.h"
#include "weather.h"

#if defined(ARDUINO)
#include "SdFat.h"
#include "Wire.h"
byte Ethernet::buffer[ETHER_BUFFER_SIZE]; // Ethernet packet buffer
SdFat sd;                                 // SD card object

void reset_all_stations();
unsigned long getNtpTime();
void manual_start_program(byte pid);
#else // header and defs for RPI/BBB
#include <sys/stat.h>
#include "server.h"
char ether_buffer[ETHER_BUFFER_SIZE];
struct sockaddr_in svr_addr, cli_addr;
socklen_t sin_len;
int sock = -1;
int client;
#endif

#define NTP_SYNC_INTERVAL       86400L  // NYP sync interval, 24 hrs
#define RTC_SYNC_INTERVAL       60      // RTC sync interval, 60 secs
#define CHECK_NETWORK_INTERVAL  30      // Network checking interval, 30 secs
#define DHCP_RENEW_INTERVAL     43200L  // DHCP renewal interval: 12 hrs
#define STAT_UPDATE_INTERVAL    900     // Statistics update interval: 15 mins
#define CHECK_WEATHER_INTERVAL  900     // Weather check interval: 15 mins
#define LCD_DIMMING_TIMEOUT      15     // LCD dimming timeout: 15 secs
#define PING_TIMEOUT            200     // Ping test timeout: 200 ms

extern char tmp_buffer[];       // scratch buffer
BufferFiller bfill;                       // buffer filler

// ====== Object defines ======
OpenSprinkler os; // OpenSprinkler object
ProgramData pd;   // ProgramdData object

#if defined(ARDUINO)

// ====== UI defines ======
static char ui_anim_chars[3] = {'.', 'o', 'O'};

#define UI_STATE_DEFAULT   0
#define UI_STATE_DISP_IP   1
#define UI_STATE_DISP_GW   2
#define UI_STATE_RUNPROG   3

static byte ui_state = UI_STATE_DEFAULT;
static byte ui_state_runprog = 0;

void ui_state_machine() {

  if (os.button_lasttime && os.button_lasttime + LCD_DIMMING_TIMEOUT < now()) {
    analogWrite(PIN_LCD_BACKLIGHT, 255-os.options[OPTION_LCD_DIMMING].value);
    os.button_lasttime = 0;
    ui_state = UI_STATE_DEFAULT;  // also recover to default state
  }

  // read button, if something is pressed, wait till release
  byte button = os.button_read(BUTTON_WAIT_HOLD);

  if (button & BUTTON_FLAG_DOWN) {   // repond only to button down events
    os.button_lasttime = now();
    analogWrite(PIN_LCD_BACKLIGHT, 255-os.options[OPTION_LCD_BACKLIGHT].value); // button is pressed, turn on LCD right away
  } else {
    return;
  }

  switch(ui_state) {
  case UI_STATE_DEFAULT:
    switch (button & BUTTON_MASK) {
    case BUTTON_1:
      if (button & BUTTON_FLAG_HOLD) {  // holding B1: stop all stations
        if (digitalRead(PIN_BUTTON_2)==0) { // if B2 is pressed, run a short test (internal test)
          manual_start_program(255);
        } else {
          reset_all_stations();
        }
      } else {  // clicking B1: display device IP and port
        os.lcd.clear();
        os.lcd_print_ip(ether.myip, 0);
        os.lcd.setCursor(0, 1);
        os.lcd_print_pgm(PSTR(":"));
        os.lcd.print(ether.hisport);
        ui_state = UI_STATE_DISP_IP;
      }
      break;
    case BUTTON_2:
      if (button & BUTTON_FLAG_HOLD) {  // holding B2: reboot
        os.reboot_dev();
      } else {  // clicking B2: display MAC and gate way IP
        os.lcd.clear();
        os.lcd_print_mac(ether.mymac);
        ui_state = UI_STATE_DISP_GW;
      }
      break;
    case BUTTON_3:
      if (button & BUTTON_FLAG_HOLD) {  // holding B3: go to main menu
        os.lcd_print_line_clear_pgm(PSTR("Run a Program:"), 0);
        os.lcd_print_line_clear_pgm(PSTR("Click B3 to list"), 1);
        ui_state = UI_STATE_RUNPROG;
      } else {  // clicking B3: switch board display (cycle through master and all extension boards)
        os.status.display_board = (os.status.display_board + 1) % (os.nboards);
      }
      break;
    }
    break;
  case UI_STATE_DISP_IP:
  case UI_STATE_DISP_GW:
    ui_state = UI_STATE_DEFAULT;
    break;
  case UI_STATE_RUNPROG:
    if ((button & BUTTON_MASK)==BUTTON_3) {
      if (button & BUTTON_FLAG_HOLD) {
        // start
        manual_start_program(ui_state_runprog);
        ui_state = UI_STATE_DEFAULT;
      } else {
        ui_state_runprog = (ui_state_runprog+1) % (pd.nprograms+1);
        os.lcd_print_line_clear_pgm(PSTR("Hold B3 to start"), 0);
        if(ui_state_runprog > 0) {
          ProgramStruct prog;
          pd.read(ui_state_runprog-1, &prog);
          os.lcd_print_line_clear_pgm(PSTR(" "), 1);
          os.lcd.setCursor(0, 1);
          os.lcd.print((int)ui_state_runprog);
          os.lcd_print_pgm(PSTR(". "));
          os.lcd.print(prog.name);
        } else {
          os.lcd_print_line_clear_pgm(PSTR("0. Test (1 min)"), 1);
        }
      }
    }
    break;
  }
}

// ======================
// Setup Function
// ======================
void do_setup() {
  /* Clear WDT reset flag. */
  MCUSR &= ~(1<<WDRF);

  DEBUG_BEGIN(9600);

  os.begin();          // OpenSprinkler init
  os.options_setup();  // Setup options

  pd.init();            // ProgramData init

  setSyncInterval(RTC_SYNC_INTERVAL);  // RTC sync interval
  // if rtc exists, sets it as time sync source
  setSyncProvider(os.status.has_rtc ? RTC.get : NULL);
  delay(500);
  os.lcd_print_time(0);  // display time to LCD

  // enable WDT
  /* In order to change WDE or the prescaler, we need to
   * set WDCE (This will allow updates for 4 clock cycles).
   */
  WDTCSR |= (1<<WDCE) | (1<<WDE);
  /* set new watchdog timeout prescaler value */
  WDTCSR = 1<<WDP3 | 1<<WDP0;  // 8.0 seconds
  /* Enable the WD interrupt (note no reset). */
  WDTCSR |= _BV(WDIE);

  // attempt to detect SD card
  os.lcd_print_line_clear_pgm(PSTR("Detecting uSD..."), 1);

  if(sd.begin(PIN_SD_CS, SPI_HALF_SPEED)) {
    os.status.has_sd = 1;
  }

  if (os.start_network()) {  // initialize network
    os.status.network_fails = 0;
  } else {
    os.status.network_fails = 1;
  }
  delay(500);

  os.apply_all_station_bits(); // reset station bits

  os.button_lasttime = now();
}

// Arduino software reset function
void(* sysReset) (void) = 0;

volatile byte wdt_timeout = 0;
// WDT interrupt service routine
ISR(WDT_vect)
{
  wdt_timeout += 1;
  // this isr is called every 8 seconds
  if (wdt_timeout > 15) {
    // reset after 120 seconds of timeout
    sysReset();
  }
}
#else
void do_setup() {
  os.begin();          // OpenSprinkler init
  os.options_setup();  // Setup options

  pd.init();            // ProgramData init
  if (os.start_network()) {  // initialize network
    DEBUG_PRINTLN("network established.");
    os.status.network_fails = 0;
  } else {
    DEBUG_PRINTLN("network failed.");
    os.status.network_fails = 1;
  }
}
#endif

void write_log(byte type, ulong curr_time);
void schedule_all_stations(ulong curr_time);
void turn_off_station(byte sid, byte mas, ulong curr_time);
void process_dynamic_events(ulong curr_time);
void check_network();
void check_weather();
void perform_ntp_sync();
void log_statistics(time_t curr_time);
void delete_log(char *name);
void analyze_get_url(char *p);

/** Main Loop */
void do_loop()
{
  static ulong last_time = 0;
  static ulong last_minute = 0;
  static uint16_t pos;

  byte bid, sid, s, pid, bitvalue;
  ProgramStruct prog;

  os.status.mas = os.options[OPTION_MASTER_STATION].value;

  time_t curr_time = os.now_tz();
  // ====== Process Ethernet packets ======
#if defined(ARDUINO)  // Process Ethernet packets for Arduino
  pos=ether.packetLoop(ether.packetReceive());
  if (pos>0) {  // packet received
    analyze_get_url((char*)Ethernet::buffer+pos);
  }
  wdt_reset();  // reset watchdog timer
  wdt_timeout = 0;

  ui_state_machine();

#else // Process Ethernet packets for RPI/BBB
  client = accept(sock, (struct sockaddr *) &cli_addr, &sin_len);
  if(client>=0) {
    read(client, ether_buffer, ETHER_BUFFER_SIZE);
    analyze_get_url(ether_buffer);
  }
#endif  // Process Ethernet packets

  // if 1 second has passed
  if (last_time != curr_time) {
    last_time = curr_time;

#if defined(ARDUINO)
    if (!ui_state)
      os.lcd_print_time(0);       // print time
#endif

    // ====== Check raindelay status ======
    if (os.status.rain_delayed) {
      if (curr_time >= os.nvdata.rd_stop_time) {  // rain delay is over
        os.raindelay_stop();
      }
    } else {
      if (os.nvdata.rd_stop_time > curr_time) {   // rain delay starts now
        os.raindelay_start();
      }
    }

    // ====== Check rain sensor status ======
    os.rainsensor_status();

    // ====== Check controller status changes and write log ======
    if (os.old_status.rain_delayed != os.status.rain_delayed) {
      if (os.status.rain_delayed) {
        // rain delay started, record time
        os.raindelay_start_time = curr_time;
      } else {
        // rain delay stopped, write log
        write_log(LOGDATA_RAINDELAY, curr_time);
      }
      os.old_status.rain_delayed = os.status.rain_delayed;
    }
    if (os.old_status.rain_sensed != os.status.rain_sensed) {
      if (os.status.rain_sensed) {
        // rain sensor on, record time
        os.rainsense_start_time = curr_time;
      } else {
        // rain sensor off, write log
        write_log(LOGDATA_RAINSENSE, curr_time);
      }
      os.old_status.rain_sensed = os.status.rain_sensed;
    }

    // ====== Schedule program data ======
    ulong curr_minute = curr_time / 60;
    boolean match_found = false;
    // since the granularity of start time is minute
    // we only need to check once every minute
    if (curr_minute != last_minute) {
      last_minute = curr_minute;
      // check through all programs
      for(pid=0; pid<pd.nprograms; pid++) {
        pd.read(pid, &prog);
        if(prog.check_match(curr_time)) {
          // program match found
          // process all selected stations
          for(sid=0;sid<os.nstations;sid++) {
            bid=sid>>3;
            s=sid&0x07;
            // skip if the station is:
            // - master station (because master cannot be scheduled independently
            // - running (cannot handle overlapping schedules of the same station)
            // - disabled
            if ((os.status.mas==sid+1) || (os.station_bits[bid]&(1<<s)) || (os.stndis_bits[bid]&(1<<s)))
              continue;

            // if station has non-zero water time and if it doesn't already have a scheduled stop time
            if (prog.durations[sid] && !pd.scheduled_stop_time[sid]) {
              // initialize schedule data by storing water time temporarily in stop_time
              // water time is scaled by watering percentage
              ulong water_time = (ulong)water_time_decode(prog.durations[sid]);
              // if the program is set to use weather scaling
              if (prog.use_weather)
                water_time = water_time * os.options[OPTION_WATER_PERCENTAGE].value / 100;
              pd.scheduled_stop_time[sid] = water_time;

              if (pd.scheduled_stop_time[sid]) {
                // check if water time is still valid
                // because it may end up being zero after scaling
                pd.scheduled_program_index[sid] = pid+1;
                match_found = true;
              }// if pd.scheduled_stop_time[sid]
            }// if prog.durations[sid]
          }// for sid
        }// if check_match
      }// for pid

      // calculate start and end time
      if (match_found) {
        schedule_all_stations(curr_time);
      }
    }//if_check_current_minute

    // ====== Run program data ======
    // Check if a program is running currently
    // If so, do station run-time keeping
    if (os.status.program_busy){
      for(bid=0;bid<os.nboards; bid++) {
        bitvalue = os.station_bits[bid];
        for(s=0;s<8;s++) {
          byte sid = bid*8+s;

          // skip master station
          if (os.status.mas == sid+1) continue;
          // check if this station is scheduled, either running or waiting to run
          if (pd.scheduled_program_index[sid] > 0) {
            // if so, check if we should turn it off
            if (curr_time >= pd.scheduled_stop_time[sid]) {
              turn_off_station(sid, os.status.mas, curr_time);
            }
          }
          // if current station is not running, check if we should turn it on
          if(!((bitvalue>>s)&1)) {
            if (curr_time >= pd.scheduled_start_time[sid] && curr_time < pd.scheduled_stop_time[sid]) {
              os.set_station_bit(sid, 1);

              // upon turning on station, process relay
              // if the station is set to activate / deactivate relay
              if(os.actrelay_bits[bid]&(1<<s)) {
                // turn relay on
                os.set_relay(1);
                if(os.options[OPTION_RELAY_PULSE].value > 0) {  // if relay is set to pulse
                  delay(os.options[OPTION_RELAY_PULSE].value*10);
                  os.set_relay(0);
                }
              } // if activate relay
              // upon turning on station, process RF
              // if the station is a RF station
              if(os.rfstn_bits[bid]&(1<<s)) {
                // send RF on signal
                os.send_rfstation_signal(sid, true);
              }
            } //if curr_time > scheduled_start_time
          } // if current station is not running
        }//end_s
      }//end_bid

      // process dynamic events
      process_dynamic_events(curr_time);

      // activate / deactivate valves
      os.apply_all_station_bits();

      // check through run-time data, calculate the last stop time of sequential stations
      boolean program_still_busy = false;
      pd.last_seq_stop_time = 0;
      ulong sst;
      for(sid=0;sid<os.nstations;sid++) {
        bid = sid>>3;
        s = sid&0x07;
        // check if any sequential station has a valid stop time
        // and the stop time must be larger than curr_time
        sst = pd.scheduled_stop_time[sid];
        if (sst>curr_time) {
          if (os.stnseq_bits[bid]&(1<<s)) {   // only need to update last_seq_stop_time for sequential stations
            pd.last_seq_stop_time = (sst>pd.last_seq_stop_time ) ? sst : pd.last_seq_stop_time;
          }
          program_still_busy = true;
        }
      }

      // if no station has a schedule
      if (program_still_busy == false) {
        // turn off all stations
        os.clear_all_station_bits();
        os.apply_all_station_bits();
        // reset runtime
        pd.reset_runtime();
        // reset program busy bit
        os.status.program_busy = 0;

        // in case some options have changed while executing the program
        os.status.mas = os.options[OPTION_MASTER_STATION].value; // update master station
      }
    }//if_some_program_is_running

    // if master statino is defined
    // handle master
    if (os.status.mas>0) {
      byte mas_on_adj = os.options[OPTION_MASTER_ON_ADJ].value;
      byte mas_off_adj= os.options[OPTION_MASTER_OFF_ADJ].value;
      byte masbit = 0;
      for(sid=0;sid<os.nstations;sid++) {
        // skip if this is the master station
        if (os.status.mas == sid+1) continue;
        bid = sid>>3;
        s = sid&0x07;
        // if this station is running and is set to activate master
        if ((os.station_bits[bid]&(1<<s)) && (os.masop_bits[bid]&(1<<s))) {
          // check if timing is within the acceptable range
          if (curr_time >= pd.scheduled_start_time[sid] + mas_on_adj &&
              curr_time <= pd.scheduled_stop_time[sid] + mas_off_adj - 60) {
            masbit = 1;
            break;
          }
        }
      }
      os.set_station_bit(os.status.mas-1, masbit);
    }

    // process dynamic events
    process_dynamic_events(curr_time);

    // activate/deactivate valves
    os.apply_all_station_bits();

#if defined(ARDUINO)
    // process LCD display
    if (!ui_state)
      os.lcd_print_station(1, ui_anim_chars[curr_time%3]);
#endif

    // check network connection
    check_network();

    // check weather
    check_weather();

    // perform ntp sync
    perform_ntp_sync();

    // calculate statistics
    log_statistics(curr_time);
  }

  #if !defined(ARDUINO)
    usleep(10000);
  #endif
}

void check_weather() {
  // do not check weather if the Use Weather option is disabled, or if network is not available, or if a program is running
  if (os.status.network_fails>0 || os.status.program_busy) return;

  uint16_t inv = 180;  // recheck every 30 seconds if didn't receive anything last time
  if (os.status.wt_received)  inv = CHECK_WEATHER_INTERVAL;
  if (!os.checkwt_lasttime || ((now() - os.checkwt_lasttime) > inv)) {
    os.checkwt_lasttime = now();
    GetWeather();
  }
}

void turn_off_station(byte sid, byte mas, ulong curr_time) {
  byte bid = sid>>3;
  byte s = sid&0x07;
  os.set_station_bit(sid, 0);

  // ignore if we are turning off a station that's not running or scheduled to run
  if (!pd.scheduled_start_time[sid])  return;

  // check if the current time is past the scheduled start time,
  // because we may be turning off a station that hasn't started yet
  if (curr_time > pd.scheduled_start_time[sid]) {
    // record lastrun log (only for non-master stations)
    if(mas!=(sid+1)) {
      pd.lastrun.station = sid;
      pd.lastrun.program = pd.scheduled_program_index[sid];
      pd.lastrun.duration = curr_time - pd.scheduled_start_time[sid];
      pd.lastrun.endtime = curr_time;
      write_log(LOGDATA_STATION, curr_time);
    }

    // upon turning off station, process relay
    // if the station is set to active / deactivate relay
    if(os.actrelay_bits[bid]&(1<<s)) {
      // turn relay off
      if(os.options[OPTION_RELAY_PULSE].value > 0) {  // if relay is set to pulse
        os.set_relay(1);
        delay(os.options[OPTION_RELAY_PULSE].value*10);
      }
      os.set_relay(0);
    }
    // upon turning off station, process RF station
    // if the station is a RF station
    if(os.rfstn_bits[bid]&(1<<s)) {
      // turn off station
      os.send_rfstation_signal(sid, false);
    }
  }

  // reset program run-time data variables
  pd.scheduled_start_time[sid] = 0;
  pd.scheduled_stop_time[sid] = 0;
  pd.scheduled_program_index[sid] = 0;

}

void process_dynamic_events(ulong curr_time) {
  // check if rain is detected
  bool rain = false;
  bool en = os.status.enabled ? true : false;
  if (os.status.rain_delayed || (os.options[OPTION_USE_RAINSENSOR].value && os.status.rain_sensed)) {
    rain = true;
  }

  byte sid, s, bid, rbits, sbits;
  for(bid=0;bid<os.nboards;bid++) {
    rbits = os.ignrain_bits[bid];
    sbits = os.station_bits[bid];
    for(s=0;s<8;s++) {
      sid=bid*8+s;
      // If this is a normal program (not a run-once or test program)
      // and either the controller is disabled, or
      // if raining and ignore rain bit is cleared
      //if (!mm && (pd.scheduled_program_index[sid] != 254) &&
      if ((pd.scheduled_program_index[sid]<99) &&
          (!en || (rain && !(rbits&(1<<s)))) ) {
        if (sbits&(1<<s)) { // if station is currently running
          turn_off_station(sid, os.status.mas, curr_time);

        } else if (pd.scheduled_program_index[sid] > 0) { // if station is currently not running but is waiting to run

          // reset program data variables
          pd.scheduled_start_time[sid] = 0;
          pd.scheduled_stop_time[sid] = 0;
          pd.scheduled_program_index[sid] = 0;
        }
      }
    }
  }
}

//void schedule_all_stations(ulong curr_time, byte seq)  // remove seq option
void schedule_all_stations(ulong curr_time) {
  ulong con_start_time = curr_time + 1;   // concurrent start time
  ulong seq_start_time = con_start_time;  // sequential start time

  int16_t station_delay = water_time_decode_signed(os.options[OPTION_STATION_DELAY_TIME].value);
  // if the sequential queue has stations running
  if (pd.last_seq_stop_time > curr_time) {
    seq_start_time = pd.last_seq_stop_time + station_delay;
  }

  byte sid;

  // go through all stations and calculate start / stop time of each station
  for(sid=0;sid<os.nstations;sid++) {
    // skip master station because it's not scheduled independently
    if (os.status.mas==sid+1) continue;
    byte bid=sid>>3;
    byte s=sid&0x07;

    // if the station is not scheduled to run (scheduled_stop_time = 0)
    // or is already scheduled (i.e. start_time > 0)
    // or is already running
    // then we will skip this station
    if(!pd.scheduled_stop_time[sid] || pd.scheduled_start_time[sid] || (os.station_bits[bid]&(1<<s)))
      continue;

    // check if this is a sequential station
    if (os.stnseq_bits[bid]&(1<<s)) {
      // sequential scheduling
      pd.scheduled_start_time[sid] = seq_start_time;
      seq_start_time += pd.scheduled_stop_time[sid];
      pd.scheduled_stop_time[sid] = seq_start_time;
      seq_start_time += station_delay; // add station delay time
      DEBUG_PRINT("[");
      DEBUG_PRINT(sid);
      DEBUG_PRINT(":");
      DEBUG_PRINT(pd.scheduled_start_time[sid]);
      DEBUG_PRINT(",");
      DEBUG_PRINT(pd.scheduled_stop_time[sid]);
      DEBUG_PRINTLN("]");
    } else {
      // concurrent scheduling
      pd.scheduled_start_time[sid] = con_start_time;
      pd.scheduled_stop_time[sid] = con_start_time + pd.scheduled_stop_time[sid];
      DEBUG_PRINT("[");
      DEBUG_PRINT(sid);
      DEBUG_PRINT(":");
      DEBUG_PRINT(pd.scheduled_start_time[sid]);
      DEBUG_PRINT(",");
      DEBUG_PRINT(pd.scheduled_stop_time[sid]);
      DEBUG_PRINTLN("]");
    }
    os.status.program_busy = 1;  // set program busy bit
	}
}

void reset_all_stations_immediate() {
  os.clear_all_station_bits();
  os.apply_all_station_bits();
  pd.reset_runtime();
}

void reset_all_stations() {
  // stop all running and scheduled stations
  ulong curr_time = os.now_tz();
  for(byte sid=0;sid<os.nstations;sid++) {
    if(pd.scheduled_program_index[sid] > 0) {
      pd.scheduled_stop_time[sid] = curr_time;
    }
  }
}


// Manually start a program
// If pid==0, this is a test program (1 minute per station)
// If pid==255, this is a short test program (2 second per station)
// If pid > 0. run program pid-1
void manual_start_program(byte pid) {
  boolean match_found = false;
  reset_all_stations_immediate();
  ProgramStruct prog;
  uint16_t dur;
  byte sid, bid, s;
  if ((pid>0)&&(pid<255)) {
    pd.read(pid-1, &prog);
  }
  for(sid=0;sid<os.nstations;sid++) {
    bid=sid>>3;
    s=sid&0x07;
    dur = 60;
    if(pid==255)  dur=2;
    else if(pid>0)
      dur = water_time_decode(prog.durations[sid]);
    if (dur>0 && !(os.stndis_bits[bid]&(1<<s))) {
      pd.scheduled_stop_time[sid] = dur;
      pd.scheduled_program_index[sid] = 254;
      match_found = true;
    }
  }
  if(match_found) {
    schedule_all_stations(os.now_tz());
  }
}

// ================================
// ====== LOGGING FUNCTIONS =======
// ================================
// Log files will be named /logs/xxxxx.txt
#if defined(ARDUINO)
char LOG_PREFIX[] = "/logs/";
#else
char LOG_PREFIX[] = "./logs/";
#endif

void make_logfile_name(char *name) {
#if defined(ARDUINO)
  sd.chdir("/");
#endif
  strcpy(tmp_buffer+TMP_BUFFER_SIZE-10, name);
  strcpy(tmp_buffer, LOG_PREFIX);
  strcat(tmp_buffer, tmp_buffer+TMP_BUFFER_SIZE-10);
  strcat_P(tmp_buffer, PSTR(".txt"));
}

const char *log_type_names[] = {
  "",
  "rs",
  "rd",
  "wl"
};

void log_statistics(time_t curr_time) {
  static byte stat_n = 0;
  static ulong stat_lasttime = 0;
  // update statistics once 15 minutes
  if (curr_time - stat_lasttime > STAT_UPDATE_INTERVAL) {
    stat_lasttime = curr_time;
    ulong wp_total = os.water_percent_avg;
    wp_total = wp_total * stat_n;
    wp_total += os.options[OPTION_WATER_PERCENTAGE].value;
    stat_n ++;
    os.water_percent_avg = byte(wp_total / stat_n);
    // writes every 4*24 times (1 day)
    if (stat_n == 96) {
      DEBUG_PRINTLN("wl");
      write_log(LOGDATA_WATERLEVEL, curr_time);
      stat_n = 0;
    }
  }
}

// write run record to log on SD card
void write_log(byte type, ulong curr_time) {
  if (!os.options[OPTION_ENABLE_LOGGING].value) return;

  // file name will be logs/xxxxx.tx where xxxxx is the day in epoch time
  ultoa(curr_time / 86400, tmp_buffer, 10);
  make_logfile_name(tmp_buffer);

#if defined(ARDUINO) // prepare log folder for Arduino
  if (!os.status.has_sd)  return;

  sd.chdir("/");
  if (sd.chdir(LOG_PREFIX) == false) {
    // create dir if it doesn't exist yet
    if (sd.mkdir(LOG_PREFIX) == false) {
      return;
    }
  }
  SdFile file;
  file.open(tmp_buffer, O_CREAT | O_WRITE );
  file.seekEnd();
#else // prepare log folder for RPI/BBB
  struct stat st;
  if(stat(LOG_PREFIX, &st)) {
    if(mkdir(LOG_PREFIX, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH)) {
      return;
    }
  }
  FILE *file;
  file = fopen(tmp_buffer, "rb+");
  if(!file) {
    file = fopen(tmp_buffer, "wb");
    if (!file)  return;
  }
  fseek(file, 0, SEEK_END);
#endif  // prepare log folder

  strcpy_P(tmp_buffer, PSTR("["));

  if(type == LOGDATA_STATION) {
    itoa(pd.lastrun.program, tmp_buffer+strlen(tmp_buffer), 10);
    strcat_P(tmp_buffer, PSTR(","));
    itoa(pd.lastrun.station, tmp_buffer+strlen(tmp_buffer), 10);
    strcat_P(tmp_buffer, PSTR(","));
    itoa(pd.lastrun.duration, tmp_buffer+strlen(tmp_buffer), 10);
  } else {
    strcat_P(tmp_buffer, PSTR("0,\""));
    strcat(tmp_buffer, log_type_names[type]);
    strcat_P(tmp_buffer, PSTR("\","));
    switch(type) {
      case LOGDATA_RAINSENSE:
        //str += (curr_time - os.rainsense_start_time);
        ultoa((curr_time - os.rainsense_start_time), tmp_buffer+strlen(tmp_buffer), 10);
        break;
      case LOGDATA_RAINDELAY:
        //str += (curr_time - os.raindelay_start_time);
        ultoa((curr_time - os.raindelay_start_time), tmp_buffer+strlen(tmp_buffer), 10);
        break;
      case LOGDATA_WATERLEVEL:
        //str += os.water_percent_avg;
        itoa(os.water_percent_avg, tmp_buffer+strlen(tmp_buffer), 10);
        break;
    }
  }
  strcat_P(tmp_buffer, PSTR(","));
  ultoa(curr_time, tmp_buffer+strlen(tmp_buffer), 10);
  strcat_P(tmp_buffer, PSTR("]\r\n"));

#if defined(ARDUINO)
  file.write(tmp_buffer);
  file.close();
#else
  fwrite(tmp_buffer, 1, strlen(tmp_buffer), file);
  fclose(file);
#endif
}


// delete log file
// if name is 'all', delete all logs
void delete_log(char *name) {
  if (!os.options[OPTION_ENABLE_LOGGING].value) return;
#if defined(ARDUINO)
  if (!os.status.has_sd) return;

  if (strncmp(name, "all", 3) == 0) {
    // delete the log folder
    SdFile file;

    if (sd.chdir(LOG_PREFIX)) {
      // delete the whole log folder
      sd.vwd()->rmRfStar();
    }
    return;
  } else {
    make_logfile_name(name);
    if (!sd.exists(tmp_buffer))  return;
    sd.remove(tmp_buffer);
  }
#else // delete_log implementation for RPI/BBB
  if (strncmp(name, "all", 3) == 0) {
    // delete the log folder
    rmdir(LOG_PREFIX);
    return;
  } else {
    make_logfile_name(name);
    remove(tmp_buffer);
  }
#endif
}

void check_network() {
#if defined(ARDUINO)
  if (os.status.program_busy) {return;}

  // do not perform network checking if the controller has just started, or if a program is running
  if (!os.network_lasttime) {
    os.start_network();
  }

  // check network condition periodically
  // check interval depends on the fail times
  // the more time it fails, the longer the gap between two checks
  ulong interval = 1 << (os.status.network_fails);
  interval *= CHECK_NETWORK_INTERVAL;
  if (now() - os.network_lasttime > interval) {
    // change LCD icon to indicate it's checking network
    if (!ui_state) {
      os.lcd.setCursor(15, 1);
      os.lcd.write(4);
    }

    os.network_lasttime = now();

    // ping gateway ip
    ether.clientIcmpRequest(ether.gwip);

    ulong start = millis();
    boolean failed = true;
    // wait at most PING_TIMEOUT milliseconds for ping result
    do {
      ether.packetLoop(ether.packetReceive());
      if (ether.packetLoopIcmpCheckReply(ether.gwip)) {
        failed = false;
        break;
      }
    } while(millis() - start < PING_TIMEOUT);
    if (failed)  {
      os.status.network_fails++;
      // clamp it to 6
      if (os.status.network_fails > 6) os.status.network_fails = 6;
    }
    else os.status.network_fails=0;
    // if failed more than once, reconnect
    if ((os.status.network_fails>2 || (now() - os.dhcpnew_lasttime > DHCP_RENEW_INTERVAL))) {
      os.dhcpnew_lasttime = now();
      //os.lcd_print_line_clear_pgm(PSTR(""),0);
      if (os.start_network())
        os.status.network_fails=0;
    }
  }
#else
  // nothing to do here
  // Linux will do this for you
#endif
}

void perform_ntp_sync() {
#if defined(ARDUINO)
  // do not perform sync if this option is disabled, or if network is not available, or if a program is running
  if (!os.options[OPTION_USE_NTP].value || os.status.network_fails>0 || os.status.program_busy) return;

  if (os.ntpsync_lasttime == 0 || (now() - os.ntpsync_lasttime > NTP_SYNC_INTERVAL)) {
    os.ntpsync_lasttime = now();
    if (!ui_state) {
      os.lcd_print_line_clear_pgm(PSTR("NTP Syncing..."),1);
    }
    ulong t = getNtpTime();
    if (t>0) {
      setTime(t);
      if (os.status.has_rtc) RTC.set(t); // if rtc exists, update rtc
    }
  }
#else
  // nothing to do here
  // Linux will do this for you
#endif
}


#if !defined(ARDUINO) // main function for RPI/BBB
int main(int argc, char *argv[]) {
  do_setup();

  while(true) {
    do_loop();
  }
  return 0;
}
#endif
