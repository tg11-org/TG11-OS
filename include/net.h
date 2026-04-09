/**
 * Copyright (C) 2026 TG11
 *
 * Network stack — Ethernet, ARP, IPv4, ICMP, UDP.
 */
#ifndef TG11_NET_H
#define TG11_NET_H

#define ETH_ALEN       6
#define ETH_HLEN       14
#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_IPV4  0x0800

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

/* Packed Ethernet header */
struct eth_hdr
{
    unsigned char  dst[ETH_ALEN];
    unsigned char  src[ETH_ALEN];
    unsigned short ethertype; /* big-endian */
} __attribute__((packed));

/* ARP packet (Ethernet/IPv4) */
struct arp_pkt
{
    unsigned short hw_type;      /* 0x0001 */
    unsigned short proto_type;   /* 0x0800 */
    unsigned char  hw_len;       /* 6 */
    unsigned char  proto_len;    /* 4 */
    unsigned short opcode;       /* 1=request, 2=reply */
    unsigned char  sender_mac[ETH_ALEN];
    unsigned char  sender_ip[4];
    unsigned char  target_mac[ETH_ALEN];
    unsigned char  target_ip[4];
} __attribute__((packed));

/* IPv4 header (no options) */
struct ipv4_hdr
{
    unsigned char  ver_ihl;      /* version=4, ihl=5 (20 bytes) */
    unsigned char  tos;
    unsigned short total_len;    /* big-endian */
    unsigned short ident;
    unsigned short flags_frag;
    unsigned char  ttl;
    unsigned char  protocol;
    unsigned short checksum;     /* big-endian */
    unsigned char  src_ip[4];
    unsigned char  dst_ip[4];
} __attribute__((packed));

/* ICMP header */
struct icmp_hdr
{
    unsigned char  type;
    unsigned char  code;
    unsigned short checksum;
    unsigned short ident;
    unsigned short seq;
} __attribute__((packed));

/* UDP header */
struct udp_hdr
{
    unsigned short src_port;  /* big-endian */
    unsigned short dst_port;  /* big-endian */
    unsigned short length;    /* big-endian */
    unsigned short checksum;  /* big-endian */
} __attribute__((packed));

/* TCP header */
struct tcp_hdr
{
    unsigned short src_port;    /* big-endian */
    unsigned short dst_port;    /* big-endian */
    unsigned int   seq_num;     /* big-endian */
    unsigned int   ack_num;     /* big-endian */
    unsigned char  data_off;    /* upper 4 bits = header len in 32-bit words */
    unsigned char  flags;       /* FIN=0x01, SYN=0x02, RST=0x04, PSH=0x08, ACK=0x10 */
    unsigned short window;      /* big-endian */
    unsigned short checksum;    /* big-endian */
    unsigned short urgent;      /* big-endian */
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* Network configuration */
struct net_config
{
    unsigned char  ip[4];
    unsigned char  gateway[4];
    unsigned char  netmask[4];
    unsigned char  dns[4];
    unsigned char  mac[ETH_ALEN];
};

extern struct net_config net_cfg;

/* Initialise the network stack (calls pci_scan + e1000_init) */
int  net_init(void);

/* Process all pending received packets */
void net_poll(void);

/* Send an ICMP echo request; returns 0 on success, -1 on error.
 * Reply will be processed in net_poll(). */
int  net_ping(const unsigned char dst_ip[4], unsigned short seq);

/* Check whether we got a ping reply for the given seq.
 * Returns 1 if reply received, 0 if still waiting. */
int  net_ping_check(unsigned short seq, unsigned long *rtt_ms);

/* Send a UDP datagram */
int  net_udp_send(const unsigned char dst_ip[4],
                  unsigned short src_port, unsigned short dst_port,
                  const void *data, unsigned long data_len);

/* ARP table helpers */
int  arp_lookup(const unsigned char ip[4], unsigned char mac_out[ETH_ALEN]);
void arp_request(const unsigned char ip[4]);

/* ── DHCP ──────────────────────────────────────────────────────── */

/* Run a full DHCP discover/offer/request/ack exchange.
 * On success: updates net_cfg.ip, gateway, netmask, dns.
 * Returns 0 on success, -1 on timeout/failure. */
int  net_dhcp_request(void);

/* ── DNS ───────────────────────────────────────────────────────── */

/* Resolve a hostname to an IPv4 address via DNS.
 * Returns 0 on success (result in ip_out), -1 on failure. */
int  net_dns_resolve(const char *hostname, unsigned char ip_out[4]);

/* ── TCP ───────────────────────────────────────────────────────── */

#define NET_TCP_MAX_CONN 4
#define NET_TCP_BUF_SIZE 4096

/* Open a TCP connection to dst_ip:dst_port.
 * Returns connection index (0..3) on success, -1 on failure. */
int  net_tcp_connect(const unsigned char dst_ip[4], unsigned short dst_port);

/* Send data on an open TCP connection.
 * Returns 0 on success, -1 on failure. */
int  net_tcp_send(int conn, const void *data, unsigned long len);

/* Receive data from a TCP connection (blocking with timeout).
 * Returns bytes read (>0), 0 if connection closed, -1 on error. */
int  net_tcp_recv(int conn, void *buf, unsigned long buf_size, unsigned long timeout_ticks);

/* Close a TCP connection (sends FIN). */
void net_tcp_close(int conn);

/* Byte-order helpers */
static inline unsigned short htons(unsigned short v)
{
    return (unsigned short)((v >> 8) | (v << 8));
}
#define ntohs(x) htons(x)

static inline unsigned int htonl(unsigned int v)
{
    return ((v >> 24) & 0xFF)
         | ((v >> 8)  & 0xFF00)
         | ((v << 8)  & 0xFF0000)
         | ((v << 24) & 0xFF000000U);
}
#define ntohl(x) htonl(x)

#endif /* TG11_NET_H */
