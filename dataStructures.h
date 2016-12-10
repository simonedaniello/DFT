//
// Created by giogge on 08/12/16.
//


#ifndef DATASTRUCTURES_H
#define DATASTRUCTURES_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>

//------------------------------------------------------------------------------------------------------STRUTTURE DATI
struct timer
{
    volatile int seqNum;
    //double lastRTT;
    //short int transmitN;
    struct timer * nextTimer;
    volatile int posInWheel;
    volatile short int isValid;
};

struct selectCell
{
    int value;
    struct timer packetTimer;
};

typedef struct datagram_t
{
    volatile int command;
    int opID;
    int seqNum;
    int ackSeqNum;
    short int isFinal;
    char content[512];
} datagram;


typedef struct handshake_t
{
    int ack; //se vale 1 sto ackando il precedente
    int windowsize; //lo imposta il server nella risposta, è opID nelle successive transazioni
    int sequenceNum;
    short int isFinal;
} handshake;

struct details
{
    int windowDimension;
    struct sockaddr_in addr;
    int sockfd;
    socklen_t Size;
    int servSeq;
    int mySeq;
    int volatile sendBase;
    //struct selectCell selectiveWnd[];
};

struct headTimer
{
    struct timer * nextTimer;
};

struct pipeMessage
{
    int seqNum;
    short int isFinal;
};
//----------------------------------------------------------------------------------------------------------------TIMER


//------------------------------------------------------------------------------------------------TERMINATION & RECOVERY

//------------------------------------------------------------------------------------------------------SELECTIVE REPEAT

void initWindow();

void sentPacket(int packetN, int retransmission);




//---------------------------------------------------------------------------------------------------------CREATE SOCKET
int createSocket();

struct sockaddr_in createStruct(unsigned short portN);

void bindSocket(int sockfd, struct sockaddr * address , socklen_t size);
//----------------------------------------------------------------------------------------------------------------------


void createThread(pthread_t * thread, void * function, void * arguments);

void * timerFunction();
void initTimerWheel();
void startTimer(int packetN, int posInWheel);
int getWheelPosition();
void clockTick();

void retransmissionServer( int pipeRT, struct details * details, datagram * packet,
                           int firstPacket, char ** FN);
void retransmissionClient( int pipeRT, struct details * details, datagram * packet,
                           int firstPacket, char ** FN);

void sendDatagram(struct details * details, struct datagram_t * sndPacket);

void sendACK(int socketfd, handshake *ACK, struct sockaddr_in * servAddr, socklen_t servLen);

void receiveACK(int mainSocket, handshake * SYN, struct sockaddr * address, socklen_t *slen);


#endif //DATASTRUCTURES_H
