//
// ESP8266 Mobile Rick Roll Captive Portal
// - User connects thinking it is a free WiFi access point
// - After accepting Terms of Service they are Rick Roll'd
//
// References (I learned everything I needed to create this from these projects)
// PlatformIO - http://platformio.org/
//              http://docs.platformio.org/en/latest/platforms/espressif.html#uploading-files-to-file-system-spiffs
// ESP8266 Captive Portal - https://www.hackster.io/rayburne/esp8266-captive-portal-5798ff
// ESP-RADIO - https://github.com/Edzelf/Esp-radio
// ESPAsyncWebServer - https://github.com/me-no-dev/ESPAsyncWebServer
// SOFTAP - Get List of Connected Clients - http://www.esp8266.com/viewtopic.php?f=32&t=5669
// How to print free memory - http://forum.wemos.cc/topic/175/how-to-print-free-memory
// WebSocketToSerial - https://github.com/hallard/WebSocketToSerial.git
// JQueryTerminal - https://github.com/jcubic/jquery.terminal
// OTA Update - http://www.penninkhof.com/2015/12/1610-over-the-air-esp8266-programming-using-platformio/
// Piezo Beep - http://framistats.com/2015/06/07/esp8266-arduino-smtp-server-part-2/
// Simple Audio Board - http://bitcows.com/?p=19
// Tone Doesn't Work - https://www.reddit.com/r/esp8266/comments/46kw38/esp8266_calling_tone_function_does_not_work/
// ArduinoJson - https://github.com/bblanchon/ArduinoJson
// EEPROM - https://gist.github.com/dogrocker/f998dde4dbac923c47c1
// Exception Causes - https://github.com/esp8266/Arduino/blob/master/doc/exception_causes.md
// WiFi Scan- https://www.linuxpinguin.de/project/wifiscanner/
// SPIFFS - https://github.com/esp8266/Arduino/blob/master/doc/filesystem.md
//          http://blog.squix.org/2015/08/esp8266arduino-playing-around-with.html
// WiFiManager - https://github.com/tzapu/WiFiManager
// ESP-GDBStub - https://github.com/esp8266/Arduino/tree/master/libraries/GDBStub

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include "ESPAsyncTCP.h"
#include "ESPAsyncWebServer.h"
#include <EEPROM.h>
#include <FS.h>
#include <Hash.h>
#include <Ticker.h>

#include "DNSServer.h"

extern "C"
{
#include "user_interface.h"

    void system_set_os_print ( uint8 onoff );
    void ets_install_putc1 ( void *routine );
}

ADC_MODE ( ADC_VCC );                                                           // Set ADC for Voltage Monitoring

// Use the internal hardware buffer
static void _u0_putc ( char c )
{
    while ( ( ( U0S >> USTXC ) & 0x7F ) == 0x7F ) ;

    U0F = c;
}


//
//******************************************************************************************
// Forward declaration of methods                                                          *
//******************************************************************************************
int setupAP ( int chan_selected );

//***************************************************************************
// Global data section.                                                     *
//***************************************************************************
float version               = 1.42;
const char *appid           = "mobile-rr";
char ssid[]                 = "FREE Highspeed WiFi";
int channel                 = 0;
char username[]             = "rick";
char password[]             = "roll";
bool DEBUG                  = 1;
int interval                = 30;                                               // 30 Minutes

// Maximum number of simultaneous clients connected (WebSocket)
#define MAX_WS_CLIENT   3

#define CLIENT_NONE     0
#define CLIENT_ACTIVE   1


#define HELP_TEXT "[[b;green;]ESP8266 Mobile Rick Roll]\n" \
        "------------------------\n" \
        "[[b;cyan;]?] or [[b;cyan;]help]    show this help\n\n" \
        "[[b;cyan;]debug {0/1}]  show/set debug output\n" \
        "[[b;cyan;]ssid 's']     show/set SSID to 's'\n" \
        "[[b;cyan;]chan {0-11}]  show/set channel (0=auto)\n" \
        "[[b;cyan;]int {n}]      show/set auto scan interval\n" \
        "               where 'n' is mins (0=off)\n" \
        "[[b;cyan;]msg 's']      show/set message to 's'\n" \
        "[[b;cyan;]user 's']     show/set username to 's'\n" \
        "[[b;cyan;]pass 's']     show/set password to 's'\n\n" \
        "[[b;cyan;]count]        show Rick Roll count\n" \
        "[[b;cyan;]info]         show system information\n" \
        "[[b;cyan;]json {e/s/i}] show EEPROM, App Settings,\n" \
        "               or System Information\n\n" \
        "[[b;cyan;]ls]           list SPIFFS files\n" \
        "[[b;cyan;]cat 's']      read SPIFFS file 's'\n" \
        "[[b;cyan;]rm 's']       remove SPIFFS file 's'\n\n" \
        "[[b;cyan;]scan]         scan WiFi networks in area\n\n" \
        "[[b;cyan;]reboot]       reboot system\n" \
        "[[b;cyan;]reset]        reset default settings\n" \
        "[[b;cyan;]save]         save settings to EEPROM"

// Web Socket client state
typedef struct
{
    uint32_t id;
    uint8_t state;
} _ws_client;

enum class statemachine
{
    none,
    scan_wifi,
    ap_change,
    read_file
};
statemachine state = statemachine::none;
int state_int;
String state_string;

IPAddress ip ( 10, 10, 10, 1 );                                                 // Private network for httpd
DNSServer dnsd;                                                                 // Create the DNS object
MDNSResponder mdns;

AsyncWebServer httpd ( 80 );                                                    // Instance of embedded webserver
//AsyncWebServer  httpsd ( 443 );
AsyncWebSocket ws ( "/ws" );                                                    // access at ws://[esp ip]/ws
_ws_client ws_client[MAX_WS_CLIENT];                                            // State Machine for WebSocket Client;

Ticker timer;                                                                   // Setup Auto Scan Timer

int rrsession;                                                                  // Rick Roll Count Session
int rrtotal;                                                                    // Rick Roll Count Total
char str_vcc[8];
int chan_selected;

//***************************************************************************
// End of global data section.                                              *
//***************************************************************************

// WiFi Encryption Types
String encryptionTypes ( int which )
{
    switch ( which )
    {
        case ENC_TYPE_WEP:
            return "WEP";
            break;

        case ENC_TYPE_TKIP:
            return "WPA/TKIP";
            break;

        case ENC_TYPE_CCMP:
            return "WPA2/CCMP";
            break;

        case ENC_TYPE_NONE:
            return "None";
            break;

        case ENC_TYPE_AUTO:
            return "Auto";
            break;

        default:
            return "Unknown";
            break;
    }
}

//***************************************************************************
//                            D B G P R I N T                               *
//***************************************************************************
void dbg_printf ( const char *format, ... )
{
    static char sbuf[1400];                                                 // For debug lines
    va_list varArgs;                                                        // For variable number of params

    va_start ( varArgs, format );                                           // Prepare parameters
    vsnprintf ( sbuf, sizeof ( sbuf ), format, varArgs );                   // Format the message
    va_end ( varArgs );                                                     // End of using parameters

    Serial.println ( sbuf );

    if ( DEBUG )                                                            // DEBUG on?
    {
        if ( ws.count() )
            ws.textAll ( sbuf );
    }
}

/** IP to String? */
String ipToString ( IPAddress ip )
{
    String res = "";

    for ( int i = 0; i < 3; i++ )
    {
        res += String ( ( ip >> ( 8 * i ) ) & 0xFF ) + ".";
    }

    res += String ( ( ( ip >> 8 * 3 ) ) & 0xFF );
    return res;
}

//***************************************************************************
//                    F O R M A T  B Y T E S                                *
//***************************************************************************
String formatBytes ( size_t bytes )
{
    if ( bytes < 1024 )
    {
        return String ( bytes ) + " B";
    }
    else if ( bytes < ( 1024 * 1024 ) )
    {
        return String ( bytes / 1024.0 ) + " KB";
    }
    else if ( bytes < ( 1024 * 1024 * 1024 ) )
    {
        return String ( bytes / 1024.0 / 1024.0 ) + " MB";
    }
    else
    {
        return String ( bytes / 1024.0 / 1024.0 / 1024.0 ) + " GB";
    }
}


//***************************************************************************
//                    S E T U P                                             *
//***************************************************************************
void setup ( void )
{
    uint8_t mac[6];
    char mdnsDomain[] = "";

    ets_install_putc1 ( ( void * ) &_u0_putc );
    system_set_os_print ( 1 );
//  system_update_cpu_freq ( 160 );                                             // Set CPU to 80/160 MHz

    Serial.begin ( 115200 );                                                // For debug
    Serial.println();

    pinMode ( LED_BUILTIN, OUTPUT );                                        // initialize onboard LED as output
    digitalWrite ( LED_BUILTIN, HIGH );                                     // Turn the LED off by making the voltage HIGH

    // Startup Banner
    dbg_printf (
        "--------------------\n"
        "ESP8266 Mobile Rick Roll Captive Portal\n"
    );

    // Load EEPROM Settings
    setupEEPROM();


    // Setup Access Point
    wifi_set_phy_mode ( PHY_MODE_11B );
    WiFi.mode ( WIFI_AP );
    chan_selected = setupAP ( channel );
    WiFi.softAPmacAddress ( mac );

    // Show Soft AP Info
    dbg_printf (
        "SoftAP MAC: %02X:%02X:%02X:%02X:%02X:%02X\n" \
        "SoftAP IP: %s\n",
        MAC2STR ( mac ),
        ipToString ( ip ).c_str()
    );

    dbg_printf ( "SYSTEM ---" );
    dbg_printf ( "getSdkVersion:      %s", ESP.getSdkVersion() );
    dbg_printf ( "getBootVersion:     %08X", ESP.getBootVersion() );
    dbg_printf ( "getBootMode:        %08X", ESP.getBootMode() );
    dbg_printf ( "getChipId:          %08X", ESP.getChipId() );
    dbg_printf ( "getCpuFreqMHz:      %d Mhz", ESP.getCpuFreqMHz() );
    dbg_printf ( "getCycleCount:      %u\n", ESP.getCycleCount() );

    dbg_printf ( "POWER ---" );
    dbg_printf ( "Voltage:            %d.%d v\n ", ( ESP.getVcc() / 1000 ), ( ESP.getVcc() % 1000 ) );

    dbg_printf ( "MEMORY ---" );
    dbg_printf ( "getFreeHeap:        %d B", ESP.getFreeHeap() );
    dbg_printf ( "getSketchSize:      %s", formatBytes ( ESP.getSketchSize() ).c_str() );
    dbg_printf ( "getFreeSketchSpace: %s", formatBytes ( ESP.getFreeSketchSpace() ).c_str() );
    dbg_printf ( "getFlashChipSize:   %s", formatBytes ( ESP.getFlashChipRealSize() ).c_str() );
    dbg_printf ( "getFlashChipSpeed:  %d MHz\n", int ( ESP.getFlashChipSpeed() / 1000000 ) );

    // Start File System
    setupSPIFFS();

    setupDNSServer();

    sprintf ( mdnsDomain, "%s.local", appid );
    dbg_printf ( "Starting mDNS Responder" );

    if ( !mdns.begin ( mdnsDomain, ip ) )
    {
        Serial.println ( ">>>>> Error setting up mDNS responder!" );
    }
    else
    {
        dbg_printf ( ">>>>> mDNS Domain: %s", mdnsDomain );
    }

    setupHTTPServer();
    setupOTAServer();

    // Setup Timer
    if ( interval )
    {
        dbg_printf ( "Starting Auto WiFi Scan Timer" );
        timer.attach_ms ( ( 1000 * 60 * interval ), onTimer );
    }

    // Setup wifi connection callbacks
    wifi_set_event_handler_cb ( wifi_handle_event_cb );

    dbg_printf ( "\nReady!\n--------------------" );
}

int setupAP ( int chan_selected )
{
    struct softap_config config;

    wifi_softap_get_config ( &config ); // Get config first.

    if ( chan_selected == 0 )
    {
        chan_selected = scanWiFi();
    }

    //dbg_printf("WiFi \n\tSSID: %s\n\tPassword: %s\n\t Channel: %d", config.ssid, config.password, config.channel);
    //dbg_printf ( "Channel %d Selected!", chan_selected );

    if ( config.channel != chan_selected ) // || config.ssid != ssid) )
    {
        dbg_printf ( "Changing WiFi channel from %d to %d.", config.channel, chan_selected );
        WiFi.softAPdisconnect ( true );
        WiFi.mode ( WIFI_AP );
        wifi_set_phy_mode ( PHY_MODE_11B );
        WiFi.softAPConfig ( ip, ip, IPAddress ( 255, 255, 255, 0 ) );
        WiFi.softAP ( ssid, NULL, chan_selected );
    }
    else
    {
        dbg_printf ( "No change." );
    }

    return chan_selected;
}


void setupEEPROM()
{
    dbg_printf ( "EEPROM - Checking" );
    EEPROM.begin ( 512 );
    eepromLoad();
    dbg_printf ( "" );
}

void setupSPIFFS()
{
    FSInfo fs_info;                                                         // Info about SPIFFS
    Dir dir;                                                                // Directory struct for SPIFFS
    File f;                                                                 // Filehandle
    String filename;                                                        // Name of file found in SPIFFS

    SPIFFS.begin();                                                         // Enable file system

    // Show some info about the SPIFFS
    uint16_t cnt = 0;
    SPIFFS.info ( fs_info );
    dbg_printf ( "SPIFFS Files\nName                           -      Size" );
    dir = SPIFFS.openDir ( "/" );                                           // Show files in FS

    while ( dir.next() )                                                    // All files
    {
        f = dir.openFile ( "r" );
        filename = dir.fileName();
        dbg_printf ( "%-30s - %9s",                                     // Show name and size
                     filename.c_str(),
                     formatBytes ( f.size() ).c_str()
                   );
        cnt++;
    }

    dbg_printf ( "%d Files, %s of %s Used",
                 cnt,
                 formatBytes ( fs_info.usedBytes ).c_str(),
                 formatBytes ( fs_info.totalBytes ).c_str()
               );
    dbg_printf ( "%s Free\n",
                 formatBytes ( fs_info.totalBytes - fs_info.usedBytes ).c_str()
               );
}

void setupDNSServer()
{
    // Setup DNS Server
    // if DNS Server is started with "*" for domain name,
    // it will reply with provided IP to all DNS request
    dbg_printf ( "Starting DNS Server" );
    dnsd.onQuery ( [] ( const IPAddress & remoteIP, const char *domain, const IPAddress & resolvedIP )
    {
        dbg_printf ( "DNS Query [%d]: %s -> %s", remoteIP[3], domain, ipToString ( resolvedIP ).c_str() );

        /*        // connectivitycheck.android.com -> 74.125.21.113
                if ( strstr(domain, "connectivitycheck.android.com") )
                    dnsd.overrideIP =  IPAddress(74, 125, 21, 113);

                // dns.msftncsi.com -> 131.107.255.255
                if ( strstr(domain, "msftncsi.com") )
                    dnsd.overrideIP =  IPAddress(131, 107, 255, 255);
         */
    } );
    dnsd.onOverride ( [] ( const IPAddress & remoteIP, const char *domain, const IPAddress & overrideIP )
    {
        dbg_printf ( "DNS Override [%d]: %s -> %s", remoteIP[3], domain, ipToString ( overrideIP ).c_str() );
    } );
    //dnsd.setErrorReplyCode ( DNSReplyCode::NoError );
    //dnsd.setTTL(0);
    dnsd.start ( 53, "*", ip );
}

void setupHTTPServer()
{
    // Web Server Document Setup
    dbg_printf ( "Starting HTTP Captive Portal" );

    // Handle requests
    httpd.on ( "/generate_204", onRequest );  //Android captive portal. Maybe not needed. Might be handled by notFound handler.
    httpd.on ( "/fwlink", onRequest );  //Microsoft captive portal. Maybe not needed. Might be handled by notFound handler.
    httpd.onNotFound ( onRequest );

    // HTTP basic authentication
    httpd.on ( "/console", HTTP_GET, [] ( AsyncWebServerRequest * request )
    {
        request->redirect ( "http://10.10.10.1/console.htm" );
    } );
    httpd.on ( "/console.htm", HTTP_GET, [] ( AsyncWebServerRequest * request )
    {
        if ( strlen ( username ) > 0 && strlen ( password ) > 0 )
            if ( !request->authenticate ( username, password ) )
                return request->requestAuthentication();

        request->send ( SPIFFS, "/console.htm" );
    } );

    httpd.on ( "/trigger", HTTP_GET, [] ( AsyncWebServerRequest * request )
    {
        rrsession++;
        rrtotal++;
        IPAddress remoteIP = request->client()->remoteIP();
        ws.printfAll ( "[[b;yellow;]Rick Roll Sent!] (%d): [%s] %s",
                       rrsession,
                       ipToString ( remoteIP ).c_str(),
                       request->header ( "User-Agent" ).c_str()
                     );
        Serial.printf ( "Rick Roll Sent! (%d): [%s] %s\n",
                        rrsession,
                        ipToString ( remoteIP ).c_str(),
                        request->header ( "User-Agent" ).c_str()
                      );
        request->send ( 200, "text/html", String ( rrsession ) );
        eepromSave();


        //List all collected headers
        int headers = request->headers();
        int i;

        for ( i = 0; i < headers; i++ )
        {
            AsyncWebHeader *h = request->getHeader ( i );
            Serial.printf ( "HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str() );
        }

        // Disconnect that station
        //wifi_softap_dhcps_client_leave(NULL, remoteIP, true);
    } );

    httpd.on ( "/info", HTTP_GET, [] ( AsyncWebServerRequest * request )
    {
        AsyncWebServerResponse *response = request->beginResponse ( 200, "text/html", getSystemInformation() );
        response->addHeader ( "Access-Control-Allow-Origin", "http://localhost" );
        request->send ( response );
    } );

    httpd.on ( "/settings", HTTP_GET, [] ( AsyncWebServerRequest * request )
    {
        AsyncWebServerResponse *response = request->beginResponse ( 200, "text/html", getApplicationSettings() );
        response->addHeader ( "Access-Control-Allow-Origin", "http://localhost" );
        request->send ( response );
    } );

    // attach AsyncWebSocket
    dbg_printf ( "Starting Websocket Console" );
    ws.onEvent ( onEvent );
    httpd.addHandler ( &ws );
    httpd.begin();
}

void setupOTAServer()
{
    dbg_printf ( "Starting OTA Update Server" );

    // Port defaults to 8266
    // ArduinoOTA.setPort(8266);

    // Hostname defaults to esp8266-[ChipID]
    ArduinoOTA.setHostname ( appid );

    // No authentication by default
    ArduinoOTA.setPassword ( password );

    // OTA callbacks
    ArduinoOTA.onStart ( []()
    {
        SPIFFS.end();                                                   // Clean SPIFFS

        ws.enable ( false );                                            // Disable client connections
        dbg_printf ( "OTA Update Started" );                            // Let connected clients know what's going on
    } );
    ArduinoOTA.onEnd ( []()
    {
        dbg_printf ( "OTA Update Complete!\n" );

        if ( ws.count() )
        {
            ws.closeAll();                                          // Close connected clients
            delay ( 1000 );
        }
    } );
    ArduinoOTA.onProgress ( [] ( unsigned int progress, unsigned int total )
    {
        dbg_printf ( "Progress: %u%%\r", ( progress / ( total / 100 ) ) );
    } );
    ArduinoOTA.onError ( [] ( ota_error_t error )
    {
        dbg_printf ( "Error[%u]: ", error );

        if ( error == OTA_AUTH_ERROR ) dbg_printf ( "OTA Auth Failed" );
        else if ( error == OTA_BEGIN_ERROR ) dbg_printf ( "OTA Begin Failed" );
        else if ( error == OTA_CONNECT_ERROR ) dbg_printf ( "OTA Connect Failed" );
        else if ( error == OTA_RECEIVE_ERROR ) dbg_printf ( "OTA Receive Failed" );
        else if ( error == OTA_END_ERROR ) dbg_printf ( "OTA End Failed" );
    } );
    ArduinoOTA.begin();
}

int scanWiFi ()
{
    int channels[11];
    std::fill_n ( channels, 11, 0 );

    dbg_printf ( "Scanning WiFi Networks" );

    for ( int count = 1; count < 4; count++ )
    {
        int networks = WiFi.scanNetworks();
        dbg_printf ( "Scan %d, %d Networks Found", count, networks );
        dbg_printf ( "RSSI  CHANNEL  ENCRYPTION  BSSID              SSID" );

        for ( int network = 0; network < networks; network++ )
        {
            String ssid_scan;
            int32_t rssi_scan;
            uint8_t sec_scan;
            uint8_t *BSSID_scan;
            int32_t chan_scan;
            bool hidden_scan;
            WiFi.getNetworkInfo ( network, ssid_scan, sec_scan, rssi_scan, BSSID_scan, chan_scan, hidden_scan );

            dbg_printf ( "%-6d%-9d%-12s%02X:%02X:%02X:%02X:%02X:%02X  %s",
                         rssi_scan,
                         chan_scan,
                         encryptionTypes ( sec_scan ).c_str(),
                         MAC2STR ( BSSID_scan ),
                         ssid_scan.c_str()
                       );

            channels[ chan_scan ]++;
        }

        WiFi.scanDelete();
    }

    // Find least used channel
    int lowest_count = 10000;

    for ( int channel = 1; channel <= 11; channel++ )
    {
        // Include side channels to account for signal overlap
        int current_count = 0;

        for ( int i = channel - 4; i <= ( channel + 4 ); i++ )
        {
            if ( i > 0 )
                current_count += channels[i];
        }

        if ( current_count < lowest_count )
        {
            lowest_count = current_count;
            chan_selected = channel;
        }

        //Serial.printf( "Channel %d = %d\n", channel, current_count);
    }

    dbg_printf ( "Channel %d is least used.", chan_selected );

    return chan_selected;
}

void readFile ( String file )
{
    File f = SPIFFS.open ( file, "r" );

    if ( !f )
    {
        Serial.println ( "file open failed" );
    }
    else
    {
        while ( f.available() )
        {
            String line = f.readStringUntil ( 'n' );

            if ( ws.count() )
                ws.textAll ( line );
        }

        f.close();
    }
}

String getSystemInformation()
{
    String json;
    StaticJsonBuffer<512> jsonBuffer;
    JsonObject &root = jsonBuffer.createObject();

    root["sdk_version"] = ESP.getSdkVersion();
    root["boot_version"] = ESP.getBootVersion();
    root["boot_mode"] = ESP.getBootMode();
    root["chipid"] = ESP.getChipId();
    root["cpu_frequency"] = ESP.getCpuFreqMHz();

    root["cycle_count"] = ESP.getCycleCount();
    root["voltage"] = ( ESP.getVcc() / 1000 );

    root["free_memory"] = ESP.getFreeHeap();
    root["sketch_size"] = ESP.getSketchSize();
    root["sketch_free"] = ESP.getFreeSketchSpace();

    root["flash_size"] = ESP.getFlashChipRealSize();
    root["flash_speed"] = ( ESP.getFlashChipSpeed() / 1000000 );

    FSInfo fs_info;
    SPIFFS.info ( fs_info );
    root["spiffs_size"] = fs_info.totalBytes;
    root["spiffs_used"] = fs_info.usedBytes;
    root["spiffs_free"] = ( fs_info.totalBytes - fs_info.usedBytes );

    char apIP[16];
    sprintf ( apIP, "%s", ipToString ( WiFi.softAPIP() ).c_str() );
    root["softap_mac"] = WiFi.softAPmacAddress();
    root["softap_ip"] = apIP;

    char staIP[16];
    sprintf ( staIP, "%s", ipToString ( WiFi.localIP() ).c_str() );
    root["station_mac"] = WiFi.macAddress();
    root["station_ip"] = staIP;

    root.printTo ( json );
    return json;
}

String getApplicationSettings()
{
    String json;
    StaticJsonBuffer<512> jsonBuffer;
    JsonObject &root = jsonBuffer.createObject();

    root["version"] = version;
    root["appid"] = appid;
    root["ssid"] = ssid;
    root["channel"] = chan_selected;
    root["interval"] = ( interval );
    root["username"] = username;
    root["password"] = password;
    root["debug"] = DEBUG;
    root["rrsession"] = rrsession;
    root["rrtotal"] = rrtotal;

    root.printTo ( json );
    return json;
}

void onTimer ()
{
    dbg_printf ( "Auto WiFi scan initiated!" );
    chan_selected = 0;
    state = statemachine::ap_change;
}

void eepromLoad()
{
    String json;
    StaticJsonBuffer<512> jsonBuffer;

    int i = 0;

    while ( EEPROM.read ( i ) != 0 )
    {
        json += char ( EEPROM.read ( i ) );
        i++;
    }

    //dbg_printf("EEPROM[%s]", json.c_str());

    JsonObject &root = jsonBuffer.parseObject ( json );

    // If Parsing failed EEPROM isn't initialized
    if ( !root.success() )
    {
        eepromInitialize();
        eepromSave();
    }
    else
    {
        // Load Settings from JSON
        sprintf ( ssid, "%s", root["ssid"].asString() );
        channel = root["channel"];
        interval = root["interval"];
        sprintf ( username, "%s", root["username"].asString() );
        sprintf ( password, "%s", root["password"].asString() );

        DEBUG = root["debug"];
        rrtotal = root["rrtotal"];

        // If the AppID doesn't match then initialize EEPROM
        if ( version != root.get<float> ( "version" ) )
        {
            dbg_printf ( "EEPROM - Version Changed" );
            eepromInitialize();
            eepromSave();
            dbg_printf ( "EEPROM - Upgraded" );
        }

        dbg_printf ( "EEPROM - Loaded" );
        root.prettyPrintTo ( Serial );
        Serial.println();
    }
}

void eepromSave()
{
    StaticJsonBuffer<512> jsonBuffer;
    JsonObject &root = jsonBuffer.createObject();

    root["version"] = version;
    root["appid"] = appid;
    root["ssid"] = ssid;
    root["channel"] = channel;
    root["interval"] = interval;
    root["username"] = username;
    root["password"] = password;
    root["debug"] = DEBUG;
    root["rrtotal"] = rrtotal;

    char buffer[512];
    root.printTo ( buffer, sizeof ( buffer ) );

    int i = 0;

    while ( buffer[i] != 0 )
    {
        EEPROM.write ( i, buffer[i] );
        i++;
    }

    EEPROM.write ( i, buffer[i] );
    EEPROM.commit();

    dbg_printf ( "EEPROM - Saved" );
    root.prettyPrintTo ( Serial );
    Serial.println();
}

void eepromInitialize()
{
    dbg_printf ( "EEPROM - Initializing" );

    for ( int i = 0; i < 512; ++i )
    {
        EEPROM.write ( i, 0 );
    }

    EEPROM.commit();
}

String getEEPROM()
{
    String json;

    int i = 0;

    while ( EEPROM.read ( i ) != 0 )
    {
        json += char ( EEPROM.read ( i ) );
        i++;
    }

    return json;
}

bool disconnectStationByIP ( IPAddress station_ip )
{
    // Do ARP Query to get MAC address of station_ip

}

bool disconnectStationByMAC ( uint8_t *station_mac )
{

}

//***************************************************************************
//                    L O O P                                               *
//***************************************************************************
// Main program loop.                                                       *
//***************************************************************************
void loop ( void )
{
    dnsd.processNextRequest();
    ArduinoOTA.handle(); // Handle remote Wifi Updates

    switch ( state )
    {
        case statemachine::scan_wifi:
            scanWiFi();
            break;

        case statemachine::ap_change:
            chan_selected = setupAP ( chan_selected );
            break;

        case statemachine::read_file:
            readFile ( state_string );
            break;
    }

    state = statemachine::none;
    state_int = 0;
    state_string = "";
}

void wifi_handle_event_cb ( System_Event_t *evt )
{
//    printf ( "event %x\n", evt->event );

    switch ( evt->event )
    {
        case EVENT_STAMODE_CONNECTED:
            printf ( "connect to ssid %s, channel %d\n",
                     evt->event_info.connected.ssid,
                     evt->event_info.connected.channel );
            break;

        case EVENT_STAMODE_DISCONNECTED:
            printf ( "disconnect from ssid %s, reason %d\n",
                     evt->event_info.disconnected.ssid,
                     evt->event_info.disconnected.reason );
            break;

        case EVENT_STAMODE_AUTHMODE_CHANGE:
            printf ( "mode: %d -> %d\n",
                     evt->event_info.auth_change.old_mode,
                     evt->event_info.auth_change.new_mode );
            break;

        case EVENT_STAMODE_GOT_IP:
            printf ( "ip: " IPSTR " ,mask: " IPSTR " ,gw: " IPSTR,
                     IP2STR ( &evt->event_info.got_ip.ip ),
                     IP2STR ( &evt->event_info.got_ip.mask ),
                     IP2STR ( &evt->event_info.got_ip.gw )
                   );
            printf ( "\n" );
            break;

        case EVENT_SOFTAPMODE_STACONNECTED:  // 5
            printf ( "station connected: %02X:%02X:%02X:%02X:%02X:%02X, AID = %d\n",
                     MAC2STR ( evt->event_info.sta_connected.mac ),
                     evt->event_info.sta_connected.aid );
            break;

        case EVENT_SOFTAPMODE_STADISCONNECTED:  // 6
            printf ( "station disconnected: %02X:%02X:%02X:%02X:%02X:%02X, AID = %d\n",
                     MAC2STR ( evt->event_info.sta_disconnected.mac ),
                     evt->event_info.sta_disconnected.aid );
            break;

        default:
            break;
    }
}

//***************************************************************************
// HTTPD onRequest                                                          *
//***************************************************************************
void onRequest ( AsyncWebServerRequest *request )
{
    digitalWrite ( LED_BUILTIN, LOW );                                      // Turn the LED on by making the voltage LOW

    IPAddress remoteIP = request->client()->remoteIP();
    dbg_printf (
        "HTTP[%d]: %s%s",
        remoteIP[3],
        request->host().c_str(),
        request->url().c_str()
    );

    String path = request->url();

    if ( ( !SPIFFS.exists ( path ) && !SPIFFS.exists ( path + ".gz" ) ) ||  ( request->host() != "10.10.10.1" ) )
    {
        AsyncWebServerResponse *response = request->beginResponse ( 302, "text/plain", "" );
//        response->addHeader ( "Cache-Control", "no-cache, no-store, must-revalidate" );
//        response->addHeader ( "Pragma", "no-cache" );
//        response->addHeader ("Expires", "-1");
//        response->setContentLength (CONTENT_LENGTH_UNKNOWN);
        response->addHeader ( "Location", "http://10.10.10.1/index.htm" );
        request->send ( response );

        /*        AsyncWebServerResponse *response = request->beginResponse(
                    511,
                    "text/html",
                    "<html><head><meta http-equiv='refresh' content='0; url=http://10.10.10.1/index.htm'></head></html>"
                );
                //response->addHeader("Cache-Control","no-cache");
                //response->addHeader("Pragma","no-cache");
                //response->addHeader ("Location", "http://10.10.10.1/index.htm" );
                request->send(response);
         */
    }
    else
    {
        char s_tmp[] = "";
        AsyncWebServerResponse *response;

        if ( !request->hasParam ( "download" ) && SPIFFS.exists ( path + ".gz" ) )
        {
            response = request->beginResponse ( SPIFFS, path, String(), request->hasParam ( "download" ) );
        }
        else
        {
            response = request->beginResponse ( SPIFFS, path );                         // Okay, send the file

        }

        response->addHeader ( "Cache-Control", "no-cache, no-store, must-revalidate" );
        response->addHeader ( "Pragma", "no-cache" );
        response->addHeader ( "Expires", "-1" );
//        response->setContentLength (CONTENT_LENGTH_UNKNOWN);
        sprintf ( s_tmp, "%d", ESP.getFreeHeap() );
        response->addHeader ( "ESP-Memory", s_tmp );
        request->send ( response );
    }

    digitalWrite ( LED_BUILTIN, HIGH );                                     // Turn the LED off by making the voltage HIGH
}


//***************************************************************************
// WebSocket onEvent                                                        *
//***************************************************************************
// Manage routing of websocket events                                       *
//***************************************************************************
void onEvent ( AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len )
{
    if ( type == WS_EVT_CONNECT )
    {
        uint8_t index;
        //dbg_printf ( "ws[%s][%u] connect\n", server->url(), client->id() );

        for ( index = 0; index < MAX_WS_CLIENT; index++ )
        {
            if ( ws_client[index].id == 0 )
            {
                ws_client[index].id = client->id();
                ws_client[index].state = CLIENT_ACTIVE;
                //dbg_printf ( "added #%u at index[%d]\n", client->id(), index );
                client->printf ( "[[b;green;]Hello Client #%u, added you at index %d]", client->id(), index );
                client->ping();
                break; // Exit for loop
            }
        }

        if ( index >= MAX_WS_CLIENT )
        {
            //dbg_printf ( "not added, table is full" );
            client->printf ( "[[b;red;]Sorry client #%u, Max client limit %d reached]", client->id(), MAX_WS_CLIENT );
            client->ping();
        }
    }
    else if ( type == WS_EVT_DISCONNECT )
    {
        //dbg_printf ( "ws[%s][%u] disconnect: %u\n", server->url(), client->id() );

        for ( uint8_t i = 0; i < MAX_WS_CLIENT; i++ )
        {
            if ( ws_client[i].id == client->id() )
            {
                ws_client[i].id = 0;
                ws_client[i].state = CLIENT_NONE;
                //dbg_printf ( "freed[%d]\n", i );
                break; // Exit for loop
            }
        }
    }
    else if ( type == WS_EVT_ERROR )
    {
        dbg_printf ( "WS[%u]: error(%u) - %s", client->id(), * ( ( uint16_t * ) arg ), ( char * ) data );
    }
    else if ( type == WS_EVT_PONG )
    {
        dbg_printf ( "WS[%u]: pong", client->id(), len );
    }
    else if ( type == WS_EVT_DATA )
    {
        //data packet
        AwsFrameInfo *info = ( AwsFrameInfo * ) arg;
        char *msg = NULL;
        size_t n = info->len;
        uint8_t index;

        // Size of buffer needed
        // String same size +1 for \0
        // Hex size*3+1 for \0 (hex displayed as "FF AA BB ...")
        n = info->opcode == WS_TEXT ? n + 1 : n * 3 + 1;
        msg = ( char * ) calloc ( n, sizeof ( char ) );

        if ( msg )
        {
            // Grab all data
            for ( size_t i = 0; i < info->len; i++ )
            {
                if ( info->opcode == WS_TEXT )
                {
                    msg[i] = ( char ) data[i];
                }
                else
                {
                    sprintf_P ( msg + i * 3, PSTR ( "%02x " ), ( uint8_t ) data[i] );
                }
            }
        }

        //dbg_printf ( "ws[%s][%u] message %s\n", server->url(), client->id(), msg );

        // Search if it's a known client
        for ( index = 0; index < MAX_WS_CLIENT; index++ )
        {
            if ( ws_client[index].id == client->id() )
            {
                //dbg_printf ( "known[%d] '%s'\n", index, msg );
                //dbg_printf ( "client #%d info state=%d\n", client->id(), ws_client[index].state );

                // Received text message
                if ( info->opcode == WS_TEXT )
                {
                    execCommand ( client, msg );
                }
                else
                {
                    dbg_printf ( "Binary 0x:%s", msg );
                }

                // Exit for loop
                break;
            } // if known client
        } // for all clients

        // Free up allocated buffer
        if ( msg )
            free ( msg );
    } // EVT_DATA
}

//***************************************************************************
// WebSocket execCommand                                                    *
//***************************************************************************
// translate and execute command                                            *
//***************************************************************************
void execCommand ( AsyncWebSocketClient *client, char *msg )
{
    bool CHANGED = false;
    uint16_t l = strlen ( msg );
    uint8_t index = MAX_WS_CLIENT;

    // Search if we're known client
    if ( client )
    {
        for ( index = 0; index < MAX_WS_CLIENT; index++ )
        {
            // Exit for loop if we are there
            if ( ws_client[index].id == client->id() )
                break;
        } // for all clients
    }
    else
    {
        return;
    }

    // Custom command to talk to device
    if ( !strncasecmp_P ( msg, PSTR ( "debug" ), 5 ) )
    {
        if ( l > 5 )
        {
            int v = atoi ( &msg[6] );

            if ( v > 0 )
            {
                DEBUG = true;
            }
            else
            {
                DEBUG = false;
            }

            CHANGED = true;
        }

        if ( DEBUG )
        {
            client->printf_P ( PSTR ( "[[b;green;]DEBUG output enabled]" ) );
        }
        else
        {
            client->printf_P ( PSTR ( "[[b;yellow;]DEBUG output disabled]" ) );
        }

    }
    // Dir files on SPIFFS system
    // --------------------------
    else if ( !strcasecmp_P ( msg, PSTR ( "ls" ) ) )
    {
        FSInfo fs_info;
        uint16_t cnt = 0;
        String filename;

        client->printf_P ( PSTR ( "[[b;green;]SPIFFS Files]\r\nName                           -      Size" ) );
        Dir dir = SPIFFS.openDir ( "/" );                               // Show files in FS

        while ( dir.next() )                                            // All files
        {
            cnt++;
            File f = dir.openFile ( "r" );
            filename = dir.fileName();
            client->printf_P ( PSTR ( "%-30s - %9s" ),              // Show name and size
                               filename.c_str(),
                               formatBytes ( f.size() ).c_str()
                             );
            f.close();
        }

        SPIFFS.info ( fs_info );
        client->printf_P ( PSTR ( "%d Files, %s of %s Used" ),
                           cnt,
                           formatBytes ( fs_info.usedBytes ).c_str(),
                           formatBytes ( fs_info.totalBytes ).c_str()
                         );
        client->printf_P ( PSTR ( "%s Free" ),
                           formatBytes ( fs_info.totalBytes - fs_info.usedBytes ).c_str()
                         );
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "cat" ), 3 ) )
    {
        if ( !strncasecmp_P ( msg, PSTR ( "cat " ), 4 ) )
        {
            if ( SPIFFS.exists ( &msg[4] ) )
            {
                state_string = &msg[4];
                state = statemachine::read_file;
            }
            else
            {
                client->printf_P ( PSTR ( "[[b;red;]cat: %s: No such file or directory]" ), &msg[4] );
            }

        }
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "rm" ), 2 ) )
    {
        if ( !strncasecmp_P ( msg, PSTR ( "rm " ), 3 ) )
        {
            if ( SPIFFS.exists ( &msg[3] ) )
            {
                SPIFFS.remove ( &msg[3] );
            }
            else
            {
                client->printf_P ( PSTR ( "[[b;red;]rm: %s: No such file or directory]" ), &msg[3] );
            }

        }
    }
    else if ( !strcasecmp_P ( msg, PSTR ( "info" ) ) )
    {
        uint8_t mac[6];
        WiFi.softAPmacAddress ( mac );
        client->printf_P ( PSTR ( "[[b;green;]SYSTEM INFORMATION]" ) );
        client->printf_P ( PSTR ( "Firmware version [[b;green;]%s %s]\n " ), __DATE__, __TIME__ );

        client->printf_P ( PSTR ( "[[b;cyan;]SYSTEM]" ) );
        client->printf_P ( PSTR ( "SDK version:  %s" ), ESP.getSdkVersion() );
        client->printf_P ( PSTR ( "Boot Version: %08X" ), ESP.getBootVersion() );
        client->printf_P ( PSTR ( "Boot Mode:    %08X" ), ESP.getBootMode() );
        client->printf_P ( PSTR ( "Chip ID:      %08X" ), ESP.getChipId() );
        client->printf_P ( PSTR ( "CPU Freq:     %d MHz" ), ESP.getCpuFreqMHz() );
        client->printf_P ( PSTR ( "Cycle Count:  %u\n " ), ESP.getCycleCount() );

        client->printf_P ( PSTR ( "[[b;cyan;]POWER]" ) );
        client->printf_P ( PSTR ( "Voltage:      %d.%d v\n " ), ( ESP.getVcc() / 1000 ), ( ESP.getVcc() % 1000 ) );

        client->printf_P ( PSTR ( "[[b;cyan;]MEMORY]" ) );
        client->printf_P ( PSTR ( "Free Memory:  %d B" ), ESP.getFreeHeap() );
        client->printf_P ( PSTR ( "Sketch Size:  %s" ), formatBytes ( ESP.getSketchSize() ).c_str() );
        client->printf_P ( PSTR ( "Sketch Free:  %s" ), formatBytes ( ESP.getFreeSketchSpace() ).c_str() );
        client->printf_P ( PSTR ( "Flash Space:  %s" ), formatBytes ( ESP.getFlashChipRealSize() ).c_str() );
        client->printf_P ( PSTR ( "Flash Speed:  %d MHz\n " ), int ( ESP.getFlashChipSpeed() / 1000000 ) );

        client->printf_P ( PSTR ( "[[b;cyan;]NETWORK]" ) );
        client->printf_P ( PSTR ( "SoftAP SSID: %s" ), ssid );
        client->printf_P ( PSTR ( "SoftAP  MAC: %02X:%02X:%02X:%02X:%02X:%02X\nSoftAP   IP: %s\n " ),
                           MAC2STR ( mac ),
                           ipToString ( ip ).c_str()
                         );

        client_status ( client );
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "user" ), 4 ) )
    {
        if ( !strncasecmp_P ( msg, PSTR ( "user " ), 5 ) )
        {
            sprintf ( username, "%s", &msg[5] );
            client->printf_P ( PSTR ( "[[b;yellow;]Changing Username:] %s" ), username );
            CHANGED = true;
        }
        else
        {
            client->printf_P ( PSTR ( "[[b;yellow;]Username:] %s" ), username );
        }
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "pass" ), 4 ) )
    {
        if ( !strncasecmp_P ( msg, PSTR ( "pass " ), 5 ) )
        {
            sprintf ( password, "%s", &msg[5] );
            client->printf_P ( PSTR ( "[[b;yellow;]Changing Password:] %s" ), password );
            CHANGED = true;
        }
        else
        {
            client->printf_P ( PSTR ( "[[b;yellow;]Password:] %s" ), password );
        }
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "ssid" ), 4 ) )
    {
        if ( l == 4 )
        {
            client->printf_P ( PSTR ( "[[b;yellow;]SSID:] %s" ), ssid );
        }
        else
        {
            sprintf ( ssid, "%s", &msg[5] );
            client->printf_P ( PSTR ( "[[b;yellow;]Changing WiFi SSID:] %s" ), ssid );

            eepromSave();
            state = statemachine::ap_change;
        }
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "chan" ), 4 ) )
    {
        if ( l == 4 )
        {
            if ( channel == 0 )
            {
                client->printf_P ( PSTR ( "[[b;yellow;]Channel:] AUTO (%d)" ), WiFi.channel() );
            }
            else
            {
                client->printf_P ( PSTR ( "[[b;yellow;]Channel:] %d" ), WiFi.channel() );
            }
        }
        else
        {
            int v = atoi ( &msg[5] );

            if ( v > 0 && v < 12 )
            {
                client->printf_P ( PSTR ( "[[b;yellow;]Changing WiFi Channel:] %d" ), v );
            }
            else
            {
                v = 0;
                client->printf_P ( PSTR ( "[[b;yellow;]Changing WiFi Channel:] AUTO" ) );
            }

            channel = v;
            chan_selected = v;
            state = statemachine::ap_change;

            CHANGED = true;
        }
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "scan" ), 4 ) )
    {
        client->printf_P ( PSTR ( "[[b;yellow;]WiFi scan initiated!]" ) );
        state = statemachine::scan_wifi;
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "int" ), 3 ) )
    {
        if ( l > 3 )
        {
            int v = atoi ( &msg[4] );

            if ( v < 1 )
            {
                v = 0;
                timer.detach();
            }
            else
            {
                v = v;
                timer.detach();
                timer.attach_ms ( 1000 * 60 * v, onTimer );
                client->printf_P ( PSTR ( "[[b;yellow;]Auto Scan:] ENABLED" ) );
            }

            interval = v;

            CHANGED = true;
        }

        if ( interval == 0 )
        {
            client->printf_P ( PSTR ( "[[b;yellow;]Auto Scan:] DISABLED" ) );
        }
        else
        {
            client->printf_P ( PSTR ( "[[b;yellow;]Auto Scan Interval:] %d min(s)" ), interval );
        }
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "save" ), 4 ) )
    {
        client->printf_P ( PSTR ( "[[b;green;]Saving Settings to EEPROM]" ) );

        eepromSave();
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "msg" ), 3 ) )
    {
        File f;

        // Open "message.htm" file stream from SPIFFS
        if ( SPIFFS.exists ( "/message.htm" ) )
        {
            //Serial.println ( "File opened r+" );
            f = SPIFFS.open ( "/message.htm", "r+" );
        }
        else
        {
            //Serial.println ( "File opened w+" );
            f = SPIFFS.open ( "/message.htm", "w+" );
        }

        if ( !f )
        {
            client->printf_P ( PSTR ( "[[b;red;]File open failed:] /message.htm" ) );
        }
        else
        {
            if ( l == 3 )
            {
                client->printf_P ( PSTR ( "[[b;yellow;]Reading:] /message.htm" ) );
                state_string = "/message.htm";
                state = statemachine::read_file;
            }
            else
            {
                Serial.printf ( "Writing File: [%s]", &msg[4] );
                client->printf_P ( PSTR ( "[[b;yellow;]Changing Message:] %s" ), &msg[4] );

                // Write Message to "message.htm" in SPIFFS
                f.print ( &msg[4] );
            }

            f.close();
        }
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "count" ), 5 ) )
    {
        if ( l > 5 )
        {
            int v = atoi ( &msg[6] );

            if ( v >= 0 )
            {
                rrsession = v;
                rrtotal = v;

                eepromSave();
            }
        }

        client->printf_P ( PSTR ( "[[b;yellow;]Rick Roll Count]: %d Session, %d Total" ), rrsession, rrtotal );
    }
    else if ( !strncasecmp_P ( msg, PSTR ( "json" ), 4 ) )
    {
        if ( l > 4 )
        {
            String json;

            if ( !strncasecmp_P ( &msg[5], PSTR ( "e" ), 1 ) )
            {
                // EEPROM
                json = getEEPROM();
            }
            else if ( !strncasecmp_P ( &msg[5], PSTR ( "s" ), 1 ) )
            {
                // Settings
                json = getApplicationSettings();
            }
            else if ( !strncasecmp_P ( &msg[5], PSTR ( "i" ), 1 ) )
            {
                // information
                json = getSystemInformation();
            }

            client->printf_P ( json.c_str() );
        }
    }
    else if ( !strcasecmp_P ( msg, PSTR ( "reset" ) ) )
    {
        client->printf_P ( PSTR ( "Resetting EEPROM to defaults" ) );
        eepromInitialize();
        ESP.restart();
    }
    else if ( ( *msg == '?' || !strcasecmp_P ( msg, PSTR ( "help" ) ) ) )
    {
        client->printf_P ( PSTR ( HELP_TEXT ) );
    }
    else if ( !strcasecmp_P ( msg, PSTR ( "reboot" ) ) )
    {
        ws.closeAll();
        delay ( 1000 );
        ESP.restart();
    }

    if ( CHANGED )
    {
        client->printf_P ( PSTR ( "[[b;yellow;]*** NOTE: Changes are not written to EEPROM until you enter 'save']" ) );
    }

    dbg_printf ( "WS[%d]: %s", client->id(), msg );
}

void client_status ( AsyncWebSocketClient *client )
{
    struct station_info *station = wifi_softap_get_station_info();
    uint8_t client_count = wifi_softap_get_station_num();
    struct ip_addr *ip;
    client->printf_P ( PSTR ( "[[b;yellow;]Connected Client(s)]: %d" ),
                       client_count
                     );
    int i = 0;

    while ( station != NULL )
    {
        ip = &station->ip;
        client->printf_P ( PSTR ( "%d: MAC: %02X:%02X:%02X:%02X:%02X:%02X\n    IP: %s" ),
                           i,
                           MAC2STR ( station->bssid ),
                           ipToString ( ip->addr ).c_str()
                         );
        i++;
        station = STAILQ_NEXT ( station, next );
    }

    wifi_softap_free_station_info();
}