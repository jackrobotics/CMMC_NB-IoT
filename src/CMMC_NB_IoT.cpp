#include "CMMC_NB_IoT.h"

#ifdef DEBUG
#define debugPrintLn(...) { if (!this->_disableDiag && this->_diagStream) this->_diagStream->println(__VA_ARGS__); }
#define debugPrint(...) { if (!this->_disableDiag && this->_diagStream) this->_diagStream->print(__VA_ARGS__); }
#warning "Debug mode is ON"
#else
#define debugPrintLn(...)
#define debugPrint(...)
#endif

#define TIMEOUT_5s 5
#define TIMEOUT_10s 10

CMMC_NB_IoT::CMMC_NB_IoT(Stream *s) {
  this->_modemSerial = s;
  this->_user_onDeviceReboot_cb = [](void) -> void { };
  this->_user_onConnecting_cb = [](void) -> void { };
  this->_user_onConnected_cb = [](void) -> void { };
  this->_user_onDeviceReady_cb = [](DeviceInfo d) -> void { };
  this->_user_onResponse_cb = [](UDPReceive d) -> void { };
  this->_socketsMap = HashMap<String, Udp*, HASH_SIZE>();
  this->_deviceNeverConnected = true;
};

CMMC_NB_IoT::~CMMC_NB_IoT() {

};

bool CMMC_NB_IoT::callCommand(String at, uint8_t timeout, int retries, char *outStr) {
  int r = 0;
  int ok = false;
  while (r < retries && !ok) {
    if (this->_writeCommand(at.c_str(), timeout, outStr) == 1) {
      ok = true;
    }
    else {
      r++;
      debugPrintLn(at.c_str());
      delay(500);
    }
    delay(50);
  }
  delay(100);
  return ok;
}

void CMMC_NB_IoT::activate() {
  while (!callCommand(F("AT+CGATT=1"), TIMEOUT_10s, 100));
}

void CMMC_NB_IoT::begin(Stream *s, uint8_t timeout) {
  if (s != 0) {
    this->_modemSerial = s;
  } 
  s->setTimeout(timeout);
  debugPrintLn("Debug mode is ON");
  while (!callCommand(F("AT"), TIMEOUT_10s));
  while (!callCommand(F("AT+NRB"), TIMEOUT_10s));
  _user_onDeviceReboot_cb();
  while (!callCommand(F("AT+CFUN=1"), TIMEOUT_10s));
  while (!callCommand(F("AT+CGSN=1"), TIMEOUT_5s, 5, this->deviceInfo.imei));
  while (!callCommand(F("AT+CGMR"), TIMEOUT_5s, 5, this->deviceInfo.firmware));
  while (!callCommand(F("AT+CIMI"), TIMEOUT_5s, 5, this->deviceInfo.imsi));
  this->_user_onDeviceReady_cb(this->deviceInfo);
}

void CMMC_NB_IoT::loop() {
  static char buf[40];
  this->_loopTimer.every_ms(1000, []() { });
  if (this->_deviceNeverConnected) {
    this->_writeCommand(F("AT+CGATT?"), 10L, buf, 1);
    bool nbNetworkConnected = String(buf).indexOf(F("+CGATT:1")) != -1;
    if (nbNetworkConnected) {
      this->_deviceNeverConnected = false;
      this->_user_onConnected_cb();
    }
    else {
      this->_user_onConnecting_cb();
    }
  }
  else
  {
    char responseBuf[255];
    this->_writeCommandRaw(F("AT+NSORF=0,100"), 1, responseBuf, 1);
    String response = String(responseBuf);
    // Serial.print("[DATE]");Serial.println(response);
    if(String(responseBuf).indexOf(F("+NSONMI:"))!=-1)
    {
      this->_writeCommandRaw(F("AT+NSORF=0,100"), 1, responseBuf, 1);
      response = String(responseBuf);
    }
    if(response.indexOf(F(","))!=-1)
    {
      int index1 = response.indexOf(F(","));
      int index2 = response.indexOf(F(","),index1+1);
      int index3 = response.indexOf(F(","),index2+1);
      int index4 = response.indexOf(F(","),index3+1);
      int index5 = response.indexOf(F(","),index4+1);
      int index6 = response.indexOf(F("\r"));

      this->_UDPReceive.socket = response.substring(0,index1).toInt();
      this->_UDPReceive.ip_address = response.substring(index1+1,index2);
      this->_UDPReceive.port = response.substring(index2+1,index3).toInt();
      this->_UDPReceive.length = response.substring(index3+1,index4).toInt();
      this->_UDPReceive.data=this->toString(response.substring(index4+1,index5));
      this->_UDPReceive.remaining_length = response.substring(index5+1,index6).toInt();
      responseBuf[0]='\0';
      response = F("");
      this->_user_onResponse_cb(this->_UDPReceive);
    }
  }
} 

String CMMC_NB_IoT:: toString(String dat)
{
  String str="";
  for(int x=0;x<dat.length();x+=2)
  {
      char c =  char_to_byte(dat[x])<<4 | char_to_byte(dat[x+1]);
    str += c;
  }
  return(str);
}

char CMMC_NB_IoT:: char_to_byte(char c)
{
  if((c>='0')&&(c<='9'))
  {
    return(c-0x30);
  }
  if((c>='A')&&(c<='F'))
  {
    return(c-55);
  }
}

Stream* CMMC_NB_IoT::getModemSerial() {
  return this->_modemSerial;
}

bool CMMC_NB_IoT::sendMessage(String msg, uint8_t socketId) {
  return this->_socketsMap.valueAt(socketId)->sendMessage(msg);
}

bool CMMC_NB_IoT::sendMessage(uint8_t *msg, size_t len, uint8_t socketId) {
  return this->_socketsMap.valueAt(socketId)->sendMessage(msg, len);
}

int CMMC_NB_IoT::createUdpSocket(String hostname, uint16_t port, UDPConfig config) {
  int idx = this->_socketsMap.size();
  String hashKey = String(hostname + ":" + port);
  char resBuffer[40];
  char buffer[40];
  sprintf(buffer, "AT+NSOCR=DGRAM,17,%d,%d", port, config);
  if (callCommand(buffer, 10, 5, resBuffer)) {
    if (!this->_socketsMap.contains(hashKey)) {
      debugPrint("socket id=");
      debugPrint(idx);
      debugPrint(" has been created. len=");
      debugPrintLn(this->_socketsMap.size());
      this->_socketsMap[hashKey] = new Udp(hostname, port, idx, this);
    }
    else {
      debugPrint("existing socket id=");
      debugPrintLn(hashKey);
    }
  }
  else {
    debugPrintLn("Create UDP Socket failed.\n");
    idx = -1;
  }
  return idx;
};

void CMMC_NB_IoT::onDeviceReady(deviceInfoCb_t cb) {
  this->_user_onDeviceReady_cb = cb;
}

void CMMC_NB_IoT::onConnecting(voidCb_t cb) {
  this->_user_onConnecting_cb = cb;
}

void CMMC_NB_IoT::onConnected(voidCb_t cb) {
  this->_user_onConnected_cb = cb;
}

void CMMC_NB_IoT::onDeviceReboot(voidCb_t cb) {
  this->_user_onDeviceReboot_cb = cb;
}

void CMMC_NB_IoT::onResponse(responseCb_t cb) { //add
  this->_user_onResponse_cb = cb;
}

bool CMMC_NB_IoT::_writeCommand(String at, uint32_t timeoutS, char *outStr, bool silent) {
  at.trim();
  timeoutS *= 1000L;
  uint32_t startMs = millis();
  uint32_t nextTimeout = startMs + timeoutS;
  bool reqSuccess = 0;
  if (!silent) {
    debugPrint(">> ");
    debugPrint(at.c_str());
  }
  // this->_modemSerial->print(at.c_str());
  this->_modemSerial->write(at.c_str(), at.length());
  this->_modemSerial->write('\r');
  String nbSerialBuffer = "@";
  while (1) {
    if (this->_modemSerial->available()) {
      String response = this->_modemSerial->readStringUntil('\n');
      response.trim();
      nbSerialBuffer += response;
      if (response.indexOf(F("OK")) != -1) {
        if (!silent) {
          String out = String(F(" (")) + String(millis() - startMs) + F("ms)");
          debugPrintLn(out.c_str());
        }
        if (outStr != NULL) {
          strcpy(outStr, nbSerialBuffer.c_str());
        }
        reqSuccess = 1;
        break;
      }
      else if (response.indexOf(F("ERROR")) != -1) {
        reqSuccess = 0;
        break;
      }
    }
    if ((millis() > nextTimeout) ) {
      nextTimeout = + timeoutS;
      reqSuccess = 0;
      debugPrintLn("Wait timeout..");
      debugPrintLn(nbSerialBuffer.c_str());
      nbSerialBuffer = "@";
      break;
    }
    else {
      delay(10);
    }
  }
  return reqSuccess;
}

bool CMMC_NB_IoT::_writeCommandRaw(String at, uint32_t timeoutS, char *outStr, bool silent) {
  at.trim();
  timeoutS *= 1000L;
  uint32_t startMs = millis();
  uint32_t nextTimeout = startMs + timeoutS;
  bool reqSuccess = 0;
  if (!silent) {
    debugPrint(">> ");
    debugPrint(at.c_str());
  }
  // this->_modemSerial->print(at.c_str());
  this->_modemSerial->write(at.c_str(), at.length());
  this->_modemSerial->write('\r');
  String nbSerialBuffer = "";
  while (1) {
    if (this->_modemSerial->available()) {
      String response = this->_modemSerial->readStringUntil('\n');
      response.trim();
      // Serial.print("[response]");Serial.println(response);
      nbSerialBuffer += response;
      if (response.length()>10 && response.indexOf(F(",")) != -1) {
        if (!silent) {
          String out = String(F(" (")) + String(millis() - startMs) + F("ms)");
          debugPrintLn(out.c_str());
        }
        if (outStr != NULL) {
          strcpy(outStr, response.c_str());
        }
        reqSuccess = 1;
        break;
      }
      else if (response.indexOf(F("ERROR")) != -1) {
        reqSuccess = 0;
        break;
      }
    }
    if ((millis() > nextTimeout) ) {
      nextTimeout = + timeoutS;
      reqSuccess = 0;
      debugPrintLn("Wait timeout..");
      debugPrintLn(nbSerialBuffer.c_str());
      nbSerialBuffer = "@";
      break;
    }
    else {
      delay(10);
    }
  }
  return reqSuccess;
}
