#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"

tcp_packet *recvpkt;
tcp_packet *sndpkt;

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;

    char buffer[100][MSS_SIZE]; // 2D buffer of 100 packets and MMS_SIZE defined in packet.c
    char one_packet_container[MSS_SIZE]; // buffer to read each packet from the sender

    struct timeval tp;

    int current_last_packet_index = 0;
    int expected_next = 0;
    int receivedEOF = 0;

    // check command line arguments
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    // socket: create the parent socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    // build the server's Internet address
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    // bind: associate the parent socket with a port 
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    // main loop: wait for a datagram, then echo it
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);

    // loop until end of file is reached 
    while (1) {

        // receive packers from sender
        if (recvfrom(sockfd, one_packet_container, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }

        recvpkt = (tcp_packet *) one_packet_container; // casting into a receiver packet 
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        // CASE 1: seq # > expected_next (missing packets in order)
        if (recvpkt->hdr.seqno > expected_next){
            if (recvpkt->hdr.data_size == 0){
                //VLOG(INFO, "End of File has been reached");
                // close file pointer
                fclose(fp);
                break;
            }
            
            // check the seq # of this packet
            else if (recvpkt->hdr.data_size != 0){
                int difference = recvpkt->hdr.seqno - expected_next;
                int integer = (int)((double) difference / DATA_SIZE);
                int decimals = (double) difference / DATA_SIZE - integer;
                int this_packet_index; 

                if (decimals > 0){
                    this_packet_index = integer + 1;
                }
                else{
                    this_packet_index = integer;
                }
                this_packet_index = this_packet_index - 1;

                // CASE 1-1: this packet > current_index
                if (this_packet_index > current_last_packet_index){
                    current_last_packet_index = this_packet_index;

                    // update buffer
                    memcpy(buffer[this_packet_index], recvpkt->data, get_data_size(recvpkt));

                    // print info of the new packet
                    gettimeofday(&tp, NULL);
                    VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
                }

                // CASE 1-2: check if this packet received is already buffered
                else{
                    if (strlen(buffer[this_packet_index]) == 0){
                        memcpy(buffer[this_packet_index], recvpkt->data, get_data_size(recvpkt));
                        
                        // print info of the next packet
                        gettimeofday(&tp, NULL);
                        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
                    }
                }
            }

            // CASE 2: seq # < expected_next (packets are still arriving)
                // continue!

            // send ACK packet 
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size; // set ackno field to seq number 
            sndpkt->hdr.ctr_flags = ACK;

            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
        }

        // CASE 3: seq # == expected_next (we have a new packet in order)
        else if (recvpkt->hdr.seqno == expected_next){

            // check if the packet is the last packet
            if (recvpkt->hdr.data_size == 0){
                //VLOG(INFO, "End of File has been reached");
                // close file pointer
                fclose(fp);
                break;
            }
            
            // check the seq # of this packet
            else if (recvpkt->hdr.data_size != 0){

                // get the current time. log it fo debugging purposes
                gettimeofday(&tp, NULL);
                VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

                // received data is written to the file fp at the specified position
                fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
                fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);

                // update the expected_next #
                expected_next += get_data_size(recvpkt);

                // check if this packet can fill in the missing index of the buffer (missing packets)
                if(strlen(buffer[0]) > 0){
                    
                    // check if the corrected order packet resolves the gap between expected_next
                    int in_order_packet_index = -1;
                    for (int i = 0; i <= current_last_packet_index; i++){
                        if(strlen(buffer[i] == 0)){
                            in_order_packet_index = i;
                            break;
                        }
                    }

                    if (in_order_packet_index == -1){
                        in_order_packet_index = current_last_packet_index + 1;
                    }

                    // write in file until last index of packet in order
                    fseek(fp, expected_next, SEEK_SET);
                    fwrite(buffer[0], 1, strlen(buffer[0]), fp);
                    expected_next += strlen(buffer[0]);

                    for (int i = 0; i < in_order_packet_index; i++){
                        memset(buffer[i], 0, DATA_SIZE);
                    }

                    // slide window according to how many indices have been filled in the buffer
                    if (in_order_packet_index < current_last_packet_index){
                        for (int i = 0; i < current_last_packet_index - in_order_packet_index; i++){
                            memcpy(buffer[i], buffer[i+in_order_packet_index], DATA_SIZE);
                        }
                        for (int j = current_last_packet_index - in_order_packet_index + 1; j <= current_last_packet_index; j++){
                            memset(buffer[j], 0, DATA_SIZE);
                        }
                    }

                    // update the current_last_packet_index
                    if (current_last_packet_index > in_order_packet_index){
                        current_last_packet_index = current_last_packet_index - in_order_packet_index - 1;
                    }
                    else{
                        current_last_packet_index = -1;
                    }
                }

            }

            // send ACK packet 
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size; // set ackno field to seq number 
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
        }

        // CASE 4: duplicate EOF packet
        else if(recvpkt->hdr.seqno == expected_next){
            // resend the packet
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = -1;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                    (struct sockaddr *) &clientaddr, clientlen) < 0){
                        error("ERROR in sendto");
                    }
        }

        // CASE 5: seq # < expected_next (discard the packets as they passed already)
        else{
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = expected_next;
            sndpkt->hdr.ctr_flags = ACK;

            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
        }

    } // loop ends after end of file reached, file is closed, receiver program terminates 
}
