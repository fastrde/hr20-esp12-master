/**
 * The MIT License (MIT)
 * Copyright (c) 2015 by Fabrice Weinberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "NTPClient.h"

namespace {

// 1 for positive, -1 for negative, 0 for zero
static int sign(long num) {
    return (num > 0) - (num < 0);
}

} // namespace

NTPClient::NTPClient(UDP& udp) {
  this->_udp            = &udp;
}

NTPClient::NTPClient(UDP& udp, int timeOffset) {
  this->_udp            = &udp;
  this->_timeOffset     = timeOffset;
}

NTPClient::NTPClient(UDP& udp, const char* poolServerName) {
  this->_udp            = &udp;
  this->_poolServerName = poolServerName;
}

NTPClient::NTPClient(UDP& udp, const char* poolServerName, int timeOffset) {
  this->_udp            = &udp;
  this->_timeOffset     = timeOffset;
  this->_poolServerName = poolServerName;
}

NTPClient::NTPClient(UDP& udp, const char* poolServerName, int timeOffset, int updateInterval) {
  this->_udp            = &udp;
  this->_timeOffset     = timeOffset;
  this->_poolServerName = poolServerName;
  this->_updateInterval = updateInterval;
}

void NTPClient::begin() {
  this->begin(NTP_DEFAULT_LOCAL_PORT);
}

void NTPClient::begin(int port) {
  this->_port = port;

  this->_udp->begin(this->_port);

  this->_udpSetup = true;
}

void NTPClient::forceUpdate(NTPClient::UpdateState &state) {
  #ifdef DEBUG_NTPClient
    Serial.println("Update from NTP Server");
  #endif

  this->sendNTPPacket();

  // Wait till data is there or timeout...
  byte timeout = 0;
  int cb = 0;
  do {
    delay ( 10 );
    cb = this->_udp->parsePacket();
    if (timeout > 100) {
        state.updated = false;
        state.error = true;
        return;  // timeout after 1000 ms
    }
    timeout++;
  } while (cb == 0);

  unsigned long prev_millis = getMillis();
  unsigned long prev_epoch = getEpochTime();

  // no prev update means we will do a full one
  bool fullUpdate = (_lastUpdate == 0);

  // Account for delay in reading the time
  unsigned long ms = millis();
  unsigned long updateMillis = ms - (10 * (timeout + 1));

  this->_udp->read(this->_packetBuffer, NTP_PACKET_SIZE);

  unsigned long highWord = word(this->_packetBuffer[40], this->_packetBuffer[41]);
  unsigned long lowWord = word(this->_packetBuffer[42], this->_packetBuffer[43]);

  unsigned long fracHighWord = word(this->_packetBuffer[44], this->_packetBuffer[45]);
  unsigned long fracLowWord = word(this->_packetBuffer[46], this->_packetBuffer[47]);

  // combine the four bytes (two words) into a long integer
  // this is NTP time (seconds since Jan 1 1900):
  unsigned long secsSince1900 = highWord << 16 | lowWord;
  unsigned long frac = fracHighWord << 16 | fracLowWord;

  // fractional part to milisecs
  uint16_t mssec = ((frac >> 7) * 125 + (1UL << 24)) >> 25;

  // so... the NTP server informs us that _lastUpdate in fact means secsSince1900
  // but it also tells us it's already mssec past that time in seconds
  // so we have to subtract that mssec time from the _lastUpdate var
  // so we get to next second sooner (millis() - _lastUpdate gets to next second sooner)
  unsigned long cur_millis = getMillis();
  unsigned long cur_epoch = getEpochTime();

  // negative values mean we're behind schedule
  long drift_ms = ((cur_epoch - prev_epoch) * 1000 + cur_millis - prev_millis) - mssec;

  this->_lastUpdate = ms;

  if (fullUpdate) {
      this->_epocMS = updateMillis;
      this->_epocMS -= mssec;
      this->_currentEpoc = secsSince1900 - SEVENZYYEARS;
      this->_driftMS = 0;
      state.drift    = 0;
  } else {
      // HACK: we can pre-correct anything rounded to 1 minute intevals, as it does not break our code
      // this will help us if we get totally lost in time
      long min_drift  = drift_ms % 60000;
      this->_epocMS  += drift_ms - min_drift;
      this->_driftMS  = min_drift;
      state.drift     = drift_ms;
  }

  // TODO: Large drift values should maybe cause time skips.
  state.updated = true;
  state.error   = false;
}

void NTPClient::update(NTPClient::UpdateState &state) {
  state.updated = false; state.error = false; state.drift = 0;

  if ((millis() - this->_lastUpdate >= this->_updateInterval)  // Update after _updateInterval
      || this->_lastUpdate == 0)  // Update if there was no update yet.
  {
      if (!this->_udpSetup) this->begin();  // setup the UDP client if needed
      this->forceUpdate(state);
  }
}

long NTPClient::slew() {
    // no slew when no sync was done....
    if (_lastUpdate == 0) return 0;

    unsigned long ms = millis();
    if (ms - _lastSlew >= 60000) {
        // correct the time resolution by shifting _lastUpdate a bit
        int correction = sign(_driftMS);
        _lastSlew   = ms;
        _epocMS     += correction;
        _driftMS    -= correction;
    }

    return _driftMS;
}

unsigned long NTPClient::getEpochTime() {
  return this->_timeOffset + // User offset
         this->_currentEpoc + // Epoc returned by the NTP server
         ((millis() - this->_epocMS) / 1000); // Time since last update
}

int NTPClient::getDay() {
  return (((this->getEpochTime()  / 86400L) + 4 ) % 7); //0 is Sunday
}

int NTPClient::getHours() {
  return ((this->getEpochTime()  % 86400L) / 3600);
}

int NTPClient::getMinutes() {
  return ((this->getEpochTime() % 3600) / 60);
}

int NTPClient::getSeconds() {
  return (this->getEpochTime() % 60);
}

int NTPClient::getMillis() {
  return ((millis() - this->_epocMS) % 1000);
}

String NTPClient::getFormattedTime() {
  unsigned long rawTime = this->getEpochTime();
  unsigned long hours = (rawTime % 86400L) / 3600;
  String hoursStr = hours < 10 ? "0" + String(hours) : String(hours);

  unsigned long minutes = (rawTime % 3600) / 60;
  String minuteStr = minutes < 10 ? "0" + String(minutes) : String(minutes);

  unsigned long seconds = rawTime % 60;
  String secondStr = seconds < 10 ? "0" + String(seconds) : String(seconds);

  return hoursStr + ":" + minuteStr + ":" + secondStr;
}

void NTPClient::end() {
  this->_udp->stop();

  this->_udpSetup = false;
}

void NTPClient::setTimeOffset(int timeOffset) {
  this->_timeOffset     = timeOffset;
}

void NTPClient::setUpdateInterval(int updateInterval) {
  this->_updateInterval = updateInterval;
}

void NTPClient::sendNTPPacket() {
  // set all bytes in the buffer to 0
  memset(this->_packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  this->_packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  this->_packetBuffer[1] = 0;     // Stratum, or type of clock
  this->_packetBuffer[2] = 6;     // Polling Interval
  this->_packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  this->_packetBuffer[12]  = 49;
  this->_packetBuffer[13]  = 0x4E;
  this->_packetBuffer[14]  = 49;
  this->_packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  this->_udp->beginPacket(this->_poolServerName, 123); //NTP requests are to port 123
  this->_udp->write(this->_packetBuffer, NTP_PACKET_SIZE);
  this->_udp->endPacket();
}
