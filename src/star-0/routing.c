#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/time.h>
#include <linux/if_ether.h>   //ETH_ALEN(6),ETH_HLEN(14),ETH_FRAME_LEN(1514),struct ethhdr

#include "../utlist.h"
#include "../log.h"
#include "../utils.h"
#include "../net.h"
#include "../tun.h"
#include "../unix.h"
#include "../console.h"
#include "../main.h"
#include "../interfaces.h"
#include "../ext/packet_cache.h"

#include "routing.h"

// incomplete

//https://www.open-mesh.org/projects/batman-adv/wiki/DistributedArpTable


#define ROOT_TIMEOUT_SECONDS 5
#define TIMEOUT_ROOTS_SECONDS 3
#define NODE_TIMEOUT_SECONDS 30
#define PACKET_CACHE_TIMEOUT_SECONDS 5

enum {
    TYPE_ROOT,
    TYPE_RREQ,
    TYPE_RREP,
    TYPE_DATA
};

typedef struct __attribute__((__packed__)) {
    uint8_t type;
    uint32_t id; // source/root id
    uint16_t seq_num;
    uint16_t hop_count;
    uint32_t sender_id; // used to spread ids for the DHT
 } ROOT;

typedef struct Node {
    uint32_t id;
    uint16_t hop_count;
    Address next_hop_addr;
    time_t updated;
    struct Node *next;
} Node;

// Fallback mechanism: looks for the coordinates of a node
// ask for position of ID
typedef struct __attribute__((__packed__)) {
    uint8_t type;
    uint32_t src_id; // sender
    uint32_t dst_id; // next dht hop
    uint32_t ask_id; // we inquire about the coords of this id
    uint16_t src_root_hop_count;
    uint16_t dst_root_hop_count;
} RREQ;

typedef struct __attribute__((__packed__)) {
    uint8_t type;
    uint32_t dst_id;
    uint32_t ask_id; //respond to this id
    uint16_t dst_root_hop_count;
    uint16_t ask_root_hop_count;
} RREP;

typedef struct __attribute__((__packed__)) {
    uint8_t type;
    uint32_t src_id;
    uint32_t dst_id;
    uint16_t dst_root_hop_count;
    uint16_t payload_length; // length in bytes
    // these field are invisible here
    //uint8_t payload[2000];
} DATA;

typedef struct CurrentRoot {
    Address next_hop_addr; // lower level address (MAC or IP) of path[0]
    uint32_t id;
    uint16_t seq_num;
    uint16_t hop_count;
    time_t updated;
} CurrentRoot;

static CurrentRoot g_current_root;
static uint16_t g_sequence_number = 0;

// all neighbors and some nodes further away
static Node *g_dht_nodes = NULL;
static int g_dht_nodes_count = 0;

static uint32_t numerical_distance(uint32_t a, uint32_t b)
{
    if (a > b) {
        return a - b;
    } else {
        return b - a;
    }
}

static void dht_node_timeout()
{
    Node *tmp;
    Node *cur;

    LL_FOREACH_SAFE(g_dht_nodes, cur, tmp) {
        if ((cur->updated + NODE_TIMEOUT_SECONDS) < gstate.time_now) {
            log_debug("timeout dht entry for id 0x%08x", cur->id);
            LL_DELETE(g_dht_nodes, cur);
        }
    }
}

static Node* nodes_find_by_id_exact(uint32_t id)
{
    Node *cur;

    LL_FOREACH(g_dht_nodes, cur) {
        if (cur->id == id) {
            return cur;
        }
    }

    return NULL;
}

static Node* nodes_find_by_id_space(uint32_t id)
{
    Node *nearest_node = NULL;
    uint32_t nearest_dist = UINT32_MAX;
    Node *cur;

    LL_FOREACH(g_dht_nodes, cur) {
        uint32_t d = numerical_distance(cur->id, g_current_root.id);
        if (nearest_node == NULL || d < nearest_dist) {
            nearest_node = cur;
            nearest_dist = d;
        }
    }

    return nearest_node;
}

static Node* nodes_find_by_hop_count(uint32_t id)
{
    Node *nearest_node = NULL;
    uint32_t nearest_dist = UINT32_MAX;
    Node *cur;

    LL_FOREACH(g_dht_nodes, cur) {
        uint32_t d = numerical_distance(cur->hop_count, g_current_root.hop_count);
        if (nearest_node == NULL || d < nearest_dist) {
            nearest_node = cur;
            nearest_dist = d;
        }
    }

    return nearest_node;
}

static void dht_remove_worst()
{
    // currently removes last
    Node *cur;
    Node *tmp;

    LL_FOREACH_SAFE(g_dht_nodes, cur, tmp) {
        if (cur->next == NULL) {
            LL_DELETE(g_dht_nodes, cur);
            free(cur);
        }
    }
}

// TODO: keep all 1 hops
static void dht_add_node(uint32_t id, uint32_t hop_count, const Address *next_hop_addr)
{
    Node *node = nodes_find_by_id_exact(id);

    if (node) {
        node->updated = gstate.time_now;
        node->hop_count = hop_count;
        memcpy(&node->next_hop_addr, next_hop_addr, sizeof(Address));
        // update
        return;
    }

    if (g_dht_nodes_count > 60) {
        dht_remove_worst();
    }

    // add node
    node = calloc(1, sizeof(Node));
    node->id = id;
    node->updated = gstate.time_now;
    node->hop_count = hop_count;
    memcpy(&node->next_hop_addr, next_hop_addr, sizeof(Address));

    if (g_dht_nodes) {
        node->next = g_dht_nodes;
        g_dht_nodes = node;
    } else {
        g_dht_nodes = node;
        node->next = NULL;
    }

    g_dht_nodes_count += 1;
}

static void current_root_init()
{
    memset(&g_current_root.next_hop_addr, 0, sizeof(Address));
    g_current_root.id = gstate.own_id;
    g_current_root.seq_num = 0;
    g_current_root.hop_count = 0;
    g_current_root.updated = gstate.time_now;
}

static size_t get_data_size(const DATA *p)
{
    return sizeof(DATA) + p->payload_length;
}

static uint8_t *get_data_payload(const DATA *p)
{
    return ((uint8_t*) p) + sizeof(DATA);
}

static void handle_DATA(int ifindex, const Address *addr, DATA *p, unsigned recv_len)
{
    if (recv_len < sizeof(DATA) || recv_len != get_data_size(p)) {
        log_debug("DATA: invalid packet size => drop");
        return;
    }

    log_debug("DATA: got packet: %s / 0x%08x => 0x%08x / %u",
        str_addr(addr), p->src_id, p->dst_id, p->dst_root_hop_count);

    if (p->src_id == gstate.own_id) {
        log_debug("DATA: own source id => drop");
        return;
    }

    if (p->dst_id == gstate.own_id) {
        log_debug("DATA: packet arrived at destiantion => accept");

        // destination is the local tun0 interface => write packet to tun0
        uint8_t *payload = get_data_payload(p);
        tun_write(payload, p->payload_length);
    } else {
        Node *node = nodes_find_by_hop_count(p->dst_id);
        if (node) {
            log_debug("DATA: next hop found => forward");
            send_ucast_l2(&node->next_hop_addr, p, get_data_size(p));
        } else {
            log_debug("DATA: no next hop found => drop");
        }
    }
}

static void send_cached_packet(uint32_t dst_id, uint16_t dst_hop_count, const Address *addr)
{
    uint8_t buffer[sizeof(DATA) + ETH_FRAME_LEN];
    DATA *data = (DATA*) &buffer[0];

    uint8_t* data_payload = get_data_payload(data);
    size_t data_payload_length = 0;
    packet_cache_get_and_remove(data_payload, &data_payload_length, dst_id);

    if (data_payload_length == 0) {
        // no cached packet found
        return;
    }

    data->type = TYPE_DATA;
    data->src_id = gstate.own_id;
    data->dst_id = dst_id;
    data->dst_root_hop_count = dst_hop_count;
    data->payload_length = data_payload_length;

    log_debug("send DATA (0x%08x => 0x%08x:%u) via %s ",
        data->src_id, data->dst_id, dst_hop_count, str_addr(addr));

    send_ucast_l2(addr, data, get_data_size(data));
}

static void handle_RREP(int ifindex, const Address *addr, RREP *p, unsigned recv_len)
{
    if (recv_len != sizeof(RREP)) {
        log_debug("RREP: invalid packet size => drop");
        return;
    }

    if (p->ask_id == gstate.own_id) {
        log_debug2("RREP: got own packet => drop");
        return;
    }

    dht_add_node(p->ask_id, p->ask_root_hop_count, addr);

    if (p->dst_id == gstate.own_id) {
        log_debug("RREP: received destination => accept");
        // send cached packets
        send_cached_packet(p->ask_id, p->ask_root_hop_count, addr);
    } else {
        Node *node = nodes_find_by_hop_count(p->dst_id);
        if (node) {
            send_ucast_l2(&node->next_hop_addr, p, recv_len);
        } else {
            log_debug("RREP: no next neighbor hop => drop");
        }
    }
}

static void handle_RREQ(int ifindex, const Address *addr, RREQ *p, unsigned recv_len)
{
    Node *node;

    if (recv_len != sizeof(RREQ)) {
        log_debug("RREQ: invalid packet size => drop");
        return;
    }

    if (p->src_id == gstate.own_id) {
        log_debug2("RREQ: got own packet => drop");
        return;
    }

    log_debug("RREQ: got packet from 0x%08x to 0x%08x to ask for 0x%08x", p->src_id, p->dst_id, p->ask_id);

    dht_add_node(p->src_id, p->src_root_hop_count, addr);

    if (p->ask_id == gstate.own_id) {
        // the request is for&about us => send reply
        log_debug("RREQ: send RREP => reply");

        RREP rrep = {
            .type = TYPE_RREP,
            .dst_id = p->src_id,
            .dst_root_hop_count = p->src_root_hop_count,
            .ask_id = p->ask_id,
            .ask_root_hop_count = g_current_root.hop_count,
        };

        send_ucast_l2(addr, &rrep, sizeof(RREP));
    } else if (p->dst_id == gstate.own_id) {
        // we are a intermediatary dht hop => forward to nearer hop on DHT
        node = nodes_find_by_id_space(p->ask_id);
        if (node) {
            log_debug("RREQ: send to next dht hop 0x%08x => forward", node->id);
            p->dst_id = node->id;
            p->dst_root_hop_count = node->hop_count;
            send_ucast_l2(&node->next_hop_addr, p, recv_len);
        } else {
            log_debug("RREQ: no next dht hop known => drop");
        }
    } else {
        // we are a intermediatary node towards a dht node lookup
        node = nodes_find_by_hop_count(p->dst_id);
        if (node) {
            log_debug("RREQ: send to next dht hop 0x%08x => forward", node->id);
            send_ucast_l2(&node->next_hop_addr, p, recv_len);
        } else {
            log_debug("RREQ: no next hop neighbor => drop");
        }
    }
}

// returns |new - cur| < UINT16_MAX/2
static int is_newer_seqnum(uint16_t cur, uint16_t new)
{
    if (cur >= new) {
        return (cur - new) > 0x7fff;
    } else {
        return (new - cur) < 0x7fff;
    }
}

static void handle_ROOT(int ifindex, const Address *addr, ROOT *p, unsigned recv_len)
{
    if (recv_len != sizeof(ROOT)) {
        log_debug("ROOT: invalid packet size => drop");
        return;
    }

    if (p->id == gstate.own_id) {
        log_debug2("ROOT: got own id => drop");
        return;
    }

    // feed the DHT
    if (p->hop_count > 0 && p->sender_id != gstate.own_id) {
    	dht_add_node(p->sender_id, p->hop_count, addr);
    }

    p->hop_count += 1;

    if (p->id < g_current_root.id) {
        log_debug("ROOT: got lower id (0x%08x < 0x%08x) => ignore", p->id, g_current_root.id);
        return;
    } else if (p->id > g_current_root.id) {
        log_debug("ROOT: got higher id (0x%08x > 0x%08x) => accept", p->id, g_current_root.id);
        memcpy(&g_current_root.next_hop_addr, addr, sizeof(Address));
        g_current_root.id = p->id;
    } else {
        // p->id == g_current_root.id
        if (is_newer_seqnum(g_current_root.seq_num, p->seq_num)) {
            if (p->hop_count <= g_current_root.hop_count) {
                log_debug("ROOT: got root update => accept");
                memcpy(&g_current_root.next_hop_addr, addr, sizeof(Address));
            } else {
                // longer route, but it might still be good as a fallback
                log_debug("ROOT: got longer root => ignore");
                return;
            }
        } else {
            log_debug("ROOT: old root update => ignore");
            return;
        }
    }

    g_current_root.hop_count = p->hop_count;
    g_current_root.seq_num = p->seq_num;
    g_current_root.updated = gstate.time_now;

    p->hop_count += 1;

    send_bcasts_l2(p, sizeof(ROOT));
}

// read traffic from tun0 and send to peers
static void tun_handler(int events, int fd)
{
    uint32_t dst_id;
    uint8_t buffer[ETH_FRAME_LEN];

    if (events <= 0) {
        return;
    }

    while (1) {
        ssize_t max_payload = sizeof(buffer) - sizeof(DATA);
        uint8_t *payload = buffer + sizeof(DATA);
        ssize_t read_len = tun_read(&dst_id, payload, max_payload);

        if (read_len <= 0 || read_len > max_payload) {
            break;
        }

        Node *node = nodes_find_by_id_exact(dst_id);

        if (node) {
            DATA *p = (DATA*) buffer;
            p->type = TYPE_DATA;
            p->src_id = gstate.own_id;
            p->dst_id = dst_id;
            p->dst_root_hop_count = node->hop_count;
            p->payload_length = read_len;

            send_ucast_l2(&node->next_hop_addr, p, get_data_size(p));
            return;
        }

        // cache packet
        packet_cache_add(dst_id, payload, read_len);

        node = nodes_find_by_id_space(dst_id);
        if (node) {
            // search for coordionates of destination
            RREQ rreq = {
                .type = TYPE_RREQ,
                .src_id = gstate.own_id,
                .src_root_hop_count = g_current_root.hop_count,
                .dst_id = node->id,
                .dst_root_hop_count = node->hop_count,
                .ask_id = dst_id,
            };

            send_ucast_l2(&node->next_hop_addr, &rreq, sizeof(RREQ));
        } else {
            log_debug("cannot find destiantion for 0x%08x", dst_id);
        }
    }
}

// read traffic from other interfaces
static void ext_handler_l2(int events, int fd)
{
    if (events <= 0) {
        return;
    }

    uint8_t buffer[ETH_FRAME_LEN];
    ssize_t numbytes = recvfrom(fd, buffer, sizeof(buffer), 0, NULL, NULL);

    if (numbytes <= sizeof(struct ethhdr)) {
        return;
    }

    uint8_t *payload = &buffer[sizeof(struct ethhdr)];
    size_t payload_len = numbytes - sizeof(struct ethhdr);
    struct ethhdr *eh = (struct ethhdr *) &buffer[0];
    int ifindex = interface_get_ifindex(fd);

    Address from_addr;
    Address to_addr;
    init_macaddr(&from_addr, &eh->h_source, ifindex);
    init_macaddr(&to_addr, &eh->h_dest, ifindex);

    switch (payload[0]) {
    case TYPE_DATA:
        handle_DATA(ifindex, &from_addr, (DATA*) payload, payload_len);
        break;
    case TYPE_ROOT:
        handle_ROOT(ifindex, &from_addr, (ROOT*) payload, payload_len);
        break;
    case TYPE_RREQ:
        handle_RREQ(ifindex, &from_addr, (RREQ*) payload, payload_len);
        break;
    case TYPE_RREP:
        handle_RREP(ifindex, &from_addr, (RREP*) payload, payload_len);
        break;
    default:
        log_warning("unknown packet type 0x%02x from %s (%s)", buffer[0], str_addr(&from_addr), str_ifindex(ifindex));
    }
}

static int console_handler(FILE *fp, int argc, char *argv[])
{
    char buf_duration[64];

    if (argc == 1 && !strcmp(argv[0], "h")) {
        fprintf(fp,
            "r: print root\n"
            "n: print routing table"
        );
    } else if (argc == 1 && !strcmp(argv[0], "r")) {
        fprintf(fp, "root-id: 0x%08x, hop_count: %u, seq_num: %u, updated: %s\n",
            g_current_root.id, g_current_root.hop_count, g_current_root.seq_num,
            format_duration(buf_duration, g_current_root.updated, gstate.time_now));
    } else if (argc == 1 && !strcmp(argv[0], "n")) {
        int counter = 0;
        Node *cur;

        fprintf(fp, "id        hop-count    next-hop-addr   updated\n");
        LL_FOREACH(g_dht_nodes, cur) {
            fprintf(fp, "0x%08x %u %s %s\n",
                cur->id,
                (unsigned) cur->hop_count,
                str_addr(&cur->next_hop_addr),
                format_duration(buf_duration, cur->updated, gstate.time_now)
            );
            counter += 1;
        }

        fprintf(fp, "%d entries\n", counter);
    } else {
        return 1;
    }

    return 0;
}

static void send_root()
{
    static time_t g_root_last_send = 0;

    // timeout foreign root
    if (g_current_root.id != gstate.own_id) {
        if ((g_current_root.updated + ROOT_TIMEOUT_SECONDS) <= gstate.time_now) {
            log_debug("timeout root 0x%08x", g_current_root.id);
            current_root_init();
        }
    }

    // send own root
    if (g_current_root.id == gstate.own_id) {
        // only send every TIMEOUT_ROOTS_SECONDS
        if (g_root_last_send && gstate.time_now < (g_root_last_send + TIMEOUT_ROOTS_SECONDS)) {
            return;
        }
        g_root_last_send = gstate.time_now;

        log_debug("send root packet");
        ROOT root = {
            .type = TYPE_ROOT,
            .id = g_current_root.id,
            .seq_num = g_sequence_number++, // g_current_root.seq_num,
            .hop_count = 0, //g_current_root.hop_count,
            .sender_id = 0,
        };
        g_current_root.seq_num = g_sequence_number - 1;
        g_current_root.updated = gstate.time_now;

        send_bcasts_l2(&root, sizeof(ROOT));
    }
}

static void init()
{
    current_root_init();
    packet_cache_init(PACKET_CACHE_TIMEOUT_SECONDS);
    net_add_handler(-1, &send_root);
    net_add_handler(-1, &dht_node_timeout);
}

void star_0_register()
{
    static const Protocol p = {
        .name = "star-0",
        .init = &init,
        .tun_handler = &tun_handler,
        .ext_handler_l2 = &ext_handler_l2,
        .console = &console_handler,
    };

    protocols_register(&p);
}