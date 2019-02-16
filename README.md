ESPHome Tuya PIR Sensor Support
===============================

Work-in-progress support for WiFi PIR sensors built on the Tuya platform. They are badged as 'Neo Coolcam PIR' and others.

Overview
--------

These devices embed an ESP8266 wifi processor paired with a Silicon Labs [EMF8 'Sleepy Bee](https://www.silabs.com/products/mcu/8-bit/efm8-sleepy-bee) ultra-low-power coprocessor. The SB1 and ESP8266 communicate via serial UART at 9600 bps. The ESB1 controls the LEDs, PIR sensor, and gates power to the ESP8266 when it is not needed.

These devices have been documented a few places; this is the first attempt that I am aware of to document the protocol and provide alternate firmware.
* https://github.com/esphome/esphome/issues/306
* https://community.home-assistant.io/t/coolcam-wifi-motion-sensor-pir/54783

Excuses
-------

I am a terrible C/C++ coder. I usually stick to Python, Perl, C#, Java, etc. I am 100% sure that my code does a bunch of really dumb and/or inefficient things. For example, I could probably replace the serial buffer with [istringstream](http://www.cplusplus.com/reference/sstream/istringstream/). Expectations about message ordering and timing may differ from the stock firmware, but it should be close enough to get the job done.

Serial Protocol Overview
------------------------

These are the message I have seen the two processors exchange during limited bench testing. There may be more. Other than the `0x55 0xAA` header (which are both inverted alternating 0/1 patterns on the wire to assist with baud detection), I have not been able to reverse engineer any particular encoding.

Handshake
---------

The first message is always a product ID request from the ESP to the SB1.

The `0x00 0x01` prefix is used by both the request and response.

**Product ID Handshake:**
```
ESP8266 -> SB1: 55 AA 00 01 00 00 00
SB1 -> ESP8266: 55 AA 00 01 00 ${"P":"Okurono2XLVRV0fB","v":"1.1.0"} 19
```

Configuration Status
--------------------

After handshaking, the sequence may go to one of three different paths as the ESP8266 is configured by the smartphone app and connnects to the cloud service.

The `0x00 0x02` prefix is used by all status messages and responses.

**WiFi SmartConfig AP (Fast Blink) Mode:**
```
ESP8266 -> SB1: 55 AA 00 02 00 01 00 02
SB1 -> ESP8266: 55 AA 00 02 00 00 01
```

**Wifi SmartConfig STA (Slow Blink) Mode:**
```
ESP8266 -> SB1: 55 AA 00 02 00 01 01 03
SB1 -> ESP8266: 55 AA 00 02 00 00 01
```

**Normal (Configured) Mode:**
```
# WiFi Connected
ESP8266 -> SB1: 55 AA 00 02 00 01 02 04
SB1 -> ESP8266: 55 AA 00 02 00 00 01

# IP Address Received from DHCP
ESP8266 -> SB1: 55 AA 00 02 00 01 03 05
SB1 -> ESP8266: 55 AA 00 02 00 00 01

# Connected to MQTT
ESP8266 -> SB1: 55 AA 00 02 00 01 04 06
SB1 -> ESP8266: 55 AA 00 02 00 00 01
```

In Configured mode, if no motion event has been detected, the ESP8266 will be powered down approximately 3.5 seconds after the final status message is ackd by the SB1. In either SmartConfig mode, the ESP8266 will be powered down after aproximately 110 seconds.

Motion events will only be fired if the device has gone to sleep after being successfully configured. If the SB1 does not see a successful configuration sequence, the ESP8266 will stay asleep until the reset button is held to re-initiate autoconfiguration. I'm not sure what happens if WiFI or internet access is unavailable following successful configuration; this has yet to be tested.

Motion Event
-----------

If the ESP8266 was powered up due to a motion event, the ack to the 'Connected to MQTT' message will be immediately followed by a motion detection message. The payload may vary; I haven't traced this extensively. There may be battery level information in here, or maybe the ESP reads it from the analog input. I'm not sure.

The ESP8266 will be powered down immediately after acking the motion detection message. The message should NOT be ackd until whatever notification you're going to send has been successfully transmitted. 

Motion detection has a 60 second cooldown after firing.

The `0x00 0x05` prefix is used by all status messages and responses.

```
SB1 -> ESP8266: 55 AA 00 05 00 05 65 01 00 01 00 70
ESP8266 -> SB1: 55 AA 00 05 00 01 00 05
```

Configuration Reset
-----

The SB1 may send a reset command to the ESP8266 at any time. This is triggered by holding down the button inside the device as described in the user manual. The Tuya firmware responds by removing all WiFi and Tuya configuration, and rebooting into SmartConfig mode. Repeated messages are used to toggle the device between STA and AP mode for SmartConfig. The SB1 does not seem to care if you ack this or not; it expects the ESP8266 to reboot anyway.

The `0x00 0x03` prefix is used by all status messages and responses.

```
SB1 -> ESP8266: 55 AA 00 03 00 00 02
ESP8266 -> SB1: 55 AA 00 03 00 00 02
```
