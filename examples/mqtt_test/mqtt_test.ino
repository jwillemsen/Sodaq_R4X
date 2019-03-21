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

#define CONSOLE_STREAM   SerialUSB
#define MODEM_STREAM     Serial1

#define CURRENT_APN      "data.mono"
#define CURRENT_URAT     "7"
#define MQTT_SERVER_NAME "test.mosquitto.org"
#define MQTT_SERVER_PORT 1883

static Sodaq_R4X r4x;
static Sodaq_SARA_R4XX_OnOff saraR4xxOnOff;
static bool isReady;

void setup()
{
    while (!CONSOLE_STREAM);
    CONSOLE_STREAM.println("Booting up...");

    MODEM_STREAM.begin(r4x.getDefaultBaudrate());
    r4x.setDiag(CONSOLE_STREAM);
    r4x.init(&saraR4xxOnOff, MODEM_STREAM);

    isReady = r4x.connect(CURRENT_APN, CURRENT_URAT);
    CONSOLE_STREAM.println(isReady ? "Network connected" : "Network connection failed");

    // r4x.execCommand("ATI");
    // r4x.execCommand("ATI9");
    // r4x.execCommand("AT+CPSMS?");
    // r4x.execCommand("AT+UPSV?");

    if (isReady) {
        isReady = r4x.mqttSetServer(MQTT_SERVER_NAME, MQTT_SERVER_PORT) && r4x.mqttLogin();
        CONSOLE_STREAM.println(isReady ? "MQTT connected" : "MQTT failed");
    }

    if (isReady) {
        uint8_t buf0[] = {'t', 'e', 's', 't', '0'};
        uint8_t buf1[] = {'t', 'e', 's', 't', '1'};

        r4x.mqttPublish("/home/test/test0", buf0, sizeof(buf0));
        r4x.mqttPublish("/home/test/test1", buf1, sizeof(buf1), 0, 0, 1);
        r4x.mqttSubscribe("/home/test/#");
    }

    CONSOLE_STREAM.println("Setup done");
}

void loop()
{
    if (CONSOLE_STREAM.available()) {
        int i = CONSOLE_STREAM.read();
        CONSOLE_STREAM.write(i);
        MODEM_STREAM.write(i);
    }

    // if (MODEM_STREAM.available()) {
    //     CONSOLE_STREAM.write(MODEM_STREAM.read());
    // }

    if (isReady) {
        r4x.mqttLoop();

        if (r4x.mqttGetPendingMessages() > 0) {
            char buffer[2048];
            uint16_t i = r4x.mqttReadMessages(buffer, sizeof(buffer));

            CONSOLE_STREAM.print("Read messages:");
            CONSOLE_STREAM.println(i);
            CONSOLE_STREAM.println(buffer);
        }
    }
}