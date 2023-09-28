enum packet_type {
    DATA,   // packet type that is used for transmitting data - if a packet has the DATA type, it typically contains actual data payload
    ACK,    // acknowledgement #
};

typedef struct {
    int seqno;
    int ackno;
    int ctr_flags;
    int data_size;
}tcp_header;

#define MSS_SIZE    1500    // maximum segment size in a single packet
#define UDP_HDR_SIZE    8   
#define IP_HDR_SIZE    20
#define TCP_HDR_SIZE    sizeof(tcp_header)
#define DATA_SIZE   (MSS_SIZE - TCP_HDR_SIZE - UDP_HDR_SIZE - IP_HDR_SIZE)

typedef struct {
    tcp_header  hdr;
    char    data[0];    // flexible array member (can be populated according to the size of data payload)
}tcp_packet;

tcp_packet* make_packet(int seq);
int get_data_size(tcp_packet *pkt);
