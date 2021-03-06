/*
    ppm - generating PPM signal
    Copyright (C) 2011 Pavel Semerad

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



/*
    use timer3 output compare and overflow to generate PPM signal
    use PWM mode 2 to be zero till OC2 and then one to overflow
    after overflow, set new OC2 and overflow values to preload registers
    also change prescaler for sync signal to enable longer time
*/


#include <string.h>
#include "ppm.h"
#include "calc.h"


// TIM3 prescalers and multiply (1000x more) values to get raw TIM3 values
//   also SPACE values for 300us space signal
// about 3.5ms max for servo pulse
#define PPM_PSC_SERVO 0x00
#define PPM_MUL_SERVO KHZ
#define PPM_300US_SERVO ((PPM_MUL_SERVO * 3 + 5) / 10)
// about 28.4ms max for sync pulse
#define PPM_PSC_SYNC  0x03
#define PPM_MUL_SYNC  (KHZ >> 3)
#define PPM_300US_SYNC ((PPM_MUL_SYNC * 3 + 5) / 10)

// length of whole frame
#define PPM_FRAME_LENGTH  22500


// channel variables
u8 channels;			// number of channels
static u8 channels2;		// number of channels * 2 (for compare in ppm_interrupt, it is quicker this way)
static u8 ppm_channel2;		// next PPM channel to send (0 is SYNC), step 2
static _Bool ppm_enabled;	// set to 1 when first ppm values was computed
// PPM values computed for direct seting to TIM3 registers
// 0 is SYNC pulse length and then channels 1-...
static u8 ppm_values[2*(MAX_CHANNELS + 1)];  // as bytes for ppm_interrupt


// set number of channels
void ppm_set_channels(u8 n) {
    // disable PPM generation till new values will not be set
    ppm_enabled = 0;
    BRES(TIM3_CR1, 0);	// disable timer
    BSET(PD_ODR, 0);	// set PPM pin to 1

    // start with generating 20ms SYNC signal
    TIM3_CCR2H = hi8(PPM_300US_SYNC);
    TIM3_CCR2L = hi8(PPM_300US_SYNC);
    TIM3_ARRH = hi8(PPM_MUL_SYNC * 20);
    TIM3_ARRL = lo8(PPM_MUL_SYNC * 20);

    channels = n;
    channels2 = (u8)(n << 1);  // also 2* value for compare in ppm_interrupt
}


// initialize PPM pin and timer 3
void ppm_init(void) {
    IO_OP(D, 0);	// PPM output pin, TIM3_CH2

    // initialize timer3 used to generate PPM signal
    BSET(CLK_PCKENR1, 6);     // enable master clock to TIM3
    TIM3_CCMR2 = 0b01111000;  // PWM2, OC2 preload, output mode
    TIM3_CCER1 = 0b00010000;  // polarity active-high, OC2 enable
    TIM3_PSCR = PPM_PSC_SYNC;
    TIM3_IER = 1;             // update interrupt enable
    TIM3_CR1 = 0b10000000;    // auto-reload, URS-all, counter-disable
    ppm_set_channels(3);
}


/*
    TIM3 overflow interrupt
    set next prescaler, output compare and overflow values to
      preload registers
*/
@interrupt void ppm_interrupt(void) {
    BRES(TIM3_SR1, 0);	// erase interrupt flag

    if (ppm_channel2) {
	if (ppm_channel2 == 2) {
	    // will be setting channel1 servo, so we are now generating
	    // SYNC signal in HW
	    // wakeup CALC task to compute new PPM values
	    // values for ARR registers will be updated after calculate done
	    // (and when we are still generating SYNC pulse)
	    awake(CALC);
	}
	// set servo channel
	TIM3_PSCR = PPM_PSC_SERVO;
	TIM3_CCR2H = hi8(PPM_300US_SERVO);
	TIM3_CCR2L = lo8(PPM_300US_SERVO);
	TIM3_ARRH = ppm_values[ppm_channel2];
	ppm_channel2++;
	TIM3_ARRL = ppm_values[ppm_channel2];
	if (++ppm_channel2 > channels2) {
	    // next to SYNC pulse
	    ppm_channel2 = 0;
	}
	return;
    }

    // set SYNC signal
    TIM3_PSCR = PPM_PSC_SYNC;
    TIM3_CCR2H = hi8(PPM_300US_SYNC);
    TIM3_CCR2L = lo8(PPM_300US_SYNC);
    TIM3_ARRH = ppm_values[0];
    TIM3_ARRL = ppm_values[1];
    ppm_channel2 = 2;  // to first channel (step 2 bytes)
}


// set new value for given servo channel (1-...), value in 0.1usec (for
//   eliminating more rounding errors)
static u32 ppm_microsecs01;
void ppm_set_value(u8 channel, u16 microsec01) {
    ppm_microsecs01 += microsec01;
    // ARR must be set to computed value - 1, that is why we are substracting
    //   5000, it is quicker way to "add 5000" and then substract 1 from result
    *(u16 *)(&ppm_values[(u8)(channel << 1)]) =
	(u16)(((u32)microsec01 * PPM_MUL_SERVO - PPM(500)) / PPM(1000));
    // for first channel, when we are still in HW SYNC generate, update
    //   new ARR values to get it applied now
    if (channel == 1) {
	sim();
	if (ppm_channel2 == 4) {
	    // next will be channel2, so we have channel1 in ARR now
	    TIM3_ARRH = ppm_values[2];
	    TIM3_ARRL = ppm_values[3];
	}
	rim();
    }
}


// calculate length of SYNC signal (substract previous channel values)
// also sets flag for ppm_interrupt to use new values
// also starts TIM3 at first call
void ppm_calc_sync(void) {
    // ARR must be set to computed value - 1, that is why we are substracting
    //   5000, it is quicker way to "add 5000" and then substract 1 from result
    *(u16 *)(&ppm_values[0]) =
	(u16)(((10 * (u32)PPM_FRAME_LENGTH - ppm_microsecs01) * PPM_MUL_SYNC
	       - PPM(500)) / PPM(1000));
    ppm_microsecs01 = 0;
    // for first ppm values, enable timer
    if (ppm_enabled)  return;
    ppm_enabled = 1;
    BSET(TIM3_EGR, 0);    // generate update event
    BSET(TIM3_CR1, 0);    // enable timer
}

