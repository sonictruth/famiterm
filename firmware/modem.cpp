// -----------------------------------------------------------------------------
// WiFi/Ethernet Modem and Telnet Server
// Copyright (C) 2018 David Hansel
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
// -----------------------------------------------------------------------------
//
// fami-modem port (ESP32-S3), modified from the original:
// - The modem's terminal is `term`: input from the USB keyboard, output to the
//   Famicom screen and Serial. It runs in its own task on core 0.
// - Removed: ESP8266 / WT32-ETH01 support, serial line settings (AT#BDR,
//   line-speed throttling; the Famicom queue paces output instead), the web
//   configuration page, mDNS, the telnet server, and the status LED.
// - WiFi credentials are stored with Preferences instead of EEPROM, and the
//   "press C" setup prompt is printed once instead of every 2 seconds.
// - The telnet terminal type is "vt52", what the Famicom terminal emulates.

#include <WiFi.h>
#include <Preferences.h>
#include "modem.h"
#include "term.h"
#include "famicom-link.h"

#define TERMINAL_TYPE "vt52"

static Preferences prefs;
static String      wifiSsid, wifiKey;

static WiFiClient    modemClient;
static unsigned long prevCharTime = 0;
static uint8_t       modemEscapeState = 0, modemExtCodes = 0, modemReg[256];
static bool          modemCommandMode = true, modemEcho = true, modemQuiet = false, modemVerbose = true;

static const unsigned int linespeeds[] = {0, 75, 110, 300, 600, 1200, 2400, 4800, 7200, 9600, 12000, 14400};
#define NSPEEDS (sizeof(linespeeds)/sizeof(linespeeds[0]))
#define DEFAULT_LINESPEED 7  // 4800: about what the Famicom drains (~480 bytes/s)

#define REG_ESC            2
#define REG_CR             3
#define REG_LF             4
#define REG_BSP            5
#define REG_GUARDTIME     12
#define REG_LINESPEED     37
#define REG_CURLINESPEED  43

enum
  {
    E_OK = 0,
    E_CONNECT,
    E_RING,
    E_NOCARRIER,
    E_ERROR,
    E_CONNECT1200,
    E_NODIALTONE,
    E_BUSY,
    E_NOANSWER,
    E_CONNECT600,
    E_CONNECT2400,
    E_CONNECT4800,
    E_CONNECT9600,
    E_CONNECT14400,
    E_CONNECT19200
  };


struct TelnetStateStruct
{
  uint8_t cmdLen;
  uint8_t cmd[4];
  bool sendBinary;
  bool receiveBinary;
  bool receivedCR;
  bool subnegotiation;
  uint8_t sbLen;
  uint8_t sb[2]; // option and qualifier of the current sub-negotiation
} modemTelnetState;


static void readString(String &out, int buflen, bool password)
{
  out = "";
  while( true )
  {
    int c = term.read();
    if( c<0 )
      delay(10);
    else if( c==10 || c==13 )
      return;
    else if( (c==8 || c==127) && out.length()>0 )
      {
        term.print(char(8)); term.print(' '); term.print(char(8));
        out.remove(out.length()-1);
      }
    else if( c >= 32 && (int)out.length()<buflen-1 )
      {
        out += char(c);
        term.print(password ? '*' : char(c));
      }
  }
}


static void clearInputBuffer()
{
  delay(100);
  while( term.available()>0 ) { term.read(); delay(10); }
}


static void GetWiFiData(const char *msg)
{
  clearInputBuffer();

  term.println(msg);
  term.println("Press C to set up WiFi.\r\n");

  for(;;)
    {
      int c = term.read();
      if( c=='C' || c=='c' ) break;
      delay(10);
    }

  WiFi.disconnect();
  term.println("Scanning for networks...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    term.println("No networks found.");
  } else {
    term.print(n);
    term.println(" networks found:");
    for (int i = 0; i < n; ++i) {
      term.print(" ");
      term.print(WiFi.SSID(i));
      term.print(" (");
      term.print(WiFi.RSSI(i));
      term.print(")");
      term.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : "*");
      delay(10);
    }
  }
  term.println();
  WiFi.disconnect();

  do
    {
      clearInputBuffer();
      term.print("SSID: ");
      readString(wifiSsid, 256, false);
      term.println();
    }
  while( wifiSsid.length()==0 );

  clearInputBuffer();
  term.print("Password: ");
  readString(wifiKey, 256, true);
  term.println();

  prefs.putString("ssid", wifiSsid);
  prefs.putString("key", wifiKey);
}


static void PrintNetworkInfo()
{
  term.print("\r\nConnected to "); term.println(wifiSsid);
  term.print("IP "); term.println(WiFi.localIP());
}


static void resetTelnetState(struct TelnetStateStruct &s)
{
  s.cmdLen = 0;
  s.sendBinary = false;
  s.receiveBinary = false;
  s.receivedCR = false;
  s.subnegotiation = false;
  s.sbLen = 0;
}


static void resetModemState()
{
  const uint8_t regDefaults[] = {2, '+',
                                 3, '\r',
                                 4, '\n',
                                 5, 8,
                                 6, 2,
                                 7, 50,
                                 8, 2,
                                 9, 6,
                                 10, 14,
                                 11, 95,
                                 12, 50,
                                 25, 5,
                                 38, 20};

  for(int i=0; i<256; i++) modemReg[i] = 0;
  for(size_t i=0; i<sizeof(regDefaults); i+=2) modemReg[regDefaults[i]] = regDefaults[i+1];

  modemEscapeState = 0;
  modemExtCodes = 0;
  modemCommandMode = true;
  modemEcho = true;
  modemQuiet = false;
  modemVerbose = true;

  if( modemClient.connected() ) modemClient.stop();
}


static void printModemCR()
{
  term.print(char(modemReg[REG_CR]));
  if( modemVerbose ) term.print(char(modemReg[REG_LF]));
}


static void printModemResult(byte code)
{
  if( !modemQuiet )
    {
      if( modemVerbose )
        {
          printModemCR();
          switch( code )
            {
            case E_OK            : term.print("OK");             break;
            case E_CONNECT       : term.print("CONNECT");        break;
            case E_RING          : term.print("RING");           break;
            case E_NOCARRIER     : term.print("NO CARRIER");     break;
            case E_ERROR         : term.print("ERROR");          break;
            case E_CONNECT1200   : term.print("CONNECT 1200");   break;
            case E_NODIALTONE    : term.print("NO DIALTONE");    break;
            case E_BUSY          : term.print("BUSY");           break;
            case E_NOANSWER      : term.print("NO ANSWER");      break;
            case E_CONNECT600    : term.print("CONNECT 600");    break;
            case E_CONNECT2400   : term.print("CONNECT 2400");   break;
            case E_CONNECT4800   : term.print("CONNECT 4800");   break;
            case E_CONNECT9600   : term.print("CONNECT 9600");   break;
            case E_CONNECT14400  : term.print("CONNECT 14400");  break;
            default:
              {
                char buf[20];
                sprintf(buf, "ERROR%i", code);
                term.print(buf);
                break;
              }
            }
        }
      else
        term.print(code);

      printModemCR();
    }
}


static byte getCmdParam(char *cmd, int &ptr)
{
  byte res = 0;

  ptr++;
  if( isdigit(cmd[ptr]) )
    {
      int p = ptr;
      while( isdigit(cmd[ptr]) ) ptr++;
      char c = cmd[ptr];
      cmd[ptr] = 0;
      res = atoi(cmd+p);
      cmd[ptr] = c;
    }

  return res;
}


#define T_SE      240 // 0xf0
#define T_NOP     241 // 0xf1
#define T_BREAK   243 // 0xf3
#define T_GOAHEAD 249 // 0xf9
#define T_SB      250 // 0xfa
#define T_WILL    251 // 0xfb
#define T_WONT    252 // 0xfc
#define T_DO      253 // 0xfd
#define T_DONT    254 // 0xfe
#define T_IAC     255 // 0xff

#define TO_SEND_BINARY        0
#define TO_ECHO               1
#define TO_SUPPRESS_GO_AHEAD  3
#define TO_TERMINAL_TYPE      24

#define TTYPE_IS   0
#define TTYPE_SEND 1

static void sendTerminalType(WiFiClient &client)
{
  static const char type[] = TERMINAL_TYPE;
  uint8_t buf[sizeof(type) + 6], n = 0;
  buf[n++] = T_IAC;
  buf[n++] = T_SB;
  buf[n++] = TO_TERMINAL_TYPE;
  buf[n++] = TTYPE_IS;
  for(size_t i=0; type[i]; i++) buf[n++] = type[i];
  buf[n++] = T_IAC;
  buf[n++] = T_SE;
  client.write(buf, n);
}

// returns TRUE if input byte "b" is part of a telnet protocol sequence and should
// NOT be passed through to the terminal
static bool handleTelnetProtocol(uint8_t b, WiFiClient &client, struct TelnetStateStruct &state)
{
  // ---- handle incoming sub-negotiation sequences

  if( state.subnegotiation )
    {
      if( state.cmdLen==0 && b == T_IAC )
        {
          state.cmdLen = 1;
          return true;
        }

      if( state.cmdLen==1 && b == T_SE )
        {
          // RFC 1091: only report the terminal type when the server asks
          if( state.sbLen==2 && state.sb[0] == TO_TERMINAL_TYPE && state.sb[1] == TTYPE_SEND )
            sendTerminalType(client);

          state.subnegotiation = false;
          state.cmdLen = 0;
          state.sbLen = 0;
          return true;
        }

      // data byte (IAC IAC is an escaped 0xff)
      state.cmdLen = 0;
      if( state.sbLen < sizeof(state.sb) ) state.sb[state.sbLen] = b;
      if( state.sbLen < 255 ) state.sbLen++;
      return true;
    }

  // ---- handle CR-NUL sequences

  if( state.receivedCR )
    {
      state.receivedCR = false;
      if( b==0 )
        {
          // CR->NUL => CR (i.e. ignore the NUL)
          return true;
        }
    }
  else if( b == 0x0d && state.cmdLen==0 && !state.receiveBinary )
    {
      // received CR while not in binary mode => remember but pass through
      state.receivedCR = true;
      return false;
    }

  // ---- handle IAC sequences

  if( state.cmdLen==0 )
    {
      // waiting for IAC byte
      if( b == T_IAC )
        {
          state.cmdLen = 1;
          state.cmd[0] = T_IAC;
          return true;
        }
    }
  else if( state.cmdLen==1 )
    {
      // received second byte of IAC sequence
      if( b == T_IAC )
        {
          // IAC->IAC sequence means regular 0xff data value
          state.cmdLen = 0;

          // we already skipped the first IAC (0xff), so just return 'false' to pass this one through
          return false;
        }
      else if( b == T_NOP || b == T_BREAK || b == T_GOAHEAD )
        {
          // NOP, BREAK, GOAHEAD => do nothing and skip
          state.cmdLen = 0;
          return true;
        }
      else if( b == T_SB )
        {
          // start of sub-negotiation
          state.subnegotiation = true;
          state.cmdLen = 0;
          state.sbLen = 0;
          return true;
        }
      else
        {
          // record second byte of sequence
          state.cmdLen = 2;
          state.cmd[1] = b;
          return true;
        }
    }
  else if( state.cmdLen==2 )
    {
      // received third (i.e. last) byte of IAC sequence
      state.cmd[2] = b;

      bool reply = true;
      if( state.cmd[1] == T_WILL )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY:        state.cmd[1] = T_DO; state.receiveBinary = true; break;
            case TO_ECHO:               state.cmd[1] = T_DO; break;
            case TO_SUPPRESS_GO_AHEAD:  state.cmd[1] = T_DO; break;
            default: state.cmd[1] = T_DONT; break;
            }
        }
      else if( state.cmd[1] == T_WONT )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY: state.receiveBinary = false; break;
            }
          state.cmd[1] = T_DONT;
        }
      else if( state.cmd[1] == T_DO )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY:       state.cmd[1] = T_WILL; state.sendBinary = true; break;
            case TO_SUPPRESS_GO_AHEAD: state.cmd[1] = T_WILL; break;
            case TO_TERMINAL_TYPE:     state.cmd[1] = T_WILL; break;
            default: state.cmd[1] = T_WONT; break;
            }
        }
      else if( state.cmd[1] == T_DONT )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY: state.sendBinary = false; break;
            }
          state.cmd[1] = T_WONT;
        }
      else
        reply = false;

      // send reply if necessary
      if( reply )
        client.write(state.cmd, 3);

      // start over
      state.cmdLen = 0;
      return true;
    }
  else
    {
      // invalid state (should never happen) => just reset
      state.cmdLen = 0;
    }

  return false;
}


static void handleModemCommand()
{
  // check if a modem AT command was received
  static int cmdLen = 0, prevCmdLen = 0;
  static char cmd[81];
  char c = 0;

  if( term.available() )
    {
      c = (term.read() & 0x7f);
      bool bsp = c == modemReg[REG_BSP] || c == 0x7f;
      if( cmdLen<80 && c>32 && c != 0x7f )
        cmd[cmdLen++] = toupper(c);
      else if( cmdLen>0 && bsp )
        cmdLen--;

      if( modemEcho )
        {
          if( bsp )
            { term.print(char(8)); term.print(' '); term.print(char(8)); }
          else
            term.print(c);
        }

      prevCharTime = millis();
    }

  if( cmdLen==2 && cmd[0]=='A' && cmd[1]=='/' )
    {
      c = modemReg[REG_CR];
      cmd[1] = 'T';
      cmdLen = prevCmdLen;
    }

  if( c == modemReg[REG_CR] )
    {
      prevCmdLen = cmdLen;
      if( cmdLen>=2 && cmd[0]=='A' && cmd[1]=='T' )
        {
          cmd[cmdLen]=0;
          int ptr = 2;
          bool connecting = false;
          int status = E_OK;
          while( status!=E_ERROR && ptr<cmdLen )
            {
              if( cmd[ptr]=='D' )
                {
                  unsigned long t = millis();

                  // if we are already connected then disconnect first
                  if( modemClient.connected() )
                    {
                      modemClient.stop();
                      modemReg[REG_CURLINESPEED] = 0;
                    }

                  ptr++; if( cmd[ptr]=='T' || cmd[ptr]=='P' ) ptr++;

                  bool haveAlpha = false;
                  int numSep = 0;
                  for(int j=ptr; j<cmdLen && !haveAlpha; j++)
                    {
                      if( isalpha(cmd[j]) )
                        haveAlpha = true;
                      else if( !isdigit(cmd[j]) )
                        {
                          numSep++;
                          while( j<cmdLen && !isdigit(cmd[j]) ) j++;
                        }
                    }

                  byte n[4];
                  IPAddress addr;
                  int port = 23;
                  if( haveAlpha )
                    {
                      int p = ptr;
                      while( cmd[p]!=':' && p<cmdLen ) p++;
                      char c = cmd[p];
                      cmd[p] = 0;
                      if( WiFi.hostByName(cmd+ptr, addr) )
                        {
                          if( p+1<cmdLen ) port = atoi(cmd+p+1);
                        }
                      else
                        status = E_NOCARRIER;
                      cmd[p] = c;
                    }
                  else if( numSep==0 )
                    {
                      if( (cmdLen-ptr==12) || (cmdLen-ptr==16) )
                        {
                          for(int j=0; j<12; j+=3)
                            {
                              char c = cmd[ptr+j+3];
                              cmd[ptr+j+3] = 0;
                              n[j/3] = atoi(cmd+ptr+j);
                              cmd[ptr+j+3] = c;
                            }

                          addr = IPAddress(n);
                          if( cmdLen-ptr==16 ) port = atoi(cmd+ptr+12);
                        }
                      else
                        status = E_ERROR;

                      ptr = cmdLen;
                    }
                  else if( numSep==3 || numSep==4 )
                    {
                      for(int j=0; j<4; j++)
                        {
                          int p = ptr;
                          while( isdigit(cmd[ptr]) && ptr<cmdLen ) ptr++;
                          char c = cmd[ptr];
                          cmd[ptr] = 0;
                          n[j] = atoi(cmd+p);
                          cmd[ptr] = c;
                          while( !isdigit(cmd[ptr]) && ptr<cmdLen ) ptr++;
                        }
                      addr = IPAddress(n);
                      if( numSep==4 ) port = atoi(cmd+ptr);
                    }
                  else
                    status = E_ERROR;

                  if( status==E_OK )
                    {
                      if( modemClient.connect(addr, port) )
                        {
                          modemCommandMode = false;
                          modemEscapeState = 0;
                          resetTelnetState(modemTelnetState);

                          if( modemReg[REG_LINESPEED]==0 )
                            modemReg[REG_CURLINESPEED] = DEFAULT_LINESPEED;
                          else
                            modemReg[REG_CURLINESPEED] = min(modemReg[REG_LINESPEED], byte(NSPEEDS-1));

                          if( modemExtCodes==0 )
                            status = E_CONNECT;
                          else
                            {
                              switch( modemReg[REG_CURLINESPEED] )
                                {
                                case 3: status = E_CONNECT; break;
                                case 4: status = E_CONNECT600; break;
                                case 5: status = E_CONNECT1200; break;
                                case 6: status = E_CONNECT2400; break;
                                case 7: status = E_CONNECT4800; break;
                                case 9: status = E_CONNECT9600; break;
                                default: status = E_CONNECT; break;
                                }
                            }

                          connecting = true;
                        }
                      else if( modemExtCodes < 2 )
                        status = E_NOCARRIER;
                      else if( WiFi.status()!=WL_CONNECTED )
                        status = E_NODIALTONE;
                      else
                        status = E_NOANSWER;

                      // force at least 1 second delay between receiving
                      // the dial command and responding to it
                      if( millis()-t<1000 ) delay(1000-(millis()-t));
                    }

                  // ATD can not be followed by other commands
                  ptr = cmdLen;
                }
              else if( cmd[ptr]=='H' )
                {
                  if( cmdLen-ptr==1 || cmd[ptr+1]=='0' )
                    {
                      // hang up
                      if( modemClient.connected() )
                        {
                          modemClient.stop();
                          modemReg[REG_CURLINESPEED] = 0;
                        }
                    }

                  ptr += 2;
                }
              else if( cmd[ptr]=='O' )
                {
                  getCmdParam(cmd, ptr);
                  if( modemClient.connected() )
                    {
                      modemCommandMode = false;
                      modemEscapeState = 0;
                      break;
                    }
                }
              else if( cmd[ptr]=='I' )
                {
                  // ATI7: Famicom link statistics, printed to Serial (see
                  // famicomLinkPrintStats() in famicom-link.cpp). Other ATIn
                  // are accepted and ignored.
                  if( getCmdParam(cmd, ptr)==7 ) famicomLinkPrintStats();
                }
              else if( cmd[ptr]=='E' )
                modemEcho = getCmdParam(cmd, ptr)!=0;
              else if( cmd[ptr]=='Q' )
                modemQuiet = getCmdParam(cmd, ptr)!=0;
              else if( cmd[ptr]=='V' )
                modemVerbose = getCmdParam(cmd, ptr)!=0;
              else if( cmd[ptr]=='X' )
                modemExtCodes = getCmdParam(cmd, ptr);
              else if( cmd[ptr]=='Z' )
                {
                  // reset parameters (ignore command parameter)
                  getCmdParam(cmd, ptr);
                  resetModemState();

                  // ATZ can not be followed by other commands
                  ptr = cmdLen;
                }
              else if( cmd[ptr]=='S' )
                {
                  static uint8_t currentReg = 0;
                  int p = ptr;
                  int reg = getCmdParam(cmd, ptr);
                  if( ptr==p+1 ) reg = currentReg;
                  currentReg = reg;

                  if( cmd[ptr]=='?' )
                    {
                      ptr++;
                      byte v = modemReg[reg];
                      if( modemVerbose ) printModemCR();
                      if( v<100 ) term.print('0');
                      if( v<10  ) term.print('0');
                      term.print(v);
                      printModemCR();
                    }
                  else if( cmd[ptr]=='=' )
                    {
                      byte v = getCmdParam(cmd, ptr);
                      if( reg != REG_CURLINESPEED )
                        modemReg[reg] = v;
                    }
                }
              else if( cmd[ptr]=='I' )
                {
                  getCmdParam(cmd, ptr);
                }
              else if( cmd[ptr]=='M' || cmd[ptr]=='L' || cmd[ptr]=='A' || cmd[ptr]=='P' || cmd[ptr]=='T' )
                {
                  // ignore speaker settings, answer requests, pulse/tone dial settings
                  getCmdParam(cmd, ptr);
                }
              else
                status = E_ERROR;
            }

          printModemResult(status);

          // delay 1 second after a "CONNECT" message
          if( connecting ) delay(1000);
        }
      else if( cmdLen>0 )
        printModemResult(E_ERROR);

      cmdLen = 0;
    }
}


static void relayModemData()
{
  if( modemClient && modemClient.connected() && modemClient.available() )
    {
      // term.write() blocks while the Famicom queue is full, which paces this
      // (and, through TCP flow control, the remote) to the console
      unsigned long t = millis();
      while( modemClient.available() && millis()-t < 100 )
        {
          uint8_t b = modemClient.read();
          if( !handleTelnetProtocol(b, modemClient, modemTelnetState) ) term.write(b);
        }
    }

  if( millis() > prevCharTime + 20*modemReg[REG_GUARDTIME] )
    {
      if( modemEscapeState==0 )
        modemEscapeState = 1;
      else if( modemEscapeState==4 )
        {
          // received [1 second pause] +++ [1 second pause]
          // => switch to command mode
          modemCommandMode = true;
          printModemResult(E_OK);
        }
    }

  if( term.available() )
    {
      uint8_t buf[260];  // up to 3 bytes per input byte past n<254, plus a final NUL
      int n = 0;
      const unsigned int millisPerChar = 5;
      unsigned long startTime = millis();

      while( term.available() && n<254 && millis()-startTime < 100 )
        {
          uint8_t b = term.read();

          // must duplicate IAC tokens
          if( b==T_IAC ) buf[n++] = b;

          // if not sending in binary mode then a stand-alone CR (without LF) must be followd by NUL
          if( !modemTelnetState.sendBinary && n>0 && buf[n-1] == 0x0d && b != 0x0a ) buf[n++] = 0;

          buf[n++] = b;
          prevCharTime = millis();

          if( modemEscapeState>=1 && modemEscapeState<=3 && b==modemReg[REG_ESC] )
            modemEscapeState++;
          else
            modemEscapeState=0;

          // wait a short time to see if another character is coming in so we
          // can send multi-character (escape) sequences in the same packet
          // some BBSs don't recognize the sequence if there is too much delay
          while( !term.available() && millis()-prevCharTime < millisPerChar ) delay(1);
        }

      // if not sending in binary mode then a stand-alone CR (without LF) must be followd by NUL
      if( !modemTelnetState.sendBinary && buf[n-1] == 0x0d && !term.available() ) buf[n++] = 0;

      modemClient.write(buf, n);
    }
}


static void connectWiFi()
{
  prefs.begin("modem");
  wifiSsid = prefs.getString("ssid");
  wifiKey  = prefs.getString("key");

  // don't let the WiFi driver write its own copy of the credentials to flash:
  // flash writes stall both cores, including the Famicom link on core 1
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  if( wifiSsid.length()==0 )
    GetWiFiData("WiFi is not set up.");

  while( WiFi.status()!=WL_CONNECTED )
    {
      int c, i=20;

      term.print("Connecting to "); term.println(wifiSsid);
      term.println("(ESC to change)");
      WiFi.begin(wifiSsid.c_str(), wifiKey.c_str());

      // wait for connection
      c = term.read();
      while( c!=27 && (WiFi.status()!=WL_CONNECTED) && i>0 )
        {
          delay(500);
          c = term.read();
          i--;
        }

      if( c==27 )
        GetWiFiData("Received ESC.");
      else if( WiFi.status()!=WL_CONNECTED )
        GetWiFiData(("Could not connect to " + wifiSsid + ".").c_str());
    }

  PrintNetworkInfo();
}


static void modemTask(void *)
{
  connectWiFi();
  resetModemState();
  while( term.available() ) term.read();
  printModemResult(E_OK);

  for(;;)
    {
      if( modemClient && modemClient.connected() )
        {
          // only relay data if not in command mode
          if( !modemCommandMode ) relayModemData();
        }
      else if( !modemCommandMode )
        {
          // connection to modem client was lost
          modemClient.stop();
          modemCommandMode = true;
          modemReg[REG_CURLINESPEED] = 0;
          printModemResult(E_NOCARRIER);
        }

      if( modemCommandMode ) handleModemCommand();

      delay(1);  // let lower-priority tasks on core 0 run (task watchdog)
    }
}


void modemBegin()
{
  xTaskCreatePinnedToCore(modemTask, "modem", 8192, NULL, 1, NULL, 0);
}
