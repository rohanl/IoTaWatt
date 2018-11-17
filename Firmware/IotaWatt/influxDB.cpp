#include "IotaWatt.h"
#include "xbuf.h"

bool      influxStarted = false;                    // True when Service started
bool      influxStop = false;                       // Stop the influx service
bool      influxRestart = false;                    // Restart the influx service
bool      influxLogHeap = false;                    // Post a heap size measurement (diag)
uint32_t  influxLastPost = 0;                       // Last acknowledge post for status report

    // Configuration settings

uint16_t  influxBulkSend = 1;                       
uint16_t  influxPort = 8086;
int32_t   influxRevision = -1;                      // Revision control for dynamic config
uint32_t  influxBeginPosting = 0;                   // Begin date specified in config
char*     influxUser = nullptr;
char*     influxPwd = nullptr; 
char*     influxRetention = nullptr;
char*     influxMeasurement = nullptr;
char*     influxFieldKey = nullptr; 
char*     influxURL = nullptr;
char*     influxDataBase = nullptr;
influxTag* influxTagSet = nullptr;  
ScriptSet* influxOutputs;      

uint32_t influxService(struct serviceBlock* _serviceBlock){

      // This is a standard IoTaWatt Service operating as a state machine.

  enum   states {initialize,        // Basic startup of the service - one time
                 queryLastPostTime, // Setup to query for last post time of each measurement
                 queryLast,         // Query last() for this measurement
                 queryLastWait,     // wait for [async] query to complete
                 getLastRecord,     // Read the logRec and prep the context for logging
                 post,              // Add a measurement to the reqData xbuf
                 sendPost,          // Send the accumulated measurements
                 waitPost};         // Wait for the [async] post to complete

  static states state = initialize;
  static IotaLogRecord* logRecord = nullptr;
  static IotaLogRecord* oldRecord = nullptr;
  static uint32_t lastRequestTime = 0;          // Time of last measurement in last or current request
  static uint32_t lastBufferTime = 0;           // Time of last measurement reqData buffer
  static uint32_t UnixNextPost = UTCTime();    // Next measurement to be posted
  static xbuf reqData;                          // Current request buffer
  static uint32_t reqUnixtime = 0;              // First measurement in current reqData buffer
  static int  reqEntries = 0;                   // Number of measurement intervals in current reqData
  static int16_t retryCount = 0;                // HTTP error count
  static asyncHTTPrequest* request = nullptr;   // -> instance of asyncHTTPrequest
  static uint32_t postFirstTime = UTCTime();   // First measurement in outstanding post request
  static uint32_t postLastTime = UTCTime();    // Last measurement in outstanding post request
  static size_t reqDataLimit = 4000;            // transaction yellow light size
  static uint32_t HTTPtoken = 0;                // HTTP resource reservation token
  static Script* script = nullptr;              // current Script


  trace(T_influx,0);                            // Announce entry
      
          // Handle current state

  switch(state){

//********************************************************* Initialize **************************************
    case initialize: {
      trace(T_influx,2);
      if(influxStop){
        influxStarted = false;
        return 0;
      }

          // We post from the log, so wait if not available.          

      if(!currLog.isOpen()){                  
        return UTCTime() + 5;
      }
      log("influxDB: started, url=%s:%d, db=%s, interval=%d", influxURL, influxPort,
              influxDataBase, influxDBInterval);
      state = queryLastPostTime;
      return 1;
    }
 
 //********************************************************* queryLastPostTime *****************************
    case queryLastPostTime:{
      trace(T_influx,3);
      influxLastPost = influxBeginPosting;
      script = influxOutputs->first();
      trace(T_influx,4);
      state = queryLast;
      return 1;
    }

  //********************************************************* queryLast *****************************    

    case queryLast:{

          // Make sure wifi is connected and there is a resource available.

      if( ! WiFi.isConnected()) return UTCTime() + 1;
       
      HTTPtoken = HTTPreserve(T_influx);
      if( ! HTTPtoken){
        return UTCTime() + 1;
      }

          // Create a new request

      if( ! request) request = new asyncHTTPrequest;
      request->setTimeout(5);
      request->setDebug(false);
      {
        char URL[100];
        sprintf_P(URL, PSTR("%s:%d/query"),influxURL,influxPort);
        request->open("POST", URL);
      }
      
      if(influxUser && influxPwd){
        xbuf xb;
        xb.printf("%s:%s", influxUser, influxPwd);
        base64encode(&xb);
        String auth = "Basic ";
        auth += xb.readString(xb.available());
        request->setReqHeader("Authorization", auth.c_str()); 
      }
      trace(T_influx,4);
      request->setReqHeader("Content-Type","application/x-www-form-urlencoded");
      reqData.flush();
      reqData.printf_P(PSTR("db=%s&epoch=s&q=SELECT LAST(%s) FROM %s"), influxDataBase,
            influxVarStr(influxFieldKey, script).c_str(),
            influxVarStr(influxMeasurement, script).c_str());
      if(influxTagSet){
        trace(T_influx,41);
        influxTag* tag = influxTagSet;
        reqData.printf_P(PSTR(" WHERE %s=\'%s\'"), tag->key, influxVarStr(tag->value, script).c_str());
      }

          // Send the request

      request->send(&reqData, reqData.available());
      trace(T_influx,42);
      state = queryLastWait;
      return 1;
    }

    case queryLastWait: {

          // If not completed, return to wait.

      trace(T_influx,5); 
      if(request->readyState() != 4){
        return 1; 
      }
      HTTPrelease(HTTPtoken);
      String response = request->responseText();
      int HTTPcode = request->responseHTTPcode();
      delete request;
      request = nullptr;
      if(HTTPcode != 200){
        log("influxDB: last entry query failed: %d", HTTPcode);
        if(HTTPcode > 204){
          Serial.print(response);
        }
        influxStop = true;
        state = post;
        return 1;
      } 
      trace(T_influx,5);
      
            // Json parse the response to get the columns and values arrays
            // and extract time

      DynamicJsonBuffer Json;
      JsonObject& results = Json.parseObject(response);
      if(results.success()){
        JsonArray& columns = results["results"][0]["series"][0]["columns"];
        JsonArray& values = results["results"][0]["series"][0]["values"][0];
        if(columns.success() && values.success()){
          for(int i=0; i<columns.size(); i++){
            if(strcmp("time",columns[i].as<char*>()) == 0){
              if(values[i].as<unsigned long>() > influxLastPost){
                influxLastPost = values[i].as<unsigned long>();
              }
              break;
            }
          }
        }
      }
      
      script = script->next();
      if(script){
        state = queryLast;
        return 1;
      }

      if(influxLastPost == 0){
        influxLastPost = UTCTime();
      }
      influxLastPost -= influxLastPost % influxDBInterval;
      log("influxDB: Start posting at %s", localDateString(influxLastPost + influxDBInterval).c_str());
      delete request;
      request = nullptr;
      state = getLastRecord;
      return 1;
    }

    case getLastRecord: {
      trace(T_influx,6);   
      if( ! oldRecord){
        oldRecord = new IotaLogRecord;
      }
      oldRecord->UNIXtime = influxLastPost;      
      currLog.readKey(oldRecord);
      trace(T_influx,6);

          // Assume that record was posted (not important).
          // Plan to start posting one interval later
      
      UnixNextPost = oldRecord->UNIXtime + influxDBInterval - (oldRecord->UNIXtime % influxDBInterval);
      
          // Advance state.

      reqData.flush();
      reqEntries = 0;
      state = post;
      return UnixNextPost;
    }

    
    case post: {
      trace(T_influx,7);

          // If stop requested, do it now.

      if(influxStop) {
        if(request && request->readyState() < 4) return 1;
        trace(T_influx,71);
        log("influxDB: Stopped. Last post %s", localDateString(influxLastPost).c_str());
        trace(T_influx,72);    
        delete oldRecord;
        oldRecord = nullptr;
        delete logRecord;
        logRecord = nullptr;
        delete request;
        request = nullptr;
        reqData.flush();
        delete[] influxUser;
        influxUser = nullptr;
        delete[] influxPwd;
        influxPwd = nullptr; 
        delete[] influxRetention;
        influxRetention = nullptr;
        delete[] influxMeasurement;
        influxMeasurement = nullptr;
        delete[] influxFieldKey;
        influxFieldKey = nullptr; 
        delete[] influxURL;
        influxURL = nullptr;
        delete[] influxDataBase;
        influxDataBase = nullptr;
        delete influxTagSet;
        influxTagSet = nullptr;  
        delete influxTagSet;
        influxOutputs;      
        influxStarted = false;
        state = initialize;
        return 0;
      }

          // If not enough entries for bulk-send, come back in one second;

      if(((currLog.lastKey() - influxLastPost) / influxDBInterval + reqEntries) < influxBulkSend){
        return UTCTime() + 1;
      } 

          // If buffer isn't full,
          // add another measurement.

      if(reqData.available() < reqDataLimit && UnixNextPost <= currLog.lastKey()){  

            // Read the next log record.

        if( ! logRecord){
          logRecord = new IotaLogRecord;            
        }
        trace(T_influx,7);
        logRecord->UNIXtime = UnixNextPost;
        currLog.readKey(logRecord);
        trace(T_influx,7);
        
            // Compute the time difference between log entries.
            // If zero, don't bother.
            
        double elapsedHours = logRecord->logHours - oldRecord->logHours;
        if(elapsedHours == 0){
          if(currLog.readNext(logRecord) == 0) {
            UnixNextPost = logRecord->UNIXtime - (logRecord->UNIXtime % influxDBInterval);
          }
          UnixNextPost += influxDBInterval;
          return UnixNextPost;  
        }
            
            // Build the request string.
            // values for each channel are (delta value hrs)/(delta log hours) = period value.
            // Update the previous (Then) buckets to the most recent values.
      
        script = influxOutputs->first();
        trace(T_influx,7);
        while(script){
          double value = script->run(oldRecord, logRecord, elapsedHours);
          if(value == value){
            reqData.write(influxVarStr(influxMeasurement, script));
            if(influxTagSet){
              trace(T_influx,71);
              influxTag* tag = influxTagSet;
              while(tag){
                reqData.printf_P(PSTR(",%s=%s"), tag->key, influxVarStr(tag->value, script).c_str());
                tag = tag->next;
              }
            }
            reqData.printf_P(PSTR(" %s=%.*f"), influxVarStr(influxFieldKey, script).c_str(), script->precision(), value);
            reqData.printf(" %d\n", UnixNextPost);
          }
          script = script->next();
        }
        trace(T_influx,7);
        delete oldRecord;
        oldRecord = logRecord;
        logRecord = nullptr;
        
        trace(T_influx,7);  
        reqEntries++;
        lastBufferTime = UnixNextPost;
        UnixNextPost +=  influxDBInterval - (UnixNextPost % influxDBInterval);
      }

            // If there's no request pending and we have bulksend entries,
            // set to post.

      if((( ! request || request->readyState() == 4) && HTTPrequestFree) && 
          (reqEntries >= influxBulkSend || reqData.available() >= reqDataLimit)){
        state = sendPost;
        if(influxLogHeap && heapMsPeriod != 0){
          reqData.printf_P(PSTR("heap"));
          influxTag* tag = influxTagSet;
          if(tag){
            Script* script = influxOutputs->first();
            reqData.printf_P(PSTR(",%s=%s"), tag->key, influxVarStr(tag->value, script).c_str());
          }
          reqData.printf_P(PSTR(" value=%d %d\n"), (uint32_t)heapMs / heapMsPeriod, UTCTime());
          heapMs = 0.0;
          heapMsPeriod = 0;
        }       
      }
      return (UnixNextPost > UTCTime()) ? UTCTime() + 1 : 1;
    }

    case sendPost: {
      trace(T_influx,8);
      if( ! WiFi.isConnected()){
        return UTCTime() + 1;
      }
      HTTPtoken = HTTPreserve(T_influx);
      if( ! HTTPtoken){
        return 1;
      }
      if( ! request){
        request = new asyncHTTPrequest;
      }
      request->setTimeout(3);
      request->setDebug(false);
      if(request->debug()){
        Serial.println(ESP.getFreeHeap()); 
        Serial.println(datef(localTime(),"hh:mm:ss"));
        Serial.println(reqData.peekString(reqData.available()));
      }
      trace(T_influx,8);
      {
        char URL[128];
        size_t len = sprintf_P(URL, PSTR("%s:%d/write?precision=s&db=%s"), influxURL, influxPort, influxDataBase);
        if(influxRetention){
          sprintf(URL+len,"&rp=%s", influxRetention);
        }
        if( ! request->open("POST", URL)){
          HTTPrelease(HTTPtoken);
          delete request;
          request = nullptr; 
          state = getLastRecord;
          return 1;
        }
      }
      trace(T_influx,8);
      request->setReqHeader("Content-Type","application/x-www-form-urlencoded");
      trace(T_influx,8);
      if(influxUser && influxPwd){
        xbuf xb;
        xb.printf("%s:%s", influxUser, influxPwd);
        base64encode(&xb);
        String auth = "Basic ";
        auth += xb.readString(xb.available());
        request->setReqHeader("Authorization", auth.c_str()); 
      }
      request->send(&reqData, reqData.available());
      reqEntries = 0;
      lastRequestTime = lastBufferTime;
      state = waitPost;
      return 1;
    } 

    case waitPost: {
      trace(T_influx,9);
      if(request && request->readyState() == 4){
        HTTPrelease(HTTPtoken);
        trace(T_influx,9);
        if(request->responseHTTPcode() != 204){
          if(++retryCount == 10){
            log("influxDB: Post Failed: %d", request->responseHTTPcode());
          }
          delete request;
          request = nullptr; 
          state = getLastRecord;
          return 1;
        }
        trace(T_influx,9);
        retryCount = 0;
        influxLastPost = lastRequestTime; 
        state = post;
        trace(T_influx,9);
      }
      return 1;
    }   
  }

  return 1;
}


bool influxConfig(const char* configObj){
  trace(T_influxConfig,0);
  DynamicJsonBuffer Json;
  JsonObject& config = Json.parseObject(configObj);
  trace(T_influxConfig,0);
  if( ! config.success()){
    log("influxDB: Json parse failed.");
    return false;
  }
  int revision = config["revision"];
  if(revision == influxRevision){
    return true;
  }
  trace(T_influxConfig,0);
  influxRevision = revision;
  influxStop = config["stop"].as<bool>();
  influxLogHeap = config["heap"].as<bool>();
  String URL = config.get<String>("url");
  if(URL.substring(0,7).equalsIgnoreCase("http://")){
    URL.remove(0,7);
  } 
  else if(URL.substring(0,8).equalsIgnoreCase("https://")){
    URL.remove(0,8);
  }  
  if(URL.indexOf(":") > 0){
    influxPort = URL.substring(URL.indexOf(":")+1).toInt();
    URL.remove(URL.indexOf(":"));
  }
  delete[] influxURL;
  influxURL = charstar(URL.c_str());
  trace(T_influxConfig,4);
  delete[] influxDataBase;
  influxDataBase = charstar(config.get<char*>("database"));
  influxDBInterval = config.get<unsigned int>("postInterval");
  influxBulkSend = config.get<unsigned int>("bulksend");
  if(influxBulkSend <1) influxBulkSend = 1;
  trace(T_influxConfig,5);
  delete[] influxUser;
  influxUser = charstar(config.get<const char*>("user"));
  delete[] influxPwd;
  influxPwd = charstar(config.get<const char*>("pwd"));  
  trace(T_influxConfig,7);
  delete[] influxRetention;
  influxRetention = charstar(config.get<const char*>("retp"));
  trace(T_influxConfig,7);
  delete[] influxMeasurement;
  influxMeasurement = charstar(config.get<const char*>("measurement"));
  if( ! influxMeasurement){
    influxMeasurement = charstar("$name");
  }
  trace(T_influxConfig,7);
  delete[] influxFieldKey;;
  influxFieldKey = charstar(config.get<const char*>("fieldkey"));
  if( ! influxFieldKey){
    influxFieldKey = charstar("value");
  }
  trace(T_influxConfig,7);
  influxBeginPosting = config.get<uint32_t>("begdate");
  
  delete influxTagSet;
  influxTagSet = nullptr;
  JsonArray& tagset = config["tagset"];
  if(tagset.success()){
    trace(T_influxConfig,8);
    for(int i=tagset.size(); i>0;){
      i--;
      influxTag* tag = new influxTag;
      tag->next = influxTagSet;
      influxTagSet = tag;
      tag->key = charstar(tagset[i]["key"].as<const char*>());
      tag->value = charstar(tagset[i]["value"].as<const char*>());
    }
  }

  delete influxOutputs;
  influxOutputs = nullptr;
  JsonVariant var = config["outputs"];
  if(var.success()){
    trace(T_influxConfig,9);
    influxOutputs = new ScriptSet(var.as<JsonArray>()); 
  }
  if( ! influxStarted) {
    trace(T_influxConfig,10);
    NewService(influxService, T_influx);
    influxStarted = true;
  }
  return true;
}

String influxVarStr(const char* in, Script* script){
  String out;
  while(*in){ 
    if(memcmp(in,"$device",7) == 0){
      out += deviceName;
      in += 7;
    }
    else if(memcmp(in,"$name",5) == 0){
      out += script->name();
      in += 5;
    }
    else if(memcmp(in,"$units",6) == 0){
      out += script->getUnits();
      in += 6;
    }
    else {
      out += *(in++);
    }
  } 
  return out;
}

