

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include "externvars.h"


struct selectCell selectiveWnd[WINDOWSIZE];
struct headTimer timerWheel[TIMERSIZE] = {NULL};


struct details details;
int timerSize = TIMERSIZE;
int nanoSleep = NANOSLEEP;
int windowSize = WINDOWSIZE;
int pipeFd[2];
int pipeSendACK[2];
datagram packet;

volatile int finalLen, globalTimerStop = 0;

int globalOpID;
pthread_mutex_t syncMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mtxPacketAndDetails = PTHREAD_MUTEX_INITIALIZER;


volatile int currentTimeSlot = 0;
volatile int rounds = 0;
volatile int roundsSender = 0;

pthread_mutex_t roundsMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t currentTSMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t headtimerMTX = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mtxTimerSleep = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condTimerSleep = PTHREAD_COND_INITIALIZER;



int offset = 10;

//------------------------------------------------------------------------------------------------------START CONNECTION

//---------------------------------------------------------------------------------------------------------CREATE SOCKET

struct sockaddr_in createStruct(unsigned short portN)
{
    struct sockaddr_in address;
    socklen_t serverLen = sizeof(struct sockaddr_in);

    memset((void *) &address, 0, serverLen);//reset del contenuto

    address.sin_family = AF_INET;
    address.sin_port = htons(portN);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    return address;
}

void bindSocket(int sockfd, struct sockaddr * address , socklen_t size)
{
    if(bind(sockfd, address, size) == -1)
    {
        perror("error in bind\n");
        exit(EXIT_FAILURE);
    }
}

int createSocket()
{
    int socketfd;
    socketfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(socketfd == -1)
    {
        perror("error in socket creation\n");
        exit(EXIT_FAILURE);
    }
    return socketfd;
}

int checkSocketAck(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd, handshake * ACK)
{
    ssize_t res;
    res = recvfrom(socketfd, (char *) ACK, sizeof(handshake), 0, (struct sockaddr *) servAddr, &servLen);

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

int checkSocketDatagram(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd, datagram * packet)
{
    ssize_t res;
    res = recvfrom(socketfd, (char *) packet, sizeof(datagram), 0, (struct sockaddr *) servAddr, &servLen);

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

//------------------------------------------------------------------------------------------------------SELECTIVE REPEAT

void initWindow()
{
    memset(selectiveWnd, 0, windowSize * sizeof(struct selectCell));
    int i;
    for(i = 0; i < windowSize; i++)
    {

        if(pthread_mutex_init(&(selectiveWnd[i].cellMtx), NULL) != 0)
        {
            perror("mutex init error");
        }

        mtxLock(&(selectiveWnd[i].cellMtx));
        selectiveWnd[i].value = 0;
        (selectiveWnd[i].packetTimer).nextTimer = NULL;
        mtxUnlock(&(selectiveWnd[i].cellMtx));
    }

    printf("inizializzo ruota della selective\n");

}

void sentPacket(int packetN, int retransmission)
{
    mtxLock(&((selectiveWnd[packetN % windowSize]).cellMtx));

    printf("preso il lock\n");

    (selectiveWnd[packetN % windowSize]).value = 1;
    ((selectiveWnd[packetN % windowSize]).packetTimer).seqNum = packetN;
    printf("updated selective repeat\n");

    int pos = getWheelPosition();
    startTimer(packetN, pos);

    mtxUnlock(&((selectiveWnd[packetN % windowSize]).cellMtx));

    if(retransmission == 0)
    {
        mtxLock(&mtxPacketAndDetails);
//        details.mySeq = (packetN+1)%MAXSEQNUM;
        details.mySeq = (details.mySeq+1)%MAXSEQNUM;
        mtxUnlock(&mtxPacketAndDetails);
    }
}

void ackSentPacket(int ackN)
{
    //printf("aggiorno selective repeat perchè ho ricevuto ack per = %d\n", ackN);

    mtxLock(&((selectiveWnd[ackN % windowSize]).cellMtx));

    if ((selectiveWnd[ackN % windowSize]).value != 0 && (selectiveWnd[ackN % windowSize]).value != 2)
    {
        //printf("aggiorno la selective repeat\n");
        ((selectiveWnd)[ackN % windowSize]).value = 2;

        //--------------------------------------------------------------------------------andrà protetto con un mutex--------------------

        //non ritrasmettere se il thread timer sta troppo vicino
        if(getWheelPosition() == (((selectiveWnd)[ackN % windowSize]).packetTimer).posInWheel)
        {
            //non fermo il timer
        }
        else
             (((selectiveWnd)[ackN % windowSize]).packetTimer).isValid = 0;
        //printf("stoppato il timer in posizione %d\n", (((selectiveWnd)[ackN % windowSize]).packetTimer).posInWheel);
        //printf("timer all'indirizzo %p\n", &(((selectiveWnd)[ackN % windowSize]).packetTimer));

        //-------------------------------------------------------------------------------------------------------------------------------
        mtxUnlock(&((selectiveWnd[ackN % windowSize]).cellMtx));
        slideWindow();
    }
    else {
        printf("mi hai ackato qualcosa che non ho mai inviato, %d\n", ackN);
        //printWindow();
        mtxUnlock(&((selectiveWnd[ackN % windowSize]).cellMtx));
    }
    //printf("esco da acksentpacket\n");
}

void printWindow()
{
    int i;
    printf("\n |");
    for (i = 0; i < windowSize; i++)
    {
        if (i == getSendBase() % windowSize)
        {
            printf(" (%d) |", (selectiveWnd)[i].value);
        }
        else {
            printf(" %d |", (selectiveWnd)[i].value);
        }
    }
    printf("\n");
}

void slideWindow()
{

    int end = 0;
    while(!end)
    {
        mtxLock(&((selectiveWnd[getSendBase()% windowSize]).cellMtx));
        while(selectiveWnd[getSendBase()%windowSize].value == 2)
        {

            selectiveWnd[getSendBase()%windowSize].value = 0;
            mtxUnlock(&((selectiveWnd[getSendBase() % windowSize]).cellMtx));

            mtxLock(&mtxPacketAndDetails);
            details.sendBase = details.sendBase + 1;
            mtxUnlock(&mtxPacketAndDetails);
            //printf("mando avanti sendBase, %d\n", details.sendBase);
        }
        end = 1;
        mtxUnlock(&((selectiveWnd[getSendBase() % windowSize]).cellMtx));
    }

    //printWindow();
}

//----------------------------------------------------------------------------------------------------------------THREAD

void createThread(pthread_t * thread, void * function, void * arguments)
{
    if(pthread_create(thread, NULL, function, arguments) != 0)
    {
        perror("error in pthread_create");
    }
    //printf("thread creato\n");
}

void initPipe(int pipefd[2])
{
    if(pipe(pipefd) == -1)
    {
        perror("error in pipe open");
    }

    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == -1)
    {
        perror("error in fcntl");
    }

}

void mtxLock(pthread_mutex_t * mtx)
{
    if(pthread_mutex_lock(mtx) == -1)
    {
        perror("error on mutex lock");
    }
}

void mtxUnlock(pthread_mutex_t * mtx)
{
    if(pthread_mutex_unlock(mtx) == -1)
    {
        perror("error on mutex unlock");
    }
}

//-----------------------------------------------------------------------------------------------------------------TIMER

void * timerFunction()
{

    printf("timer thread attivato\n\n");
    int i = 0;


    struct timer * currentTimer, * examinedtimer;
    struct pipeMessage rtxN;
    for(;;)
    {
//        for(;;)

        initTimerWheel();
        mtxLock(&currentTSMTX);
        currentTimeSlot = 0;
        mtxUnlock(&currentTSMTX);

        if(i == 0)
        {
            i++;
            mtxLock(&syncMTX);
            globalTimerStop = 2;
            mtxUnlock(&syncMTX);
        }

        if(pthread_cond_wait(&condTimerSleep, &mtxTimerSleep) == -1)
        {
            perror("error in cond_wait timer");
        }

        for(;;){}

        while(readGlobalTimerStop() == 1)
        {

            mtxLock(&headtimerMTX);
            currentTimer = timerWheel[getCurrentTimeSlot()].nextTimer;
            mtxUnlock(&headtimerMTX);

            printf("clocktick\n");
            clockTick();

            while (currentTimer != NULL)
            {
                rtxN.seqNum = currentTimer->seqNum;
                //rtxN.isFinal = 0;---------------------------------------gestire questo

                //printf("sono il timer e dico di ritrasmettere %d\n", rtxN.seqNum);
                mtxLock(&(selectiveWnd[currentTimer->seqNum % windowSize].cellMtx));

                if (currentTimer->isValid) {
                    if (write(pipeFd[1], &rtxN, sizeof(struct pipeMessage)) == -1) {
                        perror("error in pipe write");
                    }
                }
                //printf("|%d, %d|", currentTimer->seqNum, currentTimer->isValid);
                currentTimer->isValid = 0;
                examinedtimer = currentTimer;
                currentTimer = examinedtimer->nextTimer;
                examinedtimer = NULL;
                mtxUnlock(&(selectiveWnd[currentTimer->seqNum % windowSize].cellMtx));

                memset(&rtxN, 0, sizeof(struct pipeMessage));
                printf("indirizzo successivo %p\n", currentTimer);

            }

            if (usleep((useconds_t) nanoSleep) == -1) {
                perror("error on usleep");
            }
        }
    }
}

int readGlobalTimerStop()
{
    mtxLock(&syncMTX);
    int var = globalTimerStop;
    mtxUnlock(&syncMTX);
    return var;
}

void clockTick()
{
    mtxLock(&currentTSMTX);

    currentTimeSlot = (currentTimeSlot + 1) % timerSize;
    printf("%d\n", currentTimeSlot);

    mtxUnlock(&currentTSMTX);
}

int getWheelPosition()
{
    mtxLock(&currentTSMTX);
    int pos = (currentTimeSlot + offset) % timerSize;
    //printf("timer will be set in position %d since offset is %d\n\n", pos, offset);
    mtxUnlock(&currentTSMTX);
    return(pos);
}

void startTimer(int packetN, int posInWheel)
{
    memset(&((selectiveWnd[(packetN)%(windowSize)]).packetTimer), 0, sizeof(struct timer));

    ((selectiveWnd[packetN%windowSize]).packetTimer).seqNum = packetN;
    ((selectiveWnd[packetN%windowSize]).packetTimer).isValid = 1;
    ((selectiveWnd[packetN%windowSize]).packetTimer).posInWheel = posInWheel;

    mtxLock(&headtimerMTX);
    if((timerWheel[posInWheel]).nextTimer != NULL)
    {
        ((selectiveWnd[packetN%windowSize]).packetTimer).nextTimer = (timerWheel[posInWheel]).nextTimer;
    }
    else
        ((selectiveWnd[packetN%windowSize]).packetTimer).nextTimer = NULL;


    (timerWheel[posInWheel]).nextTimer = &((selectiveWnd[(packetN)%(windowSize)]).packetTimer);

    mtxUnlock(&headtimerMTX);
    //printf("indirizzo del timer : %p\n", (timerWheel[posInWheel]).nextTimer);
}

void initTimerWheel()
{
    //printf("inizializzo ruota del timer\n");
    for(int i = 0; i < timerSize; i++)
    {
        timerWheel[i].nextTimer = NULL;
    }
    //printf("inizializzazione terminata\n\n");
}

int checkPipe(struct pipeMessage *rtxN, int pipefd)
{
    memset(rtxN, 0, sizeof(struct pipeMessage));
    if(read(pipefd, rtxN, sizeof(struct pipeMessage)) == -1)
    {
        if(errno != EAGAIN)
        {
            perror("error in pipe read");
            return -1;
        }
        else
        {
            return 0;
        }
    }
    else
    {
        //printf("\n\nho trovato un rtxN\n\n");
        return 1;
    }
}

void sendDatagram(int socketfd, struct sockaddr_in * servAddr, socklen_t servLen, struct datagram_t * sndPacket, int rtx)
{
    printf("scrivo in selective %d\n", sndPacket->seqNum);
    sentPacket(sndPacket->seqNum, rtx);
    printf("fatto\n");
    if (sendto(socketfd, (char *) sndPacket, sizeof(datagram), 0, (struct sockaddr* ) servAddr, servLen)== -1) {
        perror("datagram send error");
    }
    if(usleep(5) != 0)
    {
        perror("usleep error");
    }
    printf("inviato pacchetto con numero di sequenza %u\n", sndPacket->seqNum);
}

void sendACK(int socketfd, handshake *ACK, struct sockaddr_in * servAddr, socklen_t servLen)
{
    ssize_t sentData;
    sentData = sendto(socketfd, (char *) ACK, sizeof(handshake), 0, (struct sockaddr* ) servAddr, servLen);
    if(sentData == -1)
    {
        perror("error in sending data\n");
        exit(EXIT_FAILURE);
    }
    //printf("sent ACK number %d\n", ACK->sequenceNum);
}

int receiveACK(int mainSocket, struct sockaddr * address, socklen_t *slen)
{
    int isFinal = 0;
    char * buffer = malloc(sizeof(datagram));
    if(buffer == NULL)
    {
        perror("error in buffer malloc");
    }
    else
    {
        handshake *ACK;

        ssize_t msgLen = recvfrom(mainSocket, buffer, sizeof(datagram), 0, address, slen);
        if (msgLen == -1 && errno != EAGAIN)
        {
            perror("error in recvfrom");
        }
        else if (msgLen == -1 && errno == EAGAIN)
            return 0;
        else
        {
            if (msgLen == sizeof(handshake))
            {
                ACK = (handshake *) buffer;
                isFinal = ACK->isFinal;
                ackSentPacket(ACK->sequenceNum);
                //printf("ricevuto ack con numero di sequenza %d\n", ACK->sequenceNum);
                free(ACK);
            }
            else
            {
                printf("ho ricevuto un datagramma invece che un ack, mando l'ack\n");
                datagram * duplicatePacket;
                duplicatePacket = (datagram *) buffer;
                tellSenderSendACK(duplicatePacket->seqNum, duplicatePacket->isFinal);
            }
        }
    }

    return isFinal;
}

void acceptConnection(int mainSocket, handshake * ACK, struct sockaddr * address, socklen_t *slen)
{
    ssize_t msgLen = recvfrom(mainSocket, (char *) ACK, sizeof(handshake), 0, address, slen);
    if(msgLen == -1)
    {
        perror("error in recvfrom");
    }
}

int openFile(char * fileName)
{
    printf("sto aprendo il file : %s\n", fileName);
    int fd = open(fileName, O_RDONLY);
    if (fd == -1)
    {
        perror("1: error on open file, retransmission");
        fd = open(fileName, O_RDONLY);
    }
    return fd;
}

void closeFile(int fd)
{
    if(close(fd) == -1)
    {
        perror("error in file close\n");
        exit(EXIT_FAILURE);
    }
}

void sendSignalThread(pthread_mutex_t * mtx, pthread_cond_t * condition)
{
    mtxLock(mtx);
    if(pthread_cond_signal(condition) != 0)
    {
        perror("error in cond signal");
    }
    mtxUnlock(mtx);
}

void waitForAckCycle(int socket, struct sockaddr * address, socklen_t *slen)
{
    while(receiveACK(socket, address, slen) != -1)
    {

    }
}

void getResponse(int socket, struct sockaddr_in * address, socklen_t *slen, int fd, int command)
{
    int isFinal = 0;
    datagram packet;

    mtxLock(&mtxPacketAndDetails);
    int firstPacket = details.remoteSeq + 1;//        lo passo a writeonfile insieme al pacchetto in modo da ricostruire
    mtxUnlock(&mtxPacketAndDetails);

    int ackreceived = 0;
    int alreadyDone = 0;

    while(isFinal != -1)
    {
        if(checkSocketDatagram(address, *slen, socket, &packet) == 1)
        {
            if(packet.opID == getOpID())
            {
                if(packet.seqNum < firstPacket && alreadyDone == 0)
                {
                    mtxLock(&roundsMTX);
                    rounds++;
                    mtxUnlock(&roundsMTX);
                    alreadyDone ++;
                }
                else if(packet.seqNum >= firstPacket && alreadyDone > 0)
                {
                    alreadyDone = 0;
                }
                isFinal = packet.isFinal;
                //printf("isFinal == %d\n", isFinal);
                //----------------------------------------------------------------


                writeOnFile(fd, packet.content, packet.seqNum, firstPacket, (size_t) packet.packetLen);
                //printf("pacchetto scritto\n");
                //----------------------------------------------------------------
                if (command == 0) {
                    if (!ackreceived) {
                        ackSentPacket(packet.ackSeqNum);
                        ackreceived = 1;
                    }
                }

                mtxLock(&mtxPacketAndDetails);
                details.remoteSeq = packet.seqNum;
                mtxUnlock(&mtxPacketAndDetails);
                tellSenderSendACK(packet.seqNum, packet.isFinal);
                //printf("dico di ackare il pacchetto numero %d con isFinal %d\n", packet.seqNum, packet.isFinal);
                memset(&packet, 0, sizeof(datagram));
            }
        }
        //int checkSocketDatagram(struct sockaddr_in * servAddr, socklen_t servLen, int socketfd, datagram * packet)
    }

    printf("\n\n\nho ricevuto il sommo pacchetto finale\n\n\n");
}

void writeOnFile(int file, char * content, int seqnum, int firstnum ,size_t len)
{
    int fileoffset = seqnum-firstnum;
    if(fileoffset < 0)
    {
        fileoffset = MAXSEQNUM + fileoffset;
    }
    if (firstnum != 0)//-----------------------------------------------è a 0 nella list
    {
        //printf("faccio una lseek\n");
        mtxLock(&roundsMTX);
        if ((lseek(file, (fileoffset * 512) + (rounds * MAXSEQNUM), SEEK_SET)) == -1) {
            perror("1: lseek error");
        }
        mtxUnlock(&roundsMTX);
    }
    if (write(file, content, len) == -1)
    {
        perror("error in writeOnFile");
    }

}

void tellSenderSendACK(int packetN, short int isFinal)
{
    struct pipeMessage * tellACK = malloc(sizeof(struct pipeMessage));
    if(tellACK == NULL)
    {
        perror("error in malloc (function tellSenderSendACK)");
    }
    else
    {
        tellACK->seqNum = packetN;
        tellACK->isFinal = isFinal;
        writeOnFile(pipeSendACK[1], (char *) tellACK, 0, 0, sizeof(struct pipeMessage));
    }
}

void ACKandRTXcycle(int socketfd, struct sockaddr_in * servAddr, socklen_t servLen, int command)
{
    int finish = 0;
    struct pipeMessage * pm = malloc(sizeof(struct pipeMessage));
    if(pm == NULL)
        perror("error in malloc");

    handshake * ACK ;
    while(finish != -1)
    {
        if (checkPipe(pm, pipeSendACK[0]) == 1)
        {
            ACK = malloc(sizeof(handshake));

            if (ACK == NULL)
                perror("error in malloc");
            else
            {
                //printf("devo mandare un ack con numero di sequenza : %u\n", pm->seqNum);
                finish = pm->isFinal;
                if(finish == 1) {
                    printf("valore di finish (SENDER) = %u con numero di sequenza %d\n", finish, pm->seqNum);
                }
                ACK->isFinal = pm->isFinal;
                ACK->sequenceNum = pm->seqNum;
                sendACK(socketfd, ACK, servAddr, servLen);
            }
            memset(pm, 0, sizeof(struct pipeMessage));
        }
        if (checkPipe(pm, pipeFd[0]) == 1)
        {
            datagram packetRTX = rebuildDatagram(0 , *pm, command);
            printf("datagramma ricostruito\n");
            sendDatagram(socketfd, servAddr, servLen, &packetRTX, 1);
            printf("ritrasmissione effettuata\n");
            memset(pm, 0, sizeof(struct pipeMessage));
            printf("ritrasmetto1\n");
        }
    }
}

datagram rebuildDatagram(int fd, struct pipeMessage pm, int command) {
    ssize_t readByte;
    datagram sndPacket;

    sndPacket.command = command;

    if (fd != 0)
    {
        readByte = read(fd, sndPacket.content, 512);
        int fileoffset = pm.seqNum - details.firstSeqNum;
        if(fileoffset < 0)
        {
            fileoffset = MAXSEQNUM + fileoffset;
        }

        mtxLock(&mtxPacketAndDetails);
        if (lseek(fd, 512*(fileoffset) + (getRounds() * MAXSEQNUM) , SEEK_SET) == -1)
        {
            perror("errore in lseek");
        }
        sndPacket.ackSeqNum = details.remoteSeq;
        mtxUnlock(&mtxPacketAndDetails);
        if (readByte == -1) {
            perror("error in read");
        }
        if (readByte == 0) {
            sndPacket.isFinal = -1;
        }
        if (readByte < 512 && readByte > 0) {
            sndPacket.isFinal = 1;
        }
        if (readByte == 512) {
            sndPacket.isFinal = 0;
        }
    }
    else
    {
        sndPacket.isFinal = 1;
    }


    sndPacket.seqNum = pm.seqNum;
    printf("ritrasmetto2 %d\n", pm.seqNum);
    sndPacket.opID = getOpID();

    return sndPacket;
}

int getFileLen(int fd)
{

    ssize_t len = lseek(fd, 0L, SEEK_END);
    if(len == -1){
        perror("error in lseek");
    }
    if(lseek(fd, 0L, SEEK_SET) == -1){
        perror("error in lseek");
    }
    len = len%512;
    printf("ho calcolato la grandezza del file\n");
    return (int) len;
}

char * stringParser(char * string)
{
    char * sToReturn  = malloc(512);
    char* start = strrchr(string,'/'); /* Find the last '/' */
    strcpy(sToReturn, start+1);
    return sToReturn;
}

void waitForFirstPacketSender(int socketfd, struct sockaddr_in * servAddr, socklen_t servLen)
{
    int finish = 0;
    struct pipeMessage * pm = malloc(sizeof(struct pipeMessage));
    if(pm == NULL)
        perror("error in malloc");

    while(finish != -1)
    {
        if (checkPipe(pm, pipeSendACK[0]) == 1)
        {
            finish = -1;
            free(pm);
        }
        else if (checkPipe(pm, pipeFd[0]) == 1)
        {
            //datagram packetRTX = rebuildDatagram(*pm);
            sendDatagram(socketfd, servAddr, servLen, &packet, 1);
            memset(pm, 0, sizeof(struct pipeMessage));
            printf("ritrasmetto il primo pacchetto\n");
        }
    }
}

void waitForFirstPacketListener(int socketfd, struct sockaddr_in * servAddr, socklen_t servLen)
{
    while(receiveACK(socketfd, (struct sockaddr *) servAddr, &servLen) == 0){}

    printf("sono uscito da qui \n\n\n");
    struct pipeMessage ack;
    ack.isFinal = 100; // a caso
    ack.seqNum = 100;
    tellSenderSendACK(ack.seqNum, ack.isFinal);
}

void sendSignalTimer()
{
    mtxLock(&syncMTX);
    globalTimerStop = 1;
    mtxUnlock(&syncMTX);
    if(pthread_cond_signal(&condTimerSleep) != 0)
    {
        perror("error in cond_signal timer");
    }
    //printf("segnale mandato\n");
}

int getOpID()
{
    int opID;
    mtxLock(&syncMTX);
    opID = globalOpID;
    mtxUnlock(&syncMTX);
    return opID;
}

int getSeqNum()
{
    int seq;
    mtxLock(&mtxPacketAndDetails);
    seq = details.mySeq;
    mtxUnlock(&mtxPacketAndDetails);
    return seq;
}

int getSendBase()
{
    int base;
    mtxLock(&mtxPacketAndDetails);
    base = details.sendBase;
    mtxUnlock(&mtxPacketAndDetails);
    return base;
}

int getCurrentTimeSlot()
{
    int cts;
    mtxLock(&currentTSMTX);
    cts = currentTimeSlot;
    mtxUnlock(&currentTSMTX);
    return cts;
}

void incrementRounds()
{
    mtxLock(&roundsMTX);
    roundsSender++;
    mtxUnlock(&roundsMTX);
}

int getRounds()
{
    mtxLock(&roundsMTX);
    int r = roundsSender;
    mtxUnlock(&roundsMTX);
    return r;
}
