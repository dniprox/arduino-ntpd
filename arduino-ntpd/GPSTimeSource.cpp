/*
 * File: GPSTimeSource.cpp
 * Description:
 *   A time source that reads the time from a NMEA-compliant GPS receiver.
 * Author: Mooneer Salem <mooneer@gmail.com>
 * License: New BSD License
 */

#include "config.h"

#ifdef ETH_RX_PIN
#include "utility/w5100.h"
#endif // ETH_RX_PIN

#include "GPSTimeSource.h"
#include "TimeUtilities.h"

GPSTimeSource *GPSTimeSource::Singleton_ = NULL;
volatile uint32_t overflows = 0;
volatile uint32_t overflowsRecv = 0;

void GPSTimeSource::enableInterrupts()
{
#ifdef ETH_RX_PIN
    // Enable Ethernet interrupt first to reduce difference between the two timers.
    // NOTE: NTP server must _always_ be initialized first to ensure that it occupies socket 0.
    W5100.writeIMR(0x01);
#endif

    Singleton_ = this;
    pinMode(PPS_PIN, INPUT);
    
    TCCR4A = 0 ;                    // Normal counting mode
    TCCR4B = B010;                  // set prescale bits
    TCCR4B |= _BV(ICES4);           // enable input capture when pin goes high
    TIMSK4 |= _BV(ICIE4);           // enable input capture interrupt for timer 4
    TIMSK4 |= _BV(TOIE4);           // overflow interrupt

#ifdef ETH_RX_PIN
    pinMode(ETH_RX_PIN, INPUT);
    
    TCCR5A = 0 ;                    // Normal counting mode
    TCCR5B = B010;                  // set prescale bits
                                    // enable input capture when pin goes low (default).
    TIMSK5 |= _BV(ICIE5);           // enable input capture interrupt for timer 5
    TIMSK5 |= _BV(TOIE5);           // overflow interrupt
#endif

    Serial.println("interrupts enabled");
}

void GPSTimeSource::PpsInterrupt()
{
    // Get saved time value.
    uint32_t tmrVal = (overflows << 16) | ICR4;
    
    GPSTimeSource::Singleton_->microsecondsPerSecond_ = 
        (GPSTimeSource::Singleton_->microsecondsPerSecond_ + 
        (tmrVal - Singleton_->millisecondsOfLastUpdate_)) / 2;
    GPSTimeSource::Singleton_->secondsSinceEpoch_++;
    GPSTimeSource::Singleton_->fractionalSecondsSinceEpoch_ = 0;
    GPSTimeSource::Singleton_->millisecondsOfLastUpdate_ = tmrVal;
}

void GPSTimeSource::RecvInterrupt()
{
    // Get saved time value.
    uint32_t tmrVal = (overflowsRecv << 16) | ICR5;
    uint32_t tmrDiff = tmrVal - GPSTimeSource::Singleton_->millisecondsOfLastUpdate_;

    GPSTimeSource::Singleton_->fractionalSecondsOfRecv_ =
        (tmrDiff % GPSTimeSource::Singleton_->microsecondsPerSecond_) * 
        (0xFFFFFFFF / GPSTimeSource::Singleton_->microsecondsPerSecond_);
   
    GPSTimeSource::Singleton_->secondsOfRecv_ = GPSTimeSource::Singleton_->secondsSinceEpoch_;     
    if (tmrDiff > GPSTimeSource::Singleton_->microsecondsPerSecond_)
    {
        ++GPSTimeSource::Singleton_->secondsOfRecv_;
    }
}

ISR(TIMER4_OVF_vect)
{
    ++overflows;
}

ISR(TIMER4_CAPT_vect)
{
    GPSTimeSource::PpsInterrupt();
}

ISR(TIMER5_OVF_vect)
{
    ++overflowsRecv;
}

ISR(TIMER5_CAPT_vect)
{
    GPSTimeSource::RecvInterrupt();
}

void GPSTimeSource::updateFractionalSeconds_(void)
{
    // Calculate new fractional value based on system runtime
    // since the EM-406 does not seem to return anything other than whole seconds.
    uint32_t lastTime = (overflows << 16) | TCNT4;
    uint32_t millisecondDifference = lastTime - millisecondsOfLastUpdate_;
    fractionalSecondsSinceEpoch_ = (millisecondDifference % microsecondsPerSecond_) * (0xFFFFFFFF / microsecondsPerSecond_);
}

void GPSTimeSource::now(uint32_t *secs, uint32_t *fract)
{
    while (dataSource_.available())
    {
        int c = dataSource_.read();
        if (gps_.encode(c))
        {
            // Grab time from now-valid data.
            int year;
            byte month, day, hour, minutes, second, hundredths;
            unsigned long fix_age;

            gps_.crack_datetime(&year, &month, &day,
              &hour, &minutes, &second, &hundredths, &fix_age);
            gps_.f_get_position(&lat_, &long_);
            
            // We don't want to use the time we've received if 
            // the fix is invalid.
            if (fix_age != TinyGPS::GPS_INVALID_AGE && fix_age < 5000 && year >= 2013)
            {
                uint32_t tempSeconds = 
                    TimeUtilities::numberOfSecondsSince1900Epoch(
                        year, month, day, hour, minutes, second);
                
                if (tempSeconds != secondsSinceEpoch_)
                {
                    secondsSinceEpoch_ = tempSeconds;
                    hasLocked_ = true;
                }
            }
            else
            {
                // Set time to 0 if invalid.
                // TODO: does the interface need an accessor for "invalid time"?
                secondsSinceEpoch_ = 0;
                fractionalSecondsSinceEpoch_ = 0;
            }
        }
    }
    
    updateFractionalSeconds_();
    
    if (secs)
    {
        *secs = secondsSinceEpoch_;
    }
    if (fract)
    {
        *fract = fractionalSecondsSinceEpoch_;
    }
}
