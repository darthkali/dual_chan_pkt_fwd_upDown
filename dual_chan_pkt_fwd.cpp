#include "base64.h"
#include "parson.h"

#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <wiringPi.h>
#include <wiringPiSPI.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <pthread.h>
#include <netdb.h>              // gai_strerror 
#include "LoraModem.h"		// include Lora stuff


#pragma clang diagnostic push
#pragma ide diagnostic ignored "EndlessLoop"
static const int CHANNEL = 0;
using namespace std;
using namespace rapidjson;

#define BASE64_MAX_LENGTH 341

typedef unsigned char byte;
uint32_t cp_nb_pull;


// Set location in global_conf.json
float lat =  0.0;
float lon =  0.0;
int   alt =  0;

/* Informal status fields */
char platform[24] ;    /* platform definition */
char email[40] ;       /* used for contact email */
char description[64] ; /* used for free form description */

// internet interface
char interface[6];     // Used to set the interface to communicate to the internet either eth0 or wlan0

static int keepalive_time = DEFAULT_KEEPALIVE; // send a PULL_DATA request every X seconds
/* network protocol variables */
//static struct timeval push_timeout_half = {0, (PUSH_TIMEOUT_MS * 500)}; /* cut in half, critical for throughput */
static struct timeval pull_timeout = {0, (PULL_TIMEOUT_MS * 1000)}; /* non critical for throughput */

#define TX_BUFF_SIZE    2048
#define STATUS_SIZE     1024

//Downlink stuff
#define MAX_SERVERS                 4 /* Support up to 4 servers, more does not seem realistic */
#define RX_BUFF_SIZE  1024	// Downstream received from MQTT
uint8_t buff_down[RX_BUFF_SIZE];	// Buffer for downstream
uint8_t buff_up[RX_BUFF_SIZE];	// Buffer for upstream

/* network sockets */
//static int sock_up[MAX_SERVERS]; /* sockets for upstream traffic */
static int sock_down[MAX_SERVERS]; /* sockets for downstream traffic */
pthread_t thrid_down[MAX_SERVERS];

void thread_down(void* pic);

/* gateway <-> MAC protocol variables */
//static uint32_t net_mac_h; /* Most Significant Nibble, network order */
//static uint32_t net_mac_l; /* Least Significant Nibble, network order */
/* signal handling variables */
volatile bool exit_sig = false; /* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
volatile bool quit_sig = false; /* 1 -> application terminates without shutting down the hardware */


void LoadConfiguration(string filename);
void PrintConfiguration();

void Die(const char *s)
{
  perror(s);
  exit(1);
}

void SetupLoRa(byte CE)
{
  char buff[16];
  if (CE == 0) {
    printf("Trying to detect module CE0 with ");
    printf("NSS=%s "  , PinName(ssPin, buff));
    printf("DIO0=%s " , PinName(dio0 , buff));
    printf("Reset=%s ", PinName(RST  , buff));
    printf("Led1=%s\n", PinName(Led1 , buff));
  } else {
    printf("Trying to detect module CE1 with ");
    printf("NSS=%s "  , PinName(ssPin_2, buff));
    printf("DIO0=%s " , PinName(dio0_2 , buff));
    printf("Reset=%s ", PinName(RST  , buff));
    printf("Led1=%s\n", PinName(Led1 , buff));
  }
  
  // check basic 
  if (ssPin == 0xff || dio0 == 0xff) {
    Die("Bad pin configuration ssPin and dio0 need at least to be defined");
  }


  uint8_t version = ReadRegister(REG_VERSION, CE);

  if (version == 0x22) {
    // sx1272
    printf("SX1272 detected, starting.\n");
    sx1272 = true;
  } else {
    // sx1276?
    version = ReadRegister(REG_VERSION, CE);
    if (version == 0x12) {
      // sx1276
      if (CE == 0)
      {
        printf("SX1276 detected on CE0, starting.\n");
      } else {
        printf("SX1276 detected on CE1, starting.\n");
      }
      sx1272 = false;
    } else {
      printf("Transceiver version 0x%02X\n", version);
      Die("Unrecognized transceiver");
    }
  }

  WriteRegister(REG_OPMODE, SX72_MODE_SLEEP, CE);

  // set frequency
  uint64_t frf;
  if (CE == 0)
  {
    frf = ((uint64_t)freq << 19) / 32000000;
  } else {
    frf = ((uint64_t)freq_2 << 19) / 32000000;
  }   
  WriteRegister(REG_FRF_MSB, (uint8_t)(frf >> 16), CE );
  WriteRegister(REG_FRF_MID, (uint8_t)(frf >> 8), CE );
  WriteRegister(REG_FRF_LSB, (uint8_t)(frf >> 0), CE );

  WriteRegister(REG_SYNC_WORD, 0x34, CE); // LoRaWAN public sync word

  if (sx1272) {
    if (sf == SF11 || sf == SF12) {
      WriteRegister(REG_MODEM_CONFIG1, 0x0B, CE);
    } else {
      WriteRegister(REG_MODEM_CONFIG1, 0x0A, CE);
    }
    WriteRegister(REG_MODEM_CONFIG2, (sf << 4) | 0x04, CE);
  } else {
    if (sf == SF11 || sf == SF12) {
      WriteRegister(REG_MODEM_CONFIG3, 0x0C, CE);
    } else {
      WriteRegister(REG_MODEM_CONFIG3, 0x04, CE);
    }
    WriteRegister(REG_MODEM_CONFIG1, 0x72, CE);
    WriteRegister(REG_MODEM_CONFIG2, (sf << 4) | 0x04, CE);
  }

  if (sf == SF10 || sf == SF11 || sf == SF12) {
    WriteRegister(REG_SYMB_TIMEOUT_LSB, 0x05, CE);
  } else {
    WriteRegister(REG_SYMB_TIMEOUT_LSB, 0x08, CE);
  }
  WriteRegister(REG_MAX_PAYLOAD_LENGTH, 0x80, CE);
  WriteRegister(REG_PAYLOAD_LENGTH, PAYLOAD_LENGTH, CE);
  WriteRegister(REG_HOP_PERIOD, 0xFF, CE);
  WriteRegister(REG_FIFO_ADDR_PTR, ReadRegister(REG_FIFO_RX_BASE_AD, CE), CE);

  // Set Continous Receive Mode
  WriteRegister(REG_LNA, LNA_MAX_GAIN, CE);  // max lna gain
  WriteRegister(REG_OPMODE, SX72_MODE_RX_CONTINUOS, CE);
}

void SendStat()
{
  static char status_report[STATUS_SIZE]; /* status report as a JSON object */
  char stat_timestamp[24];

  int stat_index = 0;

  digitalWrite(InternetLED, HIGH);
  /* pre-fill the data buffer with fixed fields */
  status_report[0] = PROTOCOL_VERSION;
  status_report[3] = PKT_PUSH_DATA;

  status_report[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
  status_report[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
  status_report[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
  status_report[7] = 0xFF;
  status_report[8] = 0xFF;
  status_report[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
  status_report[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
  status_report[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];

  /* start composing datagram with the header */
  uint8_t token_h = (uint8_t)rand(); /* random token */
  uint8_t token_l = (uint8_t)rand(); /* random token */
  status_report[1] = token_h;
  status_report[2] = token_l;
  stat_index = 12; /* 12-byte header */

  /* get timestamp for statistics */
  time_t t = time(NULL);
  strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));

  // Build JSON object.
  StringBuffer sb;
  Writer<StringBuffer> writer(sb);
  writer.StartObject();
  writer.String("stat");
  writer.StartObject();
  writer.String("time");
  writer.String(stat_timestamp);
  writer.String("lati");
  writer.Double(lat);
  writer.String("long");
  writer.Double(lon);
  writer.String("alti");
  writer.Int(alt);
  writer.String("rxnb");
  writer.Uint(cp_nb_rx_rcv);
  writer.String("rxok");
  writer.Uint(cp_nb_rx_ok);
  writer.String("rxfw");
  writer.Uint(cp_up_pkt_fwd);
  writer.String("ackr");
  writer.Double(0);
  writer.String("dwnb");
  writer.Uint(0);
  writer.String("txnb");
  writer.Uint(0);
  writer.String("pfrm");
  writer.String(platform);
  writer.String("mail");
  writer.String(email);
  writer.String("desc");
  writer.String(description);
  writer.EndObject();
  writer.EndObject();

  string json = sb.GetString();
  //printf("stat update: %s\n", json.c_str());
  printf("stat update: %s", stat_timestamp);
  if (cp_nb_rx_ok_tot==0) {
    printf(" no packet received yet\n");
  } else {
    printf(" %u packet%sreceived, %u packet%ssent...\n", cp_nb_rx_ok_tot, cp_nb_rx_ok_tot>1?"s ":" ", cp_nb_pull, cp_nb_pull>1?"s ":" ");
  }

  // Build and send message.
  memcpy(status_report + 12, json.c_str(), json.size());
  SendUdp(status_report, stat_index + json.size());
  digitalWrite(InternetLED, LOW);
}

int main()
{
  struct timeval nowtime;
  uint32_t lasttime;
  unsigned int led0_timer,led1_timer;

  LoadConfiguration("global_conf.json");
  PrintConfiguration();

  // Init WiringPI
  wiringPiSetup() ;
  pinMode(ssPin, OUTPUT);
  pinMode(ssPin_2, OUTPUT);
  pinMode(dio0, INPUT);
  pinMode(dio0_2, INPUT);
  pinMode(RST, OUTPUT);
  pinMode(NetworkLED, OUTPUT);
  pinMode(ActivityLED_0, OUTPUT);
  pinMode(ActivityLED_1, OUTPUT);
  pinMode(InternetLED, OUTPUT);

  // Init SPI
  wiringPiSPISetup(SPI_CHANNEL, 500000);
  wiringPiSPISetup(SPI_CHANNEL_2, 500000);

  // Setup LORA
  digitalWrite(RST, HIGH);
  delay(100);
  digitalWrite(RST, LOW);
  delay(100);
  initLoraModem(0);
  initLoraModem(1);
  //SetupLoRa(0);
  //SetupLoRa(1);

  // Prepare Socket connection
  while ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
     digitalWrite(NetworkLED, 1);
     printf("No socket connection possible yet. Retrying in 10 seconds...\n");
     delay(10000);
    digitalWrite(NetworkLED, 0);
  }

  memset((char *) &si_other, 0, sizeof(si_other));
  si_other.sin_family = AF_INET;

  ifr.ifr_addr.sa_family = AF_INET;
  strncpy(ifr.ifr_name, interface, IFNAMSIZ-1);  // use configured network interface eth0 or wlan0
  ioctl(s, SIOCGIFHWADDR, &ifr);

  // ID based on MAC Adddress of interface
  printf( "Gateway ID: %.2x:%.2x:%.2x:ff:ff:%.2x:%.2x:%.2x\n",
              (uint8_t)ifr.ifr_hwaddr.sa_data[0],
              (uint8_t)ifr.ifr_hwaddr.sa_data[1],
              (uint8_t)ifr.ifr_hwaddr.sa_data[2],
              (uint8_t)ifr.ifr_hwaddr.sa_data[3],
              (uint8_t)ifr.ifr_hwaddr.sa_data[4],
              (uint8_t)ifr.ifr_hwaddr.sa_data[5]
  );

  printf("Listening at SF%i on %.6lf Mhz.\n", sf,(double)freq/1000000);
  printf("Listening at SF%i on %.6lf Mhz.\n", sf,(double)freq_2/1000000);
  printf("-----------------------------------\n");
  
  // Setup connectivity with the server

  int i;
  int ic=0;

	/* network socket creation */
	struct addrinfo hints;
	struct addrinfo *result; /* store result of getaddrinfo */
	struct addrinfo *q; /* pointer to move into *result data */
	char host_name[64];
	char port_name[64];
	/* prepare hints to open network sockets */
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; /* should handle IP v4 or v6 automatically */
	hints.ai_socktype = SOCK_DGRAM;

  for (vector<Server_t>::iterator it = servers.begin(); it != servers.end(); ++it) {
    if (it->enabled) {
      si_other.sin_port = htons(it->port);

      SolveHostname(it->address.c_str(), it->port, &si_other);


		// TODO: setup upstream connection as done below, now we still use the old method
               /* look for server address w/ upstream port */
               // i = getaddrinfo(it->address.c_str(), "1700", &hints, &result);
               // if (i != 0) {
               //         printf("ERROR: [up] getaddrinfo on address %s (port %i) returned: %s\n", it->address.c_str(), it->port, gai_strerror(i));
               // }


                /* connect so we can send/receive packet with the server only */
                //i = connect(sock_up[ic], (struct sockaddr *) &si_other, slen);
                //i = connect(sock_up[ic], (const sockaddr*) "1700", slen);
                //if (i != 0) {
                //        printf("ERROR: [up] connect on address %s (port %i) returned: %s\n", it->address.c_str(), it->port, strerror(errno));
               // }
               // freeaddrinfo(result);

                /* look for server address w/ downstream port TODO use port from global_conf.json*/
                i = getaddrinfo(it->address.c_str(), "1700", &hints, &result);
                if (i != 0) {
                        printf("ERROR: [down] getaddrinfo on address %s (port %i) returned: %s\n", it->address.c_str(), it->port, gai_strerror(i));
                }

                /* try to open socket for downstream traffic */
                for (q=result; q!=NULL; q=q->ai_next) {
                        sock_down[ic] = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
                        if (sock_down[ic] == -1) continue; /* try next field */
                        else break; /* success, get out of loop */
                }
                if (q == NULL) {
                        printf("ERROR: [down] failed to open socket to any of server %s addresses (port %i)\n", it->address.c_str(), it->port);
                        i = 1;
                        for (q=result; q!=NULL; q=q->ai_next) {
                                getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
                                printf("INFO: [down] result %i host:%s service:%s\n", i, host_name, port_name);
                                ++i;
                        }
                }
               /* connect so we can send/receive packet with the server only */
                i = connect(sock_down[ic], q->ai_addr, q->ai_addrlen);
                if (i != 0) {
                        printf("ERROR: [down] connect address %s (port %i) returned: %s\n", it->address.c_str(), it->port, strerror(errno));
                }
                freeaddrinfo(result);

                /* If we made it through to here, this server is live */
                printf("INFO: Successfully contacted server %s (port %i) \n", it->address.c_str(), it->port);
    }
    ic++;
}


  // Thread preparation
 //  pthread_t threads[1];
  //  int rc;
  //  long t;
  //  t=0;
  //    printf("In main: creating thread %ld\n", t);
  //     rc = pthread_create(&threads[t], NULL, receiveUDPThread, (void *)t);
  //     if (rc){
  //        printf("ERROR; return code from pthread_create() is %d\n", rc);
  //        exit(-1);
  //     }
ic=0;
  for (vector<Server_t>::iterator it = servers.begin(); it != servers.end(); ++it) {
    if (it->enabled) {
      si_other.sin_port = htons(it->port);
		{
			i = pthread_create( &thrid_down[ic], NULL, (void * (*)(void *))thread_down, (void *) (long) ic);
			if (i != 0) {
				printf("ERROR: [main] impossible to create downstream thread\n");
				exit(EXIT_FAILURE);
			}
		ic++;
		}
     }
  }



  while(1) {

    // Packet received ?
    if (ReceivePacket(0)) {
      // Led ON
      if (ActivityLED_0 != 0xff) {
        digitalWrite(ActivityLED_0, 1);
      }

      // start our Led blink timer, LED as been lit in Receivepacket
      led0_timer=millis();
    }
    if (ReceivePacket(1)) {
      // Led ON
      if (ActivityLED_1 != 0xff) {
        digitalWrite(ActivityLED_1, 1);
      }

      // start our Led blink timer, LED as been lit in Receivepacket
      led1_timer=millis();
    }

   // Receive UDP PUSH_ACK messages from server. (*2, par. 3.3)
   // This is important since the TTN broker will return confirmation
   // messages on UDP for every message sent by the gateway. So we have to consume them..
   // As we do not know when the server will respond, we test in every loop.
   //
   //int received_bytes = recvfrom( handle, packet_data, sizeof(packet_data),0, (struct sockaddr*)&cliaddr, &len );

   //start the thread

    gettimeofday(&nowtime, NULL);
    uint32_t nowseconds = (uint32_t)(nowtime.tv_sec);
    if (nowseconds - lasttime >= 30) {
      lasttime = nowseconds;
      SendStat();
      cp_nb_rx_rcv = 0;
      cp_nb_rx_ok = 0;
      cp_up_pkt_fwd = 0;
    }

    // Led timer in progress ?
    if (led0_timer) {
      // Led timer expiration, Blink duration is 250ms
      if (millis() - led0_timer >= 250) {
        // Stop Led timer
        led0_timer = 0;

        // Led OFF
        if (ActivityLED_0 != 0xff) {
          digitalWrite(ActivityLED_0, 0);
        }
      }
    }
    if (led1_timer) {
      // Led timer expiration, Blink duration is 250ms
      if (millis() - led1_timer >= 250) {
        // Stop Led timer
        led1_timer = 0;

        // Led OFF
        if (ActivityLED_1 != 0xff) {
          digitalWrite(ActivityLED_1, 0);
        }
      }
    }


    // Let some time to the OS
    delay(1);
  }

  return (0);
}

void LoadConfiguration(string configurationFile)
{
  FILE* p_file = fopen(configurationFile.c_str(), "r");
  char buffer[65536];
  FileReadStream fs(p_file, buffer, sizeof(buffer));

  Document document;
  document.ParseStream(fs);

  for (Value::ConstMemberIterator fileIt = document.MemberBegin(); fileIt != document.MemberEnd(); ++fileIt) {
    string objectType(fileIt->name.GetString());
    if (objectType.compare("SX127x_conf") == 0) {
      const Value& sx127x_conf = fileIt->value;
      if (sx127x_conf.IsObject()) {
        for (Value::ConstMemberIterator confIt = sx127x_conf.MemberBegin(); confIt != sx127x_conf.MemberEnd(); ++confIt) {
          string key(confIt->name.GetString());
          if (key.compare("freq") == 0) {
            freq = confIt->value.GetUint();
          } else if (key.compare("freq_2") == 0) {
            freq_2 = (SpreadingFactor_t)confIt->value.GetUint();
          } else if (key.compare("spread_factor") == 0) {
            sf = (SpreadingFactor_t)confIt->value.GetUint();
          } else if (key.compare("pin_nss") == 0) {
            ssPin = confIt->value.GetUint();
          } else if (key.compare("pin_nss_2") == 0) {
            ssPin_2 = confIt->value.GetUint();
          } else if (key.compare("pin_dio0") == 0) {
            dio0 = confIt->value.GetUint();
          } else if (key.compare("pin_dio0_2") == 0) {
            dio0_2 = confIt->value.GetUint();
          } else if (key.compare("pin_rst") == 0) {
            RST = confIt->value.GetUint();
          } else if (key.compare("pin_NetworkLED") == 0) {
            NetworkLED = confIt->value.GetUint();
          } else if (key.compare("pin_InternetLED") == 0) {
            InternetLED = confIt->value.GetUint();
          } else if (key.compare("pin_ActivityLED_0") == 0) {
            ActivityLED_0 = confIt->value.GetUint();
          } else if (key.compare("pin_ActivityLED_1") == 0) {
            ActivityLED_1 = confIt->value.GetUint();
          }
        }
      }

    } else if (objectType.compare("gateway_conf") == 0) {

      const Value& gateway_conf = fileIt->value;
      if (gateway_conf.IsObject()) {
        for (Value::ConstMemberIterator confIt = gateway_conf.MemberBegin(); confIt != gateway_conf.MemberEnd(); ++confIt) {
          string memberType(confIt->name.GetString());
          if (memberType.compare("ref_latitude") == 0) {
            lat = confIt->value.GetDouble();
          } else if (memberType.compare("ref_longitude") == 0) {
            lon = confIt->value.GetDouble();
          } else if (memberType.compare("ref_altitude") == 0) {
            alt = confIt->value.GetUint(); 

          } else if (memberType.compare("name") == 0 && confIt->value.IsString()) {
            string str = confIt->value.GetString();
            strcpy(platform, str.length()<=24 ? str.c_str() : "name too long");
          } else if (memberType.compare("email") == 0 && confIt->value.IsString()) {
            string str = confIt->value.GetString();
            strcpy(email, str.length()<=40 ? str.c_str() : "email too long");
          } else if (memberType.compare("desc") == 0 && confIt->value.IsString()) {
            string str = confIt->value.GetString();
            strcpy(description, str.length()<=64 ? str.c_str() : "description is too long");
          } else if (memberType.compare("interface") == 0 && confIt->value.IsString()) {
            string str = confIt->value.GetString();
            strcpy(interface, str.length()<=6 ? str.c_str() : "interface too long");

          } else if (memberType.compare("servers") == 0) {
            const Value& serverConf = confIt->value;
            if (serverConf.IsObject()) {
              const Value& serverValue = serverConf;
              Server_t server;
              for (Value::ConstMemberIterator srvIt = serverValue.MemberBegin(); srvIt != serverValue.MemberEnd(); ++srvIt) {
                string key(srvIt->name.GetString());
                if (key.compare("address") == 0 && srvIt->value.IsString()) {
                  server.address = srvIt->value.GetString();
                } else if (key.compare("port") == 0 && srvIt->value.IsUint()) {
                  server.port = srvIt->value.GetUint();
                } else if (key.compare("enabled") == 0 && srvIt->value.IsBool()) {
                  server.enabled = srvIt->value.GetBool();
                }
              }
              servers.push_back(server);
            }
            else if (serverConf.IsArray()) {
              for (SizeType i = 0; i < serverConf.Size(); i++) {
                const Value& serverValue = serverConf[i];
                Server_t server;
                for (Value::ConstMemberIterator srvIt = serverValue.MemberBegin(); srvIt != serverValue.MemberEnd(); ++srvIt) {
                  string key(srvIt->name.GetString());
                  if (key.compare("address") == 0 && srvIt->value.IsString()) {
                    server.address = srvIt->value.GetString();
                  } else if (key.compare("port") == 0 && srvIt->value.IsUint()) {
                    server.port = srvIt->value.GetUint();
                  } else if (key.compare("enabled") == 0 && srvIt->value.IsBool()) {
                    server.enabled = srvIt->value.GetBool();
                  }
                }
                servers.push_back(server);
              }
            }
          }
        }
      }
    }
  }
}

void PrintConfiguration()
{
  for (vector<Server_t>::iterator it = servers.begin(); it != servers.end(); ++it) {
    printf("server: .address = %s; .port = %hu; .enable = %d\n", it->address.c_str(), it->port, it->enabled);
  }
  printf("Gateway Configuration\n");
  printf("  %s (%s)\n  %s\n", platform, email, description);
  printf("  Latitude=%.8f\n  Longitude=%.8f\n  Altitude=%d\n", lat,lon,alt);
  printf("  Interface: %s\n", interface);

}
/* -------------------------------------------------------------------------- */
/* --- THREAD 2: POLLING SERVER AND EMITTING PACKETS ------------------------ */

// TODO: factor this out and inspect the use of global variables. (Cause this is started for each server)

void thread_down(void* pic) {
 //TODO

	int i; // loop variables 
	int ic = (int) (long) pic;
	//int lastTmst;
	
	/* configuration and metadata for an outbound packet */
	//struct lgw_pkt_tx_s txpkt;
	//bool sent_immediate = false; /* option to sent the packet immediately */
	
	/* local timekeeping variables */
	struct timespec send_time; /* time of the pull request */
	struct timespec recv_time; /* time of return from recv socket call */
	
	/* data buffers */
	uint8_t buff_down[1000]; /* buffer to receive downstream packets */
	uint8_t buff_req[12]; /* buffer to compose pull requests */
	int msg_len;
	
	/* protocol variables */
	uint8_t token_h; /* random token for acknowledgement matching */
	uint8_t token_l; /* random token for acknowledgement matching */
	bool req_ack = false; /* keep track of whether PULL_DATA was acknowledged or not */
	
	/* JSON parsing variables */
	//JSON_Value *root_val = NULL;
	//JSON_Object *txpk_obj = NULL;
	//JSON_Value *val = NULL; /* needed to detect the absence of some fields */
	//const char *str; /* pointer to sub-strings in the JSON data */
	//short x0, x1;
	//short x2, x3, x4;
	//double x5, x6; 
	
	/* variables to send on UTC timestamp */
	//struct tref local_ref; /* time reference used for UTC <-> timestamp conversion */
	//struct tm utc_vector; /* for collecting the elements of the UTC time */
	//struct timespec utc_tx; /* UTC time that needs to be converted to timestamp */
	
	/* auto-quit variable */
	//uint32_t autoquit_cnt = 0; /* count the number of PULL_DATA sent since the latest PULL_ACK */

  i=0;
  char * servername;
  for (vector<Server_t>::iterator it = servers.begin(); it != servers.end() && i<=ic; ++it) {
    //printf("server: .address = %s; .port = %hu; .enable = %d\n", it->address.c_str(), it->port, it->enabled);
    servername = (char *) it->address.c_str();
    i++;
  }
	printf("INFO: [down] Thread activated for server %s\n",servername);

	/* set downstream socket RX timeout */
	i = setsockopt(sock_down[ic], SOL_SOCKET, SO_RCVTIMEO, (void *)&pull_timeout, sizeof pull_timeout);
	if (i != 0) {
		//TODO Should this failure bring the application down?
	        printf("ERROR: [down] setsockopt for server %s returned %s\n", servername, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	/* pre-fill the pull request buffer with fixed fields */
	buff_req[0] = PROTOCOL_VERSION;
	buff_req[3] = PKT_PULL_DATA;
        buff_req[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
  	buff_req[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
  	buff_req[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
  	buff_req[7] = 0xFF;
  	buff_req[8] = 0xFF;
 	buff_req[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
  	buff_req[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
  	buff_req[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];
	
	while (!exit_sig && !quit_sig) {
		
		/* TODO: auto-quit if the threshold is crossed */
		//if ((autoquit_threshold > 0) && (autoquit_cnt >= autoquit_threshold)) {
		//	exit_sig = true;
		//	printf("INFO: [down] for server %s the last %u PULL_DATA were not ACKed, exiting application\n", serv_addr[ic], autoquit_threshold);
		//	break;
		//}
		
		/* generate random token for request */
		token_h = (uint8_t)rand(); /* random token */
		token_l = (uint8_t)rand(); /* random token */
		buff_req[1] = token_h;
		buff_req[2] = token_l;
		
		/* send PULL request and record time */
		send(sock_down[ic], (void *)buff_req, sizeof buff_req, 0);
		if (debug >= 2) { printf("INFO: [up] for server %s PULL_REQ send \n", servername ); }
		clock_gettime(CLOCK_MONOTONIC, &send_time);
		//pthread_mutex_lock(&mx_meas_dw);
		//meas_dw_pull_sent += 1;
		//pthread_mutex_unlock(&mx_meas_dw);
		req_ack = false;
		//autoquit_cnt++;
		
		/* listen to packets and process them until a new PULL request must be sent */
		recv_time = send_time;
		while ((int)difftimespec(recv_time, send_time) < keepalive_time) {
			if (debug >= 2) { printf("Waiting for incoming server packets..\n"); }
			/* try to receive a datagram */
			msg_len = recv(sock_down[ic], (void *)buff_down, (sizeof buff_down)-1, 0);
			clock_gettime(CLOCK_MONOTONIC, &recv_time);
			
			/* if no network message was received, got back to listening sock_down socket */
			if (msg_len == -1) {
				//printf("WARNING: [down] recv returned %s\n", strerror(errno)); /* too verbose */
				continue;
			}
			
			/* if the datagram does not respect protocol, just ignore it */
			if ((msg_len < 4) || (buff_down[0] != PROTOCOL_VERSION) || ((buff_down[3] != PKT_PULL_RESP) && (buff_down[3] != PKT_PULL_ACK))) {
				//TODO Investigate why this message is logged only at shutdown, i.e. all messages produced here are collected and
				//     spit out at program termination. This can lead to an unstable application.
				printf("WARNING: [down] ignoring invalid packet\n");
				continue;
			}
			
			/* if the datagram is an ACK, check token */
			if (buff_down[3] == PKT_PULL_ACK) {
				if ((buff_down[1] == token_h) && (buff_down[2] == token_l)) {
					if (req_ack) {
						if (debug >=2) { printf("INFO: [down] for server %s duplicate ACK received :)\n",servername); }
					} else { /* if that packet was not already acknowledged */
						req_ack = true;
						//autoquit_cnt = 0;
						//pthread_mutex_lock(&mx_meas_dw);
						//meas_dw_ack_rcv += 1;
						//pthread_mutex_unlock(&mx_meas_dw);
						if (debug >=2) { printf("INFO: [down] for server %s PULL_ACK received in .. ms\n", servername ); } //TODO
					}
				} else { /* out-of-sync token */
					printf("INFO: [down] for server %s, received out-of-sync ACK\n",servername);
				}
			//	continue;
			}
			
			//TODO: This might generate to much logging data. The reporting should be reevaluated and an option -q should be added.
			/* the datagram is a PULL_RESP */
			buff_down[msg_len] = 0; /* add string terminator, just to be safe */
			//printf("Received buff %s\n", (char *) buff_down);
			//printf("INFO: [down] for server %s PULL_RESP received :)\n",servername); /* very verbose */
			//printf("JSON down: %s\n", (char *)(buff_down + 4)); /* DEBUG: display JSON payload */

			//Now send the packet to the node
  			//char LoraBuffer[64]; 						//buffer to hold packet to send to LoRa node
  			uint8_t * data = buff_down + 4;
 			uint8_t  ident = buff_down[3];
			int packetSize = sizeof(buff_down);
			uint16_t  token = buff_down[2]*256 + buff_down[1];
			byte CE=0; // For now hardcode to 1st RFM; need to update and use the corresponding freq or random
			struct timeval now;
  
  			// now parse the message type from the server (if any)
  			switch (ident) {
			case PKT_PUSH_DATA: // 0x00 UP
			if (debug >=1) {
				printf("PKT_PUSH_DATA:: size %i\n", packetSize);
				printf(" From TBD\n");
				printf(", port TBD\n");
				printf(", data: ");
				for (int i=0; i<packetSize; i++) {
					printf("%i",buff_down[i]);
					printf(":");
				}
				printf("\n");
			}
			break;
			case PKT_PUSH_ACK:	// 0x01 DOWN
			if (debug >= 1) {
				printf("PKT_PUSH_ACK:: size %i\n", packetSize);
				printf(" From TBD\n");
				printf(", port TBD\n");
				printf(", token: %i\n", token);
			}
			break;
			case PKT_PULL_DATA:	// 0x02 UP
				printf(" Pull Data\n");
			break;
			case PKT_PULL_RESP:	// 0x03 DOWN
				//lastTmst = micros();					// Store the tmst this package was received TODO
				// Send to the LoRa Node first (timing) and then do messaging
				if (sendPacket(data, sizeof(data)-4, CE) < 0) {
				printf("Error sending packet\n");
			}
		
			// Now respond with an PKT_PULL_ACK; 0x04 UP
			buff_up[0]=buff_down[0];
			buff_up[1]=buff_down[1];
			buff_up[2]=buff_down[2];
			buff_up[3]=PKT_PULL_ACK;
			buff_up[4]=0;
			
			// Only send the PKT_PULL_ACK to the UDP socket that just sent the data!!!
			//SendUdp((char*) buff_up, 4);
			send(sock_down[ic], (void *)buff_req, sizeof buff_req, 0);
				if (debug>=2) {
					gettimeofday(&now, NULL);
					printf("PKT_PULL_ACK:: tmst=%i\n", (uint32_t)(now.tv_sec * 1000000 + now.tv_usec));
				}
			
				if (debug >=1) {
					//printf("PKT_PULL_RESP:: size %i", packetSize);
					printf("PKT_PULL_RESP:: ");
					printf(" From server %s.\n", servername);
					//TODO: printf(", port TBD");	
					//printf(", data: TBD\n");
					//data = buff_down + 4;
					//data[packetSize] = 0;
					//print((char *)data);
					//println("..."));
				}
				cp_nb_pull++;
			break;
			case PKT_PULL_ACK:	// 0x04 DOWN; the server sends a PULL_ACK to confirm PULL_DATA receipt
				if (debug >= 2) {
					printf("PKT_PULL_ACK:: size %i\n", packetSize);
					printf(" From SERVER %s", servername);
					printf(", port TBD");	
				//TODO: printf(", data: ");
				//for (int i=0; i<packetSize; i++) {
				//	printf("%i", buff_down[i]);
				//	printf(":");
				//}
				printf("\n");
			}
			break;
			default:
				printf(", ERROR ident not recognized: %i\n", ident);
			break;
  			}
		}
	}
	printf("\nINFO: End of downstream thread for server  %s.\n",servername);
}

#pragma clang diagnostic pop