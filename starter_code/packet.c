#include <stdlib.h>
#include"packet.h"

static tcp_packet zero_packet = {.hdr={0}};
/*
 * create TCP packet with header and space for data of size len
 */
tcp_packet* make_packet(int len)
{
    tcp_packet *pkt; // pointer to tcp_packet

    pkt = malloc(TCP_HDR_SIZE + len); // allocate memory with a total size of TCP_HDR_SIZE + len

    *pkt = zero_packet; // ensure all fields are zero 

    pkt->hdr.data_size = len; // set specified len to indicate the size of the data payload
    return pkt;
}

int get_data_size(tcp_packet *pkt)
{
    return pkt->hdr.data_size;
}

