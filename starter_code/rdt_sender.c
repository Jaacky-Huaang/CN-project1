#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120      // millisecond - timeout duration for retransmitting packets

int next_seqno=0; 
int send_base=0;        // seq # of base packet in the sender's window
int window_size = 1; 

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; // stores information about th etimer used for retransmisstion
tcp_packet *sndpkt;     // packet for sending data
tcp_packet *recvpkt;    // packet for receiving data
sigset_t sigmask;       // signal for triggering an action 


void resend_packets(int sig)
{
    if (sig == SIGALRM) // if signal happened, resend the packet
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timout happend");
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
    }
}


void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL); // unblock the signal 
    setitimer(ITIMER_REAL, &timer, NULL); // store starting time
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


// takes delay and the function that would handle the signal (resend unACKed packets)
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);               // set up the signal handler for SIGALRM
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
    int portno, len;
    int next_seqno;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    init_timer(RETRY, resend_packets); // initialize timer with retry and signal handler
    next_seqno = 0;

    while (1) // continue until the end of the file is reached
    {
        // data read from file fp into the buffer with max length of DATA_SIZE
        len = fread(buffer, 1, DATA_SIZE, fp);

        // if no data read, would indicate end of file 
        if ( len <= 0)
        {
            VLOG(INFO, "End Of File has been reached");

            // send a special packet with data size 0 to receiver so that receiver also knows that 
                // it is the end of the file 
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
            break;
        }

        send_base = next_seqno; 
        next_seqno = send_base + len;
        sndpkt = make_packet(len); // create packet with updated len

        memcpy(sndpkt->data, buffer, len); // data copied from the buffer into the packet

        sndpkt->hdr.seqno = send_base;

        // loop for sending packets created 
        do {

            VLOG(DEBUG, "Sending packet %d to %s", 
                    send_base, inet_ntoa(serveraddr.sin_addr));
            /*
             * If the sendto is called for the first time, the system will
             * will assign a random port number so that server can send its
             * response to the src port.
             */
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            start_timer();
            //ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
            //struct sockaddr *src_addr, socklen_t *addrlen);

            // enter a loop to wait for an ACK
            do
            {
                if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                            (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
                {
                    error("recvfrom");
                }

                recvpkt = (tcp_packet *)buffer; // received data is cast to a tcp_packet struct 
                printf("%d \n", get_data_size(recvpkt));

                // check if the size of received data is within expected limits 
                assert(get_data_size(recvpkt) <= DATA_SIZE);

            }while(recvpkt->hdr.ackno < next_seqno); // continue as long as there is no duplicates
            
            // when expected ACK is received or the loop exits, sender stops the timer 
            stop_timer();

            /*resend pack if don't recv ACK */
        } while(recvpkt->hdr.ackno != next_seqno); // makes sure resend if not expected ACK number (next_seqno) or timeout     

        free(sndpkt);
    }

    return 0;

}



