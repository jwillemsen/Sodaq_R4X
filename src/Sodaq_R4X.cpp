/*
    Copyright (c) 2019 Sodaq.  All rights reserved.

    This file is part of Sodaq_nbIOT.

    Sodaq_nbIOT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as
    published by the Free Software Foundation, either version 3 of
    the License, or(at your option) any later version.

    Sodaq_nbIOT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with Sodaq_nbIOT.  If not, see
    <http://www.gnu.org/licenses/>.
*/

#include "Sodaq_R4X.h"
#include <Sodaq_wdt.h>
#include "time.h"

//#define DEBUG

#define EPOCH_TIME_OFF          946684800  /* This is 1st January 2000, 00:00:00 in epoch time */
#define EPOCH_TIME_YEAR_OFF     100        /* years since 1900 */
#define ATTACH_TIMEOUT          180000
#define ATTACH_NEED_REBOOT      40000
#define CGACT_TIMEOUT           150000
#define COPS_TIMEOUT            180000
#define ISCONNECTED_CSQ_TIMEOUT 10000
#define REBOOT_DELAY            15000
#define SEND_TIMEOUT            10000
#define UMQTT_TIMEOUT           60000

#define SODAQ_GSM_TERMINATOR "\r\n"
#define SODAQ_GSM_MODEM_DEFAULT_INPUT_BUFFER_SIZE 1024
#define SODAQ_GSM_TERMINATOR_LEN (sizeof(SODAQ_GSM_TERMINATOR) - 1)

#define DEFAULT_BANDMASK        "524288"
#define DEFAULT_URAT            "8"

#define STR_AT                  "AT"
#define STR_RESPONSE_OK         "OK"
#define STR_RESPONSE_ERROR      "ERROR"
#define STR_RESPONSE_CME_ERROR  "+CME ERROR:"
#define STR_RESPONSE_CMS_ERROR  "+CMS ERROR:"

#define DEBUG_STR_ERROR         "[ERROR]: "

#define NIBBLE_TO_HEX_CHAR(i)  ((i <= 9) ? ('0' + i) : ('A' - 10 + i))
#define HIGH_NIBBLE(i)         ((i >> 4) & 0x0F)
#define LOW_NIBBLE(i)          (i & 0x0F)
#define HEX_CHAR_TO_NIBBLE(c)  ((c >= 'A') ? (c - 'A' + 0x0A) : (c - '0'))
#define HEX_PAIR_TO_BYTE(h, l) ((HEX_CHAR_TO_NIBBLE(h) << 4) + HEX_CHAR_TO_NIBBLE(l))

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#ifdef DEBUG
#define debugPrint(...)   { if (_diagStream) _diagStream->print(__VA_ARGS__); }
#define debugPrintLn(...) { if (_diagStream) _diagStream->println(__VA_ARGS__); }
#warning "Debug mode is ON"
#else
#define debugPrint(...)
#define debugPrintLn(...)
#endif

#define CR '\r'
#define LF '\n'

#define SOCKET_FAIL -1
#define NOW (uint32_t)millis()

static inline bool is_timedout(uint32_t from, uint32_t nr_ms) __attribute__((always_inline));
static inline bool is_timedout(uint32_t from, uint32_t nr_ms) { return (millis() - from) > nr_ms; }


/******************************************************************************
* Main
*****************************************************************************/

Sodaq_R4X::Sodaq_R4X() :
    _modemStream(0),
    _diagStream(0),
    _appendCommand(false),
    _CSQtime(0),
    _startOn(0)
{
    _isBufferInitialized = false;
    _inputBuffer         = 0;
    _inputBufferSize     = SODAQ_GSM_MODEM_DEFAULT_INPUT_BUFFER_SIZE;
    _lastRSSI            = 0;
    _minRSSI             = -113;  // dBm
    _onoff               = 0;
    _mqttLoginResult     = -1;
    _mqttPendingMessages = -1;
    _mqttSubscribeReason = -1;
    _pin                 = 0;

    memset(_pendingUDPBytes, 0, sizeof(_pendingUDPBytes));
}

// Initializes the modem instance. Sets the modem stream and the on-off power pins.
void Sodaq_R4X::init(Sodaq_OnOffBee* onoff, Stream& stream, uint8_t cid)
{
    debugPrintLn("[init] started.");

    initBuffer(); // safe to call multiple times

    setModemStream(stream);

    _onoff = onoff;
    _cid   = cid;
}

// Turns the modem on and returns true if successful.
bool Sodaq_R4X::on()
{
    _startOn = millis();

    if (!isOn() && _onoff) {
        _onoff->on();
    }

    // wait for power up
    bool timeout = true;
    for (uint8_t i = 0; i < 10; i++) {
        if (isAlive()) {
            timeout = false;
            break;
        }
    }

    if (timeout) {
        debugPrintLn("Error: No Reply from Modem");
        return false;
    }

    return isOn(); // this essentially means isOn() && isAlive()
}

// Turns the modem off and returns true if successful.
bool Sodaq_R4X::off()
{
    // No matter if it is on or off, turn it off.
    if (_onoff) {
        _onoff->off();
    }

    return !isOn();
}

// Turns on and initializes the modem, then connects to the network and activates the data connection.
bool Sodaq_R4X::connect(const char* apn, const char* urat, const char* bandMask)
{
    if (!on()) {
        return false;
    }

    purgeAllResponsesRead();

    if (!setVerboseErrors(true)) {
        return false;
    }

    if (!execCommand("ATE0")) {
        return false;
    }

    if (!checkCFUN()) {
        return false;
    }

    if (!checkCOPS()) {
        return false;
    }

    if (!checkUrat(urat != 0 ? urat : DEFAULT_URAT)) {
        return false;
    }

    if (!checkBandMask(urat, bandMask != 0 ? bandMask : DEFAULT_BANDMASK)) {
        return false;
    }

    int8_t i = checkApn(apn);
    if (i < 0) {
        return false;
    }

    uint32_t tm = millis();

    if (!waitForSignalQuality()) {
        return false;
    }

    if (i == 0 && !attachGprs(ATTACH_TIMEOUT)) {
        return false;
    }

    if (millis() - tm > ATTACH_NEED_REBOOT) {
        reboot();
        sodaq_wdt_safe_delay(REBOOT_DELAY);
    }

    return execCommand("AT+UDCONF=1,1") && doSIMcheck();
}

// Disconnects the modem from the network.
bool Sodaq_R4X::disconnect()
{
    return execCommand("AT+CGATT=0", 40000);
}


/******************************************************************************
* Public
*****************************************************************************/

bool Sodaq_R4X::attachGprs(uint32_t timeout)
{
    uint32_t start = millis();
    uint32_t delay_count = 500;

    while (!is_timedout(start, timeout)) {
        if (isAttached()) {
            if (isDefinedIP4() || (execCommand("AT+CGACT=1", CGACT_TIMEOUT) && isDefinedIP4())) {
                return true;
            }
        }

        sodaq_wdt_safe_delay(delay_count);

        // Next time wait a little longer, but not longer than 5 seconds
        if (delay_count < 5000) {
            delay_count += 1000;
        }
    }

    return false;
}

// Gets Integrated Circuit Card ID.
// Should be provided with a buffer of at least 21 bytes.
// Returns true if successful.
bool Sodaq_R4X::getCCID(char* buffer, size_t size)
{
    if (buffer == NULL || size < 20 + 1) {
        return false;
    }

    println("AT+CCID");

    return (readResponse(buffer, size, "+CCID: ") == GSMResponseOK) && (strlen(buffer) > 0);
}

bool Sodaq_R4X::getEpoch(uint32_t* epoch)
{
    println("AT+CCLK?");

    char buffer[128];

    if (readResponse(buffer, sizeof(buffer), "+CCLK: ") != GSMResponseOK) {
        return false;
    }

    // format: "yy/MM/dd,hh:mm:ss+TZ
    int y, m, d, h, min, sec, tz;
    if (sscanf(buffer, "\"%d/%d/%d,%d:%d:%d+%d\"", &y, &m, &d, &h, &min, &sec, &tz) == 7 ||
        sscanf(buffer, "\"%d/%d/%d,%d:%d:%d\"", &y, &m, &d, &h, &min, &sec) == 6)
    {
        *epoch = convertDatetimeToEpoch(y, m, d, h, min, sec);
        return true;
    }

    return false;
}


bool Sodaq_R4X::getFirmwareVersion(char* buffer, size_t size)
{
    if (buffer == NULL || size < 30 + 1) {
        return false;
    }

    return execCommand("AT+CGMR", DEFAULT_READ_MS, buffer, size);
}

// Gets International Mobile Equipment Identity.
// Should be provided with a buffer of at least 16 bytes.
// Returns true if successful.
bool Sodaq_R4X::getIMEI(char* buffer, size_t size)
{
    if (buffer == NULL || size < 15 + 1) {
        return false;
    }

    return (execCommand("AT+CGSN", DEFAULT_READ_MS, buffer, size) == GSMResponseOK) && (strlen(buffer) > 0);
}

SimStatuses Sodaq_R4X::getSimStatus()
{
    println("AT+CPIN?");

    char buffer[32];

    if (readResponse(buffer, sizeof(buffer)) != GSMResponseOK) {
        return SimStatusUnknown;
    }

    char status[16];

    if (sscanf(buffer, "+CPIN: %" STR(sizeof(status) - 1) "s", status) == 1) {
        return startsWith("READY", status) ? SimReady : SimNeedsPin;
    }

    return SimMissing;
}

bool Sodaq_R4X::execCommand(const char* command, uint32_t timeout, char* buffer, size_t size)
{
    println(command);

    return (readResponse(buffer, size, NULL, timeout) == GSMResponseOK);
}

// Returns true if the modem replies to "AT" commands without timing out.
bool Sodaq_R4X::isAlive()
{
    return execCommand(STR_AT, 450);
}

// Returns true if the modem is attached to the network and has an activated data connection.
bool Sodaq_R4X::isAttached()
{
    println("AT+CGATT?");

    char buffer[16];

    if (readResponse(buffer, sizeof(buffer), "+CGATT: ", 10 * 1000) != GSMResponseOK) {
        return false;
    }

    return (strcmp(buffer, "1") == 0);
}

// Returns true if the modem is connected to the network and IP address is not 0.0.0.0.
bool Sodaq_R4X::isConnected()
{
    return isAttached() && waitForSignalQuality(ISCONNECTED_CSQ_TIMEOUT) && isDefinedIP4();
}

// Returns true if defined IP4 address is not 0.0.0.0.
bool Sodaq_R4X::isDefinedIP4()
{
    println("AT+CGDCONT?");

    char buffer[256];

    if (readResponse(buffer, sizeof(buffer), "+CGDCONT: ") != GSMResponseOK) {
        return false;
    }

    if (strncmp(buffer, "1,\"IP\"", 6) != 0 || strncmp(buffer + 6, ",\"\"", 3) == 0) {
        return false;
    }

    char ip[32];

    if (sscanf(buffer + 6, ",\"%*[^\"]\",\"%[^\"]\",0,0,0,0", ip) != 1) {
        return false;
    }

    return strlen(ip) >= 7 && strcmp(ip, "0.0.0.0") != 0;
}

void Sodaq_R4X::purgeAllResponsesRead()
{
    uint32_t start = millis();

    // make sure all the responses within the timeout have been read
    while ((readResponse(NULL, 0, NULL, 1000) != GSMResponseTimeout) && !is_timedout(start, 2000)) {}
}

bool Sodaq_R4X::setApn(const char* apn)
{
    print("AT+CGDCONT=");
    print(_cid);
    print(",\"IP\",\"");
    print(apn);
    println('"');

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_R4X::setIndicationsActive(bool on)
{
    print("AT+CNMI=");
    println(on ? "1" : "0");

    return (readResponse() == GSMResponseOK);
}

void Sodaq_R4X::setPin(const char * pin)
{
    size_t len = strlen(pin);
    _pin = static_cast<char*>(realloc(_pin, len + 1));
    strcpy(_pin, pin);
}

bool Sodaq_R4X::setRadioActive(bool on)
{
    print("AT+CFUN=");
    println(on ? "1" : "0");

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_R4X::setVerboseErrors(bool on)
{
    print("AT+CMEE=");
    println(on ? "2" : "0");

    return (readResponse() == GSMResponseOK);
}


/******************************************************************************
* RSSI and CSQ
*****************************************************************************/

/*
    The range is the following:
    0: -113 dBm or less
    1: -111 dBm
    2..30: from -109 to -53 dBm with 2 dBm steps
    31: -51 dBm or greater
    99: not known or not detectable or currently not available
*/
int8_t Sodaq_R4X::convertCSQ2RSSI(uint8_t csq) const
{
    return -113 + 2 * csq;
}

uint8_t Sodaq_R4X::convertRSSI2CSQ(int8_t rssi) const
{
    return (rssi + 113) / 2;
}

// Gets the Received Signal Strength Indication in dBm and Bit Error Rate.
// Returns true if successful.
bool Sodaq_R4X::getRSSIAndBER(int8_t* rssi, uint8_t* ber)
{
    static char berValues[] = { 49, 43, 37, 25, 19, 13, 7, 0 }; // 3GPP TS 45.008 [20] subclause 8.2.4

    println("AT+CSQ");

    char buffer[256];

    if (readResponse(buffer, sizeof(buffer), "+CSQ: ") != GSMResponseOK) {
        return false;
    }

    int csqRaw;
    int berRaw;

    if (sscanf(buffer, "%d,%d", &csqRaw, &berRaw) != 2) {
        return false;
    }

    *rssi = ((csqRaw == 99) ? 0 : convertCSQ2RSSI(csqRaw));
    *ber  = ((berRaw == 99 || static_cast<size_t>(berRaw) >= sizeof(berValues)) ? 0 : berValues[berRaw]);

    return true;
}


/******************************************************************************
* Sockets
*****************************************************************************/

int Sodaq_R4X::createSocket(uint16_t localPort)
{
    print("AT+USOCR=17,");
    println(localPort);

    char buffer[32];

    if (readResponse(buffer, sizeof(buffer), "+USOCR: ") != GSMResponseOK) {
        return SOCKET_FAIL;
    }

    int socketID;

    if ((sscanf(buffer, "%d", &socketID) != 1) || (socketID < 0) || (socketID > SOCKET_COUNT)) {
        return SOCKET_FAIL;
    }

    _pendingUDPBytes[socketID] = 0;

    return socketID;
}

bool Sodaq_R4X::closeSocket(uint8_t socketID)
{
    print("AT+USOCL=");
    println(socketID);

    if (readResponse() != GSMResponseOK) {
        return false;
    }

    _pendingUDPBytes[socketID] = 0;

    return true;
}

size_t Sodaq_R4X::getPendingUDPBytes(uint8_t socketID)
{
    return _pendingUDPBytes[socketID];
}

bool Sodaq_R4X::hasPendingUDPBytes(uint8_t socketID)
{
    return _pendingUDPBytes[socketID] > 0;
}

size_t Sodaq_R4X::socketReceiveBytes(uint8_t socketID, uint8_t* buffer, size_t length, SaraN2UDPPacketMetadata* p)
{
    size_t size = min(length, min(SODAQ_R4X_MAX_UDP_BUFFER, _pendingUDPBytes[socketID]));

    SaraN2UDPPacketMetadata packet;

    char tempBuffer[SODAQ_R4X_MAX_UDP_BUFFER];

    size_t receivedSize = socketReceive(socketID, p ? p : &packet, tempBuffer, size);

    if (buffer && length > 0) {
        for (size_t i = 0; i < receivedSize * 2; i += 2) {
            buffer[i / 2] = HEX_PAIR_TO_BYTE(tempBuffer[i], tempBuffer[i + 1]);
        }
    }

    return receivedSize;
}

size_t Sodaq_R4X::socketReceiveHex(uint8_t socketID, char* buffer, size_t length, SaraN2UDPPacketMetadata* p)
{
    SaraN2UDPPacketMetadata packet;

    size_t receiveSize = length / 2;

    receiveSize = min(receiveSize, _pendingUDPBytes[socketID]);

    return socketReceive(socketID, p ? p : &packet, buffer, receiveSize);
}

size_t Sodaq_R4X::socketSend(uint8_t socketID, const char* remoteIP, const uint16_t remotePort, const char* str)
{
    return socketSend(socketID, remoteIP, remotePort, (uint8_t *)str, strlen(str));
}

size_t Sodaq_R4X::socketSend(uint8_t socketID, const char* remoteIP, const uint16_t remotePort, const uint8_t* buffer, size_t size)
{
    if (size > SODAQ_MAX_UDP_SEND_MESSAGE_SIZE) {
        debugPrintLn("Message exceeded maximum size!");
        return 0;
    }

    print("AT+USOST=");
    print(socketID);
    print(",\"");
    print(remoteIP);
    print("\",");
    print(remotePort);
    print(',');
    print(size);
    print(",\"");

    for (size_t i = 0; i < size; ++i) {
        print(static_cast<char>(NIBBLE_TO_HEX_CHAR(HIGH_NIBBLE(buffer[i]))));
        print(static_cast<char>(NIBBLE_TO_HEX_CHAR(LOW_NIBBLE(buffer[i]))));
    }

    println('"');

    char outBuffer[64];

    if (readResponse(outBuffer, sizeof(outBuffer), "+USOST: ", SEND_TIMEOUT) != GSMResponseOK) {
        return 0;
    }

    int retSocketID;
    int sentLength;

    if ((sscanf(outBuffer, "%d,%d", &retSocketID, &sentLength) != 2) || (retSocketID < 0) || (retSocketID > SOCKET_COUNT)) {
        return 0;
    }

    return sentLength;
}

bool Sodaq_R4X::waitForUDPResponse(uint8_t socketID, uint32_t timeoutMS)
{
    if (hasPendingUDPBytes(socketID)) {
        return true;
    }

    uint32_t startTime = millis();

    while (!hasPendingUDPBytes(socketID) && (millis() - startTime) < timeoutMS) {
        print("AT+USORF=");
        print(socketID);
        println(",0");

        char buffer[128];

        if (readResponse(buffer, sizeof(buffer), "+USORF: ") == GSMResponseOK) {
            int retSocketID;
            int receiveSize;

            if (sscanf(buffer, "%d,%d", &retSocketID, &receiveSize) == 2) {
                _pendingUDPBytes[retSocketID] = receiveSize;
            }
        }

        sodaq_wdt_safe_delay(10);
    }

    return hasPendingUDPBytes(socketID);
}


/******************************************************************************
* MQTT
*****************************************************************************/

bool Sodaq_R4X::mqttLogin(uint32_t timeout)
{
    char buffer[16];

    _mqttLoginResult = -1;

    println("AT+UMQTTC=1");

    uint32_t startTime = millis();

    if ((readResponse(buffer, sizeof(buffer), "+UMQTTC: ", timeout) != GSMResponseOK) || !startsWith("1,1", buffer)) {
        return false;
    }

    // check URC synchronously
    while ((_mqttLoginResult == -1) && !is_timedout(startTime, timeout)) {
        mqttLoop();
    }

    return (_mqttLoginResult == 0);
}

bool Sodaq_R4X::mqttLogout()
{
    char buffer[16];

    println("AT+UMQTTC=0");

    return (readResponse(buffer, sizeof(buffer), "+UMQTTC: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("0,1", buffer);
}

void Sodaq_R4X::mqttLoop()
{
    if (!_modemStream->available()) {
        return;
    }

    int count = readLn(_inputBuffer, _inputBufferSize, 250); // 250ms, how many bytes at which baudrate?
    sodaq_wdt_reset();

    if (count <= 0) {
        return;
    }

    debugPrint("<< ");
    debugPrintLn(_inputBuffer);

    checkURC(_inputBuffer);
}

bool Sodaq_R4X::mqttPing(const char* server)
{
    char buffer[16];

    print("AT+UMQTTC=8,\"");
    print(server);
    println('"');

    return (readResponse(buffer, sizeof(buffer), "+UMQTTC: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("8,1", buffer);
}

bool Sodaq_R4X::mqttPublish(const char* topic, const uint8_t* msg, size_t size, uint8_t qos, uint8_t retain, bool useHEX)
{
    char buffer[16];

    print("AT+UMQTTC=2,");
    print(qos);
    print(',');
    print(retain);
    if (useHEX) {
        print(",1");
    }
    print(",\"");
    print(topic);
    print("\",\"");

    for (size_t i = 0; i < size; ++i) {
        if (useHEX) {
            print(static_cast<char>(NIBBLE_TO_HEX_CHAR(HIGH_NIBBLE(msg[i]))));
            print(static_cast<char>(NIBBLE_TO_HEX_CHAR(LOW_NIBBLE(msg[i]))));
        } else {
            print((char)msg[i]);
        }
    }

    println('"');

    return (readResponse(buffer, sizeof(buffer), "+UMQTTC: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("2,1", buffer);
}

// returns number of read messages
uint16_t Sodaq_R4X::mqttReadMessages(char* buffer, size_t size, uint32_t timeout)
{
    if (buffer == NULL || size == 0) {
        return 0;
    }

    char bufferIn[16];

    _mqttPendingMessages = -1;

    println("AT+UMQTTC=6");

    uint32_t startTime = millis();

    if ((readResponse(bufferIn, sizeof(bufferIn), "+UMQTTC: ", UMQTT_TIMEOUT) != GSMResponseOK) || !startsWith("6,1", bufferIn)) {
        return 0;
    }

    // check URC synchronously
    while ((_mqttPendingMessages == -1) && !is_timedout(startTime, timeout)) {
        mqttLoop();
    }

    if (_mqttPendingMessages <= 0) {
        return 0;
    }

    uint16_t messages = 0;
    uint16_t outSize  = 0;

    while (messages < _mqttPendingMessages && !is_timedout(startTime, timeout)) {
        int count = readLn(_inputBuffer, _inputBufferSize, 250);
        sodaq_wdt_reset();

        if (count <= 0) {
            continue;
        }

        bool b = startsWith("Msg:", _inputBuffer);
        if (b) { messages++; }

        if (b || startsWith("Topic:", _inputBuffer)) {
            if (outSize > 0 && outSize < size) {
                buffer[outSize++] = LF;
            }

            if (outSize >= size - 1) {
                break;
            }

            count -= b ? 4 : 6;

            if (outSize + (uint16_t)count > size - 1) {
                count = size - 1 - outSize;
            }

            memcpy(buffer + outSize, _inputBuffer + (b ? 4 : 6), count);
            outSize += count;
            buffer[outSize] = 0;
        }
    }

    _mqttPendingMessages = 0;

    return messages;
}

bool Sodaq_R4X::mqttSubscribe(const char* filter, uint8_t qos, uint32_t timeout)
{
    char buffer[16];

    _mqttSubscribeReason = -1;

    print("AT+UMQTTC=4,");
    print(qos);
    print(",\"");
    print(filter);
    println('"');

    uint32_t startTime = millis();

    if ((readResponse(buffer, sizeof(buffer), "+UMQTTC: ", UMQTT_TIMEOUT) != GSMResponseOK) || !startsWith("4,1", buffer)) {
        return false;
    }

    // check URC synchronously
    while ((_mqttSubscribeReason == -1) && !is_timedout(startTime, timeout)) {
        mqttLoop();
    }

    return (_mqttSubscribeReason == 1);
}

bool Sodaq_R4X::mqttUnsubscribe(const char* filter)
{
    char buffer[16];

    print("AT+UMQTTC=5,\"");
    print(filter);
    println('"');

    return (readResponse(buffer, sizeof(buffer), "+UMQTTC: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("5,1", buffer);
}

int8_t Sodaq_R4X::mqttGetLoginResult()
{
    return _mqttLoginResult;
}

int16_t Sodaq_R4X::mqttGetPendingMessages()
{
    return _mqttPendingMessages;
}

bool Sodaq_R4X::mqttSetAuth(const char* name, const char* pw)
{
    char buffer[16];

    print("AT+UMQTT=4,\"");
    print(name);
    print("\",\"");
    print(pw);
    println('"');

    return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("4,1", buffer);
}

bool Sodaq_R4X::mqttSetCleanSettion(bool enabled)
{
    char buffer[16];

    print("AT+UMQTT=12,");
    println(enabled ? '1' : '0');

    return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("12,1", buffer);
}

bool Sodaq_R4X::mqttSetClientId(const char* id)
{
    char buffer[16];

    print("AT+UMQTT=0,\"");
    print(id);
    println('"');

    return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("0,1", buffer);
}

bool Sodaq_R4X::mqttSetInactivityTimeout(uint16_t timeout)
{
    char buffer[16];

    print("AT+UMQTT=10,");
    println(timeout);

    return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("10,1", buffer);
}

bool Sodaq_R4X::mqttSetLocalPort(uint16_t port)
{
    char buffer[16];

    print("AT+UMQTT=1,");
    println(port);

    return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("1,1", buffer);
}

bool Sodaq_R4X::mqttSetSecureOption(bool enabled, int8_t profile)
{
    char buffer[16];

    print("AT+UMQTT=11,");
    print(enabled ? '1' : '0');

    if (profile >= 0) {
        print(',');
        println(profile);
    }

    return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("11,1", buffer);
}

bool Sodaq_R4X::mqttSetServer(const char* server, uint16_t port)
{
    char buffer[16];

    print("AT+UMQTT=2,\"");
    print(server);

    if (port > 0) {
        print("\",");
        println(port);
    } else {
        println('"');
    }

  return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("2,1", buffer);
}

bool Sodaq_R4X::mqttSetServerIP(const char* ip, uint16_t port)
{
    char buffer[16];

    print("AT+UMQTT=3,\"");
    print(ip);

    if (port > 0) {
        print("\",");
        println(port);
    } else {
        println('"');
    }

    return (readResponse(buffer, sizeof(buffer), "+UMQTT: ", UMQTT_TIMEOUT) == GSMResponseOK) && startsWith("3,1", buffer);
}


/******************************************************************************
* Private
*****************************************************************************/

int8_t Sodaq_R4X::checkApn(const char* requiredAPN)
{
    println("AT+CGDCONT?");

    char buffer[256];

    if (readResponse(buffer, sizeof(buffer), "+CGDCONT: ") != GSMResponseOK) {
        return false;
    }

    if (strncmp(buffer, "1,\"IP\"", 6) == 0 && strncmp(buffer + 6, ",\"\"", 3) != 0) {
        char apn[64];
        char ip[32];

        if (sscanf(buffer + 6, ",\"%[^\"]\",\"%[^\"]\",0,0,0,0", apn, ip) != 2) { return -1; }

        if (strlen(ip) >= 7 && strcmp(ip, "0.0.0.0") != 0) { return 1; }

        if (strcmp(apn, requiredAPN) == 0) { return 0; }
    }

    return setApn(requiredAPN) ? 0 : 1;
}

bool Sodaq_R4X::checkBandMask(const char* requiredURAT, const char* requiredBankMask)
{
    if (requiredURAT != NULL && strchr(requiredURAT, '8') == NULL) { // set BANDMASK for NB-Iot (URAT=8) only
        return true;
    }

    println("AT+UBANDMASK?");

    char buffer[128];

    if (readResponse(buffer, sizeof(buffer), "+UBANDMASK: ") != GSMResponseOK) {
        return false;
    }

    char bm0[32];
    char bm1[32];
    if (sscanf(buffer, "0,%[^,],1,%s", bm0, bm1) != 2) {
        return false;
    }

    if (strcmp(bm1, requiredBankMask) == 0) {
        return true;
    }

    print("AT+UBANDMASK=1,");
    println(requiredBankMask);

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_R4X::checkCFUN()
{
    println("AT+CFUN?");

    char buffer[64];

    if (readResponse(buffer, sizeof(buffer), "+CFUN: ") != GSMResponseOK) {
        return false;
    }

    return ((strcmp(buffer, "1") == 0) || setRadioActive(true));
}

bool Sodaq_R4X::checkCOPS()
{
    println("AT+COPS?");

    char buffer[64];

    if (readResponse(buffer, sizeof(buffer), "+COPS: ", COPS_TIMEOUT) != GSMResponseOK) {
        return false;
    }

    return ((strcmp(buffer, "0") == 0) || execCommand("AT+COPS=0", COPS_TIMEOUT));
}

bool Sodaq_R4X::checkUrat(const char* requiredURAT)
{
    println("AT+URAT?");

    char buffer[64];

    if (readResponse(buffer, sizeof(buffer), "+URAT: ") != GSMResponseOK || strlen(buffer) == 0) {
        return false;
    }

    if (strcmp(buffer, requiredURAT) == 0) {
        return true;
    }

    print("AT+URAT=");
    println(requiredURAT);

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_R4X::checkURC(char* buffer)
{
    if (buffer[0] != '+') {
        return false;
    }

    int param1, param2;
    char param3[1024];

    if (sscanf(buffer, "+UFOTAS: %d,%d", &param1, &param2) == 2) {
        #ifdef DEBUG
        debugPrint("Unsolicited: FOTA: ");
        debugPrint(param1);
        debugPrint(", ");
        debugPrintLn(param2);
        #endif

        return true;
    }

    if (sscanf(buffer, "+UUSORF: %d,%d", &param1, &param2) == 2) {
        debugPrint("Unsolicited: Socket ");
        debugPrint(param1);
        debugPrint(": ");
        debugPrintLn(param2);

        _pendingUDPBytes[param1] = param2;

        return true;
    }

    if (sscanf(buffer, "+UUMQTTC: 1,%d", &param1) == 1) {
        debugPrint("Unsolicited: MQTT login result: ");
        debugPrintLn(param1);

        _mqttLoginResult = param1;

        return true;
    }

    if (sscanf(buffer, "+UUMQTTC: 4,%d,%d,\"%[^\"]\"", &param1, &param2, param3) == 3) {
        debugPrint("Unsolicited: MQTT subscription result: ");
        debugPrint(param1);
        debugPrint(", ");
        debugPrint(param2);
        debugPrint(", ");
        debugPrintLn(param3);

       _mqttSubscribeReason = param1;

        return true;
    }

    if (sscanf(buffer, "+UUMQTTCM: 6,%d", &param1) == 1) {
        debugPrint("Unsolicited: MQTT pending messages:");
        debugPrintLn(param1);

        _mqttPendingMessages = param1;

        return true;
    }

    return false;
}

bool Sodaq_R4X::doSIMcheck()
{
    const uint8_t retry_count = 10;

    for (uint8_t i = 0; i < retry_count; i++) {
        if (i > 0) {
            sodaq_wdt_safe_delay(250);
        }

        SimStatuses simStatus = getSimStatus();
        if (simStatus == SimNeedsPin) {
            if (_pin == 0 || *_pin == '\0' || !setSimPin(_pin)) {
                debugPrintLn(DEBUG_STR_ERROR "SIM needs a PIN but none was provided, or setting it failed!");
                return false;
            }
        }
        else if (simStatus == SimReady) {
            return true;
        }
    }

    return false;
}

/**
 * 1. check echo
 * 2. check ok
 * 3. check error
 * 4. if response prefix is not empty, check response prefix, append if multiline
 * 5. check URC, if handled => continue
 * 6. if response prefis is empty, return the whole line return line buffer, append if multiline
*/
GSMResponseTypes Sodaq_R4X::readResponse(char* outBuffer, size_t outMaxSize, const char* prefix, uint32_t timeout)
{
    bool usePrefix    = prefix != NULL && prefix[0] != 0;
    bool useOutBuffer = outBuffer != NULL && outMaxSize > 0;

    uint32_t from = NOW;

    size_t outSize = 0;

    if (outBuffer) {
        outBuffer[0] = 0;
    }

    while (!is_timedout(from, timeout)) {
        int count = readLn(_inputBuffer, _inputBufferSize, 250); // 250ms, how many bytes at which baudrate?
        sodaq_wdt_reset();

        if (count <= 0) {
            continue;
        }

        debugPrint("<< ");
        debugPrintLn(_inputBuffer);

        if (startsWith(STR_AT, _inputBuffer)) {
            continue; // skip echoed back command
        }

        if (startsWith(STR_RESPONSE_OK, _inputBuffer)) {
            return GSMResponseOK;
        }

        if (startsWith(STR_RESPONSE_ERROR, _inputBuffer) ||
                startsWith(STR_RESPONSE_CME_ERROR, _inputBuffer) ||
                startsWith(STR_RESPONSE_CMS_ERROR, _inputBuffer)) {
            return GSMResponseError;
        }

        bool hasPrefix = usePrefix && useOutBuffer && startsWith(prefix, _inputBuffer);

        if (!hasPrefix && checkURC(_inputBuffer)) {
            continue;
        }

        if (hasPrefix || (!usePrefix && useOutBuffer)) {
            if (outSize > 0 && outSize < outMaxSize - 1) {
                outBuffer[outSize++] = LF;
            }

            if (outSize < outMaxSize - 1) {
                char* inBuffer = _inputBuffer;
                if (hasPrefix) {
                    int i = strlen(prefix);
                    count -= i;
                    inBuffer += i;
                }
                if (outSize + count > outMaxSize - 1) {
                    count = outMaxSize - 1 - outSize;
                }
                memcpy(outBuffer + outSize, inBuffer, count);
                outSize += count;
                outBuffer[outSize] = 0;
            }
        }
    }

    debugPrintLn("<< timed out");

    return GSMResponseTimeout;
}

void Sodaq_R4X::reboot()
{
    println("AT+CFUN=15");

    // wait up to 2000ms for the modem to come up
    uint32_t start = millis();

    while ((readResponse() != GSMResponseOK) && !is_timedout(start, 2000)) {}
}

bool Sodaq_R4X::setSimPin(const char* simPin)
{
    print("AT+CPIN=\"");
    print(simPin);
    println('"');

    return (readResponse() == GSMResponseOK);
}

size_t Sodaq_R4X::socketReceive(uint8_t socketID, SaraN2UDPPacketMetadata* packet, char* buffer, size_t size)
{
    if (!hasPendingUDPBytes(socketID)) {
        // no URC has happened, no socket to read
        debugPrintLn("Reading from without available bytes!");
        return 0;
    }

    print("AT+USORF=");
    print(socketID);
    print(',');
    println(min(size, _pendingUDPBytes[socketID]));

    char outBuffer[1024];

    if (readResponse(outBuffer, sizeof(outBuffer), "+USORF: ") != GSMResponseOK) {
        return 0;
    }

    int retSocketID;

    if (sscanf(outBuffer, "%d,\"%[^\"]\",%d,%d,\"%[^\"]\"", &retSocketID, packet->ip, &packet->port,
               &packet->length, buffer) != 5) {
        return 0;
    }

    if ((retSocketID < 0) || (retSocketID > SOCKET_COUNT)) {
        return 0;
    }

    _pendingUDPBytes[socketID] -= packet->length;

    return packet->length;
}

bool Sodaq_R4X::waitForSignalQuality(uint32_t timeout)
{
    uint32_t start = millis();
    const int8_t minRSSI = getMinRSSI();
    int8_t rssi;
    uint8_t ber;

    uint32_t delay_count = 500;

    while (!is_timedout(start, timeout)) {
        if (getRSSIAndBER(&rssi, &ber)) {
            if (rssi != 0 && rssi >= minRSSI) {
                _lastRSSI = rssi;
                _CSQtime = (int32_t)(millis() - start) / 1000;
                return true;
            }
        }

        sodaq_wdt_safe_delay(delay_count);

        // Next time wait a little longer, but not longer than 5 seconds
        if (delay_count < 5000) {
            delay_count += 1000;
        }
    }

    return false;
}


/******************************************************************************
* Utils
*****************************************************************************/

uint32_t Sodaq_R4X::convertDatetimeToEpoch(int y, int m, int d, int h, int min, int sec)
{
    struct tm tm;

    tm.tm_isdst = -1;
    tm.tm_yday  = 0;
    tm.tm_wday  = 0;
    tm.tm_year  = y + EPOCH_TIME_YEAR_OFF;
    tm.tm_mon   = m - 1;
    tm.tm_mday  = d;
    tm.tm_hour  = h;
    tm.tm_min   = min;
    tm.tm_sec   = sec;

    return mktime(&tm);
}

bool Sodaq_R4X::startsWith(const char* pre, const char* str)
{
    return (strncmp(pre, str, strlen(pre)) == 0);
}


/******************************************************************************
* Generic
*****************************************************************************/

// Returns true if the modem is on.
bool Sodaq_R4X::isOn() const
{
    if (_onoff) {
        return _onoff->isOn();
    }

    // No onoff. Let's assume it is on.
    return true;
}

void Sodaq_R4X::writeProlog()
{
    if (!_appendCommand) {
        debugPrint(">> ");
        _appendCommand = true;
    }
}

// Write a byte, as binary data
size_t Sodaq_R4X::writeByte(uint8_t value)
{
    return _modemStream->write(value);
}

size_t Sodaq_R4X::print(const String& buffer)
{
    writeProlog();
    debugPrint(buffer);

    return _modemStream->print(buffer);
}

size_t Sodaq_R4X::print(const char buffer[])
{
    writeProlog();
    debugPrint(buffer);

    return _modemStream->print(buffer);
}

size_t Sodaq_R4X::print(char value)
{
    writeProlog();
    debugPrint(value);

    return _modemStream->print(value);
};

size_t Sodaq_R4X::print(unsigned char value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_R4X::print(int value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_R4X::print(unsigned int value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_R4X::print(long value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_R4X::print(unsigned long value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_R4X::println(const __FlashStringHelper *ifsh)
{
    return print(ifsh) + println();
}

size_t Sodaq_R4X::println(const String &s)
{
    return print(s) + println();
}

size_t Sodaq_R4X::println(const char c[])
{
    return print(c) + println();
}

size_t Sodaq_R4X::println(char c)
{
    return print(c) + println();
}

size_t Sodaq_R4X::println(unsigned char b, int base)
{
    return print(b, base) + println();
}

size_t Sodaq_R4X::println(int num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_R4X::println(unsigned int num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_R4X::println(long num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_R4X::println(unsigned long num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_R4X::println(double num, int digits)
{
    writeProlog();
    debugPrint(num, digits);

    return _modemStream->println(num, digits);
}

size_t Sodaq_R4X::println(const Printable& x)
{
    return print(x) + println();
}

size_t Sodaq_R4X::println()
{
    debugPrintLn();
    size_t i = print(CR);
    _appendCommand = false;
    return i;
}

// Initializes the input buffer and makes sure it is only initialized once.
// Safe to call multiple times.
void Sodaq_R4X::initBuffer()
{
    debugPrintLn("[initBuffer]");

    // make sure the buffers are only initialized once
    if (!_isBufferInitialized) {
        _inputBuffer = static_cast<char*>(malloc(_inputBufferSize));
        _isBufferInitialized = true;
    }
}

// Sets the modem stream.
void Sodaq_R4X::setModemStream(Stream& stream)
{
    _modemStream = &stream;
}

// Returns a character from the modem stream if read within _timeout ms or -1 otherwise.
int Sodaq_R4X::timedRead(uint32_t timeout) const
{
    uint32_t _startMillis = millis();

    do {
        int c = _modemStream->read();

        if (c >= 0) {
            return c;
        }
    } while (millis() - _startMillis < timeout);

    return -1; // -1 indicates timeout
}

// Fills the given "buffer" with characters read from the modem stream up to "length"
// maximum characters and until the "terminator" character is found or a character read
// times out (whichever happens first).
// The buffer does not contain the "terminator" character or a null terminator explicitly.
// Returns the number of characters written to the buffer, not including null terminator.
size_t Sodaq_R4X::readBytesUntil(char terminator, char* buffer, size_t length, uint32_t timeout)
{
    if (length < 1) {
        return 0;
    }

    size_t index = 0;

    while (index < length) {
        int c = timedRead(timeout);

        if (c < 0 || c == terminator) {
            break;
        }

        *buffer++ = static_cast<char>(c);
        index++;
    }
    if (index < length) {
        *buffer = '\0';
    }

    return index; // return number of characters, not including null terminator
}

// Fills the given "buffer" with up to "length" characters read from the modem stream.
// It stops when a character read times out or "length" characters have been read.
// Returns the number of characters written to the buffer.
size_t Sodaq_R4X::readBytes(uint8_t* buffer, size_t length, uint32_t timeout)
{
    size_t count = 0;

    while (count < length) {
        int c = timedRead(timeout);

        if (c < 0) {
            break;
        }

        *buffer++ = static_cast<uint8_t>(c);
        count++;
    }

    return count;
}

// Reads a line (up to the SODAQ_GSM_TERMINATOR) from the modem stream into the "buffer".
// The buffer is terminated with null.
// Returns the number of bytes read, not including the null terminator.
size_t Sodaq_R4X::readLn(char* buffer, size_t size, uint32_t timeout)
{
    // Use size-1 to leave room for a string terminator
    size_t len = readBytesUntil(SODAQ_GSM_TERMINATOR[SODAQ_GSM_TERMINATOR_LEN - 1], buffer, size - 1, timeout);

    // check if the terminator is more than 1 characters, then check if the first character of it exists
    // in the calculated position and terminate the string there
    if ((SODAQ_GSM_TERMINATOR_LEN > 1) && (buffer[len - (SODAQ_GSM_TERMINATOR_LEN - 1)] == SODAQ_GSM_TERMINATOR[0])) {
        len -= SODAQ_GSM_TERMINATOR_LEN - 1;
    }

    // terminate string, there should always be room for it (see size-1 above)
    buffer[len] = '\0';

    return len;
}


/******************************************************************************
 * OnOff
 *****************************************************************************/

Sodaq_SARA_R4XX_OnOff::Sodaq_SARA_R4XX_OnOff()
{
    // First write the output value, and only then set the output mode.
    digitalWrite(SARA_ENABLE, LOW);
    pinMode(SARA_ENABLE, OUTPUT);

    digitalWrite(SARA_TX_ENABLE, LOW);
    pinMode(SARA_TX_ENABLE, OUTPUT);

    _onoff_status = false;
}

void Sodaq_SARA_R4XX_OnOff::on()
{
    digitalWrite(SARA_ENABLE, HIGH);
    digitalWrite(SARA_TX_ENABLE, HIGH);

    pinMode(SARA_R4XX_TOGGLE, OUTPUT);
    digitalWrite(SARA_R4XX_TOGGLE, LOW);
    sodaq_wdt_safe_delay(2000);
    pinMode(SARA_R4XX_TOGGLE, INPUT);

    _onoff_status = true;
}

void Sodaq_SARA_R4XX_OnOff::off()
{
    digitalWrite(SARA_ENABLE, LOW);
    digitalWrite(SARA_TX_ENABLE, LOW);

    // Should be instant
    // Let's wait a little, but not too long
    delay(50);
    _onoff_status = false;
}

bool Sodaq_SARA_R4XX_OnOff::isOn()
{
    return _onoff_status;
}
