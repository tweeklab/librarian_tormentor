#include <xc.h>         /* XC8 General Include File */

// PIC10F200 Configuration Bit Settings
#pragma config WDTE = ON        // Watchdog Timer (WDT enabled)
#pragma config CP = OFF         // Code Protect (Code protection off)
#pragma config MCLRE = OFF      // Master Clear Enable (GP3/MCLR pin fuction is digital I/O, MCLR internally tied to VDD)

#include <stdint.h>        /* For uint8_t definition */
#include <string.h>       /* For true/false definition */

// Required by the C library delay routines
// The PIC10F200 only has an internal oscillator
// option running at 4MHz.  That makes for a 1uS
// instruction cycle.
#define _XTAL_FREQ 4000000

// The amount of time we wait while buzzing to check
// if we should stop buzzing.  This is the count on
// TMR0 we check for to see how much time has passed.
// At 1:256 prescaler this is about 50mS
#define BUZZ_DONE_COUNT 195
// Number of times TMR0 has to hit BUZZ_DONE_COUNT
// in order to equal 1 second of beeping.
#define BUZZ_SECOND_COUNT 20
// Amount of time we sleep after detecting a key down
// to make sure we have filtered out bounces.  This is
// specified as the max time by Panasonic for the switch
// we are using.  This value is passed to safe_delay_ms()
// which is defined later.
#define DEBOUNCE_MS 10
// The amount of delay we add to each go around the main loop.
// This is used as a base for timing the blinking of the LED
// and sensing when the switch is held down.
#define MAIN_LOOP_DELAY_MS 100

// OPTION bits while we are in sleep mode, regardless of
// whether we are sleeping between buzzing or we are sleeping
// because someone turned us "off"
// This gives us a 1:128 prescaler assigned to the WDT
#define OPTION_SLEEP (PSA|PS0|PS1|PS2)
// OPTION bits configured upon wake up.
// This gives a 1:256 prescaler assigned to TMR0
#define OPTION_WAKE (nGPWU|PS0|PS1|PS2)

// Friendlier names for the outputs
#define LED GP0
#define BUZZ GP1
#define ENABLE GP2
// Friendlier names for the switch input.  Note we
// take advantage of the internal pullup on this pin so
// the switch is actually wired to pull down the pin when
// pressed, hence the negation, so we don't have to think
// about it in the code everywhere.
#define SW (!GP3)

// Beep frequence in Hz.  Computed down to the actual delay
// value by the preprocessor in the main loop
#define BEEP_FREQ 4000

// Tracks the number of sleep cycles we wait until
// buzzing again.  This is in units of the WDT timeout
// interval.
persistent uint8_t beep_off_delay;
// Sets up the blink pattern of the LED, generally
// for config mode.  For each bit that is a 1, starting
// with MSB, the LED will be on for MAIN_LOOP_DELAY_MS, off for
// MAIN_LOOP_DELAY_MS.  When the bit is 0, only MAIN_LOOP_DELAY_MS is consumed.
persistent uint8_t led_pattern;
// Iterator for safe_delay_ms
persistent uint8_t tmp;
// Generic counter for things we need to time.  We need to
// time the length of a beep, and we need to time the amount
// of time a key is held down.  We don't do those things at once
// so just use one variable to save space.
persistent uint8_t timer;

#define MODE_OFF 0x0
#define MODE_CONFIG 0x1
#define MODE_TORMENT 0x2
persistent uint8_t     mode;

persistent struct status_t {
    // 1 when we should be making noise
    uint8_t     noise:1;
    // 1 when we have confirmed the key is down (after debounce).
    // This is used to detect edges (transitions from up<->down
    uint8_t     keydown:1;
    // The operational mode of the device.  Values are defined here:

    // 0 - configure on time, 1 - configure off time
    uint8_t     config_type:1;
    uint8_t     off_pending:1;
    uint8_t     init_beep_off_delay:1;
} status;

persistent uint8_t on_seconds;
persistent uint8_t off_minutes;

// Standard bits we need to do in order to put the chip
// to sleep.
#define GOTO_SLEEP \
    GPIO = 0;\
    CLRWDT(); \
    TMR0 = 0; \
    OPTION = OPTION_SLEEP; \
    SLEEP();

void __safe_delay_ms(void)
{
    for (;tmp;tmp--) {
        CLRWDT();
        __delay_ms(1);
    }
}

void main(void)
{
    // Define this in main because it relies on a variable local to main.
#define safe_delay_ms(x) \
    tmp=x; \
    __safe_delay_ms();


    // We need the weak pullups for the programmer checks
    // so set up the options now.
    // We will need the pin wake-on-change later, but keep
    // it off for now.
    CLRWDT();
    TRIS = 0x0f;
    OPTION = OPTION_WAKE;

    // Programmer protection.  We need this because
    // we are using the ICSP clock and data lines
    // for other things.  Normally I'd avoid this but on a chip
    // with only 3 GPIO lines (GP3 is input only), this has to
    // be done...
    // The programmer will hold the data and clock
    // lines low when it's connected.  Since we have weak
    // pull-ups turned on and everything is in input mode now,
    // Check to see if either line is pulled low.  If it is,
    // Stop.
    // This can be used by turning OFF the target power, connecting
    // the programmer, and then turning ON the target power.  This
    // will also save us in the reset after programming by making sure
    // the program doesn't run and drive outputs with the programmer
    // still connected.  Once you disconnect the programmer, you will
    // need to reset the device.
    if (!LED || !BUZZ) {
        while(1)
            CLRWDT();
    }

    // Main program starting here.

    // IO setup
    GPIO = 0x00;
    TRIS = 0x08;

    if (__resetbits & _STATUS_GPWUF_MASK) {
        // Wake up from pin change
    } else if (!(__resetbits & (_STATUS_nTO_MASK|_STATUS_nPD_MASK))) {
        // Wake up from sleep
    } else {
        // Full power-up
        mode = MODE_OFF;
        led_pattern = 0;
        on_seconds = 1;
        timer = 0;
        off_minutes = 1;
        asm("clrf _status");
    }

    while(1) {
        GP1 = 0;
        safe_delay_ms(MAIN_LOOP_DELAY_MS);
        if (SW) {
            ENABLE = 1;
            LED = 1;
            if (!status.keydown) {
                status.keydown = 1;
                timer = 0;
            }
            if (timer++ != 10)
                goto ignore_button;
            timer = 0;
            LED = 0;
            safe_delay_ms(100);
            LED = 1;
            if (status.off_pending) {
                LED = 0;
                mode = MODE_OFF;
                goto ignore_button;
            }
            if (mode == MODE_OFF) {
                mode = MODE_TORMENT;
                status.init_beep_off_delay = 1;
            } else if (mode != MODE_CONFIG) {
                // Entering config mode for the first time,
                // we are goign to set the on time.
                mode = MODE_CONFIG;
                led_pattern = 0b10000000;
                status.config_type = 0;
                on_seconds = 0;
            } else {
                if (!status.config_type) {
                    // We were setting on time, we now change
                    // to setting off time.
                    led_pattern = 0b11000000;
                    status.config_type = 1;
                    off_minutes = 0;
                } else if (status.config_type) {
                    // We were setting off time, now go back to
                    // tormenting people
                    led_pattern = 0;
                    mode = MODE_TORMENT;
                    status.init_beep_off_delay = 1;
                }
            }
            status.off_pending = 1;
        } else {
            if (status.keydown) {
                timer = 0;
                status.keydown = 0;
                status.off_pending = 0;
                if (mode == MODE_TORMENT) {
                    timer = 2;
                    status.noise = 1;
                } else if (mode == MODE_CONFIG) {
                    if (!status.config_type) {
                        ++on_seconds;
#asm
                        MOVLW 0x07
                        ANDWF _on_seconds,F
                        BTFSC 3,2
                        INCF _on_seconds
#endasm
                        status.noise = 1;
                    } else {
                        ++off_minutes;
#asm
                        MOVLW 0x07
                        ANDWF _off_minutes,F
                        BTFSC 3,2
                        INCF _off_minutes
#endasm
                        timer = off_minutes;
                        status.noise = 1;
                    }
                }
            } else {
                if (mode == MODE_TORMENT) {
                    if (status.init_beep_off_delay) {
 #asm
                        CLRF _beep_off_delay
                        MOVF _off_minutes,W
                        MOVWF _tmp
                        // This should be 0x1a (26 loops per minute)
                        // but because the WDT oscillator is spec'd at
                        // a 5V Vcc and we are running at 3v, adjust this
                        // down a bit based on measurements at both voltages
                        // to get closer to the actual number of minutes.
                        // Because the WDT isn't really designed for tight
                        // timing, this will never be terribly accurate,
                        // but it should not really matter here.
                        MOVLW 0x15
                    add_more_beep_off_delay:
                        ADDWF _beep_off_delay,F
                        DECFSZ _tmp
                        GOTO add_more_beep_off_delay
#endasm
                        status.init_beep_off_delay = 0;
                    }
                    if (--beep_off_delay) {
                        GOTO_SLEEP;
                    }
                    status.init_beep_off_delay = 1;
                    timer = 0;
                    status.noise = 1;
                }
            }
            if (mode == MODE_OFF) {
                GOTO_SLEEP;
            }
        }

ignore_button:
        if (mode != MODE_OFF) {
            // Activate the low-side switch which allows the
            // LED and piezo element to work.
            ENABLE = 1;
        }

        // LED pattern processing logic.  If the user is holding down
        // the switch, don't touch the LED.
        if (!status.keydown) {
            if (!LED && (led_pattern & 0x80)) {
                LED = 1;
            } else {
                LED = 0;
                led_pattern = (led_pattern << 1) | (led_pattern >> 7);
            }
        }

        // Noise loop.  Put nothing else past this point.  The noise
        // loop uses TMR0 so clear it first.
        if (status.noise) {
            if (timer==0) {
#asm
                MOVF _on_seconds,W
                MOVWF _tmp
                MOVLW 0x14
            add_more_noise:
                ADDWF _timer,F
                DECFSZ _tmp
                GOTO add_more_noise
#endasm
            }
            TMR0 = 0;
        }
        while (status.noise) {
            CLRWDT();
            if (TMR0 > BUZZ_DONE_COUNT) {
                TMR0 = 0;
#asm
                // This just checks if timer has gone to 0.  The
                // Free compiler generated some seriously brain-dead assembly
                // for this check so I wrote my own.
                DECF _timer
                BTFSC STATUS,2
                BCF _status,0
#endasm
            } else {
                _delay(4);
            }
#asm
            // We fudge the parameter delay parameter to account
            // for initialization and intra-loop checks.  This works
            // out to be about 28us of overhead we need to factor
            // out of the delay loop, or 7 iterations of the loop.
            MOVLW ((1000000/BEEP_FREQ/8)-7);
            MOVWF _tmp
        timeloop:
            NOP
            DECFSZ _tmp, F
            GOTO timeloop
            NOP
#endasm
            // Toggle the piezo driver line
            GP1 = ~GP1;
            // Shut up if someone hits the button
            if (SW) {
                status.noise = 0;
            }
        }
        GP1 = 0;
    }
    // NOTREACHED
}

