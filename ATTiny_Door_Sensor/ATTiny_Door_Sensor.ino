/*
 * ****IMPORTANT************ => COMPILE PARAMETERS : Board: ATTiny13 , Processor: 13a , Processor Speed: 1.2 MHz , leave the rest to defaults as below:
 * Millis Avail, No Tone, LTO Enabled, Serial support: Half Duplex, Read+Write, BOD Level: 2.7v
 * 
 *Version 1.3 - Implements communication with ESP via 2 signal pins
 *This sketch is part of the sensor monitoring system. The ATTiny monitors the state of the sensor and outputs a short pulse on its output which is fed into the CH_PD of a ESP
 *Along with this it also outputs the signal type on 2 pins which are also decoded by the ESP as one of the three states - Wakeup , Sensor open, sensor close
 *Wakeup - The ATTiny wakes up at a certain interval to publish a "I am alive" message"
 *The ESP then processes this pulse and does the notification before shutting itself down
 *Hardware connections for this circuit are:
 *Vcc: 3.3 , GND , sensor magnetic switch between PB3 & PB4 , PB0 - o/p to ESP CH_PD , PB1/PB2 - output to Tx/Rx of ESP 
 *ATTiny13A   ESP8266-01
 *Vcc         Vcc
 *GND         GND
 *PB0         CH_PD
 *PB1         Tx //You can any GPIO pins on ESP for this
 *PB2         Rx //You can any GPIO pins on ESP for this
 *Concept to read state of the sensor: Here I use the concept of using 2 pins instead of 1 to read the value of a switch. By doing so I avoid the current flowing through the input PIN at all times. Here is what has been done:
 *If you can periodically check the state, using WDT rather than interrupts, then you can check with virtually no power.  Tie the reed switch between two GPIOs and then drive one pin to ground while 
 *the other pin is set to INPUT_PULLUP.  If the input pin follows the output then the switch is closed, if not then it's open. When done reading, drive the output high so there is no current through the switch
 *Current consumption analysis : When the sensor is closed, it takes 3 uA while when it is open it takes 700uA which is great!!!

References:
Code credit : http://brownsofa.org/blog/2011/01/10/the-compleat-attiny13-led-flasher-part-3-low-power-mode/
See similar example of sleep using WDT here: https://arduinodiy.wordpress.com/2015/06/22/flashing-an-led-with-attiny13/
 */
 
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include <avr/power.h>
#include "ATTinyUART.h"

//#define F_CPU 1200000 // 1.2 MHz //Complile this with 1.2 MHz freq (will look into this later to fix the Clock freq by code)

// Utility macros
#define adc_disable() (ADCSRA &= ~(1<<ADEN)) // disable ADC (before power-off)
#define adc_enable()  (ADCSRA |=  (1<<ADEN)) // re-enable ADC
#define bit_get(p,m) ((p) & (m))
#define bit_set(p,m) ((p) |= (m))
#define bit_clear(p,m) ((p) &= ~(m))
#define bit_flip(p,m) ((p) ^= (m))
#define bit_write(c,p,m) (c ? bit_set(p,m) : bit_clear(p,m))
#define BIT(x) (0x01 << (x))

//Types of messages sent to the signal pins
#define WAKEUP 1 // I am alive message
#define SENSOR_OPEN 2 //Sensor open message
#define SENSOR_CLOSE 3 //Sensor close message

#define PULSE_LEN_ENABLE 250//500 //pulse length in ms to be sent on sensor open event
#define WAKEUP_COUNT 86400 //wake up interval count, this is a multiple of WDT timer prescaler. Eg. WAKEUP_COUNT* WDT_PRESCALER = Total time in sec, 
                        //Note: WDT precaling is independent of the clock speed. If WDT is set to 0.5 sec & WAKEUP_COUNT = 7200 , then Wake up time = 7200*0.5 = 3600 sec = 1 hr
//Example values for WAKEUP_COUNT with WDT set as 0.5 sec: 172800 = 24 hrs , 86400 = 12 hrs
#define PULSE_LEN_SIGNAL 6000 //pulse length in ms to be sent on sensor open event, //Complile this with 1.2 MHz freq
#define ENABLE_PIN PB0 //This is connected to CH_PD on ESP
#define SIGNAL_PIN0 PB1 //This is connected to GPIO pin on ESP as input. 
#define SIGNAL_PIN1 PB2 //This is connected to GPIO pin on ESP as input. 
//State Mapping of SIGNAL_PIN0 SIGNAL_PIN1:: 11=>IDLE , 00=> WAKEUP , 01=> SENSOR_OPEN , 10=> SENSOR_CLOSED

bool sensor_open = false; //indicates that the sensor is in open state
volatile bool wdt_event = false; // indicates that a WDT event has been fired
volatile long wakeup_counter = 0; //keeps a count of how many WDT events have been fired

//WDT fires at a predefined interval to sense the state of the sensor switch
ISR(WDT_vect) {
  //disable global interrupts
  cli();
  wakeup_counter++;
  wdt_event = true;
}

void setup()
{
  //  if (F_CPU == 1200000) clock_prescale_set(clock_div_2);
  
  //Disable ADC as we dont need it, it saves power
  adc_disable(); //My measurements did not result in any significant drop in power due to this, anyway leave it in

  // Set up Port PB0,PB1,PB2,PB4 to output , PB3 to input
  DDRB = 0b00010111;
  // TO DO : Remove hard coding of Pins in above statement
  
  bit_clear(PORTB, BIT(ENABLE_PIN)); //Write initial values of LOW on PB0
  bit_set(PORTB, BIT(SIGNAL_PIN0)); //Write initial values of HIGH on PB1
  bit_set(PORTB, BIT(SIGNAL_PIN1)); //Write initial values of HIGH on PB2
  bit_set(PORTB, BIT(PB3));//enable pull up on PB3
  //  PORTB |= 0x08;
  
  // prescale WDT timer . See section Section 8.5.2 Table 8-2 in datasheet of t13A
  //WDTCR = (1<<WDP2);// 0.25 sec
  WDTCR = ((1<<WDP2) | (1<<WDP0));// 0.5 sec
  //WDTCR = ((1<<WDP2) | (1<<WDP1));// 1 sec
  //WDTCR = ((1<<WDP2) | (1<<WDP1) | (1<<WDP0));// 2 sec
  //WDTCR = (1<<WDP3) ;// 4 sec
  
  // Enable watchdog timer interrupts
  WDTCR |= (1<<WDTIE);
  sei(); // Enable global interrupts 
  // Use the Power Down sleep mode
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  //uart_puts("GO\n");
}

void sendSignal(byte mode)
{
  //Send a short HIGH to enable the ESP
  bit_set(PORTB, BIT(ENABLE_PIN));//send a high pulse on PB0 to wake up the ESP
  _delay_ms(PULSE_LEN_ENABLE);
  bit_clear(PORTB, BIT(ENABLE_PIN));//finish the pulse
  _delay_ms(1);

  //Send the signal according to the mode
  if(mode == WAKEUP)//send 00
  {
    bit_clear(PORTB, BIT(SIGNAL_PIN0));
    bit_clear(PORTB, BIT(SIGNAL_PIN1));
  }
  else if(mode == SENSOR_OPEN)//send 01
  {
    bit_clear(PORTB, BIT(SIGNAL_PIN0));
    bit_set(PORTB, BIT(SIGNAL_PIN1));
  }
  else if(mode == SENSOR_CLOSE)//send 10
  {
    bit_set(PORTB, BIT(SIGNAL_PIN0));
    bit_clear(PORTB, BIT(SIGNAL_PIN1));
  }
  _delay_ms(PULSE_LEN_SIGNAL);//maintain this signal for a certain time
  //Revert the signal to IDLE state which is 11
  bit_set(PORTB, BIT(SIGNAL_PIN0));
  bit_set(PORTB, BIT(SIGNAL_PIN1));
}

int main(void) {
   setup();
   for (;;) 
   {
      if(wdt_event)
      {
        wdt_event = false;
        //write a LOW on PB4 to read PB3
        PORTB &= 0b11101111;
        // TO DO : Remove hard coding of Pins in above statement
        //For some reason if I dont give the delay here the MCU misses reading the correct state,, I think it needs some time to stablize the voltage
        _delay_us(1);
        if((PINB & 0x08))//If PB3 is 1, sensor switch is open  // TO DO : Remove hard coding of Pins in this statement
        {
          //uart_puts("B3:1\n");
          if(!sensor_open) //send a HIGH pulse on Pb0 only if the sensor was reviously closed
          {
            sensor_open = true; //remember the state of the sensor
            sendSignal(SENSOR_OPEN);
          }
        }
        else //sensor switch is in closed position
        {
          if(sensor_open)//send a pulse only if the sensor was open previously
          {
            sensor_open = false;
            sendSignal(SENSOR_CLOSE);
          }
        }

        //We had set PB4 LOW only to read the input, PB3 , now that we're done set it to HIGH again
        PORTB |= 1<<PB4;//Write a HIGH on PB4

        //check if its time to wake up anyway irrespective of the sensor position
        if(wakeup_counter >= WAKEUP_COUNT)
        {
          wakeup_counter = 0;//reset the counter
          sendSignal(WAKEUP);
        }
      } //end if(wdt_event)
      
      //Sleep again else the loop will keep on reading the inputs and consume current
      sei();//enable interrupts again
      sleep_mode();   // go to sleep and wait for interrupt...
      
   }//end for
}//end main
