/*
 *  Joystick driver for MicroDigital Mico
 *  Copyright (C) 2002 Chris Bazley
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* ANSI headers */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <ctype.h>

/* RISC OS headers */
#include "kernel.h"
#include "swis.h"
#include "syslog.h"

/* CMHG header */
#include "MicoJoyHdr.h"
#include "MicoJoyErr.h"


/*
   Doggysoft's SysLog module is used for outputting debugging info
   Either comment out the following line, or append -DDEBUG to cc's command line
*/
//#define DEBUG

#ifdef DEBUG
static const char *log_name = "Joystick";
#endif

/*
   Default tolerance (in 탎/2) of delays in registering axis bit change
    - if too small then slow machines won't read stick at all
    - if too long then inaccurate values (caused by interrupts)
      may go undetected
*/
#define MAX_GRANULARITY 30 /* default */

/*
   Default maximum time (in 탎/2) to wait for axis bits
*/
#define MAX_AXIS_WAIT_TIME 2000 /* default */

/*
   Joystick polling frequency (in cs), must be at least 2!
*/
#define POLL_FREQUENCY 7 /* default */

/*
   Interval (in cs) between calling the Joystick_Read usage monitor
*/
#define MONITOR_INTERVAL 1000

/*
   Number of joysticks supported - for readability
   (No guarantees that you can change this and still have it work!)
*/
#define NUM_STICKS 2

/*
   Number of tests runs to do - for get_av_stick_pos() and reinit_joysticks()
*/
#define NUM_TEST_RUNS 32

/* This comes out really neat in ARM code, honest! */
#define absdiff(d, x, y) { \
  if(x > y)    \
    d = x - y; \
  else         \
    d = y - x; \
}

/* Guard against divide by zero */
#define safedivide(quotient, dividend, divisor) { \
  int div = divisor; \
  if(div != 0) \
    quotient = (dividend) / (divisor); \
  else \
    quotient = 0; \
}

/*
   Details of PC gameport on ISA card
*/

#define PC_JOY_A_X  (1u << 0)
#define PC_JOY_A_Y  (1u << 1)
#define PC_JOY_B_X  (1u << 2)
#define PC_JOY_B_Y  (1u << 3)
#define PC_JOY_A_B1 (1u << 4)
#define PC_JOY_A_B2 (1u << 5)
#define PC_JOY_B_B1 (1u << 6)
#define PC_JOY_B_B2 (1u << 7) /* status packed into single byte */

static volatile unsigned char *game_port_address; /* get address from PnP manager */
static unsigned int axes_mask = 0; /* axes bits to read - set by reinit_joysticks() */

/*
   Details of IOC chip (used for timing)
   All IOC registers padded out from a byte to a word (access as reg[0])
*/

#define IOC_ADDRESS   0x03200000

typedef struct _IOC_Timer { /* subset of IOC registers */
  unsigned char low[4]; /* count low (read) / latch low (write) */
  unsigned char high[4]; /* count high (read) / latch high (write) */
  unsigned char go[4]; /* go command (write) */
  unsigned char latch[4]; /* latch command (write)*/
} IOC_Timer;

typedef struct _IOC_Int { /* subset of IOC registers */
  unsigned char status[4]; /* (read) */
  unsigned char request[4]; /* (read) / clear interrupt bits (IRQ A only) */
  unsigned char mask[4]; /* (read/write) */
  unsigned char uk[4];
} IOC_Int;

typedef struct _IOC { /* IOC registers (All 8 bits wide) */

  volatile unsigned char control[4]; /* (read/write) */
  volatile unsigned char keyboard[4]; /* receive (read) / send (write) */
  volatile unsigned char uk1[4];
  volatile unsigned char uk2[4];

  /* IRQ / FIQ regs... */
  volatile IOC_Int  IRQ_A;
  volatile IOC_Int  IRQ_B;
  volatile IOC_Int  FIQ;

  /* Timer regs... */
  volatile IOC_Timer timer_0;
  volatile IOC_Timer timer_1;
  volatile IOC_Timer timer_2;
  volatile IOC_Timer timer_3;
} IOC;

/*
   Calibration state - for Joystick_CalibrateTopRight & Joystick_CalibrateBottomLeft
   (both must be called before completion)
*/

#define CALIB_NONE        0
#define CALIB_TOP_RIGHT   1
#define CALIB_BOTTOM_LEFT 2

static int calib_status = CALIB_NONE;

/*
   Current axis time values (possibly smoothed)
*/

static unsigned int x_axis[NUM_STICKS], y_axis[NUM_STICKS];

/*
     Values established by calibration
                                                
    min              ctr_low   ctr  ctr_high             max
     |        <---------|       |      |--------->        |
      \__  __/           \_____  _____/           \__  __/
         \/                    \/                    \/
      end_deadz            ctr_deadz              end_deadz
*/

static unsigned int x_min[NUM_STICKS], x_max[NUM_STICKS], y_min[NUM_STICKS], y_max[NUM_STICKS];
static unsigned int x_ctr_deadz[NUM_STICKS], y_ctr_deadz[NUM_STICKS], x_ctr[NUM_STICKS], y_ctr[NUM_STICKS];
static unsigned int x_end_deadz[NUM_STICKS], y_end_deadz[NUM_STICKS];
static unsigned int x_smooth[NUM_STICKS], y_smooth[NUM_STICKS];

/*
   Values used in *actual* conversion to 8-bit / 16-bit position:
*/

#define SCALER_FRAC_SHIFT 14

static unsigned int x_ctr_low[NUM_STICKS], y_ctr_low[NUM_STICKS], x_ctr_high[NUM_STICKS], y_ctr_high[NUM_STICKS];
static unsigned int x_low_scaler[NUM_STICKS], x_high_scaler[NUM_STICKS], y_low_scaler[NUM_STICKS], y_high_scaler[NUM_STICKS];


/*
   Internal joystick polling state
*/

static bool polling_stick = false, /* attached OS_CallEvery to pollstick_veneer? */
            swi_in_last_min = false, /* continue polling? (checked periodically) */
            callback_pending = false, /* outstanding CallBack to doread_veneer? */
            callback_free = true; /* may we add another CallBack? (none in progress) */

/*
  Global configuration (set using *JoystickConfig)
*/

static unsigned int max_wait = MAX_AXIS_WAIT_TIME, tolerance = MAX_GRANULARITY;
static bool smooth = true, end_zones = true, ctr_zones = true;
static unsigned int poll_freq = POLL_FREQUENCY-1;

/*
  Command syntax strings for use with OS_ReadArgs
*/

static const char config_syntax[] = "smooth/S,nosmooth/S,ctrzone/S,noctrzone/S,endzone/S,noendzone/S,tolerance/E/K,timeout/E/K,poll/E/K";
#define CONFIG_SYNTAX_SMOOTH     0
#define CONFIG_SYNTAX_NOSMOOTH   1
#define CONFIG_SYNTAX_CTRZONE    2
#define CONFIG_SYNTAX_NOCTRZONE  3
#define CONFIG_SYNTAX_ENDZONE    4
#define CONFIG_SYNTAX_NOENDZONE  5
#define CONFIG_SYNTAX_TOLERANCE  6
#define CONFIG_SYNTAX_TIMEOUT    7
#define CONFIG_SYNTAX_POLL       8

static const char calib_syntax[] = "/E/A,/A,min/E/K,ctr/E/K,max/E/K,ctrzone/E/K,endzone/E/K,smooth/E/K";
#define CALIB_SYNTAX_JOYNUM    0
#define CALIB_SYNTAX_XORY      1
#define CALIB_SYNTAX_MIN       2
#define CALIB_SYNTAX_CTR       3
#define CALIB_SYNTAX_MAX       4
#define CALIB_SYNTAX_CTRZONE   5
#define CALIB_SYNTAX_ENDZONE   6
#define CALIB_SYNTAX_SMOOTH    7

static const char reinit_syntax[] = "/E";
#define REINIT_SYNTAX_JOYNUM   0

#define  UNUSED(x)             (x = x)
/* (suppress strict compiler warnings about unused parameters) */

/* ----------------------------------------------------------------------- */
/*                       Function prototypes                               */

#define STICK_0 (1u << 0)
#define STICK_1 (1u << 1) /* flags for get_av_stick_pos() and reinit_joysticks() */

#define X_BIAS_MIN (1u << 0)
#define X_BIAS_MAX (1u << 1)
#define Y_BIAS_MIN (1u << 2)
#define Y_BIAS_MAX (1u << 3) /* directional bias for get_av_stick_pos() */

static unsigned int read_joystick(unsigned int mask, unsigned int *lost);
static void recalc_coefficients(int sticks);
static unsigned int smooth_value(unsigned int prev_value, unsigned int new_value, unsigned int stddev);
static void get_av_stick_pos(unsigned int sticks, unsigned int *x_array, unsigned int *y_array, unsigned int *x_jitdist, unsigned int *y_jitdist, int bias);
static int eval_expr(char *buffer);
static void reinit_joysticks(unsigned int sticks);
static void update_min_max(unsigned int *axis, unsigned int *jit_min, unsigned int *jit_max, int stick_num);

/* ----------------------------------------------------------------------- */
/*                         Public functions                                */

_kernel_oserror *MicoJoy_initialise(const char *cmd_tail, int podule_base, void *pw)
{
  char addr_buffer[10];
  
  UNUSED(podule_base);
  UNUSED(cmd_tail);

#ifdef DEBUG
  if(_kernel_oscli("RMEnsure SysLog 0.17 Error Needs SysLog 0.17 or later") == _kernel_ERROR)
    return _kernel_last_oserror();
#endif

#ifdef DEBUG
  xsyslog_logmessage(log_name, "Initialising Joystick module", 1);
#endif

  /*
    Check whether Plug'n'Play properly initialised
  */
  if(_kernel_getenv("PnPManager$GamesPort_Address", addr_buffer, sizeof(addr_buffer)) != NULL)
    return &gameport_not_found;
  {
    int matched= sscanf(addr_buffer, "&%x", &game_port_address);
    if(matched == EOF || matched < 1)
      return &gameport_not_found;
  }
  reinit_joysticks(STICK_0|STICK_1);
  
  /* Attach routine to monitor whether Joystick SWIs are being called */
  return _swix(OS_CallEvery, _INR(0,2), MONITOR_INTERVAL, stoppoll_veneer, pw);
}

/* ----------------------------------------------------------------------- */

_kernel_oserror *MicoJoy_swihandler(int swi_no, _kernel_swi_regs *r, void *private_word)
{
  if((swi_no == (Joystick_CalibrateTopRight-Joystick_00) || swi_no == (Joystick_CalibrateBottomLeft-Joystick_00)) && calib_status == CALIB_NONE && polling_stick) {
     /* cease polling stick for duration of calibration (just interferes) */
    _kernel_oserror *e;
#ifdef DEBUG
    xsyslog_logmessage(log_name, "Removing CallEvery to pollstick_veneer (for calibration)", 1);
#endif
    e = _swix(OS_RemoveTickerEvent, _INR(0,1), pollstick_veneer, private_word);
    if(e == NULL) {
      polling_stick = false;
    } else {
#ifdef DEBUG
      xsyslog_logmessage(log_name, e->errmess, 0);
#endif
    }
  }

  switch(swi_no) {
    case (Joystick_Read-Joystick_00):
#ifdef DEBUG
      xsyslog_logmessage(log_name, "SWI Joystick_Read", 1);
#endif
      if(calib_status != CALIB_NONE)
        return &error_calib; /* fail */

      swi_in_last_min = true;
      if(!polling_stick) {
        /* Restart polling after a period of inactivity */
        _kernel_oserror *e;
        int stick_num;
#ifdef DEBUG
        xsyslog_logmessage(log_name, "Joystick_Read after inactivity - registering CallEvery to pollstick_veneer", 1);
#endif
        e = _swix(OS_CallEvery, _INR(0,2), poll_freq, pollstick_veneer, private_word);
        if(e != NULL)
          return e;
        polling_stick = true;
        
        /* We must assume that all values are terribly out of date */
        for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
          int xc = x_ctr[stick_num], yc = y_ctr[stick_num];
          x_axis[stick_num] = xc;
          y_axis[stick_num] = yc;
       } /* next stick_num */
      } /* endif !polling_stick */

      {
        unsigned char stick_num = r->r[0] & 0xff;
        unsigned char reason_code = (r->r[0] & 0xff00) >> 8;

        switch(reason_code) {
          case 0:
            /* Read 8-bit state of an analogue or switched joystick */
            if(stick_num < NUM_STICKS) {
              /* First two joysticks are supported */
              { /* Use cached stick positions */
                signed int x, y;

                /* calc joystick 8-bit position */
#ifdef DEBUG
                printf("x_axis[%d] = %u (%u-%u) y_axis[%d] = %u (%u-%u)\n", stick_num, x_axis[stick_num], x_min[stick_num], x_max[stick_num], stick_num, y_axis[stick_num], y_min[stick_num], y_max[stick_num]);
#endif

                if(x_axis[stick_num] > x_ctr_low[stick_num]) {
                  if(x_axis[stick_num] < x_ctr_high[stick_num]) {
                    /* In centre dead zone */
                    x = 0;
                  } else {
                    /* Above centre dead zone */
                    x = (x_high_scaler[stick_num] * (x_axis[stick_num] - x_ctr_high[stick_num])) >> (SCALER_FRAC_SHIFT+8);
                  }
                } else {
                  /* Below centre dead zone */
                  x = - ((x_low_scaler[stick_num] * (x_ctr_low[stick_num] - x_axis[stick_num])) >> (SCALER_FRAC_SHIFT+8));
                }
                /* Make absolutely sure value within range */
                if(x < -127)
                  x = -127;
                else {
                  if(x > 127)
                    x = 127;
                }

                if(y_axis[stick_num] > y_ctr_low[stick_num]) {
                  if(y_axis[stick_num] < y_ctr_high[stick_num]) {
                    /* In centre dead zone */
                    y = 0;
                  } else {
                    /* Above centre dead zone */
#ifdef DEBUG
                    printf("y = %d/%d\n",y_axis[stick_num] - y_ctr_high[stick_num], y_max[stick_num] - y_ctr_high[stick_num]);
#endif
                    y = -((y_high_scaler[stick_num] * (y_axis[stick_num] - y_ctr_high[stick_num])) >> (SCALER_FRAC_SHIFT+8));
                  }
                } else {
                  /* Below centre dead zone */
                  y = (y_low_scaler[stick_num] * (y_ctr_low[stick_num] - y_axis[stick_num])) >> (SCALER_FRAC_SHIFT+8);
                }
                /* Make absolutely sure value within range */
                if(y < -127)
                  y = -127;
                else {
                  if(y > 127)
                    y = 127;
                }

                r->r[0] = (y & 0xff) | ((x & 0xff)<<8);
              }
              { /* Read fire buttons */
                unsigned char joy;
                unsigned int buttons;

                joy = *game_port_address; /* read joystick status bits */
                buttons = 0;
                if(stick_num == 0) {
                  /* return joystick A buttons */
                  if(!(joy & PC_JOY_A_B1))
                    buttons |= (1u<<16);
                  if(!(joy & PC_JOY_A_B2))
                    buttons |= (1u<<17);
                } else {
                  /* return joystick B buttons */
                  if(!(joy & PC_JOY_B_B1))
                    buttons |= (1u<<16);
                  if(!(joy & PC_JOY_B_B2))
                    buttons |= (1u<<17);
                }
                r->r[0] |= buttons;
              }
            }
            else {
              /* Other joysticks aren't supported */
              r->r[0] = 0; /* 8-bit centred, nothing pressed */
            }
            break;

          case 1:
            /* Read 16-bit state of an analogue joystick*/
            if(stick_num < NUM_STICKS) {
              /* First two joysticks are supported */

              { /* Use cached stick positions */
                signed int x, y;
                /* calc joystick 16-bit position */
#ifdef DEBUG
                printf("x_axis[%d] = %u (%u-%u) y_axis[%d] = %u (%u-%u)\n", stick_num, x_axis[stick_num], x_min[stick_num], x_max[stick_num], stick_num, y_axis[stick_num], y_min[stick_num], y_max[stick_num]);
#endif
                if(x_axis[stick_num] > x_ctr_low[stick_num]) {
                  if(x_axis[stick_num] < x_ctr_high[stick_num]) {
                    /* In centre dead zone */
                    x = 0x7fff;
                  } else {
                    /* Above centre dead zone */
                    x = 0x7fff + ((x_high_scaler[stick_num] * (x_axis[stick_num] - x_ctr_high[stick_num])) >> SCALER_FRAC_SHIFT);
                  }
                } else {
                  /* Below centre dead zone */
                  x = 0x7fff - ((x_low_scaler[stick_num] * (x_ctr_low[stick_num] - x_axis[stick_num])) >> SCALER_FRAC_SHIFT);
                }
                /* Make absolutely sure value within range */
                if(x < 0)
                  x = 0;
                else {
                  if(x > 0xffff)
                    x = 0xffff;
                }

                if(y_axis[stick_num] > y_ctr_low[stick_num]) {
                  if(y_axis[stick_num] < y_ctr_high[stick_num]) {
                    /* In centre dead zone */
                    y = 0x7fff;
                  } else {
                    /* Above centre dead zone */
#ifdef DEBUG
                    printf("y = %d/%d\n",y_axis[stick_num] - y_ctr_high[stick_num], y_max[stick_num] - y_ctr_high[stick_num]);
#endif
                    y = 0x7fff - ((y_high_scaler[stick_num] * (y_axis[stick_num] - y_ctr_high[stick_num])) >> SCALER_FRAC_SHIFT);
                  }
                } else {
                  /* Below centre dead zone */
                  y = 0x7fff + ((y_low_scaler[stick_num] * (y_ctr_low[stick_num] - y_axis[stick_num])) >> SCALER_FRAC_SHIFT);
                }
                /* Make absolutely sure value within range */
                if(y < 0)
                  y = 0;
                else {
                  if(y > 0xffff)
                    y = 0xffff;
                }

                r->r[0] = (y & 0xffff) | (x << 16);
              }
              { /* Read fire buttons */
                unsigned char joy;
                unsigned int buttons;

                joy = *game_port_address; /* read joystick status bits */
                buttons = 0;
                if(stick_num == 0) {
                  /* return joystick A buttons */
                  if(!(joy & PC_JOY_A_B1))
                    buttons |= (1u<<0);
                  if(!(joy & PC_JOY_A_B2))
                    buttons |= (1u<<1);
                } else {
                  /* return joystick B buttons */
                  if(!(joy & PC_JOY_B_B1))
                    buttons |= (1u<<0);
                  if(!(joy & PC_JOY_B_B2))
                    buttons |= (1u<<1);
                }
                r->r[1] = buttons;
              }
            }
            else {
              /* Other joysticks aren't supported */
              r->r[0] = 0x7fff7fff; /* 16-bit centre position */
              r->r[1] = 0; /* switch state */
            }
            break;

          default:
            /* Unknown reason code! */
            return &bad_reason; /* fail */
        }
      }
      return NULL; /* success */

    case (Joystick_CalibrateTopRight-Joystick_00):
      {
#ifdef DEBUG
        xsyslog_logmessage(log_name, "SWI Joystick_CalibrateTopRight", 1);
#endif
  
        calib_status |= CALIB_TOP_RIGHT;
        if(calib_status == (CALIB_TOP_RIGHT|CALIB_BOTTOM_LEFT)) {
          unsigned int x_jitdist[2], y_jitdist[2];
          int stick_num;

          get_av_stick_pos(STICK_0|STICK_1, x_max, y_min, x_jitdist, y_jitdist, X_BIAS_MIN|Y_BIAS_MAX);
          for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
            if(x_jitdist[stick_num] > x_end_deadz[stick_num])
              x_end_deadz[stick_num] = x_jitdist[stick_num];
       
            if(y_jitdist[stick_num] > y_end_deadz[stick_num])
              y_end_deadz[stick_num] = y_jitdist[stick_num];
          }
          
          /* calibration complete */
          recalc_coefficients(STICK_0|STICK_1);
          calib_status = CALIB_NONE;
        } else {
          get_av_stick_pos(STICK_0|STICK_1, x_max, y_min, x_end_deadz, y_end_deadz, X_BIAS_MIN|Y_BIAS_MAX);
        }
      }
      return NULL; /* success */

    case (Joystick_CalibrateBottomLeft-Joystick_00):
      {
#ifdef DEBUG
        xsyslog_logmessage(log_name, "SWI Joystick_CalibrateBottomLeft", 1);
#endif

        calib_status |= CALIB_BOTTOM_LEFT;
        if(calib_status == (CALIB_TOP_RIGHT|CALIB_BOTTOM_LEFT)) {
          unsigned int x_jitdist[2], y_jitdist[2];
          int stick_num;

          get_av_stick_pos(STICK_0|STICK_1, x_min, y_max, x_jitdist, y_jitdist, X_BIAS_MAX|Y_BIAS_MIN);
          for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
            if(x_jitdist[stick_num] > x_end_deadz[stick_num])
              x_end_deadz[stick_num] = x_jitdist[stick_num];
       
            if(y_jitdist[stick_num] > y_end_deadz[stick_num])
              y_end_deadz[stick_num] = y_jitdist[stick_num];
          }
          
          /* calibration complete */
          recalc_coefficients(STICK_0|STICK_1);
          calib_status = CALIB_NONE;
        } else {
          get_av_stick_pos(STICK_0|STICK_1, x_min, y_max, x_end_deadz, y_end_deadz, X_BIAS_MAX|Y_BIAS_MIN);
        }
      }
      return NULL; /* success */

    default:
      return error_BAD_SWI; /* fail */
  }
}

/* ----------------------------------------------------------------------- */

_kernel_oserror *MicoJoy_cmdhandler(const char *arg_string, int argc, int cmd_no, void *pw)
{
  UNUSED(pw);
  
#ifdef DEBUG
  printf("argc: %d cmd_no: %d \n", argc, cmd_no);
#endif /* DEBUG */
  switch(cmd_no) {

    case CMD_JoystickInfo:
      /* Syntax: *JoystickInfo */
      printf("Axis Minimum Centre Maximum Ctr zone End zone Smooth\n");
      printf("---- ------- ------ ------- -------- -------- ------\n");
      {
        int stick_num;
        for(stick_num = 0; stick_num < NUM_STICKS; stick_num++) {
          printf(" %d X", stick_num);
          printf(" %7u %6u %7u %8u %8u %6u\n", x_min[stick_num], x_ctr[stick_num], x_max[stick_num], x_ctr_deadz[stick_num], x_end_deadz[stick_num], x_smooth[stick_num]);
          /* (split so that the longer formatting string can be re-used) */
          printf(" %d Y", stick_num);
          printf(" %7u %6u %7u %8u %8u %6u\n", y_min[stick_num], y_ctr[stick_num], y_max[stick_num], y_ctr_deadz[stick_num], y_end_deadz[stick_num], y_smooth[stick_num]);
        } /* next stick_num */
      }
      break;
      
    case CMD_JoystickConfig:
      /* Syntax: *JoystickConfig [-smooth|-nosmooth] [-ctrzone|-noctrzone] [-endzone|-noendzone] [-tolerance <interval>] [-timeout <delay>] [-poll <frequency>] */
      if(argc > 0) {
        /*
           Can have no more than 9 args - the worst case includes all 3 evaluated elements (6 args) and 3 of the possible switches (3 args). Allow one memory word for each element, plus sufficient buffer space for evaluated element blocks.
         */
        char *args_buf[(6*4) + (3*8)];
        {
          _kernel_oserror *e = _swix(OS_ReadArgs, _INR(0,3), config_syntax, arg_string, args_buf, sizeof(args_buf));
          if(e != NULL)
            return e;
        }
        if((args_buf[CONFIG_SYNTAX_SMOOTH] != 0 && args_buf[CONFIG_SYNTAX_NOSMOOTH] != 0)
        || (args_buf[CONFIG_SYNTAX_CTRZONE] != 0 && args_buf[CONFIG_SYNTAX_NOCTRZONE] != 0)
        || (args_buf[CONFIG_SYNTAX_ENDZONE] != 0 && args_buf[CONFIG_SYNTAX_NOENDZONE] != 0)) {
          /* both 'smooth' and 'nosmooth' */
          return &error_command_syntax;
        }
        if(args_buf[CONFIG_SYNTAX_SMOOTH] != 0)
          smooth = true; /* enable smoothing */
        else {
          if(args_buf[CONFIG_SYNTAX_NOSMOOTH] != 0)
            smooth = false; /* disable smoothing */
        }
        {
          bool recalc = false;
          if(args_buf[CONFIG_SYNTAX_CTRZONE] != 0) {
            if(!ctr_zones) {
              recalc = true;
              ctr_zones = true; /* globally enable centre zones */
            }
          } else {
            if(args_buf[CONFIG_SYNTAX_NOCTRZONE] != 0) {
              recalc = true;
              ctr_zones = false; /* globally disable centre zones */
            }
          }
          if(args_buf[CONFIG_SYNTAX_ENDZONE] != 0) {
            if(!ctr_zones) {
              recalc = true;
              end_zones = true; /* globally enable end zones */
            }
          } else {
            if(args_buf[CONFIG_SYNTAX_NOENDZONE] != 0) {
              recalc = true;
              end_zones = false; /* globally disable end zones */
            }
          }
          if(recalc)
            recalc_coefficients(STICK_0|STICK_1); /* Make it so */
        }
        
        if(args_buf[CONFIG_SYNTAX_TOLERANCE] != 0)
          tolerance = eval_expr(args_buf[CONFIG_SYNTAX_TOLERANCE]);

        if(args_buf[CONFIG_SYNTAX_TIMEOUT] != 0)
          max_wait = eval_expr(args_buf[CONFIG_SYNTAX_TIMEOUT]);
          
        if(args_buf[CONFIG_SYNTAX_POLL] != 0) {
          int new_freq = eval_expr(args_buf[CONFIG_SYNTAX_POLL]) - 1;
          if(new_freq <= 0)
            new_freq = 1; /* minimum delay is 2 centiseconds */

          if(new_freq != poll_freq) {
            if(polling_stick) {
              /* First stop polling at old frequency */
              _kernel_oserror *e = _swix(OS_RemoveTickerEvent, _INR(0,1), pollstick_veneer, pw);
              if(e != NULL)
                return e;
           
              /* Now start polling at new frequency */
              e = _swix(OS_CallEvery, _INR(0,2), new_freq, pollstick_veneer, pw);
              if(e != NULL) {
                polling_stick = false; /* we've buggered it up */
                return e;
              } /* endif e != NULL */
            }
            poll_freq = new_freq; /* store new poll frequency */
          } /* endif new_freq != poll_freq */
        } /* endif args_buf[CONFIG_SYNTAX_TOLERANCE] != 0 */
      } /* endif argc > 0 */
      else {
        /* display current setting */
        printf("Joystick driver configuration:");
        if(smooth)
          printf(" -smooth");
        else
          printf(" -nosmooth");
        if(ctr_zones)
          printf(" -ctrzone");
        else
          printf(" -noctrzone");
        if(end_zones)
          printf(" -endzone");
        else
          printf(" -noendzone");
        printf(" -tolerance %u -timeout %u -poll %u\n", tolerance, max_wait, poll_freq+1);
      }
      break;

    case CMD_JoystickCalib:
      /* Syntax: *JoystickCalib <stick number> <axis> [-min <time>] [-ctr <time>] [-max <time>] [-ctrzone <interval>] [-endzone <interval>] [-smooth <interval>] */
      {
        /*
           Can have no more than 14 args - the worst case is 6 evaluated elements with identifiers (12 args), 1 EE without id (1 arg) and 1 string element (1 arg). Allow one memory word for each element, plus sufficient buffer space for evaluated element blocks, plus a bit extra for the string.
         */
        char *args_buf[(8*4) + (7*8) + 4];
        int joynum;
        bool change_x;
        {
          _kernel_oserror *e = _swix(OS_ReadArgs, _INR(0,3), calib_syntax, arg_string, args_buf, sizeof(args_buf));
          if(e != NULL)
            return e;
        }
        {
          joynum = eval_expr(args_buf[CALIB_SYNTAX_JOYNUM]);
          if(joynum > (NUM_STICKS-1) || joynum < 0)
            return &bad_joy_num;
        }
        {
          char *string = (char *)args_buf[CALIB_SYNTAX_XORY];
          if(strcmp(string, "x") == 0 || strcmp(string, "X") == 0) {
            change_x = true;
          } else {
            if(strcmp(string, "y") == 0 || strcmp(string, "Y") == 0)
              change_x = false;
            else
              return &error_command_syntax;
          }
        }
        if(args_buf[CALIB_SYNTAX_MIN] != 0) {
          int value = eval_expr(args_buf[CALIB_SYNTAX_MIN]);
          if(change_x)
            x_min[joynum] = value;
          else
            y_min[joynum] = value;
        }
        if(args_buf[CALIB_SYNTAX_CTR] != 0) {
          int value = eval_expr(args_buf[CALIB_SYNTAX_CTR]);
          if(change_x)
            x_ctr[joynum] = value;
          else
            y_ctr[joynum] = value;
        }
        if(args_buf[CALIB_SYNTAX_MAX] != 0) {
          int value = eval_expr(args_buf[CALIB_SYNTAX_MAX]);
          if(change_x)
            x_max[joynum] = value;
          else
            y_max[joynum] = value;
        }
        if(args_buf[CALIB_SYNTAX_CTRZONE] != 0) {
          int value = eval_expr(args_buf[CALIB_SYNTAX_CTRZONE]);
          if(change_x)
            x_ctr_deadz[joynum] = value;
          else
            y_ctr_deadz[joynum] = value;
        }
        if(args_buf[CALIB_SYNTAX_ENDZONE] != 0) {
          int value = eval_expr(args_buf[CALIB_SYNTAX_ENDZONE]);
          if(change_x)
            x_end_deadz[joynum] = value;
          else
            y_end_deadz[joynum] = value;
        }
        if(args_buf[CALIB_SYNTAX_SMOOTH] != 0) {
          int value = eval_expr(args_buf[CALIB_SYNTAX_SMOOTH]);
           if(change_x)
             x_smooth[joynum] = value;
           else
             y_smooth[joynum] = value;
        }

        /*
           Calculate correction coefficients from new calibration data
           (x_ctr_low, x_ctr_high, x_low_scaler, x_high_scaler etc)
        */
        recalc_coefficients(1u << joynum);
      }
      break;

    case CMD_JoystickReInit:
      /* Syntax: *JoystickReInit */
      {
        /*
           Can have no more than 1 arg, being 1 evaluated element without identifier. Allow one memory word for this element, plus sufficient buffer space for the evaluated element block.
        */
        char *args_buf[(1*4) + 8];
        {
          _kernel_oserror *e = _swix(OS_ReadArgs, _INR(0,3), reinit_syntax, arg_string, args_buf, sizeof(args_buf));
          if(e != NULL)
            return e;
        }
        if(args_buf[REINIT_SYNTAX_JOYNUM] != 0) {
          int joynum = eval_expr(args_buf[REINIT_SYNTAX_JOYNUM]);
          if(joynum > (NUM_STICKS-1) || joynum < 0)
            return &bad_joy_num;

          reinit_joysticks(1u << joynum);
        } else
          reinit_joysticks(STICK_0|STICK_1);
      }
  } /* endswitch */
  return NULL;
}

/* ----------------------------------------------------------------------- */

_kernel_oserror *pollstick_handler(_kernel_swi_regs *r, void *pw)
{
  /* Called every 10 cs (100,000탎) */
  UNUSED(r);
  
#ifdef DEBUG
  xsyslog_irqmode(1);
#endif
  if(callback_free) {
    /* Add a transient callback (reading the joystick here would take too long with interrupts disabled */
    _kernel_oserror *e;
#ifdef DEBUG
    xsyslog_logmessage(log_name, "Adding transient CallBack to doread_veneer", 1);
#endif
    e = _swix(OS_AddCallBack, _INR(0,1), doread_veneer, pw);
    if(e == NULL) {
      callback_pending = true;
      callback_free = false;
    }
#ifdef DEBUG
    else {
      xsyslog_logmessage(log_name, e->errmess, 0);
    }
#endif
  }
#ifdef DEBUG
  else {
    xsyslog_logmessage(log_name, "Last CallBack to doread_veneer still pending/in progress", 1);
  }
  xsyslog_irqmode(0);
#endif
  return NULL; /* success */
}

/* ----------------------------------------------------------------------- */

_kernel_oserror *stoppoll_handler(_kernel_swi_regs *r, void *pw)
{
  /* Called every 10 seconds, to disable polling if no calls to Joystick SWIs */
  UNUSED(r);

#ifdef DEBUG
  xsyslog_irqmode(1);
#endif

  if(swi_in_last_min) {
    /* Joystick_Read called recently - continue polling (or not) */
#ifdef DEBUG
    xsyslog_logmessage(log_name, "Logged Joystick_Read in last 10 seconds", 1);
#endif
    swi_in_last_min = false;
  } else {
#ifdef DEBUG
    xsyslog_logmessage(log_name, "No calls to Joystick_Read in last 10 seconds", 1);
#endif
    if(polling_stick) {
      /* Joystick_Read not called recently - cease polling */
     _kernel_oserror *e;
#ifdef DEBUG
      xsyslog_logmessage(log_name, "Removing CallEvery to pollstick_veneer", 1);
#endif
      e = _swix(OS_RemoveTickerEvent, _INR(0,1), pollstick_veneer, pw);
      /* (note this SWI *is* re-entrant!) */
      if(e == NULL)
        polling_stick = false;
#ifdef DEBUG
      else
        xsyslog_logmessage(log_name, e->errmess, 0);
#endif
    }
  }
#ifdef DEBUG
  xsyslog_irqmode(0);
#endif
  return NULL; /* success */
}

/* ----------------------------------------------------------------------- */

_kernel_oserror *doread_handler(_kernel_swi_regs *r, void *pw)
{
  /* Reading the joystick would take too long in an interrupt - this way we can take as long as we want, and call non-re-entrant SWIs too */
  UNUSED(pw);
  UNUSED(r);
  
  callback_pending = false; /* nothing to remove */
  
  if(polling_stick) {
#ifdef DEBUG
    xsyslog_logmessage(log_name, "Reached doread_handler on transient CallBack", 1);
#endif
    read_joystick(axes_mask, NULL);
  }
  callback_free = true; /* allow another one to be added */
  return NULL; /* success */
}


/* ----------------------------------------------------------------------- */

_kernel_oserror *MicoJoy_finalise(int fatal, int podule, void *pw)
{
  UNUSED(fatal);
  UNUSED(podule);
  
#ifdef DEBUG
  xsyslog_logmessage(log_name, "Finalising Joystick module", 1);
#endif

  if(polling_stick) {
    /* Remove joystick polling routine */
    _kernel_oserror *e;
#ifdef DEBUG
    xsyslog_logmessage(log_name, "Removing CallEvery to pollstick_veneer before exit", 1);
#endif
    e = _swix(OS_RemoveTickerEvent, _INR(0,1), pollstick_veneer, pw);
    if(e != NULL) {
#ifdef DEBUG
      xsyslog_logmessage(log_name, e->errmess, 0);
#endif
      return e; /* fail */
    }
    polling_stick = false;
  }
  if(callback_pending) {
    _kernel_oserror *e;
#ifdef DEBUG
    xsyslog_logmessage(log_name, "Removing outstanding CallBack to doread_veneer before exit", 1);
#endif
    e = _swix(OS_RemoveCallBack, _INR(0,1), doread_veneer, pw);
    if(e != NULL) {
#ifdef DEBUG
      xsyslog_logmessage(log_name, e->errmess, 0);
#endif
      return e; /* fail */
    }
    callback_pending = false;
  }

  /* Remove routine to monitor whether Joystick SWIs are being called */
  return _swix(OS_RemoveTickerEvent, _INR(0,1), stoppoll_veneer, pw);
}

/* ----------------------------------------------------------------------- */
/*                         Private functions                               */

static void recalc_coefficients(int sticks)
{
  /*
     Calculate correction coefficients from our calibration data
     (x_ctr_low, x_ctr_high, x_low_scaler, x_high_scaler etc)
  */
  int stick_num;
  
#ifdef DEBUG
  xsyslog_logmessage(log_name, "Recalculating correction coefficients", 50);
#endif

  for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
    if(sticks & (1u << stick_num)) {
      {
        unsigned int x_ctr_dz, y_ctr_dz;
        if(ctr_zones) {
          x_ctr_dz = x_ctr_deadz[stick_num];
          y_ctr_dz = y_ctr_deadz[stick_num];
        } else {
          x_ctr_dz = 0;
          y_ctr_dz = 0;
        }
        x_ctr_low[stick_num] = x_ctr[stick_num] - x_ctr_dz;
        x_ctr_high[stick_num] = x_ctr[stick_num] + x_ctr_dz;
#ifdef DEBUG
        xsyslogf(log_name, 50, "centre x limits for stick %d : %u,%u", stick_num, x_ctr_low[stick_num], x_ctr_high[stick_num]);
#endif
        y_ctr_low[stick_num] = y_ctr[stick_num] - y_ctr_dz;
        y_ctr_high[stick_num] = y_ctr[stick_num] + y_ctr_dz;
#ifdef DEBUG
       xsyslogf(log_name, 50, "centre y limits for stick %d : %u,%u", stick_num, y_ctr_low[stick_num], y_ctr_high[stick_num]);
#endif
      }
      {
        unsigned int x_end_dz, y_end_dz;
        if(end_zones) {
          x_end_dz = x_end_deadz[stick_num];
          y_end_dz = y_end_deadz[stick_num];
        } else {
          x_end_dz = 0;
          y_end_dz = 0;
        }
        safedivide(x_low_scaler[stick_num], (32768<<SCALER_FRAC_SHIFT), x_ctr_low[stick_num] - (x_min[stick_num] + x_end_dz));
#ifdef DEBUG
        xsyslogf(log_name, 50, "x_low_scaler for stick %d : %u/16384 (approx. %u)", stick_num, x_low_scaler[stick_num], x_low_scaler[stick_num] >> 14);
#endif

        safedivide(x_high_scaler[stick_num], (32768<<SCALER_FRAC_SHIFT), (x_max[stick_num] - x_end_dz) - x_ctr_high[stick_num]);
#ifdef DEBUG
        xsyslogf(log_name, 50, "x_high_scaler for stick %d : %u/16384 (approx. %u)", stick_num, x_high_scaler[stick_num], x_high_scaler[stick_num] >> 14);
#endif

        safedivide(y_low_scaler[stick_num], (32768<<SCALER_FRAC_SHIFT), y_ctr_low[stick_num] - (y_min[stick_num] + y_end_dz));
#ifdef DEBUG
        xsyslogf(log_name, 50, "y_low_scaler for stick %d : %u/16384 (approx. %u)", stick_num, y_low_scaler[stick_num], y_low_scaler[stick_num] >> 14);
#endif

        safedivide(y_high_scaler[stick_num], (32768<<SCALER_FRAC_SHIFT), (y_max[stick_num] - y_end_dz) - y_ctr_high[stick_num]);
#ifdef DEBUG
        xsyslogf(log_name, 50, "y_high_scaler for stick %d : %u/16384 (approx. %u)", stick_num, y_high_scaler[stick_num], y_high_scaler[stick_num] >> 14);
#endif
      }
    } /* endif sticks & (1u << stick_num) */
  } /* next stick */
}

/* ----------------------------------------------------------------------- */

static unsigned int read_joystick(unsigned int mask, unsigned int *lost)
{
  /*
     Read current position of joysticks

     Input: Bits set in mask indicate axes to read
     Returns: updated mask (bits set indicate axes that timed out)
  */
  unsigned int start_time;
  unsigned int new_x[2], new_y[2];

#ifdef DEBUG
  xsyslogf(log_name, 50, "read_joystick mask (axes to read): &%x", mask);
#endif

  _kernel_irqs_off();

  /* Write dummy byte to the gameport (set axis bits) */
  *game_port_address = 0;

  /*
    IOC Timer 0 is used for timing - ticks at 2MHz, 0.5탎 per tick
    Counts down from 19999 to 0
  */
  {
    IOC *ioc =  (IOC *)IOC_ADDRESS;
    ioc->timer_0.latch[0] = 0; /* make value appear on latch */
    start_time = (unsigned int)(ioc->timer_0.low[0] + (ioc->timer_0.high[0] << 8));
  }

  _kernel_irqs_on();
  /* (We ASSUME that by doing this we are restoring the entry state) */

  new_x[0] = UINT_MAX; new_y[0] = UINT_MAX; new_x[1] = UINT_MAX; new_y[1] = UINT_MAX;

  /*
     Time how long the axis bits take to drop back to 0
     if they take 1000탎 or longer then we give up (not connected?)   */
  {
    int prev_time = start_time, wait = 0;
    unsigned int sticks_lost = 0;

    while (mask != 0 && wait < max_wait) {
      unsigned int interval, new_time, joy;

      _kernel_irqs_off();

      /* Read gameport status byte */
      joy = ~(*game_port_address); /* now bits set indicate axes finished */

      /* Read IOC Timer 0 */
      {
        IOC *ioc =  (IOC *)IOC_ADDRESS;
        ioc->timer_0.latch[0] = 0; /* make value appear on latch */
        new_time = (unsigned int)(ioc->timer_0.low[0] + (ioc->timer_0.high[0] << 8));
      }
      _kernel_irqs_on();
      /* (We ASSUME that by doing this we are restoring the entry state) */

      if(new_time > start_time) { /* timer has wrapped */
        start_time += 20000;
        prev_time += 20000; /* the new time will be < 19999 */
      }
      wait = start_time - new_time;


      /* Check for interrupt or something disrupting loop */
      interval = prev_time - new_time;
      prev_time = new_time;

      joy &= mask; /* mask out those bits we aren't interested in */
      if (joy & PC_JOY_A_X) {
        /* axis bit is set */
        if(interval <= tolerance) {
          new_x[0] = wait;
//#ifdef DEBUG
//          xsyslogf(log_name, 50, "Got Ax - interval was %d", interval);
//#endif /* DEBUG */
        } else {
#ifdef DEBUG
          xsyslogf(log_name, 50, "Lost Ax - interval was %d", interval);
#endif /* DEBUG */
          sticks_lost |= STICK_0;
        }
        mask &= ~PC_JOY_A_X; /* mask out that bit */
      }
      if (joy & PC_JOY_A_Y) {
        /* axis bit is set */
        if(interval <= tolerance) {
          new_y[0] = wait;
//#ifdef DEBUG
//          xsyslogf(log_name, 50, "Got Ay - interval was %d", interval);
//#endif /* DEBUG */
        } else {
#ifdef DEBUG
          xsyslogf(log_name, 50, "Lost Ay - interval was %d", interval);
#endif /* DEBUG */
          sticks_lost |= STICK_0;
        }
        mask &= ~PC_JOY_A_Y; /* mask out that bit */
      }
      if (joy & PC_JOY_B_X) {
        /* axis bit is set */
        if(interval <= tolerance) {
          new_x[1] = wait;
//#ifdef DEBUG
//          xsyslogf(log_name, 50, "Got Bx - interval was %d", interval);
//#endif /* DEBUG */
        } else {
#ifdef DEBUG
          xsyslogf(log_name, 50, "Lost Bx - interval was %d", interval);
#endif /* DEBUG */
          sticks_lost |= STICK_1;
        }
        mask &= ~PC_JOY_B_X; /* mask out that bit */
      }
      if (joy & PC_JOY_B_Y) {
        /* axis bit is set */
        if(interval <= tolerance) {
          new_y[1] = wait;
//#ifdef DEBUG
//          xsyslogf(log_name, 50, "Got By - interval was %d", interval);
//#endif /* DEBUG */
        } else {
#ifdef DEBUG
          xsyslogf(log_name, 50, "Lost By - interval was %d", interval);
#endif /* DEBUG */
          sticks_lost |= STICK_1;
        }
        mask &= ~PC_JOY_B_Y; /* mask out that bit */
      }
    } /* endwhile */

    if(lost != NULL)
      *lost = sticks_lost;
  }

#ifdef DEBUG
  xsyslogf(log_name, 50, "Raw axis times Ax:%d Ay:%d Bx:%d By:%d", new_x[0], new_y[0], new_x[1], new_y[1]);
  
  /* Those mask bits still set indicate axes that timed out */
  if(mask & PC_JOY_A_X)
    xsyslog_logmessage(log_name, "(timed out waiting for Ax)", 50);
  if(mask & PC_JOY_A_Y)
    xsyslog_logmessage(log_name, "(timed out waiting for Ay)", 50);
  if(mask & PC_JOY_B_X)
    xsyslog_logmessage(log_name, "(timed out waiting for Bx)", 50);
  if(mask & PC_JOY_B_Y)
    xsyslog_logmessage(log_name, "(timed out waiting for By)", 50);

  if(start_time >= 20000)
    xsyslog_logmessage(log_name, "(timer 0 wrapped)", 50);
#endif /* DEBUG */

  {
    int stick_num;
    for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {

      if(new_x[stick_num] != UINT_MAX) {

        /*
           Smooth output value
        */
        if(smooth && x_smooth[stick_num] > 0) {
#ifdef DEBUG
          xsyslogf(log_name, 50, "Smoothing x axis of stick %d", stick_num);
#endif /* DEBUG */
          x_axis[stick_num] = smooth_value(x_axis[stick_num], new_x[stick_num], x_smooth[stick_num]);
        }
        else
          x_axis[stick_num] = new_x[stick_num];
      } /* endif new_x[stick_num] == 0 */

      if(new_y[stick_num] != UINT_MAX) {

        /*
           Smooth output value
        */
        if(smooth && y_smooth[stick_num] > 0) {
#ifdef DEBUG
          xsyslogf(log_name, 50, "Smoothing y axis of stick %d", stick_num);
#endif /* DEBUG */
          y_axis[stick_num] = smooth_value(y_axis[stick_num], new_y[stick_num], y_smooth[stick_num]);
        }
        else
          y_axis[stick_num] = new_y[stick_num];
      } /* endif new_y[stick_num] == 0 */

    } /* next stick_num */
  }
#ifdef DEBUG
  xsyslogf(log_name, 50, "Output A: x%u y%u, B: x%u y%u (poss smoothed)", x_axis[0], y_axis[0], x_axis[1], y_axis[1]);
#endif /* DEBUG */

  return mask;
}

/* ----------------------------------------------------------------------- */

static unsigned int smooth_value(unsigned int prev_value, unsigned int new_value, unsigned int stddev)
{
  if((new_value >= (prev_value - stddev)) && (new_value <= (prev_value + stddev))) {
  /* very likely to be jitter - smooth it lots */
#ifdef DEBUG
    xsyslogf(log_name, 50, "much smoothing of value %u", new_value);
#endif /* DEBUG */
    return ((prev_value * 3) + new_value) / 4;
  }

  if((new_value >= (prev_value - (stddev*2))) && (new_value <= (prev_value + (stddev*2)))) {
    /* near average jitter - smooth it rather less */
#ifdef DEBUG
    xsyslogf(log_name, 50, "moderate smoothing of value %u", new_value);
#endif /* DEBUG */
    return (prev_value + new_value) / 2;
  }

  if((new_value >= (prev_value - (stddev*4))) && (new_value <= (prev_value + (stddev*4)))) {
    /* even further away from average jitter - smooth it slightly */
#ifdef DEBUG
    xsyslogf(log_name, 50, "slight smoothing of value %u", new_value);
#endif /* DEBUG */
    return ((new_value * 3) + prev_value) / 4;

  } else {
    /* miles from jitter bounds - use new value verbatim */
#ifdef DEBUG
    xsyslogf(log_name, 50, "taking new value %u verbatim", new_value);
#endif /* DEBUG */
    return new_value;
  }
}

/* ----------------------------------------------------------------------- */

static void reinit_joysticks(unsigned int sticks)
{
  /*
    Find standard deviation from average x,y and limits of jitter
  */
  unsigned int last_x[NUM_STICKS], last_y[NUM_STICKS];
  bool old_s;

#ifdef DEBUG
  xsyslogf(log_name, 50, "Initialising joysticks '%u'", sticks);
  _swix(Hourglass_On, 0);
#endif
  {
    int stick_num;
    for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
      if(sticks & (1u << stick_num)) {
        x_axis[stick_num] = 800;
        y_axis[stick_num] = 800; /* in case axes time out */

        x_smooth[stick_num] = 0;
        y_smooth[stick_num] = 0;
      } /* endif sticks & (1u << stick_num) */
    } /* next stick_num */
  }
  
  /* We'll have raw values if you don't mind! */
  old_s = smooth;smooth = false;
  
  {
    unsigned int new_mask = 0;  /* start with presumption that nothing is connected */
    unsigned int stick_mask, lasttime;
    int test;
    
    if(sticks & STICK_0)
      stick_mask = PC_JOY_A_X | PC_JOY_A_Y;
    else
      stick_mask = 0;
    if(sticks & STICK_1)
      stick_mask |= PC_JOY_B_X | PC_JOY_B_Y;

    /* Read number of centi-seconds since last hard reset */
    _swix(OS_ReadMonotonicTime, _OUT(0), &lasttime);
 
    for(test = (NUM_TEST_RUNS-1); test >= 0; test--) {
      int stick_num;
      
      /* Note those axes that didn't time out (bits clear) */
      new_mask |= ~read_joystick(stick_mask, NULL);
 
      for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
        if(sticks & (1u << stick_num)) {
#ifdef DEBUG
          xsyslogf(log_name, 50, "test %d : x_axis[%d] = %u y_axis[%d] = %u\n", test, stick_num, x_axis[stick_num], stick_num, y_axis[stick_num]);
#endif

          if(test < (NUM_TEST_RUNS-1)) {
            unsigned int x_diff, y_diff;
            absdiff(x_diff, last_x[stick_num], x_axis[stick_num]);
            if(x_diff > x_smooth[stick_num])
              x_smooth[stick_num] = x_diff;
            absdiff(y_diff, last_y[stick_num], y_axis[stick_num]);
            if(y_diff > y_smooth[stick_num])
              y_smooth[stick_num] = y_diff;
#ifdef DEBUG
            xsyslogf(log_name, 50, "x diff:%d y_diff:%d\n", x_diff, y_diff);
#endif

          }
          last_x[stick_num] = x_axis[stick_num];
          last_y[stick_num] = y_axis[stick_num];
          
        } /* endif sticks & (1u << stick_num) */
      } /* next stick_num */

      {
        /*
           We enforce a little delay here (1cs), in order to allow the capacitors to 'cool down' (otherwise calibration conditions are not comparable to actual operation)
        */
        unsigned int newtime;
        _kernel_oserror *e;
        do {
          /* Read number of centi-seconds since last hard reset */
          e = _swix(OS_ReadMonotonicTime, _OUT(0), &newtime);
        } while(newtime == lasttime && e == NULL);
        /* (note this also works if the timer should wrap!) */
        lasttime = newtime;
      }
    } /* next test */

    /* In future, mask out those axes that consistently timed out */
    axes_mask = (axes_mask & ~stick_mask) | (new_mask & stick_mask);
#ifdef DEBUG
    xsyslogf(log_name, 50, "Axes to be read in future: &%x", axes_mask);
#endif
  }

  /*
     Re-calibrate stick at centre, having enabled any smoothing
  */
  smooth = old_s;
  get_av_stick_pos(sticks, x_ctr, y_ctr, x_ctr_deadz, y_ctr_deadz, 0);

  /*
    Can't be sure of axis limits prior to calibration, so guess
  */
  {
    int stick;
    for(stick=(NUM_STICKS-1); stick >= 0; stick--) {
      if(sticks & (1u << stick)) {
        x_min[stick] = 0;
        y_min[stick] = 0;
        x_max[stick] = x_ctr[stick]*2;
        y_max[stick] = y_ctr[stick]*2;
#ifdef DEBUG
        xsyslogf(log_name, 50, "Guessing x,y limits for stick %d : %u,%u", stick, x_max[stick], y_max[stick]);
#endif
      } /* endif sticks & (1u << stick_num) */
    }
  }

  /*
     Calculate correction coefficients from our calibration data
     (x_ctr_low, x_ctr_high, x_low_scaler, x_high_scaler etc)
  */
  recalc_coefficients(sticks);

#ifdef DEBUG
  _swix(Hourglass_Off, 0);
#endif
}

/* ----------------------------------------------------------------------- */

static void get_av_stick_pos(unsigned int sticks, unsigned int *x_array, unsigned int *y_array, unsigned int *x_jitdist, unsigned int *y_jitdist, int bias)
{
  /*
    Returns the average stick position and the maximum recorded deviation from this value
  */
  unsigned int x_tot[NUM_STICKS], y_tot[NUM_STICKS];
  unsigned int x_jit_max[NUM_STICKS], x_jit_min[NUM_STICKS], y_jit_max[NUM_STICKS], y_jit_min[NUM_STICKS];
  unsigned int last_x[NUM_STICKS], last_y[NUM_STICKS];

#ifdef DEBUG
  _swix(Hourglass_On, 0);
#endif
  {
    int stick_num;
    for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
      if(sticks & (1u << stick_num)) {
        x_tot[stick_num] = 0;
        y_tot[stick_num] = 0; /* initialise accumulators for average */
        x_jit_min[stick_num] = MAX_AXIS_WAIT_TIME;
        y_jit_min[stick_num] = MAX_AXIS_WAIT_TIME; /* start implausibly high */
        x_jit_max[stick_num] = 0;
        y_jit_max[stick_num] = 0; /* start implausibly low */
        last_x[stick_num] = UINT_MAX;
      }
    }
  }

#ifdef DEBUG
  xsyslog_logmessage(log_name, "Waiting for sticks to settle...", 50);
#endif
  {
    int test;
    unsigned int lasttime, read_axes;
    int go_go_go = 8; /* max loops to wait for all sticks to settle */

    if(sticks & STICK_0)
      read_axes = PC_JOY_A_X | PC_JOY_A_Y;
    else
      read_axes = 0;
    if(sticks & STICK_1)
      read_axes |= PC_JOY_B_X | PC_JOY_B_Y;
    read_axes &= axes_mask; /* only those not pre-marked as consistently timing out */
#ifdef DEBUG
    xsyslogf(log_name, 50, "Axes to be read: &%x", read_axes);
#endif

    /* Read number of centi-seconds since last hard reset */
    _swix(OS_ReadMonotonicTime, _OUT(0), &lasttime);
   
    test = (NUM_TEST_RUNS-1);
    while(test >= 0) {
      int stick_num;
      unsigned int lost, sticks_within_range = 0;
      read_joystick(read_axes, &lost);

#ifdef DEBUG
      if(go_go_go > 0) {
        xsyslogf(log_name, 50, "sticks with lost axis values: %u", lost);
        xsyslogf(log_name, 50, "loops until give up waiting for values to settle: %d",go_go_go);
      }
#endif

      for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
        if(sticks & (1u << stick_num)) {
          if(go_go_go == 0) {
            /* Ongoing calculation of average value */
            x_tot[stick_num] += x_axis[stick_num];
            y_tot[stick_num] += y_axis[stick_num];
#ifdef DEBUG
            xsyslogf(log_name, 50, "test %d : x_axis[%d] = %u y_axis[%d] = %u\n", test, stick_num, x_axis[stick_num], stick_num, y_axis[stick_num]);
#endif
            /* update maxima and minima */
            update_min_max(x_axis, x_jit_min, x_jit_max, stick_num);
            update_min_max(y_axis, y_jit_min, y_jit_max, stick_num);

          } else {
            /* We wait for stick to settle in new position (polling may have been disabled, so early values may be invalid) */
            if(last_x[stick_num] != UINT_MAX && !(lost & (1u << stick_num))) {
              unsigned int diff;
              absdiff(diff, last_x[stick_num], x_axis[stick_num]);
              if(diff <= x_smooth[stick_num]*2) {
                unsigned int diff;
                absdiff(diff, last_y[stick_num], y_axis[stick_num]);
                if(diff <= y_smooth[stick_num]*2) {
                  /* within range of previous position */
                  sticks_within_range |= (1u << stick_num); /* this stick has settled */
                }
              }
            }
#ifdef DEBUG
            else {
              xsyslogf(log_name, 50, "Skipping settle checks - first run or else readings lost for stick %d", stick_num);
            }
#endif
            last_x[stick_num] = x_axis[stick_num];
            last_y[stick_num] = y_axis[stick_num];
          } /* endif go_go_go == 0 */
        } /* endif sticks & (1u << stick_num) */
      } /* next stick_num */
      if(go_go_go > 0) {
        if(sticks_within_range == sticks) {
          go_go_go = 0; /* now we can start the real calculations */
#ifdef DEBUG
          xsyslog_logmessage(log_name, "Values have settled satisfactorily", 50);
#endif
        } else {
          go_go_go -= 1; /* can't wait forever! */
#ifdef DEBUG
          if(go_go_go == 0)
            xsyslog_logmessage(log_name, "Giving up on waiting for values to settle!", 50);
#endif
        }
      } else {
        if(go_go_go == 0)
          test--; /* only start counting when values have settled */
      }

      {
        /*
           We enforce a little delay here (1cs), in order to allow the capacitors to 'cool down' (otherwise calibration conditions are not comparible to actual operation)
        */
        unsigned int newtime;
        _kernel_oserror *e;
        do {
          /* Read number of centi-seconds since last hard reset */
          e = _swix(OS_ReadMonotonicTime, _OUT(0), &newtime);
        } while(newtime == lasttime && e == NULL);
        /* (note this also works if the timer should wrap!) */
        lasttime = newtime;
      }
    } /* endwhile test >= 0 */
  }
  {
    int stick_num;
    for(stick_num = (NUM_STICKS-1); stick_num >= 0; stick_num--) {
      if(sticks & (1u << stick_num)) {
        /* Finish off average calculations */
        x_array[stick_num] = x_tot[stick_num] / NUM_TEST_RUNS;
        y_array[stick_num] = y_tot[stick_num] / NUM_TEST_RUNS;
#ifdef DEBUG
        xsyslogf(log_name, 50, "average x[%d]:%d average y[%d]:%d\n",stick_num, x_array[stick_num], stick_num, y_array[stick_num]);
#endif
        {
          /*
             Deadzone should cover all recorded values,
             whilst being symmetric around the average centre value
           */
          int min,max;
          min = x_array[stick_num] - x_jit_min[stick_num];
          max = x_jit_max[stick_num] - x_array[stick_num];
          if((min > max || (bias & X_BIAS_MIN)) && !(bias & X_BIAS_MAX)) {
            /* max > min or biased towards min, and not biased towards max */
            x_jitdist[stick_num] = (unsigned int)min;
          } else {
            /* biased towards max, or max >= min and not biased towards min */
            x_jitdist[stick_num] = (unsigned int)max;
          }
#ifdef DEBUG
          xsyslogf(log_name, 50, "x deadzone for stick %d : �%u (min = %u, max = %u)", stick_num, x_jitdist[stick_num], x_jit_min[stick_num], x_jit_max[stick_num]);
#endif
  
          min = y_array[stick_num] - y_jit_min[stick_num];
          max = y_jit_max[stick_num] - y_array[stick_num];
          if((min > max || (bias & Y_BIAS_MIN)) && !(bias & Y_BIAS_MAX)) {
            /* max > min or biased towards min, and not biased towards max */
            y_jitdist[stick_num] = (unsigned int)min;
          } else {
            /* biased towards max, or max >= min and not biased towards min */
            y_jitdist[stick_num] = (unsigned int)max;
          }
#ifdef DEBUG
          xsyslogf(log_name, 50, "y deadzone for stick %d : �%u (min = %u, max = %u)", stick_num, y_jitdist[stick_num], y_jit_min[stick_num], y_jit_max[stick_num]);
#endif
        }
      }
    } /* next stick_num */
  }

#ifdef DEBUG
  _swix(Hourglass_Off, 0);
#endif
}

/* ----------------------------------------------------------------------- */

static int eval_expr(char *buffer)
{
  if(buffer[0] == 0)
    return buffer[1] | (buffer[2]<<8) | (buffer[3]<<16) | (buffer[4]<<24);
  else
    return 0;
}

/* ----------------------------------------------------------------------- */

static void update_min_max(unsigned int *axis, unsigned int *jit_min, unsigned int *jit_max, int stick_num)
{
  if(axis[stick_num] < jit_min[stick_num])
    jit_min[stick_num] = axis[stick_num];
  
  if(axis[stick_num] > jit_max[stick_num])
    jit_max[stick_num] = axis[stick_num];
}
