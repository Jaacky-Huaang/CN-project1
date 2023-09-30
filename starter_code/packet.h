enum packet_type {
    DATA,
    ACK,
};

typedef struct {
    int seqno;
    int ackno;
    int ctr_flags;
    int data_size;
}tcp_header;

#define MSS_SIZE    1500
#define UDP_HDR_SIZE    8
#define IP_HDR_SIZE    20
#define TCP_HDR_SIZE    sizeof(tcp_header)
#define DATA_SIZE   (MSS_SIZE - TCP_HDR_SIZE - UDP_HDR_SIZE - IP_HDR_SIZE)
typedef struct {
    tcp_header  hdr;
    char    data[0];
}tcp_packet;

typedef struct {
    tcp_packet *packet;  // Pointer to the actual packet
    int is_sent;         // 1 if packet is sent, 0 otherwise
    int is_acked;        // 1 if ACK is received for this packet, 0 otherwise
    struct timeval sent_time;   // Time at which packet was sent
} PacketStatus;

tcp_packet* make_packet(int seq);
int get_data_size(tcp_packet *pkt);
