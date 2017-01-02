#include <arpa/inet.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <dirent.h>
#include "server.h"

#define WINDOWSIZE 256
#define TIMERSIZE 2048
#define NANOSLEEP 500000

#define LSDIR "/home/giogge/Documenti/experiments/"
//#define LSDIR "/home/dandi/Downloads/"

int timerSize = TIMERSIZE;
int nanoSleep = NANOSLEEP;
int windowSize = WINDOWSIZE;
int sendBase;
int pipeFd[2];
int pipeSendACK[2];
datagram packet;

volatile int finalLen, globalOpID, currentTimeSlot;

struct selectCell selectiveWnd[WINDOWSIZE];
struct headTimer timerWheel[TIMERSIZE] = {NULL};
struct details details;

pthread_t timerThread;
pthread_t senderThread;

pthread_mutex_t condMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t secondConnectionCond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t condMTX2 = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t senderCond = PTHREAD_COND_INITIALIZER;

void sendCycle(int command);
void retransmitForList(int fd, struct pipeMessage * rtx);
void lsSendCycle();
void listenCycle();
int waitForAck(int socketFD, struct sockaddr_in * clientAddr);
void terminateConnection(int socketFD, struct sockaddr_in * clientAddr, socklen_t socklen, struct details *cl );
void sendSYNACK(int privateSocket, socklen_t socklen , struct details * cl);
int waitForAck2(int socketFD, struct sockaddr_in * clientAddr);
void finishHandshake();
int ls();
int receiveFirstDatagram(char * content);


// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%        <<-------------<   FUNZIONI

void listenFunction(int socketfd, struct details * details, handshake * message)
{
    initWindow();

    char buffer[100];
    printf("richiesta dal client %s\n\n\n", inet_ntop(AF_INET, &((details->addr).sin_addr), buffer, 100));

    initPipe(pipeFd);
    initPipe(pipeSendACK);

    createThread(&timerThread, timerFunction, NULL);
    createThread(&senderThread, sendFunction, NULL);

    printf("finita la creazione dei thread\n");

    startServerConnection(details, socketfd, message);

    listenCycle();
}

void * sendFunction()
{

    printf("sender thread attivato\n\n");
    if(pthread_cond_wait(&secondConnectionCond, &condMTX) != 0)
    {
        perror("error in cond wait");
    }
    printf("sono dopo la cond wait\n\n");
    startSecondConnection(&details, details.sockfd2);
    finishHandshake();

    //---------------------------------------------------------------------scheletro di sincronizzazione listener sender
    for(;;)
    {
        printf("mi metto in cond wait\n");
        if(pthread_cond_wait(&senderCond, &condMTX2) != 0)
        {
            perror("error in cond wait");
        }
        printf("sono dopo la seconda cond wait, giunse un pacchetto con comando %d\n\n", packet.command);

        //-----------------------------------------------------------------------------------------------------------------
        if(packet.command == 0)
        {
            details.sendBase = details.mySeq;
            details.firstSeqNum = details.mySeq;
            sendCycle(0);
        }
        else if(packet.command == 2)
        {
            details.sendBase = details.mySeq;
            details.firstSeqNum = details.mySeq;

            printf("\n\nè una pull di %s\n\n", packet.content);
            //sendCycle(2);
        }
        else if(packet.command == 1){
            ACKandRTXcycle(details.sockfd2, &details.addr2, details.Size2);
        }
    }

}

void finishHandshake()
{
    //aspetto ultimo ack
    int res = waitForAck2(details.sockfd2, &(details.addr2));
    if(res == 0)
    {
        //ritrasmetto
        sendSYNACK2(details.sockfd2, details.Size2, &details);
        finishHandshake();
    }
    else
    {
        //ho finito la connessione, aspetto che mi svegli il listener
        printf("fine, sono pronto\n\n");
    }
}

void listenCycle()
{
    printf("inizio il ciclo di ascolto\n");

    //datagram packet;
    //------------------------------------------------------------------------------------------------------------------
    for(;;)
    {
        printf("\nsono il listener e ricomincio il ciclo\n");
        memset(&packet, 0, sizeof(datagram));
        int res = 0;
        int timeout = 0;
        while(!res)
        {
            res = checkSocketDatagram(&(details.addr), details.Size, details.sockfd, &packet);
            if(res == -1)
            {
                perror("error in socket read");
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
                printf("è arrivata una richiesta : numero di sequenza ricevuto = %u\n", packet.seqNum);
                details.remoteSeq = packet.seqNum;
                globalOpID = packet.opID;
                sendSignalThread(&condMTX2, &senderCond);
                printf("RICHIESTA CON NUMERO DI COMANDO = %d\n\n", packet.command);
                if(packet.command == 0 || packet.command == 2)
                {
                    waitForAckCycle(details.sockfd, (struct sockaddr *) &details.addr, &details.Size);
                }
                else if (packet.command == 1)
                {
                    sendSignalThread(&condMTX2, &senderCond);
                    int fd = receiveFirstDatagram(packet.content);
                    tellSenderSendACK(packet.seqNum, 1);
                    printf("sono arrivato fino a qui\n");
                    getResponse(details.sockfd2, &(details.addr2), &(details.Size2), fd);
                }

                timeout = 0;
            }

            if(timeout == 120000)
            {
                perror("timeout on server listen");
                exit(EXIT_FAILURE);
            }
        }
        //arrivo qui quando ho ricevuto cose, chiamo una funzione tipo controllaComando() e sveglia il sender e il timer
    }
}

void startServerConnection(struct details * cl, int socketfd, handshake * message)
{
    //chiudo la socket pubblica nel processo figlio
    closeFile(socketfd);

    //apro la socket dedicata al client su una porta casuale
    int privateSocket;
    privateSocket = createSocket();
    socklen_t socklen = sizeof(struct sockaddr_in);

    //collego una struct legata a una porta effimera, dedicata al client
    struct sockaddr_in serverAddress;
    //mi metto su una porta effimera, indicandola con sin_port = 0
    serverAddress = createStruct(0); //create struct with ephemeral port
    printf("ho creato la struct dedicata\n");

    //per il client rimango in ascolto su questa socket
    bindSocket(privateSocket, (struct sockaddr *) &serverAddress, socklen);

    details.remoteSeq = (message->sequenceNum);

    //mando il datagramma ancora senza connettermi

    sendSYNACK(privateSocket, socklen, cl);
    terminateConnection(privateSocket, &(cl->addr), socklen, cl);

}

void startSecondConnection(struct details * cl, int socketfd)
{
    //chiudo la socket pubblica nel processo figlio
    closeFile(socketfd);

    //apro la socket dedicata al client su una porta casuale
    details.sockfd2 = createSocket();
    details.Size2 = sizeof(struct sockaddr_in);

    //collego una struct legata a una porta effimera, dedicata al client
    struct sockaddr_in serverAddress;
    //mi metto su una porta effimera, indicandola con sin_port = 0
    serverAddress = createStruct(0); //create struct with ephemeral port
    printf("ho creato la seconda struct dedicata\n");

    details.addr2 = serverAddress;

    //per il client rimango in ascolto su questa socket
    bindSocket(details.sockfd2, (struct sockaddr *) &(details.addr2), details.Size2);

    //mando il datagramma ancora senza connettermi
    sendSYNACK2(details.sockfd2, details.Size2, cl);
    //terminateConnection(privateSocket, &(cl->addr), socklen, cl);

}

void terminateConnection(int socketFD, struct sockaddr_in * clientAddr, socklen_t socklen, struct details *cl )
{
    int rcvSequence = waitForAck(socketFD, clientAddr);
    if(rcvSequence == -1)
    {
        perror("error in connection");
    }
    else if(rcvSequence > 0)
    {
        printf("ACK ricevuto con numero di sequenza : %d. fine connessione parte 1\n", rcvSequence);
        //avvio il thread listener per connettersi su una nuova socket
        //cond signal e il listener mi manda un secondo SYNACK, chiudendo socket eccetera

        sendSignalThread(&condMTX, &secondConnectionCond);

        //in teoria ora posso connettere la socket
    }
    else //se ritorna 0 devo ritrasmettere
    {
        sendSYNACK(socketFD, socklen , cl);
        terminateConnection(socketFD, clientAddr, socklen, cl);
    }
}

int waitForAck(int socketFD, struct sockaddr_in * clientAddr)
{
    if (fcntl(socketFD, F_SETFL, O_NONBLOCK) == -1)
    {
        perror("error in fcntl");
    }
    socklen_t slen = sizeof(struct sockaddr_in);
    handshake ACK;
    struct pipeMessage rtxN;
    int sockResult;
    for(;;)
    {
        if(checkPipe(&rtxN, pipeFd[0]))
        {
            printf("devo ritrasmettere\n");
            return 0;
        }
        sockResult = checkSocketAck(clientAddr, slen, socketFD, &ACK);
        if (sockResult == -1)
        {
            perror("error in socket read");
            return -1;
        }
        if (sockResult == 1)
        {
            details.addr = *clientAddr;
            details.Size = slen;
            details.sockfd = socketFD;
            details.remoteSeq = ACK.sequenceNum;

            ackSentPacket(ACK.ack);
            //--------------------------------------------INIT GLOBAL DETAILS
            return ACK.sequenceNum;
        }
    }
}

int waitForAck2(int socketFD, struct sockaddr_in * clientAddr)
{
    if (fcntl(socketFD, F_SETFL, O_NONBLOCK) == -1)
    {
        perror("error in fcntl");
    }
    socklen_t slen = sizeof(struct sockaddr_in);
    handshake ACK;
    struct pipeMessage rtxN;
    int sockResult;
    for(;;)
    {
        if(checkPipe(&rtxN, pipeFd[0]))
        {
            printf("devo ritrasmettere\n");
            return 0;
        }
        sockResult = checkSocketAck(clientAddr, slen, socketFD, &ACK);
        if (sockResult == -1)
        {
            perror("error in socket read");

        }
        if (sockResult == 1)
        {
            ackSentPacket(ACK.ack);
            //--------------------------------------------INIT GLOBAL DETAILS
            return ACK.sequenceNum;
        }
    }
}

void sendSYNACK(int privateSocket, socklen_t socklen , struct details * cl)
{
    handshake SYN_ACK;
    srandom((unsigned int)getpid());
    SYN_ACK.sequenceNum = rand() % 4096;

    sendBase = SYN_ACK.sequenceNum;

    SYN_ACK.ack = details.remoteSeq;
    sendACK(privateSocket, &SYN_ACK, &(cl->addr), socklen);

    sentPacket(SYN_ACK.sequenceNum, 0);
    printf("SYNACK inviato, numero di sequenza : %d\n", SYN_ACK.sequenceNum);

}

void sendSYNACK2(int privateSocket, socklen_t socklen , struct details * cl)
{
    handshake SYN_ACK;

    SYN_ACK.sequenceNum = details.mySeq;

    SYN_ACK.ack = details.remoteSeq;
    sendACK(privateSocket, &SYN_ACK, &(cl->addr), socklen);

    sentPacket(SYN_ACK.sequenceNum, 0);
    printf("SYNACK inviato, numero di sequenza : %d\n", SYN_ACK.sequenceNum);

}

int ls()
{

    char listFilename[17];
    memset(listFilename,0,17);
    strcpy(listFilename, "lsTempXXXXXX");

    int fd = mkstemp(listFilename);
    while(fd == -1)
    {
        perror("1: error in list tempfile open");
        fd = mkstemp(listFilename);

    }
    unlink(listFilename);


    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir (LSDIR)) != NULL)
    {
        while ((ent = readdir (dir)) != NULL)
        {
            if((ent->d_name)[0] != '.')
            {
                dprintf(fd, "%s\n", ent->d_name);
            }
        }
        closedir (dir);
    }
    else
        perror ("errore nell'apertura della directory");

    lseek(fd, 0, SEEK_SET);
    return fd;
}

int receiveFirstDatagram(char * content)
{
    int fd;
    char *s = malloc(100);
    if (s == NULL) {
        perror("error in malloc");
    }
    if (sscanf(content, "%s %d", s, &finalLen) == EOF) {
        perror("1: error in reading words from standard input, first sscanf push");
    }
    char * path = "/pushObjects/";  //     <<----------------------------------------------< DA CAMBIARE ASSOLUTAMENTE
    strcat(path, s);
    if((fd = open(path, O_RDWR | O_TRUNC | O_CREAT, 777) == -1)){
        perror("error in opening/creating file");
    }
    return fd;
}

void sendCycle(int command)
{
    printf("sono il sender del server, sto per fare la list o la pull\n");
    int fd;

    if(command == 0)
    {
        fd = ls();
    }
    else
    {
        //bisogna fare la pull del file scritto nel pacchetto
        char * absolutepath = malloc(50);
        if (absolutepath == NULL)
        {
            perror("error in path malloc");
        }

        strcat(absolutepath, LSDIR);
        strcat(absolutepath, packet.content);
        printf("%s\n\n", packet.content);
        fd = openFile(absolutepath);

    }

    int seqnum = details.mySeq;
    int finalSeq = -1;
    int isFinal = 0;
    ssize_t readByte;
    datagram sndPacket;
    struct pipeMessage rtx;
    while(details.sendBase != finalSeq || isFinal == 0)
    {
        while(seqnum%WINDOWSIZE - details.sendBase > 256)
        {
            if(checkPipe(&rtx, pipeFd[0]) != 0)
            {
                printf("ritrasmetto\n");
                memset(sndPacket.content, 0, 512);
                if(lseek(fd, 512*(rtx.seqNum - details.firstSeqNum), SEEK_SET) == -1){
                    perror("errore in lseek");
                }
                if(read(fd, sndPacket.content, 512)==-1){
                    perror("error in read");
                }

                sndPacket.isFinal = rtx.isFinal;
                sndPacket.ackSeqNum = details.remoteSeq;
                sndPacket.seqNum = rtx.seqNum;
                sndPacket.opID = globalOpID;
                sendDatagram(details.sockfd2, &(details.addr2), details.Size2, &sndPacket);
            }
        }
        if (isFinal == 1)
        {
            if(checkPipe(&rtx, pipeFd[0]) != 0)
            {
                printf("ciao giogge! \n\nritrasmetti\n");
            }
        }
        else
        {
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
                sndPacket.ackSeqNum = details.remoteSeq;
                sndPacket.seqNum = seqnum;
                sndPacket.opID = globalOpID;
                printf("ho inviato un pacchetto ackando %u\n", details.remoteSeq);
                sendDatagram(details.sockfd2, &(details.addr2), details.Size2, &sndPacket);

                seqnum = details.mySeq;

            }
            else
            {
                printf("ritrasmetti\n");
            }
        }
    }
    memset(sndPacket.content, 0, 512);
    sndPacket.isFinal = -1;
    sendDatagram(details.sockfd2, &(details.addr2), details.Size2, &sndPacket);
    printf("inviato il pacchetto definitivo con isFinal = -1 \n");
}
