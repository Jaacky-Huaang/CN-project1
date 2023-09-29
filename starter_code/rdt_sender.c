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
#define RETRY  120 //millisecond

int next_seqno=0;
int send_base=0;
int window_size = 10;

int last_seqno; //the sequence number of the last packet

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
struct timeval current_time;

PacketStatus window[window_size];
int duplicate_ack = 0;
int duplicate_count=0

tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;      

//the signal-handler function to deal with timeout
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timout happend");
        //if timeout happens, it means that the oldest packet in the window has not been ACKed
        //resend the all the packets in the window that has not been acked yet
        for (int i = 0; i < window_size; i++)
        {
            if (window[i].is_sent && !window[i].is_acked)
            {
                sndpkt = window[i].packet;
                VLOG(INFO, "Resending packet: %d", sndpkt->hdr.seqno);
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                            ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
                //start a new timer for the oldest packet in the window
                //recursively call itself untill timeout stops occuring 
                //which is when stop_timer() function is called 
                //which is when ACK is properly received
                init_timer(RETRY, resend_packets);
                start_timer();
                free(sndpkt);
                break;
            }
            
        }
    }
}


void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}



void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

float get_time_difference(struct timeval t1, struct timeval t2)
{
    float diff = (t1.tv_sec - t2.tv_sec) * 1000.0f;
    diff += (t1.tv_usec - t2.tv_usec) / 1000.0f;
    return diff;
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

int find_start_of_empty_slots(struct PacketStatus* window, int size) {
    for (int i = size - 1; i >= 0; i--) 
    {
        if (window[i].packet != NULL) 
        {
            return i + 1;  
        }
    }
    return 0;  
}

void initialize_window_slot(struct PacketStatus window_slot) 
{
    window_slot.packet = NULL;
    window_slot.is_sent = 0;
    window_slot.is_acked = 0;
    window_slot.sent_time.tv_sec = 0;
    window_slot.sent_time.tv_usec = 0;
    
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

    next_seqno = 0;

    //initialize the window to be all 0
    for (int i = 0; i < window_size; i++) {
        initialize_window_slot(window[i]);
    }

    //loop while EOF is not reached
    while (1)
    {   
        //find the first index of empty slots in the window
        int start_empty_index = find_start_of_empty_slots(window, window_size);

        //the window would be left with packets sent but not ACKed
        //if the window is full, start_empty_index=window_size
        //the loop would not be entered, and would be waiting for ACK
        //if there are empty slots in the window, fill them with packets and send them out

        //this loop is reentered everytime an ACK (not duplicate) is received and window has shifted
        for (int i =start_empty_index; i<window_size; i++)
        {   
            //read data from the file, "len" is the data size of the current packet i
            len = fread(buffer, 1, DATA_SIZE, fp);

            //if EOF is reached, send an empty packet
            if (len <= 0)
            {
                VLOG(INFO, "End Of File has been reached");
                sndpkt = make_packet(0);
                //record the sequence number of the last packet as the current sequence number
                last_seqno = next_seqno;
                sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                        (const struct sockaddr *)&serveraddr, serverlen);
                free(sndpkt);
                //remember to close the file
                fclose(fp);
                //break from the loop when file is completely read
                break;
            }

            sndpkt = make_packet(len);
            //store the data into the packet by copying from the buffer
            memcpy(sndpkt->data, buffer, len);
            send_base = next_seqno;
            //update the sequence number of the next packet
            next_seqno = send_base + len;
            sndpkt->hdr.seqno = send_base;
            //send_base will be updated when ACK is properly received, not here
            //for the first packet, next_seqno = send_base =0, so it makes sense as well

            window[i].packet = sndpkt;
            window[i].is_sent = 1;
            window[i].is_acked = 0;
            //record the time stamp of the current packet's sent time
            gettimeofday(&window[i].sent_time, NULL);

            //failure to send
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
            error("the packet is not sent");
            }

            //initialize the timer for the send_base of the current window 
            if (i==0)
            {   
                //record current time in current_time
                gettimeofday(&current_time,NULL);
                //calculate the time that the first packet has spent in flight
                //if it has not been sent, window[0].sent_time would be very close to current_time
                //therefore, the timeout would be basically RETRY
                float flight_time = get_time_difference(current_time, window[0].sent_time);
                init_timer(RETRY-flight_time, resend_packets);
                start_timer();
            }

            //free up the memory after sent
            //memset(sndpkt->data,0, strlen(buffer[0]));
            free(sndpkt);

            //if the first packet is sent, it becomes the oldest packet in flight
            //so we start the timer of it
            

        }
    }

        char ack_buffer[MSS_SIZE];
        //in the previous while loop, packets have been sent
        //here in this while loop, we wait for ACKs
        while (1) 
        {   

            //get the ACK from the receiver
            if(recvfrom(sockfd, ack_buffer, MSS_SIZE, 0,(struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
            {
            error("recvfrom");
            }

            recvpkt = (tcp_packet *)ack_buffer;
            printf("%d \n", get_data_size(recvpkt));
            assert(get_data_size(recvpkt) <= DATA_SIZE);

            //case 1: the sequence number of the received ACK packet = last_seqno
            //which has been recorded when the last packet was sent
            if(recvpkt->hdr.ackno == last_seqno) 
            {
            stop_timer();
            VLOG(INFO, "Received last ACK");
            //break from the loop of waiting for ACK
            break;
            }

            //case 2: the sequence number of the received ACK packet > send_base
            if (recvpkt->hdr.ackno > send_base)
            {
                //update send_base with the cumulative ACK
                //this means every packet before recvpkt->hdr.ackno has been ACKed
                send_base = recvpkt->hdr.ackno;

                // stop the timer. 
                stop_timer();

                //calculate how many window slots need to be shifted
                int shift = ceil(recvpkt->hdr.ackno - send_base)/MSS_SIZE;
                
                //update the window
                //free the memory of the ACKed packet
                for (int i = 0; i < shift; i++) 
                {   
                    window[i].is_acked = 1;  
                    free(window[i].packet);  // Free the tcp_packet
                    
                }

                //move the oldest un-ACKed packet to the beginning of the window
                for (int i=0; i<window_size-shift; i++)
                {
                    window[i] = window[i+shift];
                }
                
                //clear the remaining slots in the window for the new packets
                for (int i = window_size - shift; i < window_size; i++) 
                {
                    initialize_window_slot(window[i]);
                }
                
                
            }
            
            //case 3: the sequence number of the received ACK packet < send_base
            //this means the ACK is for a packet that has been ACKed before (a duplicate ACK)
            else if (recvpkt->hdr.ackno < send_base)
            {   
                //duplicate_ack is the sequence number of the last ACKed packet
                if (recvpkt->hdr.ackno != duplicate_ack)
                {
                    duplicate_ack = recvpkt->hdr.ackno;
                    duplicate_count = 1;
                } 
                else 
                {
                    if(duplicate_count < 3)
                    {
                        //duplicate_count is the times of the same ACK received
                        duplicate_count++; 
                    } 
                    else
                    {
                        VLOG(INFO, "Received 3 dup ACK retransmitting: %d", send_base);
                        duplicate_count = 0;
                        stop_timer();
                        // resend the oldest packet immediately by making the timeout very small 
                        timer.it_value.tv_sec = 1 / 1000;
                        timer.it_value.tv_usec = (1 % 1000) * 1000;
                        start_timer();
                    }
                }

            }     

        }

    return 0;

}



