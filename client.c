#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/sendfile.h>
#include <sys/time.h>
#include "dataStructures.h"

#define WINDOWSIZE 256
#define TIMERSIZE 2048
#define NANOSLEEP 500000

//#define PULLDIR "/home/giogge/Documenti/clientHome/"
#define PULLDIR "/home/dandi/exp/"


int timerSize = TIMERSIZE;
int nanoSleep = NANOSLEEP;
int windowSize = WINDOWSIZE;
int sendBase;
int pipeFd[2];
int pipeSendACK[2];
volatile int globalTimerStop = 0;
volatile int globalOpID;
volatile int fdList, finalLen;
struct headTimer timerWheel[TIMERSIZE] = {NULL};
datagram packet;

int fdglob;


void retransmitForPush(int fd, struct pipeMessage * rtx);
void pushSender();
void clientSendFunction();
void * clientListenFunction();
void sendSYN(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd);
void sendSYN2(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd);
void send_ACK(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd, int synackSN);
void printfListInSTDOUT();
void pushListener();
void initProcess();
void sendCycle();
void startClientConnection(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd);
void listenCycle();
void parseInput(char * s);
void listPullListener(int fd, int command);
int checkUserInput(char * buffer);
int waitForSYNACK(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd);
int getFileLen(int fd);
char * stringParser(char * string);

// %%%%%%%%%%%%%%%%%%%%%%%    globali    %%%%%%%%%%%%%%%%%%%%%%%%%%

struct details details;
pthread_t listenThread, timerThread;
struct selectCell selectiveWnd[WINDOWSIZE];

pthread_mutex_t syncMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t condMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t condMTX2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mtxPacketAndDetails = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t secondConnectionCond = PTHREAD_COND_INITIALIZER;
pthread_cond_t senderCond = PTHREAD_COND_INITIALIZER;

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

//------------------------------------------------------------------------------------------------------------------MAIN
int main()
{

    printf("connection is starting... \n\n\n");
    clientSendFunction();
    exit(EXIT_SUCCESS);
}

void clientSendFunction()
{
    initPipe(pipeFd);
    initPipe(pipeSendACK);

    initProcess();

    struct pipeMessage rtxN;
    memset(&rtxN, 0, sizeof(struct pipeMessage));

    for(;;)
    {
        printf("sono il sender e mi metto in condWait\n");
        if(pthread_cond_wait(&senderCond, &condMTX2) != 0)
        {
            perror("error in sender cond wait");
        }

        printf("\n\nsono il sender e sono stato svegliato\n\n");
        if(packet.command == 0 || packet.command == 2)
        {
            sendDatagram(details.sockfd, &details.addr, details.Size, &packet);
            printf("pacchetto inviato \n\n");
            ACKandRTXcycle(details.sockfd, &details.addr, details.Size);
        }
        else
        {
            mtxLock(&mtxPacketAndDetails);
            details.sendBase = details.mySeq;
            details.firstSeqNum= details.mySeq;
            mtxUnlock(&mtxPacketAndDetails);
            //pushSender();
            sendCycle();
        }
    }
}

void initProcess()
{
    initWindow();

    // %%%%%%%%%%%%%%%%    thread       %%%%%%%%%%%%%%%%%

    createThread(&listenThread, clientListenFunction, NULL);

    createThread(&timerThread, timerFunction, NULL);

    // %%%%%%%%%%%%%%%%    variabili    %%%%%%%%%%%%%%%%%

    int socketfd;
    struct sockaddr_in senderServerAddress;
    socklen_t serverLen = sizeof(struct sockaddr_in);

    //----------------------------------------------------


    senderServerAddress = createStruct(4242); //create struct with server port
    socketfd = createSocket();

    if (fcntl(socketfd, F_SETFL, O_NONBLOCK) == -1)
    {
        perror("error in fcntl");
    }
    //printf("ho settato la socket a non bloccante\n");

    //printf("starting handshake procedure\n\n");
    startClientConnection( &senderServerAddress, serverLen, socketfd);
}

void startClientConnection(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd)
{
    sendSYN(servAddr, servLen, socketfd);
    int rcvSequence = waitForSYNACK(servAddr, servLen, socketfd);
    if(rcvSequence == -1)
    {
        perror("error in connection");
    }
    else if(rcvSequence > 0)//ho ricevuto il SYNACK
    {

        //salvo i dettagli di connessione in details
        mtxLock(&mtxPacketAndDetails);
        details.addr = *servAddr;
        details.Size = servLen;
        details.sockfd = socketfd;
        mtxUnlock(&mtxPacketAndDetails);

        //segnalo al listener
        sendSignalThread(&condMTX, &secondConnectionCond);

    }
    else //se ritorna -1 devo ritrasmettere
    {
        initWindow();
        startClientConnection(servAddr, servLen, socketfd);
    }

}

void * clientListenFunction()
{
    printf("listener thread attivato\n\n");

    mtxLock(&mtxPacketAndDetails);
    details.Size2 = sizeof(struct sockaddr_in);
    details.sockfd2 = createSocket();
    mtxUnlock(&mtxPacketAndDetails);

    if(pthread_cond_wait(&secondConnectionCond, &condMTX) != 0)
    {
        perror("error in cond wait");
    }

    printf("inizio seconda connessione\n\n");

    sendSYN2(&(details.addr), details.Size, details.sockfd2);
    waitForSYNACK(&(details.addr2), details.Size2, details.sockfd2);
    send_ACK(&(details.addr2), details.Size2, details.sockfd2, details.remoteSeq);

    listenCycle();
    return (EXIT_SUCCESS);
}

void listenCycle()
{
    char * s = malloc(512);
    int timeout = 0;
    int res = 0;
    if(s == NULL)
    {
        perror("error in malloc");
    }

    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1)
    {
        perror("error in fcntl");
    }

    for(;;)
    {
        mtxLock(&syncMTX);
        globalTimerStop = 0;
        mtxUnlock(&syncMTX);

        memset(&packet, 0, sizeof(datagram));
        res = 0;
        printf("insert command : \n");
        struct timespec opStart, opEnd;

        while(!res)
        {
            res = checkUserInput(s);
            if(res == -1)
            {
                perror("error in stdin read");
            }
            else if(res == 0)
            {
                if(usleep(100000) == -1)
                {
                    perror("error on usleep");
                }
                timeout++;
            }
            else
            {
                //prendo un timestamp
                clock_gettime(CLOCK_REALTIME, &opStart);

                printf("processing request\n");
                sendSignalTimer();
                parseInput(s);
                timeout = 0;

                clock_gettime(CLOCK_REALTIME, &opEnd);

                ssize_t len = lseek(fdglob, 0, SEEK_END)+1;

                printf("\n\n\n----------------------------------------------------------------\n");
                printf("|\toperazione completata in %lu millisecondi  \n", (opEnd.tv_sec - opStart.tv_sec) * 1000);
                printf("|\tdimensione del file: %d kB\n", (int) len/1000);
                printf("|\tvelocità media: %lu kB/s  \n", (len/1000)/((opEnd.tv_nsec - opStart.tv_nsec)/1000000000 +1));
                printf("----------------------------------------------------------------\n");
                printf("\n\n");
                //provvisorio
                //poi mi devo mettere a sentire i dati ricevuti dalla socket
                //res = 0;
            }

            if(timeout == 120000)
            {
                perror("timeout on input listen");
                exit(EXIT_FAILURE);
            }
        }
    }
}

void parseInput(char * s)
{
    if (strncmp(s,"list", 4) == 0)//---------------------------------------------------------------listener list command
    {
        char listFilename[17];

        memset(listFilename,0,17);
        strcpy(listFilename, "lsTempXXXXXX");
        fdList = mkstemp(listFilename);
        while(fdList == -1)
        {
            perror("1: error in list tempfile open");
            fdList = mkstemp(listFilename);
        }

        fdglob = fdList;
        unlink(listFilename);

        listPullListener(fdList, 0);
    }
    else if (strncmp(s, "push", 4) == 0)//---------------------------------------------------------listener push command
    {
        printf("'push'\n");
        if (sscanf(s, "%*s %s", packet.content) == EOF) {
            perror("1: error in reading words from standard input, first sscanf push");
        }
        pushListener();
    }
    else if (strncmp(s, "pull", 4) == 0)//---------------------------------------------------------listener pull command
    {
        printf("'pull'\n");

        char * content = malloc(500);
        if(content == NULL)
            perror("error on malloc");

        char * fileName;
        int fdPull;



        if (sscanf(s, "%*s %s", content) == EOF) {
            perror("1: error in reading words from standard input, first sscanf pull");
            free(s);
            free(content);
        }

        mtxLock(&mtxPacketAndDetails);
        memset(packet.content, 0, 512);
        strcpy(packet.content, content);
        mtxUnlock(&mtxPacketAndDetails);

        printf("richiesta di pull per il pacchetto %s\n", content);

        fileName = malloc(512);
        strcat(fileName, PULLDIR);
        strcat(fileName, content);

        printf("|    path : %s \n", fileName);

        if ((fdPull = open(fileName, O_CREAT | O_TRUNC | O_RDWR, 00777)) == -1)
        {
            perror("1: file already exists on push");
        }

        fdglob = fdPull;

        printf("|    file created with name : %s \n", content);

        listPullListener(fdPull, 2);
    }
    else if (strncmp(s, "quit", 4) == 0)//---------------------------------------------------------listener quit command
    {
        printf("quit'\n");
    }
    else if (strncmp(s, "help", 4) == 0)//---------------------------------------------------------listener help command
    {
        printf("'help\n");
    }
    else
    {
        printf("command does not exist, enter 'list', 'push', 'pull', 'help' or 'exit'\n");
    }
}

void listPullListener(int fd, int command)
{

    //---------------------proteggere con mutex
    mtxLock(&mtxPacketAndDetails);
    packet.command = command;
    packet.isFinal = 1;
    srandom((unsigned int)getpid());
    packet.opID = (int) random() % 4096;

    mtxLock(&syncMTX);
    globalOpID = packet.opID;
    mtxUnlock(&syncMTX);

    packet.seqNum = details.mySeq;
    mtxUnlock(&mtxPacketAndDetails);
    //-----------------------------------------

    sendSignalThread(&condMTX2, &senderCond);

    //aspetto il pacchetto con le dimensioni per settare finallen
    datagram firstDatagram;
    while(checkSocketDatagram(&(details.addr2), details.Size2, details.sockfd2, &firstDatagram) != 1) {}

    printf("faccio un sscanf\n");

    mtxLock(&syncMTX);
    if(sscanf(firstDatagram.content, "%d", &finalLen) == EOF)
    {
        perror("error on scanf");
    }
    printf("ho ricevuto la lunghezza del pacchetto finale %d\n", finalLen);
    mtxUnlock(&syncMTX);

    tellSenderSendACK(firstDatagram.seqNum, 1);
    //aspetto datagrammi
    printf("aspetto datagrammi\n");
    getResponse(details.sockfd2, &(details.addr2), &(details.Size2), fd);

    if(command == 0)
    {
        printfListInSTDOUT();
    }

}

int checkUserInput(char * buffer)
{
    ssize_t res;
    res = read(STDIN_FILENO, buffer, 512);
    if((res == -1) && (errno != EAGAIN))
    {
        return -1;
    }
    else if(res > 0)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

void printfListInSTDOUT()
{
    printf("\n----------------------LIST-----------------------\n\n");

    off_t count = lseek(fdList, 0L, SEEK_END);
    while (count == -1) {
        perror("1: error in file size measurement\n");
        sleep(1);
        count = lseek(fdList, 0L, SEEK_END);
    }
    while(lseek(fdList, 0L, SEEK_SET) == -1){
        perror("1: error in lseek");
        lseek(fdList, 0L, SEEK_SET);
    }

    while (sendfile(STDOUT_FILENO, fdList, 0L, (size_t) count) == -1) {
        perror("error in sendfile");
        sendfile(STDOUT_FILENO, fdList, 0L, (size_t) count);
    }

    printf("\n------------------------------------------------\n\n");
    if (ftruncate(fdList, 0) == -1) {
        perror("0: error in truncating file");
    }

    if( close(fdList) == -1)
    {
        perror("0: error in list file close");
    }
}

void pushListener()
{
    //---------------------proteggere con mutex
    mtxLock(&mtxPacketAndDetails);
    packet.command = 1;
    packet.isFinal = 0;
    srandom((unsigned int)getpid());
    packet.opID = (int) random() % 4096;

    packet.seqNum = details.mySeq;
    details.firstSeqNum = details.mySeq;
    mtxUnlock(&mtxPacketAndDetails);

    mtxLock(&syncMTX);
    globalOpID = packet.opID;
    printf("pacchetto inviato OPID %d\n", packet.opID);
    mtxUnlock(&syncMTX);
    //-----------------------------------------

    sendSignalThread(&condMTX2, &senderCond);
    waitForFirstPacketListener(details.sockfd2, &(details.addr2), details.Size2);
    waitForAckCycle(details.sockfd2, (struct sockaddr *) &details.addr2, &details.Size2);
    printf("--------------SONO USCITO-------------------\n\n\n\n");
}

void pushSender()
{
    mtxLock(&mtxPacketAndDetails);
    volatile int seqnum = details.mySeq;
    volatile int sndbase = details.sendBase;
    mtxUnlock(&mtxPacketAndDetails);
    volatile int finalSeq = -1, isFinal = 0;

    ssize_t readByte;
    datagram sndPacket;
    struct pipeMessage rtx;

    mtxLock(&mtxPacketAndDetails);
    int fd = open(packet.content, O_RDONLY);
    if(fd == -1){
        perror("error in open");
    }

    fdglob = fd;
    mtxUnlock(&mtxPacketAndDetails);

    int len = getFileLen(fd);
    memset(sndPacket.content, 0, 512);
    char * s = malloc(100);
    if(s == NULL){
        perror("error in malloc");
    }
    s = stringParser(packet.content);
    if(sprintf(sndPacket.content, "%s %d", s, len) < 0)
    {
        perror("error in sprintf");
    }

    printf("sono arrivato fin qui, la stringa da inviare è %s con numero di sequenza iniziale : %d\n", sndPacket.content, seqnum);
    sndPacket.seqNum = seqnum;
    sndPacket.command = 1;
    sndPacket.isFinal = 1;
    mtxLock(&syncMTX);
    sndPacket.opID = globalOpID;
    mtxUnlock(&syncMTX);
    sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
    waitForFirstPacketSender(details.sockfd, &(details.addr), details.Size);
    seqnum = details.mySeq;
    printf("valore iniziale di sendBase = %d\n", details.sendBase);

    while((((details.sendBase)%WINDOWSIZE) != (finalSeq%WINDOWSIZE)))
    {
        while(isFinal == 0)
        {
            mtxLock(&mtxPacketAndDetails);
            sndbase = details.sendBase;
            mtxUnlock(&mtxPacketAndDetails);

            printf("seqnum %d, sndBase %d\n",(details.mySeq)%WINDOWSIZE , (details.sendBase)%WINDOWSIZE);
            while((details.mySeq)%WINDOWSIZE - (details.sendBase)%WINDOWSIZE > 256)
            {
                if(checkPipe(&rtx, pipeFd[0]) != 0)
                {
                    retransmitForPush(fd, &rtx);
                }
                mtxLock(&mtxPacketAndDetails);
                sndbase = details.sendBase;
                mtxUnlock(&mtxPacketAndDetails);
            }
            if (checkPipe(&rtx, pipeFd[0]) == 0)
            {
                memset(sndPacket.content, 0, 512);
                readByte = read(fd, sndPacket.content, 512);
                if (readByte < 512 && readByte >= 0)
                {
                    finalSeq = details.mySeq;
                    isFinal = 1;
                    printf("il pacchetto è finale (grandezza ultimo pacchetto : %d, numero di sequenza : %d)\n", (int) readByte, finalSeq);
                }
                sndPacket.isFinal = (short) isFinal;

                mtxLock(&mtxPacketAndDetails);
                sndPacket.ackSeqNum = details.remoteSeq;
                seqnum = details.mySeq;
                mtxUnlock(&mtxPacketAndDetails);

                sndPacket.seqNum = seqnum;

                mtxLock(&syncMTX);
                sndPacket.opID = globalOpID;
                mtxUnlock(&syncMTX);

                sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);

            }
            else
                retransmitForPush(fd, &rtx);
        }

        //PROTEGGI CON I MUTEX
        mtxLock(&mtxPacketAndDetails);
        sndbase = details.sendBase;
        mtxUnlock(&mtxPacketAndDetails);

        if ((sndbase%WINDOWSIZE) != (finalSeq%WINDOWSIZE))
        {
            //printf("sndBase modulo WINDOWSIZE = %d, finalseq modulo c0se = %d\n", (sndbase%WINDOWSIZE),(finalSeq%WINDOWSIZE) );
            //printWindow();
            if (checkPipe(&rtx, pipeFd[0]) != 0) {
                retransmitForPush(fd, &rtx);
            }
        }
    }
    printf("mi appresto a mandare il pacchetto finale\n");
    mtxLock(&mtxPacketAndDetails);
    memset(sndPacket.content, 0, 512);
    mtxUnlock(&mtxPacketAndDetails);
    sndPacket.isFinal = -1;
    sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
    printf("inviato il pacchetto definitivo con isFinal = -1 \n");
}

void retransmitForPush(int fd, struct pipeMessage * rtx)
{
    printf("ritrasmetto\n");
    datagram sndPacket;

    mtxLock(&mtxPacketAndDetails);
    if(lseek(fd, 512*(rtx->seqNum - details.firstSeqNum), SEEK_SET) == -1)
    {
        perror("errore in lseek");
    }
    mtxUnlock(&mtxPacketAndDetails);

    if(read(fd, sndPacket.content, 512)==-1)
    {
        perror("error in read");
    }

    sndPacket.isFinal = rtx->isFinal;

    mtxLock(&mtxPacketAndDetails);
    sndPacket.ackSeqNum = details.remoteSeq;
    mtxUnlock(&mtxPacketAndDetails);

    sndPacket.seqNum = rtx->seqNum;

    mtxLock(&syncMTX);
    sndPacket.opID = globalOpID;
    mtxUnlock(&syncMTX);

    sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%% CONNESSIONE

void sendSYN(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd)
{
    handshake SYN;

    srandom((unsigned int)getpid());
    SYN.sequenceNum = (int) random() % 4096;

    sendBase = SYN.sequenceNum;

    // il prossimo seqnum utile
    mtxLock(&mtxPacketAndDetails);
    details.remoteSeq = SYN.sequenceNum;
    mtxUnlock(&mtxPacketAndDetails);

    sendACK(socketfd, &SYN, servAddr, servLen);
    sentPacket(SYN.sequenceNum, 0);


    printf("ho inviato il SYN. numero di sequenza : %d\n", SYN.sequenceNum);
}

void sendSYN2(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd)
{
    handshake SYN;

    mtxLock(&mtxPacketAndDetails);
    SYN.sequenceNum = details.mySeq;
    SYN.ack = details.remoteSeq;
    mtxUnlock(&mtxPacketAndDetails);

    sendACK(socketfd, &SYN, servAddr, servLen);
    sentPacket(SYN.sequenceNum, 0);


    printf("ho inviato il SYN2. numero di sequenza : %d e ack per il server %d\n", SYN.sequenceNum, SYN.ack);
}

int waitForSYNACK(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd)
{
    handshake SYNACK;
    int sockResult;
    struct pipeMessage rtxN;
    for(;;)
    {
        if(checkPipe(&rtxN, pipeFd[0]))
        {
            printf("devo ritrasmettere\n");
            return 0;
        }
        sockResult = checkSocketAck(servAddr, servLen, socketfd, &SYNACK);
        if(sockResult == -1)
        {
            perror("error in socket read");
            return -1;
        }
        if(sockResult == 1)
        {
            printf("SYNACK ricevuto. numero di sequenza : %d\n", SYNACK.sequenceNum);
            ackSentPacket(SYNACK.ack);
            mtxLock(&mtxPacketAndDetails);
            details.remoteSeq = SYNACK.sequenceNum;//---------------------------------------------serve al syn2
            mtxUnlock(&mtxPacketAndDetails);
            //--------------------------------------------INIT GLOBAL DETAILS
            return SYNACK.sequenceNum;
        }
    }
}

void send_ACK(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd, int synackSN)
{
    handshake ACK;
    ACK.ack = synackSN;

    mtxLock(&mtxPacketAndDetails);
    ACK.sequenceNum = details.remoteSeq;
    mtxUnlock(&mtxPacketAndDetails);

    ACK.windowsize = windowSize;

    sendACK(socketfd, &ACK, servAddr, servLen);
    //sentPacket(ACK.sequenceNum, 0);
    printf("ACK finale inviato. Numero di sequenza : %d\n", ACK.sequenceNum);
}

void sendCycle()
{
    int fd;
    datagram sndPacket;


    fd = open(packet.content, O_RDONLY);
    if(fd == -1) {
        perror("error in open");
    }


    int len = getFileLen(fd);
    memset(sndPacket.content, 0, 512);

    char *s = malloc(100);
    if (s == NULL) {
        perror("error in malloc");
    }
    s = stringParser(packet.content);
    if (sprintf(sndPacket.content, "%s %d",s, len) < 0) {
        perror("error in sprintf");
    }

    mtxLock(&mtxPacketAndDetails);
    sndPacket.seqNum = details.mySeq;
    mtxUnlock(&mtxPacketAndDetails);

    sndPacket.command = 1;
    sndPacket.isFinal = 1;
    sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);


    waitForFirstPacketSender(details.sockfd, &(details.addr), details.Size);
    printf("sono arrivato fin qui, la stringa da inviare è %s con numero di sequenza iniziale : %d\n", sndPacket.content, details.mySeq);



    mtxLock(&mtxPacketAndDetails);
    int seqnum = details.mySeq;
    details.firstSeqNum = seqnum;
    int sndBase = details.sendBase;
    mtxUnlock(&mtxPacketAndDetails);

    int finalSeq = -1;
    int isFinal = 0;
    ssize_t readByte;

    struct pipeMessage rtx;
    while(sndBase%WINDOWSIZE != finalSeq%WINDOWSIZE)
    {
        while(isFinal == 0)
        {
            mtxLock(&mtxPacketAndDetails);
            sndBase = details.sendBase;
            mtxUnlock(&mtxPacketAndDetails);

            while(seqnum%WINDOWSIZE - sndBase%WINDOWSIZE > 256)
            {
                if (checkPipe(&rtx, pipeFd[0]) != 0)
                {
                    printf("ritrasmetto\n");
                    sndPacket = rebuildDatagram(fd, rtx);
                    sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
                }
                mtxLock(&mtxPacketAndDetails);
                sndBase = details.sendBase;
                mtxUnlock(&mtxPacketAndDetails);
            }
            if (checkPipe(&rtx, pipeFd[0]) == 0)
            {
                memset(sndPacket.content, 0, 512);
                readByte = read(fd, sndPacket.content, 512);
                if (readByte < 512 && readByte >= 0)
                {
                    finalSeq = seqnum;
                    isFinal = 1;
                    printf("il pacchetto è finale (grandezza ultimo pacchetto : %d)\n", (int) readByte);
                }
                sndPacket.isFinal = (short) isFinal;

                mtxLock(&mtxPacketAndDetails);
                sndPacket.ackSeqNum = details.remoteSeq;
                mtxUnlock(&mtxPacketAndDetails);

                sndPacket.seqNum = seqnum;

                mtxLock(&syncMTX);
                sndPacket.opID = globalOpID;
                mtxUnlock(&syncMTX);

                //printf("ho inviato un pacchetto ackando %u\n", details.remoteSeq);
                sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
                seqnum = details.mySeq;

            }
            else
            {
                //ritrasmetti
                sndPacket = rebuildDatagram(fd, rtx);
                sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
            }
        }

        mtxLock(&mtxPacketAndDetails);
        sndBase = details.sendBase;
        mtxUnlock(&mtxPacketAndDetails);

        if(sndBase%WINDOWSIZE != finalSeq%WINDOWSIZE)
        {
            printf("sndBase modulo WINDOWSIZE = %d, finalseq modulo c0se = %d\n", (sndBase%WINDOWSIZE),(finalSeq%WINDOWSIZE) );
            sleep(1);
            if(checkPipe(&rtx, pipeFd[0]) != 0)
            {
                printf("ritrasmetto\n");
                sndPacket = rebuildDatagram(fd, rtx);
                sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
            }
        }
    }
    memset(sndPacket.content, 0, 512);
    sndPacket.isFinal = -1;
    sendDatagram(details.sockfd, &(details.addr), details.Size, &sndPacket);
    printf("inviato il pacchetto definitivo con isFinal = -1 \n");
}
