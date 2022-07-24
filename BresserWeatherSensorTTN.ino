///////////////////////////////////////////////////////////////////////////////
// BresserWeatherSensorTTN.ino
// 
// Bresser 5-in-1/6-in-1 868 MHz Weather Sensor Radio Receiver 
// based on ESP32 and RFM95W - 
// provides data via LoRaWAN to The Things Network
//
// The RFM95W radio transceiver is used
// in FSK mode to receive weather sensor data
// and
// in LoRaWAN mode to connect to The Things Network
//
// Based on:
// ---------
// Bresser5in1-CC1101 by Sean Siford (https://github.com/seaniefs/Bresser5in1-CC1101)
// https://github.com/merbanan/rtl_433/blob/master/src/devices/bresser_6in1.c
// RadioLib by Jan Gromeš (https://github.com/jgromes/RadioLib)
// MCCI LoRaWAN LMIC library by Thomas Telkamp and Matthijs Kooijman / Terry Moore, MCCI (https://github.com/mcci-catena/arduino-lmic)
// MCCI Arduino LoRaWAN Library by Terry Moore, MCCI (https://github.com/mcci-catena/arduino-lorawan)
// Lora-Serialization by Joscha Feth (with modifications)
// ESP32AnalogRead by Kevin Harrington (madhephaestus) (https://github.com/madhephaestus/ESP32AnalogRead)
// OneWire - for Arduino by Paul Stoffregen (https://github.com/PaulStoffregen/OneWire)
// DallasTemperature / Arduino-Temperature-Control-Library by Miles Burton (https://github.com/milesburton/Arduino-Temperature-Control-Library) 
//
// Library dependencies (tested versions):
// ---------------------------------------
// (install via normal Arduino Library installer:) 
// MCCI Arduino Development Kit ADK     0.2.2
// MCCI LoRaWAN LMIC library            4.1.1
// MCCI Arduino LoRaWAN Library         0.9.2
// RadioLib                             5.2.0
// ESP32AnalogRead                      0.2.0 (optional)
// OneWire                              2.3.7 (optional)
// DallasTemperature                    3.9.0 (optional)
// ATC MiThermometer Library            0.0.1 (optional)
// LoRa_Serialization
//               https://github.com/matthias-bs/lora-serialization,
//   forked from https://github.com/thesolarnomad/lora-serialization LATEST
//   => included with BresserWeatherSensorTTN/src
//
// BresserWeatherSensorReceiver         0.0.3
//   https://github.com/matthias-bs/BresserWeatherSensorReceiver
//   => add directory to BresserWeatherSensorTTN/src
//
//
// created: 06/2022
//
//
// MIT License
//
// Copyright (c) 2022 Matthias Prinke
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//
// History:
//
// 20220620 Created
//
// ToDo:
// - add monthly/daily/hourly precipitation values
// - add Bresser soil moisture sensor integration (option)
//
// Notes:
// - After a successful transmission, the controller can go into deep sleep
// - If joining the network or transmitting uplink data fails,
//   the controller can go into deep sleep
// - Weather sensor data is only received at startup; this avoids
//   reconfiguration of the RFM95W module and its SW drivers - 
//   i.e. to work as a weather data relay to TTN, enabling sleep mode 
//   is basically the only useful option
// - The ESP32's RTC RAM is used to store information about the LoRaWAN 
//   network session; this speeds up the connection after a restart
//   significantly
// - The ESP32's Bluetooth LE interface is used to access sensor data (option)
//
///////////////////////////////////////////////////////////////////////////////

#define ARDUINO_LMIC_CFG_NETWORK_TTN 1

#include <Arduino_LoRaWAN_network.h>
#include <Arduino_LoRaWAN_EventLog.h>
#include <arduino_lmic.h>

//-----------------------------------------------------------------------------
//
// User Configuration
//

// Enable debug mode (debug messages via serial port)
//#define _BWS_DEBUG_MODE_

// Enable TTN debug mode - this generates dummy weather data and skips weather sensor reception 
//#define TTN_DEBUG

// Enable sleep mode - sleep after successful transmission to TTN (recommended!)
#define SLEEP_EN

// If SLEEP_EN is defined, MCU will sleep for SLEEP_INTERVAL seconds after succesful transmission
#define SLEEP_INTERVAL 360

// Force deep sleep after a certain time, even if transmission was not completed
#define FORCE_SLEEP

// During initialization (not joined), force deep sleep after SLEEP_TIMEOUT_INITIAL (if enabled)
#define SLEEP_TIMEOUT_INITIAL 1800

// If already joined, force deep sleep after SLEEP_TIMEOUT_JOINED seconds (if enabled)
#define SLEEP_TIMEOUT_JOINED 600

// Timeout for weather sensor data reception (seconds)
#define WEATHERSENSOR_TIMEOUT 60

// Enable transmission of weather sensor ID
//#define SENSORID_EN

// Enable battery / supply voltage measurement
#define ADC_EN

// Enable OneWire temperature measurement
#define ONEWIRE_EN

// Enable BLE temperature/humidity measurement
// Note: BLE requires a lot of program memory!
#define MITHERMOMETER_EN
//-----------------------------------------------------------------------------

#ifdef ONEWIRE_EN
    // Dallas/Maxim OneWire Temperature Sensor
    #include <DallasTemperature.h>
#endif

#ifdef ADC_EN
    // ESP32 calibrated Analog Input Reading
    #include <ESP32AnalogRead.h>
#endif

// BresserWeatherSensorLib
#include "src/BresserWeatherSensorReceiver/src/WeatherSensorCfg.h"
#include "src/BresserWeatherSensorReceiver/src/WeatherSensor.h"

#ifdef MITHERMOMETER_EN
    // BLE Temperature/Humidity Sensor
    #include <ATC_MiThermometer.h>
#endif

// forked from "Lora Serialization" by Joscha Feth
#include "src/LoRa_Serialization/src/LoraMessage.h"

// Pin mapping for ESP32
// SPI2 is used on ESP32 per default! (e.g. see https://github.com/espressif/arduino-esp32/tree/master/variants/doitESP32devkitV1)
#define PIN_LMIC_NSS      14
#define PIN_LMIC_RST      12
#define PIN_LMIC_DIO0     4
#define PIN_LMIC_DIO1     16
#define PIN_LMIC_DIO2     17

#ifdef ADC_EN
    #define PIN_ADC_IN        A0
    //#define PIN_ADC_IN        34
#endif

//#define PIN_ADC0_IN         36
//#define PIN_ADC1_IN         39
//#define PIN_ADC2_IN         34
#define PIN_ADC3_IN         A3

#ifdef PIN_ADC3_IN
    // Voltage divider R1 / (R1 + R2) -> V_meas = V(R1 + R2); V_adc = V(R1)
    const float ADC3_DIV         = 0.5;       
    const uint8_t ADC3_SAMPLES   = 10;
#endif

#ifdef ONEWIRE_EN
    #define PIN_ONEWIRE_BUS   5
#endif

#ifdef ADC_EN
    // Voltage divider R1 / (R1 + R2) -> V_meas = V(R1 + R2); V_adc = V(R1)
    const float UBATT_DIV         = 0.5;       
    const uint8_t UBATT_SAMPLES   = 10;
#endif

// Uplink message payload size (with some reserve)
const uint8_t PAYLOAD_SIZE = 36;

// RTC Memory Handling
#define MAGIC1 (('m' << 24) | ('g' < 16) | ('c' << 8) | '1')
#define MAGIC2 (('m' << 24) | ('g' < 16) | ('c' << 8) | '2')
#define EXTRA_INFO_MEM_SIZE 64

#ifdef MITHERMOMETER_EN
    // BLE scan time in seconds
    const int bleScanTime = 10;

    // List of known sensors' BLE addresses
    std::vector<std::string> knownBLEAddresses = {"a4:c1:38:bf:e1:bc"};
#endif

#define DEBUG_PORT Serial
#if defined(_BWS_DEBUG_MODE_)
    #define DEBUG_PRINTF(...) { DEBUG_PORT.printf(__VA_ARGS__); }
    #define DEBUG_PRINTF_TS(...) { DEBUG_PORT.printf("%d ms: ", osticks2ms(os_getTime())); \
                                   DEBUG_PORT.printf(__VA_ARGS__); }
#else
  #define DEBUG_PRINTF(...) {}
  #define DEBUG_PRINTF_TS(...) {}
#endif

    
/****************************************************************************\
|
|	The LoRaWAN object
|
\****************************************************************************/

class cMyLoRaWAN : public Arduino_LoRaWAN_ttn {
public:
    cMyLoRaWAN() {};
    using Super = Arduino_LoRaWAN_ttn;
    void setup();
    
protected:
    // you'll need to provide implementation for this.
    virtual bool GetOtaaProvisioningInfo(Arduino_LoRaWAN::OtaaProvisioningInfo*) override;

    // NetTxComplete() activates deep sleep mode (if enabled)
    virtual void NetTxComplete(void) override;

    // NetJoin() changes <sleepTimeout>
    virtual void NetJoin(void) override;
    
    // Used to store/load data to/from persistent (at least during deep sleep) memory 
    virtual void NetSaveSessionInfo(const SessionInfo &Info, const uint8_t *pExtraInfo, size_t nExtraInfo) override;
    //virtual bool GetSavedSessionInfo(SessionInfo &Info, uint8_t*, size_t, size_t*) override;
    virtual void NetSaveSessionState(const SessionState &State) override;
    virtual bool NetGetSessionState(SessionState &State) override;
    virtual bool GetAbpProvisioningInfo(AbpProvisioningInfo *pAbpInfo) override;  
};


/****************************************************************************\
|
|	The sensor object
|
\****************************************************************************/

class cSensor {
public:
    /// \brief the constructor. Deliberately does very little.
    cSensor() {};

    float getTemperature(void);
    uint16_t getVoltage(void);
    uint16_t getVoltage(ESP32AnalogRead &adc, uint8_t samples, float divider);
    bool uplinkRequest(void) {
        m_fUplinkRequest = true;
    };
    ///
    /// \brief set up the sensor object
    ///
    /// \param uplinkPeriodMs optional uplink interval. If not specified,
    ///         transmit every six minutes.
    ///
    void setup(std::uint32_t uplinkPeriodMs = 6 * 60 * 1000);

    ///
    /// \brief update sensor loop.
    ///
    /// \details
    ///     This should be called from the global loop(); it periodically
    ///     gathers and transmits sensor data.
    ///
    void loop();

    float    temperature1; //<! temperature (DS18B20)
    uint32_t voltage;      //<! battery voltage
    
private:
    void doUplink();

    bool m_fUplinkRequest;              // set true when uplink is requested
    bool m_fBusy;                       // set true while sending an uplink
    std::uint32_t m_uplinkPeriodMs;     // uplink period in milliseconds
    std::uint32_t m_tReference;         // time of last uplink
};

/****************************************************************************\
|
|	Globals
|
\****************************************************************************/

// the global LoRaWAN instance.
cMyLoRaWAN myLoRaWAN {};

// the global sensor instance
cSensor mySensor {};

// the global event log instance
Arduino_LoRaWAN::cEventLog myEventLog;

// The pin map. This form is convenient if the LMIC library
// doesn't support your board and you don't want to add the
// configuration to the library (perhaps you're just testing).
// This pinmap matches the FeatherM0 LoRa. See the arduino-lmic
// docs for more info on how to set this up.
const cMyLoRaWAN::lmic_pinmap myPinMap = {
     .nss = PIN_LMIC_NSS,
     .rxtx = cMyLoRaWAN::lmic_pinmap::LMIC_UNUSED_PIN,
     .rst = PIN_LMIC_RST,
     .dio = { PIN_LMIC_DIO0, PIN_LMIC_DIO1, PIN_LMIC_DIO2 },
     .rxtx_rx_active = 0,
     .rssi_cal = 0,
     .spi_freq = 8000000,
};

// The following variables are stored in the ESP32's RTC RAM -
// their value is retained after a Sleep Reset.
RTC_DATA_ATTR uint32_t                        magicFlag1;
RTC_DATA_ATTR Arduino_LoRaWAN::SessionState   rtcSavedSessionState;
RTC_DATA_ATTR uint32_t      magicFlag2;
RTC_DATA_ATTR Arduino_LoRaWAN::SessionInfo    rtcSavedSessionInfo;
RTC_DATA_ATTR size_t                          rtcSavedNExtraInfo;
RTC_DATA_ATTR uint8_t                         rtcSavedExtraInfo[EXTRA_INFO_MEM_SIZE];
RTC_DATA_ATTR bool                            runtimeExpired = 0;

#ifdef ADC_EN
    // ESP32 ADC with calibration
    ESP32AnalogRead adc;
#endif

// ESP32 ADC with calibration
#ifdef PIN_ADC0_IN
    ESP32AnalogRead adc0;
#endif
#ifdef PIN_ADC1_IN
    ESP32AnalogRead adc1;
#endif
#ifdef PIN_ADC2_IN
    ESP32AnalogRead adc2;
#endif
#ifdef PIN_ADC3_IN
    ESP32AnalogRead adc3;
#endif

// Setup Bresser Weather Sensor
WeatherSensor weatherSensor;


#ifdef ONEWIRE_EN
    // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
    OneWire oneWire(PIN_ONEWIRE_BUS);

    // Pass our oneWire reference to Dallas Temperature. 
    DallasTemperature temp_sensors(&oneWire);
#endif

#ifdef MITHERMOMETER_EN
    // Setup BLE Temperature/Humidity Sensors
    ATC_MiThermometer miThermometer(knownBLEAddresses);
#endif

// Uplink payload buffer
static uint8_t loraData[PAYLOAD_SIZE];

// Force sleep mode after <sleepTimeout> has been reached (if FORCE_SLEEP is defined) 
ostime_t sleepTimeout;

/****************************************************************************\
|
|	Provisioning info for LoRaWAN OTAA
|
\****************************************************************************/

//
// For normal use, we require that you edit the sketch to replace FILLMEIN_x
// with values assigned by the your network. However, for regression tests,
// we want to be able to compile these scripts. The regression tests define
// COMPILE_REGRESSION_TEST, and in that case we define FILLMEIN_x to non-
// working but innocuous values.
//
// #define COMPILE_REGRESSION_TEST 1

#ifdef COMPILE_REGRESSION_TEST
# define FILLMEIN_8     1, 0, 0, 0, 0, 0, 0, 0
# define FILLMEIN_16    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
#else
# warning "You must replace the values marked FILLMEIN with real values from the TTN control panel!"
# define FILLMEIN_8 (#dont edit this, edit the lines that use FILLMEIN_8)
# define FILLMEIN_16 (#dont edit this, edit the lines that use FILLMEIN_16)
#endif

// APPEUI, DEVEUI and APPKEY
#include "secrets.h"

#ifndef SECRETS
    #define SECRETS
    
    // The following constants should be copied to secrets.h and configured appropriately
    // according to the settings from TTN Console
    
    // deveui, little-endian (lsb first)
    static const std::uint8_t deveui[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    
    // appeui, little-endian (lsb first)
    static const std::uint8_t appeui[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    
    // appkey: just a string of bytes, sometimes referred to as "big endian".
    static const std::uint8_t appkey[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
#endif

/****************************************************************************\
|
|	setup()
|
\****************************************************************************/

void setup() {
    // set baud rate
    Serial.begin(115200);

    // wait for serial to be ready
    //while (! Serial)
    //    yield();
    delay(500);

    sleepTimeout = sec2osticks(SLEEP_TIMEOUT_INITIAL);

    DEBUG_PRINTF_TS("setup()\n");

    // set up the log; do this first.
    myEventLog.setup();
    DEBUG_PRINTF("myEventlog.setup() - done\n");

    // set up the sensors.
    mySensor.setup();
    DEBUG_PRINTF("mySensor.setup() - done\n");

    // set up lorawan.
    myLoRaWAN.setup();
    DEBUG_PRINTF("myLoRaWAN.setup() - done\n");

    mySensor.uplinkRequest();
}

/****************************************************************************\
|
|	loop()
|
\****************************************************************************/

void loop() {
    // the order of these is arbitrary, but you must poll them all.
    myLoRaWAN.loop();
    mySensor.loop();
    myEventLog.loop();

    #ifdef FORCE_SLEEP
        if (os_getTime() > sleepTimeout) {
            DEBUG_PRINTF_TS("Sleep timer expired!\n");
            DEBUG_PRINTF("Shutdown()\n");
            runtimeExpired = true;
            myLoRaWAN.Shutdown();
            magicFlag1 = 0;
            magicFlag2 = 0;
            ESP.deepSleep(SLEEP_INTERVAL * 1000000);
        }
    #endif
}

/****************************************************************************\
|
|	LoRaWAN methods
|
\****************************************************************************/

// our setup routine does the class setup and then registers an event handler so
// we can see some interesting things
void
cMyLoRaWAN::setup() {
    // simply call begin() w/o parameters, and the LMIC's built-in
    // configuration for this board will be used.
    bool res = this->Super::begin(myPinMap);
    DEBUG_PRINTF("Arduino_LoRaWAN::begin(): %d\n", res);


//    LMIC_selectSubBand(0);
    LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100);

    this->RegisterListener(
        // use a lambda so we're "inside" the cMyLoRaWAN from public/private perspective
        [](void *pClientInfo, uint32_t event) -> void {
            auto const pThis = (cMyLoRaWAN *)pClientInfo;

            // for tx start, we quickly capture the channel and the RPS
            if (event == EV_TXSTART) {
                // use another lambda to make log prints easy
                myEventLog.logEvent(
                    (void *) pThis,
                    LMIC.txChnl,
                    LMIC.rps,
                    0,
                    // the print-out function
                    [](cEventLog::EventNode_t const *pEvent) -> void {
                        Serial.print(F(" TX:"));
                        myEventLog.printCh(std::uint8_t(pEvent->getData(0)));
                        myEventLog.printRps(rps_t(pEvent->getData(1)));
                    }
                );
            }
            // else if (event == some other), record with print-out function
            else {
                // do nothing.
            }
        },
        (void *) this   // in case we need it.
        );
}

// this method is called when the LMIC needs OTAA info.
// return false to indicate "no provisioning", otherwise
// fill in the data and return true.
bool
cMyLoRaWAN::GetOtaaProvisioningInfo(
    OtaaProvisioningInfo *pInfo
    ) {
    // these are the same constants used in the LMIC compliance test script; eases testing
    // with the RedwoodComm RWC5020B/RWC5020M testers.

    // initialize info
    memcpy(pInfo->DevEUI, deveui, sizeof(pInfo->DevEUI));
    memcpy(pInfo->AppEUI, appeui, sizeof(pInfo->AppEUI));
    memcpy(pInfo->AppKey, appkey, sizeof(pInfo->AppKey));

    return true;
}

// This method is called after the node has joined the network.
void
cMyLoRaWAN::NetJoin(
    void) {
    DEBUG_PRINTF_TS("NetJoin()\n");
    sleepTimeout = os_getTime() + sec2osticks(SLEEP_TIMEOUT_JOINED);
}

// This method is called after transmission has been completed.
// If enabled, the controller goes into deep sleep mode now.
void
cMyLoRaWAN::NetTxComplete(
    void) {
    DEBUG_PRINTF_TS("NetTxComplete()\n");
    #ifdef SLEEP_EN
        DEBUG_PRINTF("Shutdown()\n");
        myLoRaWAN.Shutdown();
        ESP.deepSleep(SLEEP_INTERVAL * 1000000);
    #endif
}

#ifdef _BWS_DEBUG_MODE_
// Print session info for debugging
void printSessionInfo(const cMyLoRaWAN::SessionInfo &Info)
{
    Serial.printf("Tag:\t\t%d\n", Info.V1.Tag);
    Serial.printf("Size:\t\t%d\n", Info.V1.Size);
    Serial.printf("Rsv2:\t\t%d\n", Info.V1.Rsv2);
    Serial.printf("Rsv3:\t\t%d\n", Info.V1.Rsv3);
    Serial.printf("NetID:\t\t0x%08X\n", Info.V1.NetID);
    Serial.printf("DevAddr:\t0x%08X\n", Info.V1.DevAddr);
    Serial.printf("NwkSKey:\t");
    for (int i=0; i<15;i++) {
      Serial.printf("%02X ", Info.V1.NwkSKey[i]);  
    }
    Serial.printf("\n");
    Serial.printf("AppSKey:\t");
    for (int i=0; i<15;i++) {
      Serial.printf("%02X ", Info.V1.AppSKey[i]);  
    }
    Serial.printf("\n");    
}

// Print session state for debugging
void printSessionState(const cMyLoRaWAN::SessionState &State)
{
    Serial.printf("Tag:\t\t%d\n", State.V1.Tag);
    Serial.printf("Size:\t\t%d\n", State.V1.Size);
    Serial.printf("Region:\t\t%d\n", State.V1.Region);
    Serial.printf("LinkDR:\t\t%d\n", State.V1.LinkDR);
    Serial.printf("FCntUp:\t\t%d\n", State.V1.FCntUp);
    Serial.printf("FCntDown:\t%d\n", State.V1.FCntDown);
    Serial.printf("gpsTime:\t%d\n", State.V1.gpsTime);
    Serial.printf("globalAvail:\t%d\n", State.V1.globalAvail);
    Serial.printf("Rx2Frequency:\t%d\n", State.V1.Rx2Frequency);
    Serial.printf("PingFrequency:\t%d\n", State.V1.PingFrequency);
    Serial.printf("Country:\t%d\n", State.V1.Country);
    Serial.printf("LinkIntegrity:\t%d\n", State.V1.LinkIntegrity);
    // There is more in it...
}
#endif

// Save Info to ESP32's RTC RAM
// if not possible, just do nothing and make sure you return false
// from NetGetSessionState().
void
cMyLoRaWAN::NetSaveSessionInfo(
    const SessionInfo &Info,
    const uint8_t *pExtraInfo,
    size_t nExtraInfo
    ) {
    if (nExtraInfo > EXTRA_INFO_MEM_SIZE)
        return;
    rtcSavedSessionInfo = Info;
    rtcSavedNExtraInfo = nExtraInfo;
    memcpy(rtcSavedExtraInfo, pExtraInfo, nExtraInfo);
    magicFlag2 = MAGIC2;
    DEBUG_PRINTF_TS("NetSaveSessionInfo()\n");
    #ifdef _BWS_DEBUG_MODE_
        printSessionInfo(Info);
    #endif
}

/// Return saved session info (keys) from ESP32's RTC RAM
///
/// if you have persistent storage, you should provide a function
/// that gets the saved session info from persistent storage, or
/// indicate that there isn't a valid saved session. Note that
/// the saved info is opaque to the higher level.
///
/// \return true if \p sessionInfo was filled in, false otherwise.
///
/// Note:
/// According to "Purpose of NetSaveSessionInfo #165"
/// (https://github.com/mcci-catena/arduino-lorawan/issues/165)
/// "GetSavedSessionInfo() is effectively useless and should probably be removed to avoid confusion."
/// sic!
#if false
bool 
cMyLoRaWAN::GetSavedSessionInfo(
                SessionInfo &sessionInfo,
                uint8_t *pExtraSessionInfo,
                size_t nExtraSessionInfo,
                size_t *pnExtraSessionActual
                ) {
    if (magicFlag2 != MAGIC2) {
        // if not provided, default zeros buf and returns false.
        memset(&sessionInfo, 0, sizeof(sessionInfo));
        if (pExtraSessionInfo) {
            memset(pExtraSessionInfo, 0, nExtraSessionInfo);
        }
        if (pnExtraSessionActual) {
            *pnExtraSessionActual = 0;
        }
        DEBUG_PRINTF_TS("GetSavedSessionInfo() - failed\n");
        return false;
    } else {
        sessionInfo = rtcSavedSessionInfo;
        if (pExtraSessionInfo) {
            memcpy(pExtraSessionInfo, rtcSavedExtraInfo, nExtraSessionInfo);
        }
        if (pnExtraSessionActual) {
            *pnExtraSessionActual = rtcSavedNExtraInfo;
        }
        DEBUG_PRINTF_TS("GetSavedSessionInfo() - o.k.\n");
        #ifdef _BWS_DEBUG_MODE_
            printSessionInfo(sessionInfo);
        #endif
        return true;
    }
}
#endif

// Save State in RTC RAM. Note that it's often the same;
// often only the frame counters change.
// [If not possible, just do nothing and make sure you return false
// from NetGetSessionState().]
void
cMyLoRaWAN::NetSaveSessionState(const SessionState &State) {
    rtcSavedSessionState = State;
    magicFlag1 = MAGIC1;
    DEBUG_PRINTF_TS("NetSaveSessionState()\n");
    #ifdef _BWS_DEBUG_MODE_
        printSessionState(State);
    #endif
}

// Either fetch SessionState from somewhere and return true or...
// return false, which forces a re-join.
bool
cMyLoRaWAN::NetGetSessionState(SessionState &State) {
    if (magicFlag1 == MAGIC1) {
        State = rtcSavedSessionState;
        DEBUG_PRINTF_TS("NetGetSessionState() - o.k.\n");
        #ifdef _BWS_DEBUG_MODE_
            printSessionState(State);
        #endif
        return true;
    } else {
        DEBUG_PRINTF_TS("NetGetSessionState() - failed\n");
        return false;
    }
}

// Get APB provisioning info - this is also used in OTAA after a succesful join.
// If it can be provided in OTAA mode after a restart, no re-join is needed.
bool
cMyLoRaWAN::GetAbpProvisioningInfo(AbpProvisioningInfo *pAbpInfo) {
    SessionState state;

    // ApbInfo:
    // --------
    // uint8_t         NwkSKey[16];
    // uint8_t         AppSKey[16];
    // uint32_t        DevAddr;
    // uint32_t        NetID;
    // uint32_t        FCntUp;
    // uint32_t        FCntDown;
    
    if ((magicFlag1 != MAGIC1) || (magicFlag2 != MAGIC2)) {
         return false;
    }
    DEBUG_PRINTF_TS("GetAbpProvisioningInfo()\n");

    pAbpInfo->DevAddr = rtcSavedSessionInfo.V2.DevAddr;
    pAbpInfo->NetID   = rtcSavedSessionInfo.V2.NetID;
    memcpy(pAbpInfo->NwkSKey, rtcSavedSessionInfo.V2.NwkSKey, 16);
    memcpy(pAbpInfo->AppSKey, rtcSavedSessionInfo.V2.AppSKey, 16);
    NetGetSessionState(state);
    pAbpInfo->FCntUp   = state.V1.FCntUp;
    pAbpInfo->FCntDown = state.V1.FCntDown;

    #ifdef _BWS_DEBUG_MODE_
        Serial.printf("NwkSKey:\t");
        for (int i=0; i<15;i++) {
          Serial.printf("%02X ", pAbpInfo->NwkSKey[i]);  
        }
        Serial.printf("\n");
        Serial.printf("AppSKey:\t");
        for (int i=0; i<15;i++) {
          Serial.printf("%02X ", pAbpInfo->AppSKey[i]);  
        }
        Serial.printf("\n");
        Serial.printf("FCntUp: %d\n", state.V1.FCntUp);
    #endif
    return true;
}


/****************************************************************************\
|
|	Sensor methods
|
\****************************************************************************/

void
cSensor::setup(std::uint32_t uplinkPeriodMs) {
    // set the initial time.
    this->m_uplinkPeriodMs = uplinkPeriodMs;
    this->m_tReference = millis();

    #ifdef ADC_EN
        // Use ADC with PIN_ADC_IN
        adc.attach(PIN_ADC_IN);
    #endif

    #ifdef PIN_ADC3_IN
        // Use ADC3 with PIN_ADC3_IN
        adc3.attach(PIN_ADC3_IN);
    #endif
    
    #ifdef MITHERMOMETER_EN
        miThermometer.begin();
    #endif
    
    #ifndef TTN_DEBUG
        weatherSensor.begin();
        weatherSensor.getData(WEATHERSENSOR_TIMEOUT * 1000, true);
    #else
        weatherSensor.genMessage();
    #endif
    if (weatherSensor.data_ok) {
        DEBUG_PRINTF("Receiving Weather Sensor Data o.k.\n");
    } else {
        DEBUG_PRINTF("Receiving Weather Sensor Data failed.\n");
    }
}

void
cSensor::loop(void) {
    auto const tNow = millis();
    auto const deltaT = tNow - this->m_tReference;

    if (deltaT >= this->m_uplinkPeriodMs) {
        // request an uplink
        this->m_fUplinkRequest = true;

        // keep trigger time locked to uplinkPeriod
        auto const advance = deltaT / this->m_uplinkPeriodMs;
        this->m_tReference += advance * this->m_uplinkPeriodMs; 
    }

    // if an uplink was requested, do it.
    if (this->m_fUplinkRequest) {
        this->m_fUplinkRequest = false;
        this->doUplink();
    }
}

#ifdef ADC_EN
//
// Get supply / battery voltage
//
uint16_t
cSensor::getVoltage(void)
{
    float voltage_raw = 0;
    for (uint8_t i=0; i < UBATT_SAMPLES; i++) {
        voltage_raw += float(adc.readMiliVolts());
    }
    uint16_t voltage = int(voltage_raw / UBATT_SAMPLES / UBATT_DIV);
     
    DEBUG_PRINTF("Voltage = %dmV\n", voltage);

    return voltage;
}
#endif

//
// Get supply / battery voltage
//
uint16_t
cSensor::getVoltage(ESP32AnalogRead &adc, uint8_t samples, float divider)
{
    float voltage_raw = 0;
    for (uint8_t i=0; i < samples; i++) {
        voltage_raw += float(adc.readMiliVolts());
    }
    uint16_t voltage = int(voltage_raw / samples / divider);
     
    DEBUG_PRINTF("Voltage = %dmV\n", voltage);

    return voltage;
}

#ifdef ONEWIRE_EN
//
// Get temperature from Maxim OneWire Sensor
//
float
cSensor::getTemperature(void)
{
    // Call sensors.requestTemperatures() to issue a global temperature 
    // request to all devices on the bus
    temp_sensors.requestTemperatures(); // Send the command to get temperatures
        
    // After we got the temperatures, we can print them here.
    // We use the function ByIndex, and as an example get the temperature from the first sensor only.
    float tempC = temp_sensors.getTempCByIndex(0);

    #ifdef _DEBUG_MODE_
        // Check if reading was successful
        if (tempC != DEVICE_DISCONNECTED_C) {
            DEBUG_PRINTF("Temperature = %.2f°C\n", tempC);
        } else {
            DEBUG_PRINTF("Error: Could not read temperature data\n");
        }
    #endif
}
#endif
    
//
// Prepare uplink data for transmission
//
void
cSensor::doUplink(void) {
    // if busy uplinking, just skip
    if (this->m_fBusy) {
        DEBUG_PRINTF_TS("doUplink(): busy\n");
        return;
    }
    // if LMIC is busy, just skip
    if (LMIC.opmode & (OP_POLL | OP_TXDATA | OP_TXRXPEND)) {
        DEBUG_PRINTF_TS("doUplink(): other operation in progress\n");    
        return;
    }
    
    // Note:
    // Currently Weather Sensor Data can only be received during initialization if
    // the radio transceiver is shared between the BresserWeatherSensorLib and the LoRaWAN library.
    // Therefore the node must perform a reset after each succesful uplink.
    //
    // The required procedure to update the Weather Sensor Data (if radio receiver shared with LoRaWAN) would be:
    // 1. Save radio registers modified by LoRaWAN
    // 2. Re-initialize radio for sensor
    // 3. Receive data
    // 4. Restore radio registers modified by LoRaWAN
    //
    // ToDo:
    // Which registers are actually shared between FSK and LoRaWAN mode?
    // Is there any additional state to be preserved?
    
    #ifdef ONEWIRE_EN
        float     water_temp_c        = getTemperature();
    #endif
    #ifdef ADC_EN
        uint16_t  supply_voltage      = getVoltage();
    #endif
    #ifdef PIN_ADC3_IN
        uint16_t  battery_voltage     = getVoltage(adc3, ADC3_SAMPLES, ADC3_DIV);
    #endif
    bool          mithermometer_valid = false;
    #ifdef MITHERMOMETER_EN
        float     indoor_temp_c;
        float     indoor_humidity;
        
    
        // Set sensor data invalid
        miThermometer.resetData();
        
        // Get sensor data - run BLE scan for <bleScanTime>
        miThermometer.getData(bleScanTime);
    #endif
    
    DEBUG_PRINTF("--- Uplink Data ---\n");
    DEBUG_PRINTF("Air Temperature:   % 3.1f °C\n", weatherSensor.temp_c);
    DEBUG_PRINTF("Humidity:           %2d   %%\n", weatherSensor.humidity);
    DEBUG_PRINTF("Rain Gauge:      %7.1f mm\n",     weatherSensor.rain_mm);
    DEBUG_PRINTF("Wind Speed (avg.):   %3.1f m/s\n",   weatherSensor.wind_avg_meter_sec);
    DEBUG_PRINTF("Wind Speed (max.):   %3.1f m/s\n",   weatherSensor.wind_gust_meter_sec);
    DEBUG_PRINTF("Wind Direction:     %4.1f °\n",     weatherSensor.wind_direction_deg);
    if (water_temp_c != DEVICE_DISCONNECTED_C) {
        DEBUG_PRINTF("Water Temperature: % 2.1f °C\n",  water_temp_c);
    } else {
        DEBUG_PRINTF("Water Temperature:   --.- °C\n");
        water_temp_c = -30.0;
    }
    #ifdef ADC_EN
        DEBUG_PRINTF("Supply  Voltage:  %4d   mV\n",       supply_voltage);
    #endif
    #ifdef PIN_ADC3_IN
        DEBUG_PRINTF("Battery Voltage:  %4d   mV\n",       battery_voltage);
    #endif
    #ifdef MITHERMOMETER_EN
        if (miThermometer.data[0].valid) {
            mithermometer_valid = true;
            indoor_temp_c   = miThermometer.data[0].temperature/100.0;
            indoor_humidity = miThermometer.data[0].humidity/100.0;
            DEBUG_PRINTF("Indoor Air Temp.:  % 3.1f °C\n", miThermometer.data[0].temperature/100.0);
            DEBUG_PRINTF("Indoor Humidity:    %3.1f %%\n", miThermometer.data[0].humidity/100.0);
        } else {
            DEBUG_PRINTF("Indoor Air Temp.:   --.- °C\n");
            DEBUG_PRINTF("Indoor Humidity:    --   %%\n");
            indoor_temp_c   = -30;
            indoor_humidity = 0;
        }
    #endif
    DEBUG_PRINTF("\n");
    
    LoraEncoder encoder(loraData);
    #ifdef SENSORID_EN
        encoder.writeUint32(weatherSensor.sensor_id);
    #endif
    encoder.writeBitmap(false, false, false, false, 
                        runtimeExpired,
                        mithermometer_valid,
                        weatherSensor.data_ok,
                        weatherSensor.battery_ok);
    encoder.writeTemperature(weatherSensor.temp_c);
    encoder.writeUint8(weatherSensor.humidity);
    #ifdef ENCODE_AS_FLOAT
        encoder.writeRawFloat(weatherSensor.wind_gust_meter_sec);
        encoder.writeRawFloat(weatherSensor.wind_avg_meter_sec);
        encoder.writeRawFloat(weatherSensor.wind_direction_deg);
    #else
        encoder.writeUint16(weatherSensor.wind_gust_meter_sec_fp1);
        encoder.writeUint16(weatherSensor.wind_avg_meter_sec_fp1);
        encoder.writeUint16(weatherSensor.wind_direction_deg_fp1);
    #endif
    encoder.writeRawFloat(weatherSensor.rain_mm);
    #ifdef ADC_EN
        encoder.writeUint16(supply_voltage);
    #endif
    #ifdef PIN_ADC3_IN
        encoder.writeUint16(battery_voltage);
    #endif
    #ifdef ONEWIRE_EN
        encoder.writeTemperature(water_temp_c);
    #endif
    #ifdef MITHERMOMETER_EN
        encoder.writeTemperature(indoor_temp_c);
        encoder.writeUint8((uint8_t)(indoor_humidity+0.5));

        // BLE Tempoerature/Humidity Sensor: delete results fromBLEScan buffer to release memory
        miThermometer.clearScanResults();
    #endif    
    //encoder.writeRawFloat(radio.getRSSI()); // FIXME: Integer should be sufficient

    this->m_fBusy = true;
    
    if (! myLoRaWAN.SendBuffer(
        loraData, sizeof(loraData),
        // this is the completion function:
        [](void *pClientData, bool fSucccess) -> void {
            auto const pThis = (cSensor *)pClientData;
            pThis->m_fBusy = false;
        },
        (void *)this,
        /* confirmed */ true,
        /* port */ 1
        )) {
        // sending failed; callback has not been called and will not
        // be called. Reset busy flag.
        this->m_fBusy = false;
    }
}