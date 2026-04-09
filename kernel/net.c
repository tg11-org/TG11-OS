/**
 * Copyright (C) 2026 TG11
 *
 * Network stack — Ethernet, ARP, IPv4, ICMP, UDP.
 */
#include "net.h"
#include "e1000.h"
#include "pci.h"
#include "arch.h"
#include "memory.h"
#include "serial.h"
#include "terminal.h"
#include "timer.h"

/* ── Configuration ─────────────────────────────────────────────── */

struct net_config net_cfg;

/* QEMU user-mode networking defaults:
 *   Guest IP:  10.0.2.15
 *   Gateway:   10.0.2.2
 *   DNS:       10.0.2.3
 *   Netmask:   255.255.255.0
 */
static void net_set_defaults(void)
{
    net_cfg.ip[0] = 10; net_cfg.ip[1] = 0;
    net_cfg.ip[2] = 2;  net_cfg.ip[3] = 15;

    net_cfg.gateway[0] = 10; net_cfg.gateway[1] = 0;
    net_cfg.gateway[2] = 2;  net_cfg.gateway[3] = 2;

    net_cfg.netmask[0] = 255; net_cfg.netmask[1] = 255;
    net_cfg.netmask[2] = 255; net_cfg.netmask[3] = 0;

    net_cfg.dns[0] = 10; net_cfg.dns[1] = 0;
    net_cfg.dns[2] = 2;  net_cfg.dns[3] = 3;

    e1000_get_mac(net_cfg.mac);
}

/* ── ARP cache ─────────────────────────────────────────────────── */

#define ARP_CACHE_SIZE 16

struct arp_entry
{
    unsigned char ip[4];
    unsigned char mac[ETH_ALEN];
    int           valid;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

static int ip_eq(const unsigned char a[4], const unsigned char b[4])
{
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

static void ip_copy(unsigned char dst[4], const unsigned char src[4])
{
    dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3];
}

static void mac_copy(unsigned char dst[ETH_ALEN], const unsigned char src[ETH_ALEN])
{
    int i; for (i=0; i<ETH_ALEN; i++) dst[i]=src[i];
}

static void arp_cache_put(const unsigned char ip[4], const unsigned char mac[ETH_ALEN])
{
    int i;
    /* Update existing entry */
    for (i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip))
        {
            mac_copy(arp_cache[i].mac, mac);
            return;
        }
    }
    /* Find empty slot */
    for (i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (!arp_cache[i].valid)
        {
            ip_copy(arp_cache[i].ip, ip);
            mac_copy(arp_cache[i].mac, mac);
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Overwrite slot 0 */
    ip_copy(arp_cache[0].ip, ip);
    mac_copy(arp_cache[0].mac, mac);
    arp_cache[0].valid = 1;
}

int arp_lookup(const unsigned char ip[4], unsigned char mac_out[ETH_ALEN])
{
    int i;
    for (i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip))
        {
            mac_copy(mac_out, arp_cache[i].mac);
            return 0;
        }
    }
    return -1;
}

/* ── Packet buffer ─────────────────────────────────────────────── */

static unsigned char pkt_buf[2048];

/* ── Helpers ───────────────────────────────────────────────────── */

static void mem_copy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    unsigned long i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void mem_zero(void *dst, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned long i;
    for (i = 0; i < n; i++) d[i] = 0;
}

static unsigned short ip_checksum(const void *data, unsigned long len)
{
    const unsigned short *p = (const unsigned short *)data;
    unsigned long sum = 0;
    unsigned long i;
    for (i = 0; i < len / 2; i++)
        sum += p[i];
    if (len & 1)
        sum += ((const unsigned char *)data)[len - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum);
}

/* ── Build & send Ethernet frame ───────────────────────────────── */

static int send_eth(const unsigned char dst_mac[ETH_ALEN],
                    unsigned short ethertype,
                    const void *payload, unsigned long payload_len)
{
    unsigned char frame[1518];
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    unsigned long total;

    if (payload_len + ETH_HLEN > sizeof(frame)) return -1;

    mac_copy(eh->dst, dst_mac);
    mac_copy(eh->src, net_cfg.mac);
    eh->ethertype = htons(ethertype);

    mem_copy(frame + ETH_HLEN, payload, payload_len);
    total = ETH_HLEN + payload_len;
    if (total < 60) total = 60; /* minimum Ethernet frame */

    return e1000_send(frame, total);
}

/* ── Send ARP request ──────────────────────────────────────────── */

void arp_request(const unsigned char ip[4])
{
    struct arp_pkt arp;
    static const unsigned char bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

    arp.hw_type    = htons(1);
    arp.proto_type = htons(0x0800);
    arp.hw_len     = 6;
    arp.proto_len  = 4;
    arp.opcode     = htons(1); /* request */
    mac_copy(arp.sender_mac, net_cfg.mac);
    ip_copy(arp.sender_ip, net_cfg.ip);
    mem_zero(arp.target_mac, 6);
    ip_copy(arp.target_ip, ip);

    send_eth(bcast, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static void arp_reply(const unsigned char dst_mac[ETH_ALEN],
                      const unsigned char dst_ip[4])
{
    struct arp_pkt arp;

    arp.hw_type    = htons(1);
    arp.proto_type = htons(0x0800);
    arp.hw_len     = 6;
    arp.proto_len  = 4;
    arp.opcode     = htons(2); /* reply */
    mac_copy(arp.sender_mac, net_cfg.mac);
    ip_copy(arp.sender_ip, net_cfg.ip);
    mac_copy(arp.target_mac, dst_mac);
    ip_copy(arp.target_ip, dst_ip);

    send_eth(dst_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

/* ── Handle incoming ARP ───────────────────────────────────────── */

static void handle_arp(const unsigned char *data, unsigned long len)
{
    const struct arp_pkt *arp;
    if (len < sizeof(struct arp_pkt)) return;

    arp = (const struct arp_pkt *)data;

    if (ntohs(arp->hw_type) != 1 || ntohs(arp->proto_type) != 0x0800) return;
    if (arp->hw_len != 6 || arp->proto_len != 4) return;

    /* Always cache the sender's MAC/IP */
    arp_cache_put(arp->sender_ip, arp->sender_mac);

    if (ntohs(arp->opcode) == 1) /* ARP request */
    {
        /* If they are asking for our IP, reply */
        if (ip_eq(arp->target_ip, net_cfg.ip))
            arp_reply(arp->sender_mac, arp->sender_ip);
    }
    /* ARP replies are handled by the cache_put above */
}

/* ── ICMP ping tracking ────────────────────────────────────────── */

#define PING_SLOTS 8

struct ping_slot
{
    unsigned short seq;
    unsigned long  send_tick;
    unsigned long  rtt_ms;
    int            active;
    int            replied;
};

static struct ping_slot ping_slots[PING_SLOTS];
static unsigned short   ping_ident = 0x1234;

/* ── Handle incoming ICMP ──────────────────────────────────────── */

static void handle_icmp(const unsigned char *ip_payload, unsigned long ip_payload_len,
                        const unsigned char src_ip[4])
{
    const struct icmp_hdr *icmp;

    if (ip_payload_len < sizeof(struct icmp_hdr)) return;
    icmp = (const struct icmp_hdr *)ip_payload;

    if (icmp->type == 8 && icmp->code == 0) /* Echo Request */
    {
        /* Build echo reply */
        unsigned char reply[1500];
        struct ipv4_hdr *rip;
        struct icmp_hdr *ricmp;
        unsigned long icmp_len = ip_payload_len;
        unsigned long total;
        unsigned char dst_mac[ETH_ALEN];

        if (icmp_len + sizeof(struct ipv4_hdr) > sizeof(reply)) return;

        rip = (struct ipv4_hdr *)reply;
        ricmp = (struct icmp_hdr *)(reply + sizeof(struct ipv4_hdr));

        /* Copy ICMP payload */
        mem_copy(ricmp, ip_payload, icmp_len);
        ricmp->type = 0; /* Echo Reply */
        ricmp->checksum = 0;
        ricmp->checksum = ip_checksum(ricmp, icmp_len);

        /* Build IP header */
        total = sizeof(struct ipv4_hdr) + icmp_len;
        mem_zero(rip, sizeof(struct ipv4_hdr));
        rip->ver_ihl   = 0x45;
        rip->ttl        = 64;
        rip->protocol   = IP_PROTO_ICMP;
        rip->total_len  = htons((unsigned short)total);
        ip_copy(rip->src_ip, net_cfg.ip);
        ip_copy(rip->dst_ip, src_ip);
        rip->checksum = ip_checksum(rip, sizeof(struct ipv4_hdr));

        if (arp_lookup(src_ip, dst_mac) == 0)
            send_eth(dst_mac, ETH_TYPE_IPV4, reply, total);
    }
    else if (icmp->type == 0 && icmp->code == 0) /* Echo Reply */
    {
        int i;
        for (i = 0; i < PING_SLOTS; i++)
        {
            if (ping_slots[i].active &&
                ntohs(icmp->ident) == ping_ident &&
                ntohs(icmp->seq) == ping_slots[i].seq)
            {
                unsigned long now = timer_ticks();
                ping_slots[i].rtt_ms = (now - ping_slots[i].send_tick) * 10;
                ping_slots[i].replied = 1;
                ping_slots[i].active  = 0;
                break;
            }
        }
    }
}

/* ── Handle incoming UDP ───────────────────────────────────────── */

/* UDP receive slot for waiting callers (DHCP, DNS) */
static struct {
    int             waiting;
    unsigned short  port;          /* local port we're listening on */
    unsigned char   src_ip[4];     /* filled on receive */
    unsigned short  src_port;      /* filled on receive */
    const unsigned char *data;     /* points into pkt_buf (valid until next net_poll) */
    unsigned long   data_len;
    int             received;
} udp_rx;

static void handle_udp(const unsigned char *ip_payload, unsigned long ip_payload_len,
                       const unsigned char src_ip[4])
{
    const struct udp_hdr *udp;
    unsigned short src_port, dst_port, udp_len;
    const unsigned char *data;
    unsigned long data_len;

    if (ip_payload_len < sizeof(struct udp_hdr)) return;
    udp = (const struct udp_hdr *)ip_payload;

    src_port = ntohs(udp->src_port);
    dst_port = ntohs(udp->dst_port);
    udp_len  = ntohs(udp->length);

    if (udp_len < sizeof(struct udp_hdr) || udp_len > ip_payload_len) return;

    data     = ip_payload + sizeof(struct udp_hdr);
    data_len = udp_len - sizeof(struct udp_hdr);

    /* Dispatch to waiting receiver */
    if (udp_rx.waiting && dst_port == udp_rx.port && !udp_rx.received)
    {
        ip_copy(udp_rx.src_ip, src_ip);
        udp_rx.src_port = src_port;
        udp_rx.data     = data;
        udp_rx.data_len = data_len;
        udp_rx.received = 1;
        return;
    }

    /* Log received UDP to serial */
    serial_write("[net] UDP ");
    {
        char num[6];
        int n, tmp;
        tmp = (int)src_port; n = 0;
        if (tmp == 0) { num[n++] = '0'; }
        else { char rev[6]; int r=0; while(tmp>0){rev[r++]='0'+(tmp%10);tmp/=10;} while(r>0) num[n++]=rev[--r]; }
        num[n] = '\0'; serial_write(num);
    }
    serial_write(" -> ");
    {
        char num[6];
        int n, tmp;
        tmp = (int)dst_port; n = 0;
        if (tmp == 0) { num[n++] = '0'; }
        else { char rev[6]; int r=0; while(tmp>0){rev[r++]='0'+(tmp%10);tmp/=10;} while(r>0) num[n++]=rev[--r]; }
        num[n] = '\0'; serial_write(num);
    }
    serial_write(" len=");
    {
        char num[6];
        int n, tmp;
        tmp = (int)data_len; n = 0;
        if (tmp == 0) { num[n++] = '0'; }
        else { char rev[6]; int r=0; while(tmp>0){rev[r++]='0'+(tmp%10);tmp/=10;} while(r>0) num[n++]=rev[--r]; }
        num[n] = '\0'; serial_write(num);
    }
    serial_write("\r\n");

    (void)data;
    (void)src_ip;
}

/* ── Handle incoming IPv4 ──────────────────────────────────────── */

/* Forward declaration for TCP handler */
static void handle_tcp(const unsigned char *ip_payload, unsigned long ip_payload_len,
                       const unsigned char src_ip[4]);

static void handle_ipv4(const unsigned char *data, unsigned long len)
{
    const struct ipv4_hdr *ip;
    unsigned long hdr_len, total_len, payload_len;
    const unsigned char *payload;

    if (len < sizeof(struct ipv4_hdr)) return;
    ip = (const struct ipv4_hdr *)data;

    if ((ip->ver_ihl >> 4) != 4) return; /* not IPv4 */
    hdr_len   = (unsigned long)(ip->ver_ihl & 0x0F) * 4;
    total_len = ntohs(ip->total_len);

    if (hdr_len < 20 || total_len < hdr_len || total_len > len) return;

    /* Only accept packets addressed to us or broadcast */
    if (!ip_eq(ip->dst_ip, net_cfg.ip))
    {
        static const unsigned char bcast_ip[4] = {255,255,255,255};
        if (!ip_eq(ip->dst_ip, bcast_ip)) return;
    }

    payload     = data + hdr_len;
    payload_len = total_len - hdr_len;

    switch (ip->protocol)
    {
    case IP_PROTO_ICMP:
        handle_icmp(payload, payload_len, ip->src_ip);
        break;
    case IP_PROTO_TCP:
        handle_tcp(payload, payload_len, ip->src_ip);
        break;
    case IP_PROTO_UDP:
        handle_udp(payload, payload_len, ip->src_ip);
        break;
    default:
        break;
    }
}

/* ── Handle a received Ethernet frame ──────────────────────────── */

static void handle_frame(const unsigned char *frame, unsigned long len)
{
    const struct eth_hdr *eh;
    unsigned short etype;

    if (len < ETH_HLEN) return;
    eh = (const struct eth_hdr *)frame;
    etype = ntohs(eh->ethertype);

    switch (etype)
    {
    case ETH_TYPE_ARP:
        handle_arp(frame + ETH_HLEN, len - ETH_HLEN);
        break;
    case ETH_TYPE_IPV4:
        handle_ipv4(frame + ETH_HLEN, len - ETH_HLEN);
        break;
    default:
        break;
    }
}

/* ── Public API ────────────────────────────────────────────────── */

int net_init(void)
{
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;
    for (i = 0; i < PING_SLOTS; i++)
        ping_slots[i].active = 0;

    serial_write("[net] scanning PCI bus...\r\n");
    pci_scan();

    if (e1000_init() != 0)
    {
        serial_write("[net] no NIC found, networking disabled\r\n");
        serial_write("[net] hint: pass -netdev user,id=net0 -device e1000,netdev=net0 to QEMU\r\n");
        return -1;
    }

    net_set_defaults();

    serial_write("[net] stack ready: IP 10.0.2.15\r\n");
    return 0;
}

void net_poll(void)
{
    unsigned long pkt_len;
    int count = 0;

    while (count < 64) /* process up to 64 packets per call */
    {
        int rc = e1000_poll_rx(pkt_buf, sizeof(pkt_buf), &pkt_len);
        if (rc <= 0) break;
        handle_frame(pkt_buf, pkt_len);
        count++;
    }
}

/* ── Resolve IP -> MAC, using gateway if off-subnet ────────────── */

static int resolve_mac(const unsigned char dst_ip[4], unsigned char mac_out[ETH_ALEN])
{
    const unsigned char *target;
    int same_subnet = 1;
    int i;

    for (i = 0; i < 4; i++)
    {
        if ((dst_ip[i] & net_cfg.netmask[i]) !=
            (net_cfg.ip[i] & net_cfg.netmask[i]))
        {
            same_subnet = 0;
            break;
        }
    }

    target = same_subnet ? dst_ip : net_cfg.gateway;

    if (arp_lookup(target, mac_out) == 0)
        return 0;

    /* Send ARP request and wait briefly */
    arp_request(target);
    {
        unsigned long deadline = timer_ticks() + 200; /* 2 seconds */
        while (timer_ticks() < deadline)
        {
            net_poll();
            if (arp_lookup(target, mac_out) == 0)
                return 0;
        }
    }

    return -1; /* ARP timeout */
}

int net_ping(const unsigned char dst_ip[4], unsigned short seq)
{
    unsigned char pkt[sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr) + 32];
    struct ipv4_hdr *ip;
    struct icmp_hdr *icmp;
    unsigned char *payload;
    unsigned long total;
    unsigned char dst_mac[ETH_ALEN];
    int slot, i;

    /* Find a free ping slot */
    slot = -1;
    for (i = 0; i < PING_SLOTS; i++)
    {
        if (!ping_slots[i].active && !ping_slots[i].replied)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    /* Resolve MAC */
    if (resolve_mac(dst_ip, dst_mac) != 0) return -1;

    /* Build ICMP echo request */
    total = sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr) + 32;
    mem_zero(pkt, total);

    ip = (struct ipv4_hdr *)pkt;
    ip->ver_ihl   = 0x45;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_ICMP;
    ip->total_len  = htons((unsigned short)total);
    ip_copy(ip->src_ip, net_cfg.ip);
    ip_copy(ip->dst_ip, dst_ip);
    ip->checksum = ip_checksum(ip, sizeof(struct ipv4_hdr));

    icmp = (struct icmp_hdr *)(pkt + sizeof(struct ipv4_hdr));
    icmp->type  = 8; /* Echo Request */
    icmp->code  = 0;
    icmp->ident = htons(ping_ident);
    icmp->seq   = htons(seq);

    payload = (unsigned char *)icmp + sizeof(struct icmp_hdr);
    for (i = 0; i < 32; i++) payload[i] = (unsigned char)('A' + (i % 26));

    icmp->checksum = ip_checksum(icmp, sizeof(struct icmp_hdr) + 32);

    ping_slots[slot].seq       = seq;
    ping_slots[slot].send_tick = timer_ticks();
    ping_slots[slot].rtt_ms    = 0;
    ping_slots[slot].active    = 1;
    ping_slots[slot].replied   = 0;

    return send_eth(dst_mac, ETH_TYPE_IPV4, pkt, total);
}

int net_ping_check(unsigned short seq, unsigned long *rtt_ms)
{
    int i;
    for (i = 0; i < PING_SLOTS; i++)
    {
        if (ping_slots[i].replied && ping_slots[i].seq == seq)
        {
            if (rtt_ms) *rtt_ms = ping_slots[i].rtt_ms;
            ping_slots[i].replied = 0; /* consume */
            return 1;
        }
    }
    return 0;
}

int net_udp_send(const unsigned char dst_ip[4],
                 unsigned short src_port, unsigned short dst_port,
                 const void *data, unsigned long data_len)
{
    unsigned char pkt[1500];
    struct ipv4_hdr *ip;
    struct udp_hdr *udp;
    unsigned long total;
    unsigned char dst_mac[ETH_ALEN];

    if (data_len + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) > sizeof(pkt))
        return -1;

    if (resolve_mac(dst_ip, dst_mac) != 0) return -1;

    total = sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + data_len;
    mem_zero(pkt, total);

    ip = (struct ipv4_hdr *)pkt;
    ip->ver_ihl   = 0x45;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_UDP;
    ip->total_len  = htons((unsigned short)total);
    ip_copy(ip->src_ip, net_cfg.ip);
    ip_copy(ip->dst_ip, dst_ip);
    ip->checksum = ip_checksum(ip, sizeof(struct ipv4_hdr));

    udp = (struct udp_hdr *)(pkt + sizeof(struct ipv4_hdr));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((unsigned short)(sizeof(struct udp_hdr) + data_len));
    udp->checksum = 0; /* optional for IPv4 */

    mem_copy(pkt + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr), data, data_len);

    return send_eth(dst_mac, ETH_TYPE_IPV4, pkt, total);
}

/* ── UDP broadcast send (for DHCP — src 0.0.0.0, dst 255.255.255.255) ── */

static int net_udp_send_broadcast(unsigned short src_port, unsigned short dst_port,
                                  const void *data, unsigned long data_len)
{
    unsigned char pkt[1500];
    struct ipv4_hdr *ip;
    struct udp_hdr *udp;
    unsigned long total;
    static const unsigned char bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const unsigned char zero_ip[4] = {0,0,0,0};
    static const unsigned char bcast_ip[4] = {255,255,255,255};

    if (data_len + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) > sizeof(pkt))
        return -1;

    total = sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + data_len;
    mem_zero(pkt, total);

    ip = (struct ipv4_hdr *)pkt;
    ip->ver_ihl   = 0x45;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_UDP;
    ip->total_len  = htons((unsigned short)total);
    ip_copy(ip->src_ip, zero_ip);
    ip_copy(ip->dst_ip, bcast_ip);
    ip->checksum = ip_checksum(ip, sizeof(struct ipv4_hdr));

    udp = (struct udp_hdr *)(pkt + sizeof(struct ipv4_hdr));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((unsigned short)(sizeof(struct udp_hdr) + data_len));
    udp->checksum = 0;

    mem_copy(pkt + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr), data, data_len);

    return send_eth(bcast_mac, ETH_TYPE_IPV4, pkt, total);
}

/* ══════════════════════════════════════════════════════════════════
 *  DHCP Client
 * ══════════════════════════════════════════════════════════════════ */

/* DHCP packet layout (simplified, fixed-size) */
struct dhcp_pkt
{
    unsigned char  op;           /* 1=request, 2=reply */
    unsigned char  htype;        /* 1=Ethernet */
    unsigned char  hlen;         /* 6 */
    unsigned char  hops;
    unsigned int   xid;          /* transaction ID */
    unsigned short secs;
    unsigned short flags;        /* 0x8000 = broadcast */
    unsigned char  ciaddr[4];    /* client IP */
    unsigned char  yiaddr[4];    /* your (offered) IP */
    unsigned char  siaddr[4];    /* server IP */
    unsigned char  giaddr[4];    /* gateway IP */
    unsigned char  chaddr[16];   /* client hardware address */
    unsigned char  sname[64];    /* server name (zero) */
    unsigned char  file[128];    /* boot filename (zero) */
    unsigned char  cookie[4];    /* magic cookie 99.130.83.99 */
    unsigned char  options[312]; /* DHCP options */
} __attribute__((packed));

#define DHCP_MAGIC_0  99
#define DHCP_MAGIC_1 130
#define DHCP_MAGIC_2  83
#define DHCP_MAGIC_3  99

/* Helper: wait for a UDP packet on a given port with timeout.
 * Copies up to max_len bytes into out_buf. Returns actual data_len or 0 on timeout. */
static unsigned long udp_recv_wait(unsigned short port, unsigned char *out_buf,
                                   unsigned long max_len, unsigned long timeout_t)
{
    unsigned long deadline = timer_ticks() + timeout_t;
    unsigned long got;

    udp_rx.waiting  = 1;
    udp_rx.port     = port;
    udp_rx.received = 0;

    while (timer_ticks() < deadline)
    {
        net_poll();
        if (udp_rx.received)
        {
            got = udp_rx.data_len;
            if (got > max_len) got = max_len;
            mem_copy(out_buf, udp_rx.data, got);
            udp_rx.waiting  = 0;
            udp_rx.received = 0;
            return got;
        }
    }

    udp_rx.waiting  = 0;
    udp_rx.received = 0;
    return 0;
}

static void dhcp_build_discover(struct dhcp_pkt *pkt, unsigned int xid)
{
    mem_zero(pkt, sizeof(*pkt));
    pkt->op    = 1; /* BOOTREQUEST */
    pkt->htype = 1;
    pkt->hlen  = 6;
    pkt->xid   = htonl(xid);
    pkt->flags = htons(0x8000); /* broadcast */
    mac_copy(pkt->chaddr, net_cfg.mac);
    pkt->cookie[0] = DHCP_MAGIC_0;
    pkt->cookie[1] = DHCP_MAGIC_1;
    pkt->cookie[2] = DHCP_MAGIC_2;
    pkt->cookie[3] = DHCP_MAGIC_3;

    /* Options: message type = discover */
    int oi = 0;
    pkt->options[oi++] = 53;  /* DHCP message type */
    pkt->options[oi++] = 1;
    pkt->options[oi++] = 1;   /* DISCOVER */
    /* Parameter request list */
    pkt->options[oi++] = 55;
    pkt->options[oi++] = 3;
    pkt->options[oi++] = 1;   /* subnet mask */
    pkt->options[oi++] = 3;   /* router */
    pkt->options[oi++] = 6;   /* DNS */
    pkt->options[oi++] = 255; /* end */
}

static void dhcp_build_request(struct dhcp_pkt *pkt, unsigned int xid,
                               const unsigned char server_ip[4],
                               const unsigned char offered_ip[4])
{
    mem_zero(pkt, sizeof(*pkt));
    pkt->op    = 1;
    pkt->htype = 1;
    pkt->hlen  = 6;
    pkt->xid   = htonl(xid);
    pkt->flags = htons(0x8000);
    mac_copy(pkt->chaddr, net_cfg.mac);
    pkt->cookie[0] = DHCP_MAGIC_0;
    pkt->cookie[1] = DHCP_MAGIC_1;
    pkt->cookie[2] = DHCP_MAGIC_2;
    pkt->cookie[3] = DHCP_MAGIC_3;

    int oi = 0;
    pkt->options[oi++] = 53;  /* DHCP message type */
    pkt->options[oi++] = 1;
    pkt->options[oi++] = 3;   /* REQUEST */
    /* Requested IP */
    pkt->options[oi++] = 50;
    pkt->options[oi++] = 4;
    pkt->options[oi++] = offered_ip[0];
    pkt->options[oi++] = offered_ip[1];
    pkt->options[oi++] = offered_ip[2];
    pkt->options[oi++] = offered_ip[3];
    /* Server identifier */
    pkt->options[oi++] = 54;
    pkt->options[oi++] = 4;
    pkt->options[oi++] = server_ip[0];
    pkt->options[oi++] = server_ip[1];
    pkt->options[oi++] = server_ip[2];
    pkt->options[oi++] = server_ip[3];
    /* Parameter request list */
    pkt->options[oi++] = 55;
    pkt->options[oi++] = 3;
    pkt->options[oi++] = 1;   /* subnet mask */
    pkt->options[oi++] = 3;   /* router */
    pkt->options[oi++] = 6;   /* DNS */
    pkt->options[oi++] = 255; /* end */
}

/* Parse DHCP options from an OFFER or ACK response.
 * Returns the DHCP message type (2=offer, 5=ack) or 0 on error. */
static int dhcp_parse_options(const unsigned char *opts, unsigned long opts_len,
                              unsigned char server_ip[4],
                              unsigned char subnet[4],
                              unsigned char router[4],
                              unsigned char dns[4])
{
    int msg_type = 0;
    unsigned long i = 0;

    while (i < opts_len)
    {
        unsigned char opt = opts[i++];
        if (opt == 255) break;     /* end */
        if (opt == 0) continue;    /* padding */
        if (i >= opts_len) break;
        unsigned char len = opts[i++];
        if (i + len > opts_len) break;

        switch (opt)
        {
        case 53: /* DHCP Message Type */
            if (len >= 1) msg_type = opts[i];
            break;
        case 54: /* Server Identifier */
            if (len >= 4 && server_ip) ip_copy(server_ip, &opts[i]);
            break;
        case 1:  /* Subnet Mask */
            if (len >= 4 && subnet) ip_copy(subnet, &opts[i]);
            break;
        case 3:  /* Router */
            if (len >= 4 && router) ip_copy(router, &opts[i]);
            break;
        case 6:  /* DNS */
            if (len >= 4 && dns) ip_copy(dns, &opts[i]);
            break;
        }
        i += len;
    }
    return msg_type;
}

int net_dhcp_request(void)
{
    static struct dhcp_pkt dpkt;   /* static to avoid huge stack */
    static unsigned char reply[600];
    unsigned long rlen;
    unsigned char offered_ip[4], server_ip[4], subnet[4], router[4], dns[4];
    unsigned int xid = 0x54473131; /* "TG11" */
    int msg_type;

    mem_zero(offered_ip, 4);
    mem_zero(server_ip, 4);
    mem_zero(subnet, 4);
    mem_zero(router, 4);
    mem_zero(dns, 4);

    /* 1. Send DISCOVER */
    dhcp_build_discover(&dpkt, xid);
    if (net_udp_send_broadcast(68, 67, &dpkt, sizeof(dpkt)) != 0)
        return -1;

    /* 2. Wait for OFFER (5 seconds) */
    rlen = udp_recv_wait(68, reply, sizeof(reply), 500);
    if (rlen < 240) return -1;

    /* Validate: must be reply with our xid */
    if (reply[0] != 2) return -1; /* not BOOTREPLY */
    {
        unsigned int rxid = ((unsigned int)reply[4] << 24)
                          | ((unsigned int)reply[5] << 16)
                          | ((unsigned int)reply[6] << 8)
                          | ((unsigned int)reply[7]);
        if (rxid != xid) return -1;
    }

    /* Extract offered IP (yiaddr at offset 16) */
    ip_copy(offered_ip, &reply[16]);

    /* Parse options (starting at offset 240 = after cookie) */
    msg_type = dhcp_parse_options(&reply[240], rlen - 240,
                                 server_ip, subnet, router, dns);
    if (msg_type != 2) return -1; /* not an OFFER */

    /* 3. Send REQUEST */
    dhcp_build_request(&dpkt, xid, server_ip, offered_ip);
    if (net_udp_send_broadcast(68, 67, &dpkt, sizeof(dpkt)) != 0)
        return -1;

    /* 4. Wait for ACK (5 seconds) */
    rlen = udp_recv_wait(68, reply, sizeof(reply), 500);
    if (rlen < 240) return -1;

    if (reply[0] != 2) return -1;
    {
        unsigned int rxid = ((unsigned int)reply[4] << 24)
                          | ((unsigned int)reply[5] << 16)
                          | ((unsigned int)reply[6] << 8)
                          | ((unsigned int)reply[7]);
        if (rxid != xid) return -1;
    }

    ip_copy(offered_ip, &reply[16]);

    /* Re-parse options from ACK (may differ from OFFER) */
    mem_zero(server_ip, 4); mem_zero(subnet, 4);
    mem_zero(router, 4); mem_zero(dns, 4);
    msg_type = dhcp_parse_options(&reply[240], rlen - 240,
                                 server_ip, subnet, router, dns);
    if (msg_type != 5) return -1; /* not an ACK */

    /* 5. Apply configuration */
    ip_copy(net_cfg.ip, offered_ip);
    if (subnet[0] || subnet[1] || subnet[2] || subnet[3])
        ip_copy(net_cfg.netmask, subnet);
    if (router[0] || router[1] || router[2] || router[3])
        ip_copy(net_cfg.gateway, router);
    if (dns[0] || dns[1] || dns[2] || dns[3])
        ip_copy(net_cfg.dns, dns);

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  DNS Resolver
 * ══════════════════════════════════════════════════════════════════ */

static unsigned long str_len(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/* Build a DNS query packet. Returns total packet length, or 0 on error. */
static unsigned long dns_build_query(unsigned char *buf, unsigned long buf_size,
                                     const char *hostname, unsigned short txid)
{
    unsigned long pos = 0;
    unsigned long name_len = str_len(hostname);
    unsigned long i;

    /* Minimum: 12 (header) + name + 2 (QTYPE) + 2 (QCLASS) */
    if (12 + name_len + 2 + 4 > buf_size) return 0;

    mem_zero(buf, buf_size < 512 ? buf_size : 512);

    /* DNS header */
    buf[0] = (unsigned char)(txid >> 8);
    buf[1] = (unsigned char)(txid & 0xFF);
    buf[2] = 0x01; /* RD (recursion desired) */
    buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01; /* QDCOUNT = 1 */
    /* ANCOUNT, NSCOUNT, ARCOUNT = 0 */
    pos = 12;

    /* Encode QNAME: "www.example.com" → 3www7example3com0 */
    i = 0;
    while (i < name_len)
    {
        unsigned long label_start = i;
        unsigned long lbl_len;
        while (i < name_len && hostname[i] != '.') i++;
        lbl_len = i - label_start;
        if (lbl_len == 0 || lbl_len > 63) return 0;
        if (pos + 1 + lbl_len >= buf_size) return 0;
        buf[pos++] = (unsigned char)lbl_len;
        {
            unsigned long k;
            for (k = 0; k < lbl_len; k++)
                buf[pos++] = (unsigned char)hostname[label_start + k];
        }
        if (i < name_len) i++; /* skip '.' */
    }
    buf[pos++] = 0; /* end of QNAME */

    /* QTYPE = A (1) */
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;
    /* QCLASS = IN (1) */
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;

    return pos;
}

/* Parse a DNS response. Returns 0 on success (IP in ip_out), -1 on failure. */
static int dns_parse_response(const unsigned char *buf, unsigned long len,
                              unsigned short txid, unsigned char ip_out[4])
{
    unsigned short resp_id, flags, qdcount, ancount;
    unsigned long pos;
    int i;

    if (len < 12) return -1;

    resp_id = (unsigned short)((buf[0] << 8) | buf[1]);
    if (resp_id != txid) return -1;

    flags = (unsigned short)((buf[2] << 8) | buf[3]);
    if (!(flags & 0x8000)) return -1; /* not a response */
    if (flags & 0x000F) return -1;    /* RCODE != 0 (error) */

    qdcount = (unsigned short)((buf[4] << 8) | buf[5]);
    ancount = (unsigned short)((buf[6] << 8) | buf[7]);

    pos = 12;

    /* Skip question section */
    for (i = 0; i < (int)qdcount; i++)
    {
        /* Skip QNAME */
        while (pos < len)
        {
            if (buf[pos] == 0) { pos++; break; }
            if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; break; }
            pos += 1 + buf[pos];
        }
        pos += 4; /* QTYPE + QCLASS */
    }

    /* Parse answer records, looking for A record */
    for (i = 0; i < (int)ancount && pos < len; i++)
    {
        unsigned short rtype, rclass, rdlen;

        /* Skip NAME (pointer or labels) */
        while (pos < len)
        {
            if (buf[pos] == 0) { pos++; break; }
            if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; break; }
            pos += 1 + buf[pos];
        }

        if (pos + 10 > len) return -1;
        rtype  = (unsigned short)((buf[pos] << 8) | buf[pos+1]); pos += 2;
        rclass = (unsigned short)((buf[pos] << 8) | buf[pos+1]); pos += 2;
        pos += 4; /* TTL */
        rdlen  = (unsigned short)((buf[pos] << 8) | buf[pos+1]); pos += 2;

        if (rtype == 1 && rclass == 1 && rdlen == 4 && pos + 4 <= len)
        {
            ip_copy(ip_out, &buf[pos]);
            return 0;
        }
        pos += rdlen;
    }

    return -1; /* no A record found */
}

int net_dns_resolve(const char *hostname, unsigned char ip_out[4])
{
    static unsigned char qbuf[512];
    static unsigned char rbuf[512];
    unsigned long qlen, rlen;
    unsigned short txid = 0xDA01;

    qlen = dns_build_query(qbuf, sizeof(qbuf), hostname, txid);
    if (qlen == 0) return -1;

    /* Send query to configured DNS server, port 53 */
    if (net_udp_send(net_cfg.dns, 1053, 53, qbuf, qlen) != 0)
        return -1;

    /* Wait for response (3 seconds) */
    rlen = udp_recv_wait(1053, rbuf, sizeof(rbuf), 300);
    if (rlen == 0) return -1;

    return dns_parse_response(rbuf, rlen, txid, ip_out);
}

/* ══════════════════════════════════════════════════════════════════
 *  TCP Stack (basic)
 * ══════════════════════════════════════════════════════════════════ */

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

struct tcp_conn {
    int            active;
    enum tcp_state state;
    unsigned char  remote_ip[4];
    unsigned short local_port;
    unsigned short remote_port;
    unsigned int   seq;          /* our next sequence number */
    unsigned int   ack;          /* next expected byte from remote */
    unsigned char  rx_buf[NET_TCP_BUF_SIZE];
    unsigned long  rx_len;       /* bytes available in rx_buf */
    int            rx_fin;       /* remote sent FIN */
};

static struct tcp_conn tcp_conns[NET_TCP_MAX_CONN];
static unsigned short  tcp_next_port = 49152;

/* TCP pseudo-header checksum */
static unsigned short tcp_checksum(const unsigned char src_ip[4],
                                   const unsigned char dst_ip[4],
                                   const void *tcp_seg, unsigned long tcp_len)
{
    unsigned long sum = 0;
    const unsigned short *p;
    unsigned long i;

    /* Pseudo-header: src_ip(4) + dst_ip(4) + zero(1) + proto(1) + tcp_len(2) */
    sum += ((unsigned short)src_ip[0] << 8) | src_ip[1];
    sum += ((unsigned short)src_ip[2] << 8) | src_ip[3];
    sum += ((unsigned short)dst_ip[0] << 8) | dst_ip[1];
    sum += ((unsigned short)dst_ip[2] << 8) | dst_ip[3];
    sum += IP_PROTO_TCP;
    sum += (unsigned short)tcp_len;

    /* TCP segment */
    p = (const unsigned short *)tcp_seg;
    for (i = 0; i < tcp_len / 2; i++)
        sum += ntohs(p[i]);
    if (tcp_len & 1)
        sum += ((const unsigned char *)tcp_seg)[tcp_len - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((unsigned short)(~sum));
}

/* Send a raw TCP segment with flags. data/data_len can be NULL/0. */
static int tcp_send_segment(struct tcp_conn *c, unsigned char flags,
                            const void *data, unsigned long data_len)
{
    unsigned char pkt[1500];
    struct ipv4_hdr *ip;
    struct tcp_hdr *tcp;
    unsigned long tcp_hdr_len = 20;
    unsigned long total;
    unsigned char dst_mac[ETH_ALEN];

    if (tcp_hdr_len + data_len + sizeof(struct ipv4_hdr) > sizeof(pkt))
        return -1;

    if (resolve_mac(c->remote_ip, dst_mac) != 0)
        return -1;

    total = sizeof(struct ipv4_hdr) + tcp_hdr_len + data_len;
    mem_zero(pkt, total);

    ip = (struct ipv4_hdr *)pkt;
    ip->ver_ihl  = 0x45;
    ip->ttl       = 64;
    ip->protocol  = IP_PROTO_TCP;
    ip->total_len = htons((unsigned short)total);
    ip_copy(ip->src_ip, net_cfg.ip);
    ip_copy(ip->dst_ip, c->remote_ip);
    ip->checksum = ip_checksum(ip, sizeof(struct ipv4_hdr));

    tcp = (struct tcp_hdr *)(pkt + sizeof(struct ipv4_hdr));
    tcp->src_port = htons(c->local_port);
    tcp->dst_port = htons(c->remote_port);
    tcp->seq_num  = htonl(c->seq);
    tcp->ack_num  = htonl(c->ack);
    tcp->data_off = (unsigned char)((tcp_hdr_len / 4) << 4);
    tcp->flags    = flags;
    tcp->window   = htons(NET_TCP_BUF_SIZE);
    tcp->checksum = 0;

    if (data_len > 0)
        mem_copy(pkt + sizeof(struct ipv4_hdr) + tcp_hdr_len, data, data_len);

    tcp->checksum = tcp_checksum(net_cfg.ip, c->remote_ip,
                                 pkt + sizeof(struct ipv4_hdr),
                                 tcp_hdr_len + data_len);

    return send_eth(dst_mac, ETH_TYPE_IPV4, pkt, total);
}

/* Handle incoming TCP segment */
static void handle_tcp(const unsigned char *ip_payload, unsigned long ip_payload_len,
                       const unsigned char src_ip[4])
{
    const struct tcp_hdr *tcp;
    unsigned short src_port, dst_port;
    unsigned int seq_num, ack_num;
    unsigned char flags;
    unsigned long hdr_len, data_len;
    const unsigned char *data;
    struct tcp_conn *c = (void *)0;
    int i;

    if (ip_payload_len < sizeof(struct tcp_hdr)) return;
    tcp = (const struct tcp_hdr *)ip_payload;

    src_port = ntohs(tcp->src_port);
    dst_port = ntohs(tcp->dst_port);
    seq_num  = ntohl(tcp->seq_num);
    ack_num  = ntohl(tcp->ack_num);
    flags    = tcp->flags;
    hdr_len  = (unsigned long)((tcp->data_off >> 4) & 0x0F) * 4;

    if (hdr_len < 20 || hdr_len > ip_payload_len) return;
    data     = ip_payload + hdr_len;
    data_len = ip_payload_len - hdr_len;

    /* Find matching connection */
    for (i = 0; i < NET_TCP_MAX_CONN; i++)
    {
        if (tcp_conns[i].active &&
            tcp_conns[i].local_port == dst_port &&
            tcp_conns[i].remote_port == src_port &&
            ip_eq(tcp_conns[i].remote_ip, src_ip))
        {
            c = &tcp_conns[i];
            break;
        }
    }

    if (!c) return; /* no connection for this segment */

    /* RST kills the connection immediately */
    if (flags & TCP_RST)
    {
        c->state  = TCP_CLOSED;
        c->active = 0;
        return;
    }

    switch (c->state)
    {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK))
        {
            c->ack = seq_num + 1;
            c->seq = ack_num;
            c->state = TCP_ESTABLISHED;
            /* Send ACK to complete handshake */
            tcp_send_segment(c, TCP_ACK, (void *)0, 0);
        }
        break;

    case TCP_ESTABLISHED:
        /* Handle incoming data */
        if (data_len > 0 && seq_num == c->ack)
        {
            unsigned long space = NET_TCP_BUF_SIZE - c->rx_len;
            unsigned long copy = data_len < space ? data_len : space;
            if (copy > 0)
            {
                mem_copy(c->rx_buf + c->rx_len, data, copy);
                c->rx_len += copy;
            }
            c->ack = seq_num + (unsigned int)data_len;
            tcp_send_segment(c, TCP_ACK, (void *)0, 0);
        }
        /* Handle FIN */
        if (flags & TCP_FIN)
        {
            if (data_len == 0) c->ack = seq_num + 1;
            c->rx_fin = 1;
            c->state = TCP_CLOSE_WAIT;
            tcp_send_segment(c, TCP_ACK, (void *)0, 0);
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK)
        {
            c->seq = ack_num;
            if (flags & TCP_FIN)
            {
                c->ack = seq_num + 1;
                c->state = TCP_TIME_WAIT;
                tcp_send_segment(c, TCP_ACK, (void *)0, 0);
                /* Immediately close in our simple implementation */
                c->state  = TCP_CLOSED;
                c->active = 0;
            }
            else
            {
                c->state = TCP_FIN_WAIT_2;
            }
        }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FIN)
        {
            c->ack = seq_num + 1;
            tcp_send_segment(c, TCP_ACK, (void *)0, 0);
            c->state  = TCP_CLOSED;
            c->active = 0;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK)
        {
            c->state  = TCP_CLOSED;
            c->active = 0;
        }
        break;

    default:
        break;
    }
}

int net_tcp_connect(const unsigned char dst_ip[4], unsigned short dst_port)
{
    struct tcp_conn *c = (void *)0;
    int idx = -1, i;
    unsigned long deadline;

    /* Find a free slot */
    for (i = 0; i < NET_TCP_MAX_CONN; i++)
    {
        if (!tcp_conns[i].active)
        {
            c = &tcp_conns[i];
            idx = i;
            break;
        }
    }
    if (!c) return -1;

    mem_zero(c, sizeof(*c));
    c->active      = 1;
    c->state       = TCP_SYN_SENT;
    ip_copy(c->remote_ip, dst_ip);
    c->local_port  = tcp_next_port++;
    if (tcp_next_port == 0) tcp_next_port = 49152;
    c->remote_port = dst_port;
    c->seq         = 1000 + timer_ticks(); /* simple ISN */

    /* Send SYN */
    if (tcp_send_segment(c, TCP_SYN, (void *)0, 0) != 0)
    {
        c->active = 0;
        return -1;
    }
    c->seq++; /* SYN consumes one sequence number */

    /* Wait for SYN-ACK (5 seconds) */
    deadline = timer_ticks() + 500;
    while (timer_ticks() < deadline)
    {
        net_poll();
        if (c->state == TCP_ESTABLISHED)
            return idx;
        if (c->state == TCP_CLOSED)
        {
            c->active = 0;
            return -1;
        }
    }

    /* Timeout */
    c->active = 0;
    c->state  = TCP_CLOSED;
    return -1;
}

int net_tcp_send(int conn, const void *data, unsigned long len)
{
    struct tcp_conn *c;

    if (conn < 0 || conn >= NET_TCP_MAX_CONN) return -1;
    c = &tcp_conns[conn];
    if (!c->active || c->state != TCP_ESTABLISHED) return -1;

    /* Send in chunks of up to 1400 bytes (safe MSS) */
    {
        const unsigned char *p = (const unsigned char *)data;
        unsigned long remaining = len;
        while (remaining > 0)
        {
            unsigned long chunk = remaining > 1400 ? 1400 : remaining;
            if (tcp_send_segment(c, TCP_ACK | TCP_PSH, p, chunk) != 0)
                return -1;
            c->seq += (unsigned int)chunk;
            p += chunk;
            remaining -= chunk;
            /* Brief delay between chunks */
            {
                unsigned long wait = timer_ticks() + 2;
                while (timer_ticks() < wait) net_poll();
            }
        }
    }
    return 0;
}

int net_tcp_recv(int conn, void *buf, unsigned long buf_size, unsigned long timeout_ticks)
{
    struct tcp_conn *c;
    unsigned long deadline;
    unsigned long copy;

    if (conn < 0 || conn >= NET_TCP_MAX_CONN) return -1;
    c = &tcp_conns[conn];
    if (!c->active) return -1;

    deadline = timer_ticks() + timeout_ticks;

    while (timer_ticks() < deadline)
    {
        net_poll();

        if (c->rx_len > 0)
        {
            copy = c->rx_len < buf_size ? c->rx_len : buf_size;
            mem_copy(buf, c->rx_buf, copy);
            /* Shift remaining data */
            if (copy < c->rx_len)
            {
                unsigned long remaining = c->rx_len - copy;
                unsigned long j;
                for (j = 0; j < remaining; j++)
                    c->rx_buf[j] = c->rx_buf[copy + j];
                c->rx_len = remaining;
            }
            else
            {
                c->rx_len = 0;
            }
            return (int)copy;
        }

        /* Connection closed by remote? */
        if (c->rx_fin || c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED)
            return 0;
    }

    return -1; /* timeout */
}

void net_tcp_close(int conn)
{
    struct tcp_conn *c;

    if (conn < 0 || conn >= NET_TCP_MAX_CONN) return;
    c = &tcp_conns[conn];
    if (!c->active) return;

    if (c->state == TCP_ESTABLISHED)
    {
        c->state = TCP_FIN_WAIT_1;
        tcp_send_segment(c, TCP_FIN | TCP_ACK, (void *)0, 0);
        c->seq++;

        /* Wait briefly for FIN-ACK */
        {
            unsigned long deadline = timer_ticks() + 200;
            while (timer_ticks() < deadline && c->active)
                net_poll();
        }
    }
    else if (c->state == TCP_CLOSE_WAIT)
    {
        c->state = TCP_LAST_ACK;
        tcp_send_segment(c, TCP_FIN | TCP_ACK, (void *)0, 0);
        c->seq++;
        {
            unsigned long deadline = timer_ticks() + 200;
            while (timer_ticks() < deadline && c->active)
                net_poll();
        }
    }

    c->state  = TCP_CLOSED;
    c->active = 0;
}
