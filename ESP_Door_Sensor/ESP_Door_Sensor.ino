/*
 * NOTE IMPORTANT : BUILD SETTINGS : File Size: 512K with 32K SPIFFS for ESP01 or choose similar size for ESP12
 * 1.4.1 - removed config of individual topics , rather only take main topic and hard code subsequent paths. Also included more messages to be logged on MQ Broker , like TESTING mode etc
 * 1.4.0 - makes user configuration configurable via a config file (as in Version 1.0)
 * 1.3 - Implements receiving message type from ATTiny on Rx/Tx pins via a 2 bit code
 * Works with ATTiny door sensor V 1.3 onwards
 * 
 * TO DO List
 * Remove usage of FS/SPIFFS in this, take values form secrets file and build a binary specific for each module as is the case with ESPHome
 * Introduce AsyncMQTTclient library to publish messages with QoS 1 , you get back an ack id which confirms the message was published , if not , you get back 0
 * If you get back 0 , you have to publish the message again (not sure if you mark the message try as duplicate though
 * Mark the messages & topics as retained (flag passed during publish)
 * Remove the usage of WiFiManager - i think this is not needed at least for my case , why complicate things as if ESP ever gets stuck in a loop without Wifi, it will drain the battery
 * Solve the bug where I dont see the topic on MQTT in spite the fact that this program publishes it, maybe this will go away once I switch to AsyncMQTT. This bug is causing an issue as
 * the battery status goes to unknown after some time while I would want it to stay at the last known value - i think this might get solved by retained flag
 * Introduce a status LED on GPIO2 to blink thrice and show that a message publish was successful. This can be done as CONFIG functionality is not needed. Even if you need it, you can set
 * the GPIO2 as INPUT on startup (as you do now) and then change it to OUTPUT 
 * 
*/
#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <WiFiManager.h>          //For managing WiFi & Config values , https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>          //For working with json config files on FS, https://github.com/bblanchon/ArduinoJson
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

//#define TESTING_MODE //used to prevent using Rx & Tx as input pins , rather use them as normal serial pins for debugging , comment this out during normal operation
//#define DEBUG //BEAWARE that this statement should be before #include <DebugMacros.h> else the macros wont work as they are based on this #define
#include "Debugutils.h" //This file is located in the Sketches\libraries\DebugUtils folder

//Types of messages decoded via the signal pins
#define SENSOR_NONE 0
#define SENSOR_WAKEUP 1
#define SENSOR_OPEN 2
#define SENSOR_CLOSED 3
#define MAX_MQTT_CONNECT_RETRY 4 //max no of retries to connect to MQTT server

//Hold pin will hold CH_PD HIGH till we're executing the setup, the last step would be set it LOW which will power down the ESP
#define HOLD_PIN 0  // defines GPIO0 as the hold pin (will hold CH_PD high untill we power down).
#define SIGNAL_PIN0 1 //Bit 1 of the signal which indicates the message type
#define SIGNAL_PIN1 3 //Bit 2 of the signal which indicates the message type
//State Mapping of SIGNAL_PIN0 SIGNAL_PIN1:: 11=>IDLE , 00=> SENSOR_WAKEUP , 01=> SENSOR OPEN , 10=> SENSOR CLOSED
#define CONFIG_PIN 2 //This is used to put the ESP into AP config mode, normally HIGH , LOW when in Config mode
#define MSG_ON "on" //payload for ON
#define MSG_OFF "off"//payload for OFF
#define AP_PASSWORD "password" //default password to connect to AP

ADC_MODE(ADC_VCC);//connects the internal ADC to VCC pin and enables measuring Vcc

char VERSION[] = "1.4.3"; 
//User configuration section
char mqtt_server[16] = "";//IP address of the MQTT server
const short mqtt_port = 1883;
char mqtt_user[20] = "";//username to connect to MQTT server
char mqtt_pswd[20] = "";//password to connect to MQTT server
char mqtt_client_name[20] = "_sensor";//Main_Door_sensor // Client connections cant have the same connection name
char mqtt_topic[50] = ""; //eg:"home/main_door";
char ip_address[16] = ""; //static IP to be assigned to the chip eg 192.168.1.60
//User configuration section

WiFiClient espClient;
PubSubClient client(espClient);
bool shouldSaveConfig = false; //flag for saving data


void setup() 
{
  DBEGIN(115200);
  pinMode(HOLD_PIN, OUTPUT);
  digitalWrite(HOLD_PIN, HIGH);  // sets GPIO0 to high (this holds CH_PD high even if the input signal goes LOW)

  short CURR_MSG = SENSOR_NONE; //This stores the message type deciphered from the states of the signal pins
  short PREV_MSG = SENSOR_NONE; //This stores the message type deciphered from the states of the signal pins

  #ifndef TESTING_MODE
  if(SIGNAL_PIN0 == 1 || SIGNAL_PIN0 == 3)
    pinMode(SIGNAL_PIN0, FUNCTION_3);//Because we're using Rx & Tx as inputs here, we have to set the input type
  if(SIGNAL_PIN1 == 1 || SIGNAL_PIN1 == 3)
    pinMode(SIGNAL_PIN1, FUNCTION_3);//Because we're using Rx & Tx as inputs here, we have to set the input type     
  pinMode(SIGNAL_PIN0, INPUT_PULLUP);     
  pinMode(SIGNAL_PIN1, INPUT_PULLUP);
  #endif
  
  pinMode(CONFIG_PIN,INPUT_PULLUP);
  DPRINTLN("");
  DPRINTLN("Version:" + String(VERSION));
  if(readConfig() && WiFi.SSID()!="")//Only proceed if you have a valid SSID and are able to read config values from the FS config file config.json
  {
    DPRINTLN("Going to setup wifi");
    WiFi.printDiag(Serial); //Remove this line if you do not want to see WiFi password printed
    setupWiFi();
  }
  
  bool startAP = false;
  if((WiFi.status() == WL_CONNECTED))
  {
    DPRINT("WiFi connected, IP Address:");
    DPRINTLN(WiFi.localIP());
    
    client.setServer(mqtt_server, mqtt_port);
    //TO DO : you can read the input values in a single statement directly from registers and then compare using a mask
    // TO DO : Shift the reading of pins to before reading config so that even if ATTiny removes the signal, ESP can still take its own time in publishing the message

    //Read the type of message we've got from the ATiny
    #ifdef TESTING_MODE
      CURR_MSG = SENSOR_WAKEUP;
    #else
    if((digitalRead(SIGNAL_PIN0) == LOW) && (digitalRead(SIGNAL_PIN1) == LOW))
      CURR_MSG = SENSOR_WAKEUP;
    else if((digitalRead(SIGNAL_PIN0) == LOW) && (digitalRead(SIGNAL_PIN1) == HIGH))
      CURR_MSG = SENSOR_OPEN;
    else if((digitalRead(SIGNAL_PIN0) == HIGH) && (digitalRead(SIGNAL_PIN1) == LOW))
      CURR_MSG = SENSOR_CLOSED;
    //else nothing to do, invalid mode
    #endif

    while(CURR_MSG != PREV_MSG)
    {
      if(CURR_MSG != SENSOR_NONE)
      {
        publishMessage(CURR_MSG);
        //Allow a delay to let MQTT publish the message as the publish method is asyncronous, If I dont put this, the ESP powers down before the msg is published
        delay(1000);
      }
      
      //Read the sensor again to see if it has changed from last time, if yes then repeat the loop to publish this message
      PREV_MSG = CURR_MSG;
      if((digitalRead(SIGNAL_PIN0) == LOW) && (digitalRead(SIGNAL_PIN1) == LOW))
        CURR_MSG = SENSOR_WAKEUP;
      else if((digitalRead(SIGNAL_PIN0) == LOW) && (digitalRead(SIGNAL_PIN1) == HIGH))
        CURR_MSG = SENSOR_OPEN;
      else if((digitalRead(SIGNAL_PIN0) == HIGH) && (digitalRead(SIGNAL_PIN1) == LOW))
        CURR_MSG = SENSOR_CLOSED;
      //else nothing to do, invalid mode
    }
  }
  else
  {
    DPRINTLN("WiFi credentials/Config not found. Waiting for trigger to run the config utility....");
    for(char i=0;i<100;i++)//wait for a total of 10 secs
    {
      if(digitalRead(CONFIG_PIN) == LOW) //If config pin is LOW then it means we have to load the AP Portal
      {
        startAP = true;
        break;
      }
      delay(100);
    }
    DPRINTLN("Trigger not received for config utility, exiting....");
  }

  if((digitalRead(CONFIG_PIN) == LOW) || startAP) //If config pin is LOW then it means we have to load the AP Portal
  {
    //The AP portal is started only if we dont find SSID OR config params OR explicitly by the user by putting CONFIG pin LOW
    startupAP();
    DPRINTLN("Going to setup wifi after AP setup");
    setupWiFi();
  }
  
  DPRINTLN("powering down");
  digitalWrite(HOLD_PIN, LOW);  // set GPIO 0 low this takes CH_PD & powers down the ESP
}

void setupWiFi() 
{
  //Set the IP address (This results in faster connection to WiFi ~ 3sec
  short ip[4];
  char * item = strtok(ip_address, ".");
  char index = 0;
  while (item != NULL) {
    ip[index++] = atoi(item);
    item = strtok(NULL, ".");
  }
  IPAddress esp_ip(ip[0], ip[1], ip[2], ip[3]);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.config(esp_ip, gateway, subnet);
  WiFi.hostname(mqtt_client_name);// Set Hostname.
  DPRINTLN(".....");
  
  WiFi.mode(WIFI_STA); // Force to station mode because if device was switched off while in access point mode it will start up next time in access point mode.
  WiFi.begin();
  for(short i=0;i<3000;i++) //break after 30 sec 3000*10 msec
  {
    if(WiFi.status() != WL_CONNECTED)
      delay(10);//Dont increase this delay, I set it to 500 and it takes a very long time to connect, I think this blocks the execution
    else
    {
      DPRINT("WiFi connected, IP Address:");
      DPRINTLN(WiFi.localIP());
      break;
    }
  }
}

void publishMessage(short msg_type) {
  // Loop until we're reconnected
  char i = 0;
  while (!client.connected()) 
  {
    i++;
    DPRINT("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(mqtt_client_name,mqtt_user,mqtt_pswd)) {
      DPRINTLN("connected");
      char publish_topic[65] = ""; //variable accomodates 50 characters of main topic + 15 char of sub topic
      strcpy(publish_topic,mqtt_topic);
      strcat(publish_topic,"/status"); 
      if(msg_type == SENSOR_OPEN)
        client.publish(publish_topic, MSG_ON);
      else if(msg_type == SENSOR_CLOSED)
        client.publish(publish_topic, MSG_OFF);

      strcpy(publish_topic,mqtt_topic);
      strcat(publish_topic,"/availability"); 
      client.publish(publish_topic, "online");

      //measure batery voltage and publish that too
      int battery_Voltage = ESP.getVcc();
      char batt_volt[6];
      itoa(battery_Voltage, batt_volt, 10);

      short testing_mode = 0;
      #ifdef TESTING_MODE
        testing_mode = 1;
      #endif

      //I follow google style naming convention for json which is camelCase
      String state_json = String("{\"upTime\":") + millis() + String(",\"vcc\":") + batt_volt + String(",\"version\":\"") + VERSION + String("\",\"testingMode\":") + testing_mode + \
      String(",\"IPAddr\":\"") + WiFi.localIP().toString() + String("\"}");
      strcpy(publish_topic,mqtt_topic);
      strcat(publish_topic,"/state"); 
      client.publish(publish_topic, state_json.c_str());
      DPRINTLN("published messages");
    } 
    else 
    {
      DPRINT("failed, rc=");
      DPRINT(client.state());
      DPRINTLN(" try again in 1 second");
      delay(1000);
    }
    if(i >= MAX_MQTT_CONNECT_RETRY)
      break;
  }
}


void startupAP()
{
  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name, placeholder/prompt, default, length
  WiFiManagerParameter custom_mqtt_server("mqtt_server", "MQTT Server", mqtt_server, 16);
  WiFiManagerParameter custom_mqtt_user("mqtt_user", "MQTT user", mqtt_user, 20);
  WiFiManagerParameter custom_mqtt_pswd("mqtt_pswd", "MQTT password", mqtt_pswd, 20);
  WiFiManagerParameter custom_mqtt_client_name("mqtt_client_name", "MQTT Client Name", mqtt_client_name, 20);
  WiFiManagerParameter custom_mqtt_topic("mqtt_topic", "MQTT Main Topic", mqtt_topic, 50);
  WiFiManagerParameter custom_ip_address("ip_address", "Chip Static IP Address", ip_address, 16);

  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pswd);
  wifiManager.addParameter(&custom_mqtt_client_name);
  wifiManager.addParameter(&custom_mqtt_topic);
  wifiManager.addParameter(&custom_ip_address);

  //reset settings - for testing
  //wifiManager.resetSettings();
  //ESP.eraseConfig();

  //set minimu quality of signal so it ignores AP's under that quality
  //defaults to 8%
  //wifiManager.setMinimumSignalQuality();
  
  //sets timeout until configuration portal gets turned off useful to make it all retry or go to sleep in seconds
  wifiManager.setTimeout(300);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  String ssid = "ESP" + String(WiFi.macAddress());
  ssid.replace(":","");
  bool result = false;
  DPRINTLN("On Demand portal");
  result = wifiManager.startConfigPortal(ssid.c_str(), AP_PASSWORD);
  
  if (!result) {
    DPRINTLN("failed to connect on demand portal and hit timeout");
    return;
  }

  //read updated parameters
  strcpy(mqtt_server,custom_mqtt_server.getValue());
  strcpy(mqtt_user,custom_mqtt_user.getValue());
  strcpy(mqtt_pswd,custom_mqtt_pswd.getValue());
  strcpy(mqtt_client_name,custom_mqtt_client_name.getValue());
  strcpy(mqtt_topic,custom_mqtt_topic.getValue());
  strcpy(ip_address,custom_ip_address.getValue());


  //save the custom parameters to FS
  if (shouldSaveConfig) {
    DPRINTLN("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_user"] = mqtt_user;
    json["mqtt_pswd"] = mqtt_pswd;
    json["mqtt_client_name"] = mqtt_client_name;
    json["mqtt_topic"] = mqtt_topic;
    json["ip_address"] = ip_address;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      DPRINTLN("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }
}



/*
 * Reads the ocnfig values from the FS.
*/
bool readConfig()
{
  DPRINTLN("mounting FS...");
  if (SPIFFS.begin()) {
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);DPRINTLN("");
        if (json.success()) 
        {
          strcpy(mqtt_server,json["mqtt_server"]);
          strcpy(mqtt_user,json["mqtt_user"]);
          strcpy(mqtt_pswd,json["mqtt_pswd"]);
          strcpy(mqtt_client_name,json["mqtt_client_name"]);
          strcpy(mqtt_topic,json["mqtt_topic"]);
          strcpy(ip_address,json["ip_address"]);
          
          DPRINTLN(ip_address);
          //All these values are mandatory for the program to proceed, if any of them is blank , return failure
          if(mqtt_server == "" || mqtt_user == ""  || mqtt_pswd == "" || mqtt_client_name == "" || mqtt_topic == "" || ip_address == ""){
            DPRINTLN("Mandatory config values are blank");
            return false;
          }

        } 
        else {
          DPRINTLN("failed to load json config");
          return false;
        }
      }
    }
    else{
      DPRINTLN("/config.json does not exist");
      return false;
    }
    
  } else {
    DPRINTLN("failed to mount FS");
    return false;
  }
  return true;
}

//callback notifying us of the need to save config
void saveConfigCallback () {
  DPRINTLN("Should save config");
  shouldSaveConfig = true;
}

void loop() 
{
  //Nothing to do here as the setup does all the work and then powers down the ESP by writing a LOW signal to CH_PD  
  DPRINTLN("in loop");
  delay(2000);
}
