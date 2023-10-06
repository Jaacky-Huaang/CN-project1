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
#include <math.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //millisecond

int next_seqno=0;
int send_base=0;
int window_size = 10;

int last_seqno=-1; //the sequence number of the last packet

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
struct timeval current_time;

PacketStatus window[10];//PacketStatus is a new struct defined in packet.h to store the status of each packet in the window

int duplicate_ack = -1; //the sequence number of the last ACKed packet
int duplicate_count = 0; //the times of the last ACK received, it will triger fast retransmit when it reaches 3

//tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet *resndpkt;
sigset_t sigmask; 

//the signal-handler function to deal with timeout, "resend_packets" is passed in as the "sig_handler" parameter
void init_timer(int delay, void (*sig_handler)(int)){
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

//to start the timer
void start_timer(){
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

//to stop the timer
void stop_timer(){
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

//to calculate the time difference between two timevals
float get_time_difference(struct timeval t1, struct timeval t2){
    float diff = (t1.tv_sec - t2.tv_sec) * 1000.0f;
    diff += (t1.tv_usec - t2.tv_usec) / 1000.0f;
    return diff;
}

void print_window(PacketStatus window[]){
    for (int i = 0; i < 10; i++){
        if(window[i].packet != NULL) {
            printf("window[%d].packet: %d\n", i, window[i].packet->hdr.seqno);
        } else {
            printf("window[%d].packet: NULL\n", i);
        }
        printf("window[%d].is_sent: %d\n", i, window[i].is_sent);
    }
}

//the signal-handler function to deal with timeout
void resend_packets(int sig){
    if (sig == SIGALRM) {
        VLOG(INFO, "Timeout happend");
        
        // find the oldest packet in the window that has been sent but not acked
        // retransmit the oldest packet in the window
        //print_window(window);
        //resndpkt = window[0].packet;
        VLOG(INFO, "Resending packet: %d", resndpkt->hdr.seqno);
        if (sendto(sockfd, resndpkt, TCP_HDR_SIZE + get_data_size(resndpkt), 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
            error("sendto");
        }
        // update the sent time for the oldest packet in the window
        gettimeofday(&window[0].sent_time, NULL);
        
        // calculate the new timeout value based on flight time
        float flight_time = get_time_difference(current_time, window[0].sent_time);
        if (flight_time < 0) {
            flight_time = 1/100000; // set a very small timeout
        }
        init_timer(RETRY - flight_time, resend_packets);
        start_timer();
        //free(sndpkt);
            
        
    }
}

   

int find_start_of_empty_slots(PacketStatus window[], int size){
    for (int i = 0; i <size; i++) 
    {
        if (window[i].packet == NULL) 
        {
            return i;  
        }else{
            //printf("window[%d].packet: %s\n", i, window[i].packet->data);
        }
    }
    return -1;   //if the window is full, return -1 to indicate
}

//to initialize a window slot in the window
void initialize_window_slot(PacketStatus *window_slot){
    window_slot->packet = NULL;
    window_slot->is_sent = 0;
    window_slot->is_acked = 0;
    window_slot->sent_time.tv_sec = 0;
    window_slot->sent_time.tv_usec = 0;
}


int main (int argc, char **argv){
    int portno, len;
    char *hostname;
    char buffer[MSS_SIZE];
    FILE *fp;
    PacketStatus window[window_size];

    //PacketStatus is a new struct defined in packet.h to store the status of each packet in the window
    //window is an array of PacketStatus struct

    /* check command line arguments */
    if (argc != 4){
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    
    hostname = argv[1];
    portno = atoi(argv[2]);

    //opening the file
    fp = fopen(argv[3], "r");
    if (fp == NULL){
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
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0){
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);
    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //initialize the window to be all 0
    for (int i = 0; i < window_size; i++){
        initialize_window_slot(&window[i]);
    }

    //to get the file size and print it
    //to make sure sample.txt is not cleared and file_size!=0
    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);
    printf("File size: %d bytes\n", file_size);
    fseek(fp, 0, SEEK_SET);

    init_timer(RETRY, resend_packets);

    // loop while EOF is not reached
    // this is the main loop comprising of two sub-loops
    // the first sub-loop is to send packets
    // the second sub-loop is to receive ACKs
    while (1){   
        // find the first index of empty slots in the window
        int start_empty_index = find_start_of_empty_slots(window, window_size);
        if (start_empty_index == -1){
            printf("the window is full\n");
        }

        else if (next_seqno >= file_size){ //if the file is all loaded in the window
            printf("the file is completely read\n");
            // if the window is full, start_empty_index = window_size
            // the first sub-loop would not be entered, and would jump to the second
            // sub-loop and wait for ACKs
        }

        else
        { 
            // if there are empty slots in the window, fill them with packets and send them out
            printf("the first empty slot in the window: %d\n", start_empty_index);
            
            // the window would be left with packets sent but not ACKed
            printf("start sending packets...\n");
            printf("the oldest packet in this window: %d\n", send_base);
            printf("the next packet to be sent: %d\n", next_seqno);

            // this loop is reentered everytime an ACK (not duplicate) is received and window has shifted
            for (int i = start_empty_index; i < window_size; i++){   
                // printf("i: %d\n", i);
                // read data from the file, "len" is the data size of the current packet i
                len = fread(buffer, 1, DATA_SIZE, fp);
                // printf("len: %d\n", len);

                // if the file is completely read
                if (feof(fp)) {
                    printf("End Of File\n");
                } 
                else if (ferror(fp)) {
                    perror("Error reading file\n");
                }

                tcp_packet *sndpkt;
                sndpkt = make_packet(len);
                // store the data into the packet by copying from the buffer
                memcpy(sndpkt->data, buffer, len);

                sndpkt->hdr.seqno = next_seqno;
                // update the current packet's sequence number with the next sequence number
                // send_base will be updated when ACK is properly received, not here
                // for the first packet, next_seqno = send_base =0, so it makes sense as well
                // update the sequence number of the next packet

                next_seqno += len;
                //update the sequence number of the next packet

                printf("send_base after updating: %d\n", send_base);
                printf("next_seqno after updating: %d\n", next_seqno);
                printf("Sending packet: %d to host %s\n", sndpkt->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
                printf("----------------------------------\n");

                // update the packet status in the window
                window[i].packet = sndpkt;
                window[i].is_sent = 1;
                window[i].is_acked = 0;
                // record the time stamp of the current packet's sent time
                // which will be used to reset the timer
                gettimeofday(&window[i].sent_time, NULL);

                // after preparation work, send the packet
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0){
                    error("the packet is not sent");
                }

                //print_window(window);
                //clear the buffer
                //memset(buffer, 0, sizeof(buffer));  

                
                
                // initialize the timer for the send_base of the current window 
                if (i==0){   
                    resndpkt=window[0].packet;
                    printf("initializing resndpkt: %d\n", resndpkt->hdr.seqno);
                    // record current time in current_time
                    gettimeofday(&current_time,NULL);
                    // calculate the time that the first packet has spent in flight
                    // if it has not been sent, window[0].sent_time would be very close to current_time
                    // therefore, the timeout would be basically RETRY
                    float flight_time = get_time_difference(current_time, window[0].sent_time);
                    // if flight_time is negative, it means that the last packet in the window is also timeout
                    // so we set the timeout to be very small to resend the last packet immediately
                    if (flight_time < 0){
                        flight_time = RETRY-1/100000;
                    }
                    printf("initializing timer with time%f\n", RETRY-flight_time);
                    init_timer(RETRY-flight_time, resend_packets);
                    start_timer();
                }

                //free(sndpkt);
                if (len < DATA_SIZE){
                    VLOG(INFO, "End Of File has been reached");
                    // record the sequence number of the last packet as the current sequence number
                    last_seqno = next_seqno;
                    printf("last_seqno: %d\n", last_seqno);
                    // remember to close the file
                    fclose(fp);
                    // break from the loop when file is completely read
                    break;
                }
                
                // if the first packet is sent, it becomes the oldest packet in flight
                // so we start the timer of it
                
            }
        }
        

        char ack_buffer[MSS_SIZE];
        // in the previous while loop, packets have been sent
        // here in this while loop, we wait for ACKs

        
          
        while(1)
        {
            printf("waiting for ACK...\n");
            // get the ACK from the receiver
            int bytes_received = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0,(struct sockaddr *) &serveraddr, (socklen_t *)&serverlen);
            if(bytes_received < 0){
                error("ERROR in recvfrom\n");
            } 
            else {
                //printf("Received %d bytes.\n", bytes_received);
            }

            recvpkt = (tcp_packet *)ack_buffer;
            //VLOG(INFO, "Received the ACK");
            printf("Received ACK with seqno: %d\n", recvpkt->hdr.ackno);
            //printf("packet size: %d \n", recvpkt->hdr.data_size); 
            assert(get_data_size(recvpkt) <= DATA_SIZE);


            // CASE 1: the sequence number of the received ACK packet = last_seqno
            //which has been recorded when the last packet was sent
            if(recvpkt->hdr.ackno == last_seqno){
                stop_timer();
                VLOG(INFO, "Received last ACK");
                //break from the loop of waiting for ACK and exiting the whole program
                return 0;
            }

            // CASE 2: the sequence number of the received ACK packet > send_base
            if (recvpkt->hdr.ackno > send_base){   
                // stop the timer. 
                stop_timer();

                // calculate how many window slots need to be shifted
                // shift # = # of ACKed packets
                int shift = (recvpkt->hdr.ackno - send_base)/DATA_SIZE;

                // update send_base with the cumulative ACK (latest packet that was ACKed)
                send_base = recvpkt->hdr.ackno;

                // Update packet status for ACKed packets
                for (int i = 0; i < shift; i++){  
                    window[i].packet = NULL;  // Free the tcp_packet
                }

                // setting the oldest un-ACKed to the beginning of the window
                for (int i=0; i < window_size - shift; i++){
                    window[i] = window[i+shift];
                }
                
                // emptying the remaining slots to be filled with new packets
                for (int i = window_size - shift; i < window_size; i++){
                    initialize_window_slot(&window[i]);
                }

                resndpkt=window[0].packet;
                printf("initializing resndpkt: %d\n", resndpkt->hdr.seqno);

                start_timer();

                //break from the loop and go to fill the window with more packets
                break;
            }
            
            // CASE 3: the sequence number of the received ACK packet <= send_base
            // this means the ACK is for a packet that has been ACKed before (a duplicate ACK)
            else{   
                printf("this is the case where recvpkt->hdr.ackno < send_base\n");
                
                //duplicate_ack is the sequence number of the last ACKed packet
                if (recvpkt->hdr.ackno != duplicate_ack){
                    duplicate_ack = recvpkt->hdr.ackno;
                    printf("duplicate_ack recorded: %d\n", duplicate_ack);
                    duplicate_count = 1;
                } 
                else {
                    if(duplicate_count < 3){
                        //duplicate_count is the times of the same ACK received
                        duplicate_count++; 
                    } 
                    else{   
                        //if there are three consecutive duplicate ACKs, fast retransmit
                        stop_timer();
                        printf("3 duplicate ACKs, should retransmit: %d\n", send_base);
                        duplicate_count = 0;
                        //int index = ceil((recvpkt->hdr.ackno - send_base)/DATA_SIZE);
                        //printf("index: %d\n", index);
                        //check the window status
                        
                        //print_window(window);
                        tcp_packet* sndpkt = window[0].packet;
                        VLOG(INFO, "Resending packet after 3 duplicates: %d", sndpkt->hdr.seqno);
                        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                                    (const struct sockaddr *)&serveraddr, serverlen) < 0) 
                        {
                            error("sendto");
                        }
                        start_timer();
                        //free(sndpkt);
                        
                    }
                //reinitialize the recvpkt and 
                //memset(recvpkt, 0, sizeof(tcp_packet));
                //memset(ack_buffer, 0, sizeof(ack_buffer));
                }
            }
        }
    }
    return 0;
}
