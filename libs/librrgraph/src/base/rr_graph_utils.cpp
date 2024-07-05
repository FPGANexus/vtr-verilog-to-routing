#include <queue>
#include <random>

#include "rr_graph_utils.h"
#include "vpr_error.h"
#include "rr_graph_obj.h"
#include "rr_graph_builder.h"

std::vector<RRSwitchId> find_rr_graph_switches(const RRGraph& rr_graph,
                                               const RRNodeId& from_node,
                                               const RRNodeId& to_node) {
    std::vector<RRSwitchId> switches;
    std::vector<RREdgeId> edges = rr_graph.find_edges(from_node, to_node);
    if (true == edges.empty()) {
        /* edge is open, we return an empty vector of switches */
        return switches;
    }

    /* Reach here, edge list is not empty, find switch id one by one
     * and update the switch list
     */
    for (auto edge : edges) {
        switches.push_back(rr_graph.edge_switch(edge));
    }

    return switches;
}

int seg_index_of_cblock(const RRGraphView& rr_graph, t_rr_type from_rr_type, int to_node) {
    if (from_rr_type == CHANX)
        return (rr_graph.node_xlow(RRNodeId(to_node)));
    else
        /* CHANY */
        return (rr_graph.node_ylow(RRNodeId(to_node)));
}

int seg_index_of_sblock(const RRGraphView& rr_graph, int from_node, int to_node) {
    t_rr_type from_rr_type, to_rr_type;

    from_rr_type = rr_graph.node_type(RRNodeId(from_node));
    to_rr_type = rr_graph.node_type(RRNodeId(to_node));

    if (from_rr_type == CHANX) {
        if (to_rr_type == CHANY) {
            return (rr_graph.node_xlow(RRNodeId(to_node)));
        } else if (to_rr_type == CHANX) {
            if (rr_graph.node_xlow(RRNodeId(to_node)) > rr_graph.node_xlow(RRNodeId(from_node))) { /* Going right */
                return (rr_graph.node_xhigh(RRNodeId(from_node)));
            } else { /* Going left */
                return (rr_graph.node_xhigh(RRNodeId(to_node)));
            }
        } else {
            VPR_FATAL_ERROR(VPR_ERROR_ROUTE,
                            "in seg_index_of_sblock: to_node %d is of type %d.\n",
                            to_node, to_rr_type);
            return OPEN; //Should not reach here once thrown
        }
    }
    /* End from_rr_type is CHANX */
    else if (from_rr_type == CHANY) {
        if (to_rr_type == CHANX) {
            return (rr_graph.node_ylow(RRNodeId(to_node)));
        } else if (to_rr_type == CHANY) {
            if (rr_graph.node_ylow(RRNodeId(to_node)) > rr_graph.node_ylow(RRNodeId(from_node))) { /* Going up */
                return (rr_graph.node_yhigh(RRNodeId(from_node)));
            } else { /* Going down */
                return (rr_graph.node_yhigh(RRNodeId(to_node)));
            }
        } else {
            VPR_FATAL_ERROR(VPR_ERROR_ROUTE,
                            "in seg_index_of_sblock: to_node %d is of type %d.\n",
                            to_node, to_rr_type);
            return OPEN; //Should not reach here once thrown
        }
    }
    /* End from_rr_type is CHANY */
    else {
        VPR_FATAL_ERROR(VPR_ERROR_ROUTE,
                        "in seg_index_of_sblock: from_node %d is of type %d.\n",
                        from_node, from_rr_type);
        return OPEN; //Should not reach here once thrown
    }
}

vtr::vector<RRNodeId, std::vector<RREdgeId>> get_fan_in_list(const RRGraphView& rr_graph) {
    vtr::vector<RRNodeId, std::vector<RREdgeId>> node_fan_in_list;

    node_fan_in_list.resize(rr_graph.num_nodes(), std::vector<RREdgeId>(0));
    node_fan_in_list.shrink_to_fit();

    //Walk the graph and increment fanin on all dwnstream nodes
    rr_graph.rr_nodes().for_each_edge(
        [&](RREdgeId edge, __attribute__((unused)) RRNodeId src, RRNodeId sink) {
            node_fan_in_list[sink].push_back(edge);
        });

    return node_fan_in_list;
}

void set_sink_locs(const RRGraphView& rr_graph, RRGraphBuilder& rr_graph_builder, const DeviceGrid& grid) {
    auto node_fanins = get_fan_in_list(rr_graph);

    // Keep track of offsets for SINKs for each tile type, to avoid repeated
    // calculations
    using Offset = vtr::Point<size_t>;
    std::unordered_map<t_physical_tile_type_ptr, std::unordered_map<size_t, Offset>> physical_type_offsets;

    // Iterate over all SINK nodes
    for (size_t node = 0; node < rr_graph.num_nodes(); ++node) {
        auto node_id = RRNodeId(node);

        if (rr_graph.node_type((RRNodeId)node_id) != e_rr_type::SINK)
            continue;

        // Skip 1x1 tiles
        size_t tile_xlow = rr_graph.node_xlow(node_id);
        size_t tile_ylow = rr_graph.node_ylow(node_id);
        size_t tile_xhigh = rr_graph.node_xhigh(node_id);
        size_t tile_yhigh = rr_graph.node_yhigh(node_id);

        size_t tile_width = tile_xhigh - tile_xlow;
        size_t tile_height = tile_yhigh - tile_ylow;

        if (tile_width == 0 && tile_height == 0)
            continue;

        // See if we have encountered this tile type/ptc combo before, and used saved offset if so
        size_t tile_layer = rr_graph.node_layer(node_id);
        t_physical_tile_type_ptr tile_type = grid.get_physical_type({(int)tile_xlow, (int)tile_ylow, (int)tile_layer});

        size_t sink_ptc = rr_graph.node_ptc_num(node_id);

        if ((physical_type_offsets.find(tile_type) != physical_type_offsets.end()) && (physical_type_offsets[tile_type].find(sink_ptc) != physical_type_offsets[tile_type].end())) {
            auto new_x = (short)((int)tile_xlow + physical_type_offsets[tile_type].at(sink_ptc).x());
            auto new_y = (short)((int)tile_ylow + physical_type_offsets[tile_type].at(sink_ptc).y());

            // Set new coordinates
            rr_graph_builder.set_node_coordinates(node_id, new_x, new_y, new_x, new_y);

            continue;
        }

        /* We have not seen this tile type/ptc combo before */

        // The IPINs of the current SINK node
        std::unordered_set<RRNodeId> sink_ipins = {};

        // IPINs are always one node away from the SINK. So, we just get the fanins of the SINK
        // and add them to the set
        for (auto edge : node_fanins[node_id]) {
            RRNodeId pin = rr_graph.edge_src_node(edge);

            VTR_ASSERT_SAFE(rr_graph.node_type(pin) == e_rr_type::IPIN);

            // Make sure IPIN in the same cluster as origin
            size_t curr_x = rr_graph.node_xlow(pin);
            size_t curr_y = rr_graph.node_ylow(pin);
            if ((curr_x < tile_xlow) || (curr_x > tile_xhigh) || (curr_y < tile_ylow) || (curr_y > tile_yhigh))
                continue;

            sink_ipins.insert(pin);
        }

        /* Set SINK locations as average of collected IPINs */

        if (sink_ipins.empty())
            continue;

        // Use float so that we can take average later
        std::vector<float> x_coords;
        std::vector<float> y_coords;

        // Add coordinates of each "cluster-edge" pin to vectors
        for (const auto& pin : sink_ipins) {
            size_t pin_x = rr_graph.node_xlow(pin);
            size_t pin_y = rr_graph.node_ylow(pin);

            VTR_ASSERT_SAFE(pin_x == (size_t)rr_graph.node_xhigh(pin));
            VTR_ASSERT_SAFE(pin_y == (size_t)rr_graph.node_yhigh(pin));

            x_coords.push_back((float)pin_x);
            y_coords.push_back((float)pin_y);
        }

        auto x_avg = (short)round(std::accumulate(x_coords.begin(), x_coords.end(), 0.f) / (double)x_coords.size());
        auto y_avg = (short)round(std::accumulate(y_coords.begin(), y_coords.end(), 0.f) / (double)y_coords.size());

        // Remove old indices from RRSpatialLookup
        for (size_t x = tile_xlow; x <= tile_xhigh; ++x) {
            for (size_t y = tile_ylow; y <= tile_yhigh; ++y) {
                if (x == (size_t)x_avg && y == (size_t)y_avg)
                    continue;

                rr_graph_builder.node_lookup().remove_node(node_id, (int)tile_layer, (int)x, (int)y, SINK, sink_ptc);
            }
        }

        // Save offset for this tile/ptc combo
        if (physical_type_offsets.find(tile_type) == physical_type_offsets.end())
            physical_type_offsets[tile_type] = {};

        physical_type_offsets[tile_type].insert({sink_ptc, {x_avg - tile_xlow, y_avg - tile_ylow}});

        // Set new coordinates
        rr_graph_builder.set_node_coordinates(node_id, x_avg, y_avg, x_avg, y_avg);
    }
}

bool inter_layer_connections_limited_to_opin(const RRGraphView& rr_graph) {
    bool limited_to_opin = true;
    for (const auto& from_node : rr_graph.nodes()) {
        for (t_edge_size edge : rr_graph.edges(from_node)) {
            RRNodeId to_node = rr_graph.edge_sink_node(from_node, edge);
            int from_layer = rr_graph.node_layer(from_node);
            int to_layer = rr_graph.node_layer(to_node);

            if (from_layer != to_layer) {
                if (rr_graph.node_type(from_node) != e_rr_type::OPIN) {
                    limited_to_opin = false;
                    break;
                }
            }
        }
        if (!limited_to_opin) {
            break;
        }
    }

    return limited_to_opin;
}