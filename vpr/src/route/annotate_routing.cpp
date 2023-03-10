/********************************************************************
 * This file includes functions that are used to annotate routing results
 * from VPR to OpenFPGA
 *******************************************************************/
/* Headers from vtrutil library */
#include "vtr_assert.h"
#include "vtr_time.h"
#include "vtr_log.h"

#include "vpr_error.h"
#include "rr_graph.h"
#include "annotate_routing.h"

/********************************************************************
 * Create a mapping between each rr_node and its mapped nets 
 * based on VPR routing results
 * - Store the net ids mapped to each routing resource nodes
 * - Mapped nodes should have valid net ids (except SOURCE and SINK nodes)
 * - Unmapped rr_node will use invalid ids 
 *******************************************************************/
vtr::vector<RRNodeId, ParentNetId> annotate_rr_node_nets(const Netlist<>& net_list,
                                                         const DeviceContext& device_ctx,
                                                         const RoutingContext& routing_ctx,
                                                         const bool& verbose,
                                                         bool is_flat) {
    size_t counter = 0;
    vtr::ScopedStartFinishTimer timer("Annotating rr_node with routed nets");

    const auto& rr_graph = device_ctx.rr_graph;

    vtr::vector<RRNodeId, ParentNetId> rr_node_nets;
    rr_node_nets.resize(rr_graph.num_nodes(), ParentNetId::INVALID());

    for (auto net_id : net_list.nets()) {
        if (net_list.net_is_ignored(net_id)) {
            continue;
        }

        /* Ignore used in local cluster only, reserved one CLB pin */
        if (net_list.net_sinks(net_id).empty()) {
            continue;
        }
        t_trace* tptr = routing_ctx.trace[net_id].head;
        while (tptr != nullptr) {
            const RRNodeId& rr_node = RRNodeId(tptr->index);
            /* Ignore source and sink nodes, they are the common node multiple starting and ending points */
            if ((SOURCE != rr_graph.node_type(rr_node))
                && (SINK != rr_graph.node_type(rr_node))) {
                /* Sanity check: ensure we do not revoke any net mapping
                 * In some routing architectures, node capacity is more than 1
                 * which allows a node to be mapped by multiple nets
                 * Therefore, the sanity check should focus on the nodes 
                 * whose capacity is 1 
                 */
                if ((rr_node_nets[rr_node])
                    && (1 == rr_graph.node_capacity(rr_node))
                    && (net_id != rr_node_nets[rr_node])) {
                    VPR_FATAL_ERROR(VPR_ERROR_ANALYSIS,
                                    "Detect two nets '%s' and '%s' that are mapped to the same rr_node '%ld'!\n%s\n",
                                    net_list.net_name(net_id).c_str(),
                                    net_list.net_name(rr_node_nets[rr_node]).c_str(),
                                    size_t(rr_node),
                                    describe_rr_node(rr_graph,
                                                     device_ctx.grid,
                                                     device_ctx.rr_indexed_data,
                                                     (int)size_t(rr_node),
                                                     is_flat)
                                        .c_str());
                } else {
                    rr_node_nets[rr_node] = net_id;
                }
                counter++;
            }
            tptr = tptr->next;
        }
    }

    VTR_LOGV(verbose, "Done with %d nodes mapping\n", counter);

    return rr_node_nets;
}
