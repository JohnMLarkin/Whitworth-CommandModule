#include "mbed.h"
#include "NAL9602.h"
#include "RN41.h"
#include "TMP36.h"
#include "launchControlComm.h"
#include "FlightParameters.h"
#include "commandSequences.h"


/** Command Module Microcontroller
 *
 * @author John M. Larkin (jlarkin@whitworth.edu)
 * @version 0.2
 * @date 2018
 * @copyright MIT License
 *
 * Version History:
 *  0.1 - Terminal emulation mode only for testing of NAL9602 library
 *  0.2 - Interface with Launch Control app and more testing
 */

char versionString[] = "0.2";
char dateString[] = "8/01/2018";

// LPC1768 connections
Serial pc(USBTX,USBRX);       // Serial connection via USB
RN41 bt(p9,p10);            // Bluetooth connection via RN-41
NAL9602 sat(p28,p27);         // NAL 9602 modem interface object
TMP36 intTempSensor(p18);     // Internal temperature sensor
TMP36 extTempSensor(p20);     // External temperature sensor
AnalogIn batterySensor(p19);  // Command module battery monitor
DigitalOut powerStatus(p24);  // red (command module powered)
DigitalOut gpsStatus(p22);    // green (GPS unit powered)
DigitalOut satStatus(p21);    // blue (Iridium radio powered)
DigitalOut podStatus(p23);    // amber, clear (XBee connection to pods)
DigitalOut futureStatus(p25); // amber, opaque (currently used to indicate when parsing command from BT)

// Flight state and settings
struct FlightParameters flight;

// Timing objects
Timer timeSinceTrans;  // time since last SBD transmission
Timer checkTime;       // timer in pending mode to do checks increasing altitude

int main() {
  time_t t;  // Time structure
  bool gps_success;  // Was GPS update a success?
  char sbdFlags; // byte of flags (bit 0 = gps, 1 = lo )
  Ticker statusTicker;  // Ticker controlling update of status LEDs
  Timer pauseTime;  // wait for things to respond but if not, move on

  flight.mode = 0;            // flag for mode (0 = lab)
  flight.transPeriod = 60;    // time between SBD transmissions (in s) during flight
  flight.triggerHeight = 100; // trigger active flight if this many meters above ground

  NetworkRegistration regResponse;
  BufferStatus buffStatus;
  int err;
  bool success;

  sat.verboseLogging = false;

  // Satellite modem startup
  pauseTime.start();
  while (!sat.modem.readable() && pauseTime<5) {
  }
  pauseTime.stop();
  pauseTime.reset();
  sat.saveStartLog(5);

  // Bluetooth start-up sequence
  powerStatus = 0;
  gpsStatus = 0;
  satStatus = 0;
  podStatus = 0;
  pauseTime.start();
  while (!bt.modem.readable() && pauseTime < 60) {
    futureStatus = 0;
    powerStatus = 1;
    wait(0.2);
    powerStatus = 0;
    gpsStatus = 1;
    wait(0.2);
    gpsStatus = 0;
    satStatus = 1;
    wait(0.2);
    satStatus = 0;
    podStatus = 1;
    wait(0.2);
    podStatus = 0;
    futureStatus = 1;
    wait(0.2);
  }
  if (pauseTime < 60) {
    bt.connected = true;
  }
  pauseTime.stop();
  pauseTime.reset();
  futureStatus = 0;
  powerStatus = 1;

  if (bt.connected) {
    bt.modem.printf("\r\n----------------------------------------------------------------------------------------------------\r\n");
    bt.modem.printf("Near Space Command Module, v. %s (%s)\r\n", versionString, dateString);
    bt.modem.printf("John M. Larkin, Department of Engineering and Physics\r\nWhitworth University\r\n\r\n");
    bt.modem.printf("\r\n----------------------------------------------------------------------------------------------------\r\n");
    bt.modem.printf("NAL 9602 power-up log:\r\n");
    sat.echoStartLog(bt.modem);
    bt.modem.printf("\r\n----------------------------------------------------------------------------------------------------\r\n");
    bt.modem.printf("Battery = %0.2f V\r\n", getBatteryVoltage());
    bt.modem.printf(" \r\nSynchronizing clock with satellites...\r\n");
  }

  sat.gpsNoSleep();
  sat.setModeGPS(stationary);
  while (!sat.validTime) {
    sat.syncTime();
    if (!sat.validTime)
      wait(15);
  }
  time(&t);
  if (bt.connected) {
    bt.modem.printf("%s (UTC)\r\n", ctime(&t));
    bt.modem.printf("\r\n----------------------------------------------------------------------------------------------------\r\n");
  }

  if (bt.modem.readable()) {
    parseLaunchControlInput(bt.modem, sat); // really should just be handshake detect but I'm lazy (for now)
  }
  statusTicker.attach(&updateStatusLED, 1.0);
  sat.verboseLogging = false;  // "true" is causing system to hang during gpsUpdate

  while (true) {
    switch (flight.mode) {
      /************************************************************************
       *  Lab mode (no SBD transmission)
       *
       *  Can be promoted to mode 1 by Launch Control
       ***********************************************************************/
      case 0:
        if (bt.modem.readable()) {
          futureStatus = 1;
          parseLaunchControlInput(bt.modem, sat);
          futureStatus = 0;
        }
        break;

      /************************************************************************
       *  Flight mode, pre-liftoff (SBD transmissions, but not too frequent)
       *
       *  Can be demoted to mode 0 by Launch Control
       *  Can be promoted to mode 2 if altitude crosses threshold
       ***********************************************************************/
      case 1: // Flight mode, pre-liftoff
        if (timeSinceTrans > PRE_TRANS_PERIOD/5) {
          timeSinceTrans.reset();
          int bars = sat.signalQuality();
          bt.modem.printf("Satellite bars: %i\r\n", bars);
          if (bars) {
            regResponse = sat.joinNetwork();
            bt.modem.printf("Reg status: %i\r\n", regResponse.status);
            bt.modem.printf("Reg error: %i\r\n", regResponse.err);
            buffStatus = sat.getBufferStatus();
            bt.modem.printf("Message in outgoing buffer?: %i\r\n", buffStatus.outgoingFlag);
            bt.modem.printf("MOMSN: %i\r\n", buffStatus.outgoingMsgNum);
            bt.modem.printf("Message in incoming buffer?: %i\r\n", buffStatus.incomingFlag);
            bt.modem.printf("MTMSN: %i\r\n", buffStatus.incomingMsgNum);
            bt.modem.printf("Ring alert?: %i\r\n", buffStatus.raFlag);
            bt.modem.printf("Number of incoming messages waiting: %i\r\n", buffStatus.numMsgWaiting);
          }
          // bt.modem.printf("Preparing to send SBD...\r\n");
          // sbdFlags = send_SBD_message(bt, sat);
          // bt.modem.printf("SBD transmission flags:\r\n");
          // bt.modem.printf("\t GPS - %i\r\n", sbdFlags & 0x1);
          // bt.modem.printf("\t MSG - %i\r\n", sbdFlags & 0x2);
          // bt.modem.printf("\t TRANS - %i\r\n", sbdFlags & 0x4);
        }
        if (pauseTime > 15) {
          pauseTime.reset();
          if (!gps_success)
            gps_success = sat.gpsUpdate();
          if (gps_success) {
            if (sat.altitude() > (flight.groundAltitude + flight.triggerHeight)) {
              bt.modem.printf("Changing to flight mode 2.  Good bye!\r\n");
              changeModeToFlight(bt, sat);
            }
          }
        }
        if (bt.modem.readable()) {
          futureStatus = 1;
          parseLaunchControlInput(bt.modem, sat);
          futureStatus = 0;
        }
        gps_success = false;
        break;

      case 2: // Flight mode, moving!
        break;

      case 3: // Flight mode, landed
        break;
    }
  }
 }
