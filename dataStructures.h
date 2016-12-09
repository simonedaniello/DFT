//
// Created by giogge on 08/12/16.
//


#ifndef DATASTRUCTURES_H
#define DATASTRUCTURES_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>


struct timer
{
    volatile int seqNum;
    //double lastRTT;
    //short int transmitN;
    volatile struct timer * nextTimer;
    volatile int posInWheel;
    volatile short int isValid;
};


struct selectCell
{
    int value;
    //struct timer packetTimer;
    int seqNum;
    volatile struct timer *wheelTimer;
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
    struct selectCell selectiveWnd[];
};


/*------------------TIMER ------------------------------------------------------------------------------*/


/*------------------TERMINATION & RECOVERY -------------------------------------------------------------*/


/*------------------SELECTIVE REPEAT -------------------------------------------------------------*/

struct sockaddr_in createStruct(unsigned short portN);



void initWindow(int dimension, struct selectCell *window);

//---------------------------------------------------------------------------------------------------------CREATE SOCKET
int createSocket();

void bindSocket(int sockfd, struct sockaddr * address , socklen_t size);
/*-------------------------------------------------------------------------------------------------------*/


#endif //FTOUDP_DATASTRUCTURES_H