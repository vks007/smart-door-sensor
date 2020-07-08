/*
 *secrets.h - file to store secret strings like WiFi credentials ets
 * Include this file into your programs and refer the variables here to pick up thier values
 * This file should not be checked into git. Advantage of this file is that you dont have to check in your secret info on git
*/

#ifndef SECRETS_H
#define SECRETS_H

#define SSID1 "YOUR SSID"
#define SSID1_PSWD "YOUR PSWD"
#define SSID2 "YOUR SSID2"
#define SSID2_PSWD "YOUR PSWD2"
#define OTA_PSWD "API PSWD"
#define MQTT_SERVER1 IPAddress(192,XXX,X,XX)
#define MQTT_PORT1 1883
#define MQTT_USER1 "MQTT USR"
#define MQTT_PSWD1 "MQTT PSWD"
#define GATEWAY1 IPAddress(192,168,1,1)
#define SUBNET1 IPAddress(255,255,255,0)

#endif