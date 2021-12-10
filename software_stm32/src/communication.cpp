/**
  ******************************************************************************
  * @file    communication.c
  * @author  
  * @version V1.0
  * @date    
  * @brief   communication module to ESP32
  ******************************************************************************
  *
  */

#include <Arduino.h>
#include "hardware.h"
#include "motor.h"
#include "communication.h"
#include "temperature.h"

#define COMM_SER				Serial1		// serial port to ESP32
//#define COMM_DBG				Serial3		// serial port for debugging
#define COMM_DBG				Serial6		// serial port for debugging

#define COMM_MAX_CMD_LEN		15			// max length of a command without arguments
#define COMM_ARG_CNT			 2			// number of allowed command arguments
#define COMM_MAX_ARG_LEN		20			// max length of one command argument
#define SEND_BUFFER_LEN			800


void communication_setup (void) {
	
	// UART to ESP32
	COMM_SER.setRx(PA10);			//STM32F401 blackpill USART1 RX PA10
	COMM_SER.setTx(PA9);			//STM32F401 blackpill USART1 TX PA9
	COMM_SER.begin(115200);
	while(!COMM_SER);
	//COMM_SER.println("alive");COMM_SER.flush();
}


/**
  * @brief  Handler
  * @param  None
  * @retval None
  */
int16_t communication_loop (void) {
	static char buffer[300];
    static char *bufptr = buffer;
    static unsigned int buflen = 0;
    int availcnt;
    unsigned int found = 0;

	char*       cmdptr;
    char*	    cmdptrend;
    char        cmd[10];
    char		arg0[20];	
    char        arg1[20];		
	char*		arg0ptr = arg0;
	char*		arg1ptr = arg1;
	uint8_t		argcnt = 0;
    
	uint16_t		x;
	uint16_t		y;
	char sendbuffer[30];
    char valbuffer[10];

	char sendbuf[SEND_BUFFER_LEN];

	availcnt = COMM_SER.available(); 
    if(availcnt>0)
    {    
        for (int c = 0; c < availcnt; c++)
        {           
            *bufptr++ = (char) COMM_SER.read();
            buflen++;
        }
        if (buflen>=sizeof(buffer)) {
            buffer[sizeof(buffer)-1] = '\n';
        }
    }

	// if there is a little in the buffer
    if(buflen >= 5) 
    {
        for (unsigned int c = 0; c < buflen; c++)
        {           
            if(buffer[c] == '\n') 
            {
				if(buffer[c-1]=='\r') buffer[c-1] = '\0';
                else buffer[c] = '\0';
                //COMM_SER.print("recv "); COMM_SER.println(buffer);
                found = 1;

                buflen = 0;           	// reset counter
                bufptr = buffer;    	// reset ptr
            }
        }
    }

	// was something received
	if(found) {

		// devide buffer into command and data
		// ****************************************
		cmdptr = buffer;

		for(unsigned int xx=0;xx<3;xx++){
			cmdptrend = strchr(cmdptr,' ');
			if (cmdptrend!=NULL) {
				*cmdptrend = '\0';
				if(xx==0) 		strncpy(cmd,cmdptr,sizeof(cmd)-1);		// command
				else if(xx==1) { strncpy(arg0,cmdptr,sizeof(arg0)-1); argcnt=1;	} 	// 1st argument
				else if(xx==2) {	strncpy(arg1,cmdptr,sizeof(arg1)-1); argcnt=2;	} 	// 2nd argument
				cmdptr = cmdptrend + 1;
			}
		}

		// evaluate data
		// ****************************************************************************************

		
		// set target position
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		if(memcmp(APP_PRE_SETTARGETPOS,&cmd[0],5) == 0) {
			x = atoi(arg0ptr);
			y = atoi(arg1ptr);

			if(argcnt == 2) {				
				COMM_DBG.println("comm: set target position");
				if (y >= 0 && y <= 100 && x < ACTUATOR_COUNT) 
				{
					myvalves[x].target_position = (byte) y;
					COMM_SER.println(APP_PRE_SETTARGETPOS);
				}
			}
			else COMM_DBG.println("to few arguments");

			//return CMD_LEARN;
		}

		// get valve data (position, meancurrent, status, temperature)
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else if(memcmp(APP_PRE_GETVLVDATA,&cmd[0],5) == 0) {
			x = atoi(arg0ptr);

			if(argcnt == 1) {				
				//COMM_DBG.println("comm: get valve data");
				//if(0) 
				if (x < ACTUATOR_COUNT) 
				{
					sendbuffer[0] = '\0';
					//COMM_SER.println("sending new target value");
                    
					strcat(sendbuffer, APP_PRE_GETVLVDATA);
					strcat(sendbuffer, " ");
                    
					itoa(x, valbuffer, 10);		// valve nr
					strcat(sendbuffer, valbuffer);
					strcat(sendbuffer, " ");       
                    
					itoa(myvalves[x].actual_position, valbuffer, 10);      
					strcat(sendbuffer, valbuffer);
					strcat(sendbuffer, " ");
                    
					itoa(myvalves[x].meancurrent, valbuffer, 10);      
					strcat(sendbuffer, valbuffer);
					strcat(sendbuffer, " ");
                    
					itoa(myvalves[x].status, valbuffer, 10);      
					strcat(sendbuffer, valbuffer);
					strcat(sendbuffer, " ");
                    
					if(myvalves[x].sensorindex<MAXSENSORCOUNT)
					{
						itoa(tempsensors[myvalves[x].sensorindex].temperature, valbuffer, 10);      
						strcat(sendbuffer, valbuffer);
					}
					else strcat(sendbuffer, "-500");
					strcat(sendbuffer, " ");
                    
					COMM_SER.println(sendbuffer);
					//COMM_DBG.println(sendbuffer);		// debug				
				}
			}
			else COMM_SER.println("to few arguments");

			//return CMD_LEARN;
		}

		// disable uart tx, helps flashing ESP32 without unplugging
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else if(memcmp(APP_PRE_SETTXENA,&cmd[0],5) == 0) {
			x = atoi(arg0ptr);

			if(argcnt == 1) {				
				
				if (x == 0) 
				{					
					COMM_DBG.println("comm: disable tx pin");
					// set uart1 tx pin PA9 to input, no pull
					#warning fixme
					//MODIFY_REG(GPIOA->CRH, GPIO_CRH_CNF9 + GPIO_CRH_MODE9, GPIO_CRH_CNF9_0);
				}
				else {
					COMM_DBG.println("comm: enable tx pin");
					// set uart1 tx pin PA9 to output 10 Mhz, alternate function
					#warning fixme
					//MODIFY_REG(GPIOA->CRH, GPIO_CRH_CNF9 + GPIO_CRH_MODE9, GPIO_CRH_CNF9_1 + GPIO_CRH_MODE9_0);
				}
			}
			else COMM_DBG.println("to few arguments");

			//return CMD_LEARN;
		}

		// get value of supply temperature sensor
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else if(memcmp(APP_PRE_GETSUPPLYSENS,&cmd[0],5) == 0) {
			
			
		}

		// get onewire sensor data - sensor count and data and adress of all connected sensors
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else if(memcmp(APP_PRE_GETONEWIREDATA,&cmd[0],5) == 0) {
			get_sensordata(sendbuf, SEND_BUFFER_LEN);
			COMM_SER.print(APP_PRE_GETONEWIREDATA);
			COMM_SER.print(" ");			
			COMM_SER.println(sendbuf);
		}

		// ESPalive
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else if(memcmp("ESP",&cmd[0],3) == 0) {	
			COMM_DBG.println("received ESPalive 22");		
			if (buflen >= 8 && memcmp("ESPalive",cmd,8) == 0) {
				COMM_DBG.println("received ESPalive");
			}			 
		}

		// set sensor index
		// x - valve index
		// y - temp sensor index
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else if(memcmp(APP_PRE_SETSENSORINDEX,&cmd[0],5) == 0) {
			x = atoi(arg0ptr); // valve index
			y = atoi(arg1ptr); // temp sensor index

			if(argcnt == 2) {				
				COMM_DBG.println("comm: set sensor index");
				if (x >= 0 && x < ACTUATOR_COUNT && y < MAXSENSORCOUNT) 
				{
					myvalves[x].sensorindex = (byte) y;
					COMM_SER.println(APP_PRE_SETTARGETPOS);
				}
			}
			else COMM_DBG.println("to few arguments");
		}

		// open all valves request
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else if(memcmp(APP_PRE_SETALLVLVOPEN,&cmd[0],5) == 0) {
			COMM_DBG.println("got open all valves request");
			for(unsigned int xx=0;xx<ACTUATOR_COUNT;xx++){
				myvalves[xx].target_position = 100;
			}
		}

		// unknown command
		// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
		else {
			//COMM_DBG.println("unknown command received from ESP");
		}

	}
	else return -1;


return 0;
}


