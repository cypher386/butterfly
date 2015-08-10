/* Copyright 2015 Outscale SAS
 *
 * This file is part of Butterfly.
 *
 * Butterfly is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 *
 * Butterfly is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Butterfly.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib.h>
#include <arpa/inet.h>
#include <sys/sysinfo.h>
#include <utility>
#include "api/server/app.h"
#include "api/server/pg.h"
#include "api/server/graph.h"

Graph::Graph(void) {
    // Init rpc queue
    queue = g_async_queue_new();
    started = false;
}

Graph::~Graph(void) {
    struct rpc_queue *a;

    if (!started)
        return;

    // Stop poller thread
    exit();
    pthread_join(poller_thread, NULL);

    // Empty and unref queue
    a = (struct rpc_queue *)g_async_queue_try_pop(queue);
    while (a != NULL) {
        g_free(a);
        a = (struct rpc_queue *)g_async_queue_try_pop(queue);
    }
    g_async_queue_unref(queue);

    // Byby packetgraph
    Pg::vhost_stop();
    Pg::stop();
    started = false;
}

bool Graph::start(int argc, char **argv) {
    struct ether_addr mac;

    // Start packetgraph
    if (!Pg::start(argc, argv)) {
        return false;
    }

    // Init global for certain bricks
    Pg::nic_start();
    Pg::vhost_start(app::config.socket_folder);

    // Create nic brick
    nic = Brick(Pg::nic_new_by_id("port 0", 1, 1, WEST_SIDE, 0),
                Pg::destroy);
    if (nic.get() == NULL) {
        LOG_ERROR_("brick-nic failed");
        return false;
    }

    // Create vtep brick
    Pg::nic_get_mac(nic.get(), &mac);
    vtep = Brick(Pg::vtep_new("vxlan", 1, 50, WEST_SIDE,
                       app::config.external_ip, mac, 1),
                       Pg::destroy);
    if (nic.get() == NULL) {
        LOG_ERROR_("brick-vtep failed");
        return false;
    }

    // Run poller
    pthread_create(&poller_thread, NULL, Graph::poller, this);

    started = true;
    return true;
}

void *Graph::poller(void *graph) {
    Graph *g = reinterpret_cast<Graph *>(graph);
    struct rpc_update_pollable_bricks *list = NULL;
    struct rpc_queue *q = NULL;
    uint64_t cnt = 0;
    uint16_t pkts_count;

    /** TODO(jerome.jutteau) Set option in butterfly
     * configuration.
     */
    Graph::set_cpu(0);

    g_async_queue_ref(g->queue);

    // TODO(jerome.jutteau) do we have to call firewall's garbage collector ?
    // Maybe this should be included in firewall's brick poll ?

    /* The main packet poll loop. */
    for (;;) {
        /* Let's see if there is any update every 100 000 pools. */
        if (cnt++ % 100000 == 0) {
            if (g->poller_update(&q)) {
                list = q ? &q->update_pollable_bricks : NULL;
            } else {
                LOG_DEBUG_("poll thread will now exit");
                break;
            }
        }

        /* Poll all pollable NICs. */
        if (list != NULL) {
            for (uint32_t v = 0; v < list->size; v++)
                Pg::poll(list->bricks[v], &pkts_count);
        }
    }
    g_async_queue_unref(g->queue);
    g_free(q);
    pthread_exit(NULL);
}

int Graph::set_cpu(int core_id) {
    cpu_set_t cpu_set;
    pthread_t t;

    if (core_id < 0 || core_id >= get_nprocs())
        return EINVAL;

    t = pthread_self();
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    return pthread_setaffinity_np(t, sizeof(cpu_set_t), &cpu_set);
}

bool Graph::poller_update(struct rpc_queue **list) {
    struct rpc_queue *a;
    struct rpc_queue *tmp;

    // Unqueue calls
    a = (struct rpc_queue *) g_async_queue_try_pop(queue);
    while (a != NULL) {
        switch (a->action) {
            case EXIT:
                return false;
            case LINK:
                Pg::link(a->link.w, a->link.e);
                break;
            case UNLINK:
                Pg::unlink(a->unlink.b);
                break;
            case FIREWALL_RELOAD:
                Pg::firewall_reload(a->firewall_reload.b);
                break;
            case ADD_VNI:
                Pg::vtep_add_vni(a->add_vni.vtep,
                                 a->add_vni.neighbor,
                                 a->add_vni.vni,
                                 a->add_vni.multicast_ip);
                break;
            case UPDATE_POLLABLE_BRICKS:
                // Swap with the old list
                tmp = a;
                a = *list;
                *list = tmp;
                break;
            default:
                LOG_ERROR_("brick poller has wrong RPC value");
                break;
        }
        g_free(a);
        a = (struct rpc_queue *) g_async_queue_try_pop(queue);
    }

    return true;
}

std::string Graph::nic_add(const app::Nic &nic) {
    std::string path = "";
    // TODO(jerome.jutteau)
    return path;
}
void Graph::nic_del(std::string id) {
    if (!started) {
        LOG_ERROR_("Graph has not been stared");
        return;
    }

    // TODO(jerome.jutteau)
    // WAIT that queue is done before cleaning bricks
}

std::string Graph::nic_export(std::string id) {
    if (!started) {
        LOG_ERROR_("Graph has not been stared");
        return "";
    }

    // TODO(jerome.jutteau)
    std::string data = "";
    return data;
}

void Graph::nic_get_stats(std::string id, uint64_t *in, uint64_t *out) {
    if (!started) {
        LOG_ERROR_("Graph has not been stared");
        return;
    }

    // TODO(jerome.jutteau)
    *in = *out = 42;
}

void Graph::nic_config_anti_spoof(std::string nic_id, bool enable) {
    if (!started) {
        LOG_ERROR_("Graph has not been stared");
        return;
    }

    // TODO(jerome.jutteau)
}

void Graph::nic_config_ip(std::string id, const std::vector<app::Ip> &ip_list) {
    if (!started) {
        LOG_ERROR_("Graph has not been stared");
        return;
    }

    // TODO(jerome.jutteau)
}

void Graph::fw_update(const app::Nic &nic) {
    if (!started) {
        LOG_ERROR_("Graph has not been stared");
        return;
    }

    // TODO(jerome.jutteau)
}

void Graph::fw_add_rule(std::string nic_id, const app::Rule &rule) {
    if (!started) {
        LOG_ERROR_("Graph has not been stared");
        return;
    }

    // TODO(jerome.jutteau)
}

std::string Graph::dot() {
    // Build the graph from the physical NIC
    return Pg::graph_dot(nic.get());
}

void Graph::exit() {
    struct rpc_queue *a = g_new(struct rpc_queue, 1);
    a->action = EXIT;
    g_async_queue_push(queue, a);
}

void Graph::link(Brick w, Brick e) {
    struct rpc_queue *a = g_new(struct rpc_queue, 1);
    a->action = LINK;
    a->link.w = w.get();
    a->link.e = e.get();
    g_async_queue_push(queue, a);
}

void Graph::unlink(Brick b) {
    struct rpc_queue *a = g_new(struct rpc_queue, 1);
    a->action = UNLINK;
    a->unlink.b = b.get();
    g_async_queue_push(queue, a);
}

void Graph::firewall_reload(Brick b) {
    struct rpc_queue *a = g_new(struct rpc_queue, 1);
    a->action = FIREWALL_RELOAD;
    a->firewall_reload.b = b.get();
    g_async_queue_push(queue, a);
}

void Graph::add_vni(Brick vtep, Brick neighbor, uint32_t vni) {
    uint32_t multicast_ip = build_multicast_ip(vni);
    struct rpc_queue *a = g_new(struct rpc_queue, 1);
    a->action = ADD_VNI;
    a->add_vni.vtep = vtep.get();
    a->add_vni.neighbor = neighbor.get();
    a->add_vni.vni = vni;
    a->add_vni.multicast_ip = multicast_ip;
    g_async_queue_push(queue, a);
}

void Graph::update_pollable_bricks() {
    // Create a table with all pollable bricks
    std::map<uint32_t, struct graph_vni>::iterator vni_it;
    std::map<std::string, struct graph_nic>::iterator nic_it;
    struct rpc_queue *a = g_new(struct rpc_queue, 1);
    struct rpc_update_pollable_bricks &p = a->update_pollable_bricks;

    a->action = UPDATE_POLLABLE_BRICKS;
    // Add physical NIC brick
    p.size = 1;
    p.bricks[0] = nic.get();
    // Add all vhost bricks
    for (vni_it = vnis.begin();
            vni_it != vnis.end();
            vni_it++) {
        for (nic_it = vni_it->second.nics.begin();
                nic_it != vni_it->second.nics.end();
                nic_it ++) {
            if (p.size + 1 >= GRAPH_VHOST_MAX_SIZE) {
                LOG_ERROR_("Not enough pollable bricks slot available");
                break;
            }
            p.bricks[p.size] = nic_it->second.vhost.get();
            p.size++;
        }
    }

    // Pass this new listing to packetgraph thread
    g_async_queue_push(queue, a);
}

uint32_t Graph::build_multicast_ip(uint32_t vni) {
    // Build mutlicast IP, CIDR: 224.0.0.0/4
    // (224.0.0.0 to 239.255.255.255)
    // 224 and 239 are already used.
    uint32_t multicast_ip = htonl(vni);
    reinterpret_cast<uint8_t *>(& multicast_ip)[0] = 230;
    return multicast_ip;
}
