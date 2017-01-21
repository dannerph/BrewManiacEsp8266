#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <Hash.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

//#include <GDBStub.h>
#include "config.h"

#include "WiFiSetup.h"
#include "HttpUpdateHandler.h"
#include "BrewManiacProxy.h"
#include "BrewManiacWeb.h"
//#include "SPIFFSEditor.h"
#include "ESPUpdateServer.h"

#include "TimeKeeper.h"

#if SerialDebug == true
#define DebugOut(a) DebugPort.print(a)
#define DBG_PRINTF(...) DebugPort.printf(__VA_ARGS__)
#else
#define DebugOut(a) 
#define DBG_PRINTF(...)
#endif

#if EnableBrewLog
#include "BrewLogger.h"
#endif

extern void brewmaniac_setup();
extern void brewmaniac_loop();

#define ResponseAppleCNA true

/**************************************************************************************/
/**************************************************************************************/

#define WS_PATH "/ws"
#define SSE_PATH "/status.php"
#define LOGS_PATH "/logs.php"

#define CHART_DATA_PATH "/chart.php"
#define SETTING_PATH "/settings.php"
#define AUTOMATION_PATH "/automation.php"
#define BUTTON_PATH "/button.php"
#define UPDATE_AUTOMATION_PATH "/saveauto.php"
#define UPDATE_SETTING_PATH "/savesettings.php"

#define NETCFG_PATH "/netcfg.php"
#define SCAN_SENSOR_PATH "/scan.php"
#define DEFAULT_INDEX_FILE     "index.htm"

#define MAX_CONFIG_LEN 256
#define JSON_BUFFER_SIZE 256
#define CONFIG_FILENAME "/network.cfg"

#define MaxNameLength 32

char _gHostname[MaxNameLength];
char _gUsername[MaxNameLength];
char _gPassword[MaxNameLength];
bool _gSecuredAccess;


#if UseSoftwareSerial == true
SoftwareSerial wiSerial(SW_RX_PIN,SW_TX_PIN);
#else
#define wiSerial Serial
#endif

AsyncWebServer server(80);

BrewManiacWeb bmWeb;

#if EnableBrewLog
BrewLogger brewLogger;
#endif

typedef union _address{
                uint8_t bytes[4];  // IPv4 address
                uint32_t dword;
} IPV4Address;




class TemperatureLogHandler:public AsyncWebHandler
{
	void handleRequest(AsyncWebServerRequest *request){
		if( request->url() == LOGS_PATH){
			if(request->hasParam("dl")){
				int index=request->getParam("dl")->value().toInt();
				//DBG_PRINTF("Get log index:%d\n",index);
				char buf[40];
				brewLogger.createFilename(buf,index);
				if(SPIFFS.exists(buf)){
					request->send(SPIFFS,buf,"application/octet-stream");
				}else{
					request->send(404); 
				}
			}else{
				// list
				FileInfo* list=brewLogger.getLogFileInfo();
				String json=String("[");
				bool comma=false;
				for(int i=0;i<MAX_FILE_NUMBER;i++){
					if(list[i].index >=0){
						if(comma) json +=",";
						else comma=true;
						
						json += "{\"f\":" +String(list[i].index) +",\"t\":" 
							+String(list[i].time) +"}";
					}else{
						break;
					}
				}
				json += "]";
				request->send(200, "text/json;",json);
			}
			return;
		}
		int offset;
		if(request->hasParam("offset")){
			offset=request->getParam("offset")->value().toInt();
			//DBG_PRINTF("offset= %d\n",offset);
		}else{
			offset=0;
		}
		size_t size=brewLogger.beginCopyAfter(offset);
		if(size >0){
			request->send("application/octet-stream", size, [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t { 
				return brewLogger.read(buffer, maxLen,index);
			});
		}else{
			request->send(204);
		}		
	}
public:
	TemperatureLogHandler(){}
	bool canHandle(AsyncWebServerRequest *request){
	 	if(request->url() == CHART_DATA_PATH || request->url() ==LOGS_PATH) return true;
	 	return false;
	}

};


TemperatureLogHandler logHandler; 


static const char* configFormat =
R"END(
{"host":"%s",
"user":"%s",
"pass":"%s",
"secured":%d
}
)END";

void requestRestart(bool disc);

class NetworkConfig:public AsyncWebHandler
{
protected:
	void makeConfig(char *buff,char* host, char* user, char* pass,bool secured)
	{
		sprintf(buff,configFormat,host,user,pass,secured? 1:0);
	}
public:
	NetworkConfig(){}
	
	void handleRequest(AsyncWebServerRequest *request){
		if(request->method() == HTTP_POST){
			String data=request->getParam("data", true, false)->value();
			DynamicJsonBuffer jsonBuffer(JSON_BUFFER_SIZE);
			JsonObject& root = jsonBuffer.parseObject(data.c_str());
			if (!root.success()){
				DBG_PRINTF("Invalid JSON string\n");
				request->send(404);
				return;
			}
			if(!root.containsKey("user") || !root.containsKey("pass")
			 || (strcmp(_gUsername,root["user"]) !=0) 
			 || (strcmp(_gPassword,root["pass"]) !=0) ){
				//DBG_PRINTF("expected user:%s pass:%s\n",_gUsername,_gPassword);
			 	request->send(400);
			 	return;
			}
			if(root.containsKey("disconnect")){ 
				requestRestart(true);
  				request->send(200);
  				return;
			}
			
			File config=SPIFFS.open(CONFIG_FILENAME,"w+");
  			if(!config){
  				request->send(500);
  				return;
  			}
  				
			const char *nhost;
			if(root.containsKey("host")) nhost= root["host"];
			else nhost=_gHostname;
			
			const char *nuser;
			if(root.containsKey("nuser")) nuser=root["nuser"];
			else nuser=_gUsername;
			
			const char *npass;
			if(root.containsKey("npass")) npass= root["npass"];
			else npass=_gPassword;
			
			bool nsecure;
			byte value=root["secured"];
			if(root.containsKey("secured")) nsecure=( 0 != value);
			else nsecure=_gSecuredAccess;
				  				
			char configBuff[MAX_CONFIG_LEN];
			makeConfig(configBuff,(char *)nhost,(char *)nuser,(char *)npass,nsecure);
  			config.printf(configBuff);
  			config.close();

			request->send(200);

		}else if(request->method() == HTTP_GET){
			if(SPIFFS.exists(CONFIG_FILENAME)){
				request->send(SPIFFS,CONFIG_FILENAME, "text/json");
			}else{
	//			char configBuff[MAX_CONFIG_LEN];
	//			makeConfig(configBuff,_gHostname,_gUsername,_gPassword,_gSecuredAccess);
			String rsp=String("{\"host\":\"") + String(_gHostname) 
				+ String("\",\"secured\":") + (_gSecuredAccess? "1":"0") +"}";

				request->send(200, "text/json",rsp);
			}
		}
	}
	
	bool canHandle(AsyncWebServerRequest *request){
	 	if(request->url() == NETCFG_PATH) return true;
	 	
	 	return false;
	}
	
	void loadSetting(void){
		// try open configuration
		char configBuf[MAX_CONFIG_LEN];
		File config=SPIFFS.open(CONFIG_FILENAME,"r+");

		DynamicJsonBuffer jsonBuffer(JSON_BUFFER_SIZE);

		if(config){
			size_t len=config.readBytes(configBuf,MAX_CONFIG_LEN);
			configBuf[len]='\0';
		}
		JsonObject& root = jsonBuffer.parseObject(configBuf);
	
		if(!config
				|| !root.success()
				|| !root.containsKey("host")
				|| !root.containsKey("user")
				|| !root.containsKey("pass")){
		
  			strcpy(_gHostname,Default_HOSTNAME);
  			strcpy(_gUsername,Default_USERNAME);
  			strcpy(_gPassword,Default_PASSWORD);
			_gSecuredAccess=false;
		
		}else{
			config.close();
		  	
  			strcpy(_gHostname,root["host"]);
  			strcpy(_gUsername,root["user"]);
  			strcpy(_gPassword,root["pass"]);
  			_gSecuredAccess=(root.containsKey("secured"))? (bool)(root["secured"]):false;
  		}
		
	}
};

NetworkConfig networkConfig;

class BmwHandler: public AsyncWebHandler 
{
public:
	BmwHandler(void){}
	void handleRequest(AsyncWebServerRequest *request){
		
		if(_gSecuredAccess && !request->authenticate(_gUsername, _gPassword))
	        return request->requestAuthentication();

	 	if(request->method() == HTTP_GET && request->url() == SETTING_PATH ){
	 		String json;
	 		bmWeb.getSettings(json);
	 		request->send(200, "text/json", json);
		
			//piggyback the time from browser 
			if(request->hasParam("time")){
  				AsyncWebParameter* tvalue = request->getParam("time");
  				DBG_PRINTF("Set Time:%ld, current:%ld\n",tvalue->value().toInt(),TimeKeeper.getTimeSeconds());
	 			TimeKeeper.setCurrentTime(tvalue->value().toInt());
	 		}
	 		
	 	}else if(request->method() == HTTP_GET && request->url() == AUTOMATION_PATH){
	 		String json;
	 		bmWeb.getAutomation(json);
	 		request->send(200, "text/json", json);
#if	MaximumNumberOfSensors	> 1 		
	 	}else if(request->method() == HTTP_GET && request->url() == SCAN_SENSOR_PATH){
	 		bmWeb.scanSensors();
	 		request->send(200);
#endif
	 	}else if(request->method() == HTTP_GET && request->url() == BUTTON_PATH){
			if(request->hasParam("code")){
				AsyncWebParameter* p = request->getParam("code");
				byte code=p->value().toInt();
	 			bmWeb.sendButton(code & 0xF, (code & 0xF0)!=0);
	 			request->send(200);
	 		}else{
	 			request->send(400);
	 		}
	 	}else if(request->method() == HTTP_POST && request->url() == UPDATE_AUTOMATION_PATH){
	 		String data=request->getParam("data", true, false)->value();
	 		DebugOut("saveauto.php:\n");
	 		DebugOut(data.c_str());
	 		if(bmWeb.updateAutomation(data)){
	 			request->send(200, "text/json", "{\"code\":0,\"result\":\"OK\"}");
	 		}else{
	 			request->send(400);
	 		}
	 		
	 	}else if(request->method() == HTTP_POST && request->url() == UPDATE_SETTING_PATH){
	 		String data=request->getParam("data", true, false)->value();
	 		DebugOut("savesettings.php:\n");
	 		DebugOut(data.c_str());
	 		if(bmWeb.updateSettings(data)){
	 			request->send(200, "text/json", "{\"code\":0,\"result\":\"OK\"}");
	 		}else{
	 			request->send(400);
	 		}
	 	}else if(request->method() == HTTP_GET){
		 	AsyncWebServerResponse *response;
		 	
			String path=request->url();
	 		if(path.endsWith("/")) path +=DEFAULT_INDEX_FILE;

  			if(path.endsWith(".js")){

	 			String pathWithJgz = path.substring(0,path.lastIndexOf('.')) + ".jgz";
				//DBG_PRINTF("checking with:%s\n",pathWithJgz.c_str());
  			  	if(SPIFFS.exists(pathWithJgz)){
  			  		//DBG_PRINTF("response with:%s\n",pathWithJgz.c_str());
		 			response = request->beginResponse(SPIFFS, pathWithJgz,"application/javascript");
					response->addHeader("Content-Encoding", "gzip");
					response->addHeader("Cache-Control","max-age=2592000");
					request->send(response);
					return;
				}
  			}else{
  				//DBG_PRINTF("non js file:\"%s\"\n",path.c_str());
  			}  			
	 		
	 		String pathWithGz = path + ".gz";
  			if(SPIFFS.exists(pathWithGz)){
	 			response = request->beginResponse(SPIFFS, pathWithGz,"application/x-gzip");
				response->addHeader("Content-Encoding", "gzip");
  			}else{
	 			response = request->beginResponse(SPIFFS, path);
			}

			response->addHeader("Cache-Control","max-age=2592000");
			request->send(response);

		}	 	
	 }
	 
	bool canHandle(AsyncWebServerRequest *request){
	 	if(request->method() == HTTP_GET){
	 		if(request->url() == SETTING_PATH || request->url() == AUTOMATION_PATH || request->url() == BUTTON_PATH  || request->url() == SCAN_SENSOR_PATH)
	 			return true;
	 		else{
				// get file
				String path=request->url();
	 			if(path.endsWith("/")) path +=DEFAULT_INDEX_FILE;
	 			//DBG_PRINTF("request:%s\n",path.c_str());
				//if(fileExists(path)) return true; 
				if(SPIFFS.exists(path)) return true;
  				
  				if(path.endsWith(".js")){
	 				String pathWithJgz = path.substring(0,path.lastIndexOf('.')) + ".jgz";
					//DBG_PRINTF("checking with:%s\n",pathWithJgz.c_str());
  			  		if(SPIFFS.exists(pathWithJgz)) return true;
  			  	}

  				String pathWithGz = path + ".gz";
  				if(SPIFFS.exists(pathWithGz)) return true;
		
	 		}
	 	}else if(request->method() == HTTP_POST){
	 		if(request->url() == UPDATE_AUTOMATION_PATH || request->url() == UPDATE_SETTING_PATH)
	 			return true;	 	
	 	}
	 	return false;
	 }	 
};

BmwHandler bmwHandler;

#if UseWebSocket == true
AsyncWebSocket ws(WS_PATH);

void processRemoteCommand( uint8_t *data, size_t len)
{
	StaticJsonBuffer<128> jsonBuffer;
	char buf[128];
	int i;
	for(i=0;i< len && i<127;i++){
		buf[i]=data[i];
	}
	buf[i]='\0';
	JsonObject& root = jsonBuffer.parseObject(buf);

	if (root.success() && root.containsKey("btn") ){
		int code = root["btn"];
		bmWeb.sendButton(code & 0xF, (code & 0xF0)!=0);
	}
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len)
{
	if(type == WS_EVT_CONNECT){
    	DBG_PRINTF("ws[%s][%u] connect\n", server->url(), client->id());
		String json;
		bmWeb.getCurrentStatus(json);
		client->text(json);
  	} else if(type == WS_EVT_DISCONNECT){
    	DBG_PRINTF("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
  	} else if(type == WS_EVT_ERROR){
    	DBG_PRINTF("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  	} else if(type == WS_EVT_PONG){
    	DBG_PRINTF("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len)?(char*)data:"");
  	} else if(type == WS_EVT_DATA){
    	AwsFrameInfo * info = (AwsFrameInfo*)arg;
    	String msg = "";
    	if(info->final && info->index == 0 && info->len == len){
      		//the whole message is in a single frame and we got all of it's data
      		DBG_PRINTF("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT)?"text":"binary", info->len);
			processRemoteCommand(data,info->len);

		} else {
      		//message is comprised of multiple frames or the frame is split into multiple packets
#if 0  // for current application, the data should not be segmented  
      		if(info->index == 0){
        		if(info->num == 0)
          		DBG_PRINTF("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
        		DBG_PRINTF("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      		}

      		DBG_PRINTF("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT)?"text":"binary", info->index, info->index + len);

	        for(size_t i=0; i < info->len; i++) {
    	    	//msg += (char) data[i];
        	}
		
      		//DBG_PRINTF("%s\n",msg.c_str());

			if((info->index + len) == info->len){
				DBG_PRINTF("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        		if(info->final){
        			DBG_PRINTF("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
        		}
      		}
#endif
      	}
    }
}

#endif // #if UseWebSocket == true


#if UseServerSideEvent == true
AsyncEventSource sse(SSE_PATH);

void sseConnect(AsyncEventSourceClient *client){
	String json;
	bmWeb.getCurrentStatus(json,true);
	client->send(json.c_str());
}

#endif


void broadcastMessage(String msg)
{
#if UseWebSocket == true
	ws.textAll(msg);
#endif

#if UseServerSideEvent == true
	sse.send(msg.c_str());
#endif
}
void broadcastMessage(const char* msg)
{
#if UseWebSocket == true
	ws.textAll(msg);
#endif

#if UseServerSideEvent == true
	sse.send(msg);
#endif
}

void bmwEventHandler(BrewManiacWeb* bmw, BmwEventType event)
{
	if(event==BmwEventAutomationChanged){
		// request reload automation
		broadcastMessage("{\"update\":\"recipe\"}");
	}else if(event==BmwEventSettingChanged){
		// request reload setting
		broadcastMessage("{\"update\":\"setting\"}");
	}else if(event==BmwEventStatusUpdate || event==BmwEventButtonLabel){
		String json;
//		if( event==BmwEventButtonLabel) DebugOut("Buttons\n");
		bmw->getCurrentStatus(json);
		broadcastMessage(json);

	}else if(event==BmwEventBrewEvent){
		String json;
		bmw->getLastEvent(json);
		broadcastMessage(json);		
	}else if(event==BmwEventPwmChanged){
		String json;
		bmw->getSettingPwm(json);
		broadcastMessage(json);

	}else if(event==BmwEventSettingTemperatureChanged){
		String json;
		bmw->getSettingTemperature(json);
		broadcastMessage(json);
	}
}

#if ResponseAppleCNA == true

class AppleCNAHandler: public AsyncWebHandler 
{
public:
	AppleCNAHandler(){}
	void handleRequest(AsyncWebServerRequest *request){
		request->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
	}
	bool canHandle(AsyncWebServerRequest *request){
		String host=request->host();
		//DBG_PRINTF("Request host:");
		//DBG_PRINTF(host.c_str());
		//DBG_PRINTF("\n");
  		if(host.indexOf(String("apple")) >=0
  		|| host.indexOf(String("itools")) >=0 
  		|| host.indexOf(String("ibook")) >=0 
  		|| host.indexOf(String("airport")) >=0 
  		|| host.indexOf(String("thinkdifferent")) >=0 
  		|| host.indexOf(String("akamai")) >=0 ){
  			return true;
  		}
  		return false;
	}
};

AppleCNAHandler appleCNAHandler;

#endif //#if ResponseAppleCNA == true

HttpUpdateHandler httpUpdateHandler(FIRMWARE_UPDATE_URL,JS_UPDATE_URL);

bool testSPIFFS(void)
{
	File vf=SPIFFS.open("/BME_TestWrite.t","w+");
	if(!vf){
  		DebugOut("Failed to open file for test\n");
		return false;
	}
	const char *str="test string\n";
	vf.print(str);
	vf.close();
	DebugOut("Close file writing\n");
	
	File rf=SPIFFS.open("/BME_TestWrite.t","r");
	if(!rf){
  		DebugOut("Failed to open file for test for reading\n");
		return false;
	}
//	String c=rf.readString();
	String c=rf.readStringUntil('\n');
	rf.close();
	
	DebugOut("Reading back data:");
	DebugOut(c.c_str());
	return true;
}
#define PROFILING false
#if PROFILING == true
unsigned long _profileMaximumLoop=0;
unsigned long _profileLoopBegin;
#endif

void displayIP(bool apmode){
	IPV4Address ip;
	if(apmode){
		ip.dword = WiFi.softAPIP();
		bmWeb.setIp(ip.bytes,true);
	}else{
		ip.dword = WiFi.localIP();
		bmWeb.setIp(ip.bytes);
	}
}

void setup(void){
	//0. initilze debug port
	#if SerialDebug == true
  	DebugPort.begin(115200);
  	DebugOut("Start..\n");
  	DebugPort.setDebugOutput(true);
  	#endif

	#if SwapSerial == true && UseSoftwareSerial != true
	Serial.swap();
	#endif

	//1.Initialize file system
	//start SPI Filesystem
  	if(!SPIFFS.begin()){
  		// TO DO: what to do?
  		DebugOut("SPIFFS.being() failed");
  	}else{
  		DebugOut("SPIFFS.being() Success");
  	}

	//1b. load nsetwork conf
	networkConfig.loadSetting();

	//DBG_PRINTF("hostname:%s, user:%s, pass:%s, secured:%d\n",_gHostname,_gUsername,_gPassword,_gSecuredAccess);

	// 2. start brewmaniac part, so that LCD will be ON.
	brewmaniac_setup();

		  	
	//3. Start WiFi  
	WiFiSetup.begin(_gHostname);

  	DebugOut("Connected! IP address: ");
  	DebugOut(WiFi.localIP());
	if (!MDNS.begin(_gHostname)) {
		DebugOut("Error setting mDNS responder");
	}	
	// TODO: SSDP responder
	if(WiFiSetup.isApMode())
		TimeKeeper.begin(false);
	else
		TimeKeeper.begin("time.nist.gov","time.windows.com","de.pool.ntp.org");

	//4. check version
	bool forcedUpdate;
	String jsVersion;
	File vf=SPIFFS.open("/version.txt","r");
	if(!vf){
		jsVersion="0";
  		DebugOut("Failed to open version.txt");
  		forcedUpdate=true;

	}else{
		jsVersion=vf.readString();
		DebugOut("Version.txt:");
		DebugOut(jsVersion.c_str());
		forcedUpdate=false;
	}
	
	//5. setup Web Server
	if(forcedUpdate){
		//5.1 forced to update
		httpUpdateHandler.setUrl("/");
		httpUpdateHandler.setVersion(BME8266_VERSION,jsVersion);
		server.addHandler(&httpUpdateHandler);
	}else{
		//5.1 HTTP Update page
		httpUpdateHandler.setUrl(ONLINE_UPDATE_PATH);
		httpUpdateHandler.setVersion(BME8266_VERSION,jsVersion);
		httpUpdateHandler.setCredential(_gUsername,_gPassword);
		server.addHandler(&httpUpdateHandler);
		

		//5.2 Normal serving pages 
		//5.2.1 status report through SSE
#if UseWebSocket == true
		ws.onEvent(onWsEvent);
  		server.addHandler(&ws);
#endif

#if	UseServerSideEvent == true
  		sse.onConnect(sseConnect);
  		server.addHandler(&sse);
#endif 
		server.addHandler(&networkConfig);
		server.addHandler(&bmwHandler);

#if ResponseAppleCNA == true
		if(WiFiSetup.isApMode())
			server.addHandler(&appleCNAHandler);
#endif 
		server.addHandler(&logHandler);
		//5.2.2 SPIFFS is part of the serving pages
		//securedAccess need additional check
		// server.serveStatic("/", SPIFFS, "/","public, max-age=259200"); // 3 days
	}
    
	server.on("/system",[](AsyncWebServerRequest *request){
		FSInfo fs_info;
		SPIFFS.info(fs_info);
		request->send(200,"","totalBytes:" +String(fs_info.totalBytes) +
		" usedBytes:" + String(fs_info.usedBytes)+" blockSize:" + String(fs_info.blockSize)
		+" pageSize:" + String(fs_info.pageSize)
		+" heap:"+String(ESP.getFreeHeap()));
	});
#if PROFILING == true
	server.on("/profile",[](AsyncWebServerRequest *request){
		request->send(200,"","max loop time:" +String(_profileMaximumLoop));
	});
#endif	
	// 404 NOT found.
  	//called when the url is not defined here
	server.onNotFound([](AsyncWebServerRequest *request){
		request->send(404);
	});
	
	//6. start Web server
	server.begin();
	DebugOut("HTTP server started\n");

	MDNS.addService("http", "tcp", 80);
	
	// 7. try to connnect Arduino
  	bmWeb.onEvent(bmwEventHandler);
	
	// 8. start WEB update pages.	
	ESPUpdateServer_setup(_gUsername,_gPassword);

	// 9. display IP
	displayIP(WiFiSetup.isApMode());
	
	DebugOut("End Setup\n");
}

#define SystemStateOperating 0
#define SystemStateRestartPending 1
#define SystemStateWaitRestart 2

#define TIME_RESTART_TIMEOUT 3000

bool _disconnectBeforeRestart;
static unsigned long _time;
byte _systemState=SystemStateOperating;
void requestRestart(bool disc)
{
	_disconnectBeforeRestart=disc;
	_systemState =SystemStateRestartPending;
}

#define IS_RESTARTING (_systemState!=SystemStateOperating)


void loop(void){
#if PROFILING == true
	_profileLoopBegin = millis();
#endif

	ESPUpdateServer_loop();
  	bmWeb.loop();

  	brewmaniac_loop();
 
	if(WiFiSetup.stayConnected()){
		if(WiFiSetup.isApMode()){
			TimeKeeper.setInternetAccessibility(false);
			displayIP(true);
		}else{
			if(WiFi.status() != WL_CONNECTED){
				uint8_t nullip[]={0,0,0,0};
				bmWeb.setIp(nullip);
			}else{
				displayIP(false);
			}
		}
	}
  	
  	httpUpdateHandler.runUpdate();

  	if(_systemState ==SystemStateRestartPending){
	  	_time=millis();
	  	_systemState =SystemStateWaitRestart;
  	}else if(_systemState ==SystemStateWaitRestart){
  		if((millis() - _time) > TIME_RESTART_TIMEOUT){
  			if(_disconnectBeforeRestart){
  				WiFi.disconnect();
  				WiFiSetup.setAutoReconnect(false);
  				delay(1000);
  			}
//  			ESP.restart();
  		}
  	}


#if PROFILING == true
	unsigned long thisloop = millis() - _profileLoopBegin;
	if(thisloop > _profileMaximumLoop) _profileMaximumLoop = thisloop;
#endif

}


