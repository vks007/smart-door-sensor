
#ifndef DOOR_CONFIG_H
#define DOOR_CONFIG_H

#if defined(MAIN_DOOR)
  #define DEVICE_NAME             "main_door"
  #define MQTT_TOPIC              "home/main_door"
  #define ESP_IP_ADDRESS          IPAddress(192,168,1,50)

#elif defined(TERRACE_DOOR)
  #define DEVICE_NAME             "terrace_door"
  #define MQTT_TOPIC              "home/terrace_door"
  #define ESP_IP_ADDRESS          IPAddress(192,168,1,51)

#else
  #error "Door type not selected, see Door_config.h"
#endif

#endif
