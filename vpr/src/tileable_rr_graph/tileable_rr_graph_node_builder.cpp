/************************************************************************
 *  This file contains functions that are used to allocate nodes
 *  for the tileable routing resource graph builder
 ***********************************************************************/
/* Headers from vtrutil library */
#include "vtr_assert.h"
#include "vtr_log.h"
#include "vtr_geometry.h"

/* Headers from openfpgautil library */
#include "openfpga_side_manager.h"

#include "vpr_types.h"
#include "vpr_utils.h"

#include "rr_node.h"

#include "rr_graph_builder_utils.h"
#include "rr_graph_builder.h"
#include "tileable_chan_details_builder.h"
#include "tileable_rr_graph_node_builder.h"
#include "rr_rc_data.h"

/************************************************************************
 * Find the number output pins by considering all the grid
 ***********************************************************************/
static size_t estimate_num_grid_rr_nodes_by_type(const DeviceGrid& grids,
                                                 const size_t& layer,
                                                 const t_rr_type& node_type) {
    size_t num_grid_rr_nodes = 0;

    for (size_t ix = 0; ix < grids.width(); ++ix) {
        for (size_t iy = 0; iy < grids.height(); ++iy) {
            t_physical_tile_loc tile_loc(ix, iy, layer);
            /* Skip EMPTY tiles */
            if (true == is_empty_type(grids.get_physical_type(tile_loc))) {
                continue;
            }

            /* Skip height > 1 or width > 1 tiles (mostly heterogeneous blocks) */
            if ((0 < grids.get_width_offset(tile_loc))
                || (0 < grids.get_height_offset(tile_loc))) {
                continue;
            }

            enum e_side io_side = NUM_SIDES;

            /* If this is the block on borders, we consider IO side */
            if (true == is_io_type(grids.get_physical_type(tile_loc))) {
                vtr::Point<size_t> io_device_size(grids.width() - 1, grids.height() - 1);
                vtr::Point<size_t> grid_coordinate(ix, iy);
                io_side = determine_io_grid_pin_side(io_device_size, grid_coordinate);
            }

            switch (node_type) {
                case OPIN:
                    /* get the number of OPINs */
                    num_grid_rr_nodes += get_grid_num_pins(grids, layer, ix, iy, DRIVER, io_side);
                    break;
                case IPIN:
                    /* get the number of IPINs */
                    num_grid_rr_nodes += get_grid_num_pins(grids, layer, ix, iy, RECEIVER, io_side);
                    break;
                case SOURCE:
                    /* SOURCE: number of classes whose type is DRIVER */
                    num_grid_rr_nodes += get_grid_num_classes(grids, layer, ix, iy, DRIVER);
                    break;
                case SINK:
                    /* SINK: number of classes whose type is RECEIVER */
                    num_grid_rr_nodes += get_grid_num_classes(grids, layer, ix, iy, RECEIVER);
                    break;
                default:
                    VTR_LOGF_ERROR(__FILE__, __LINE__,
                                   "Invalid routing resource node!\n");
                    exit(1);
            }
        }
    }

    return num_grid_rr_nodes;
}

/************************************************************************
 * For X-direction Channel: CHANX
 * We pair each x-direction routing channel to the grid below it
 * as they share the same coordinate
 *
 * As such, the range of CHANX coordinate starts from x = 1, y = 0
 * which is the grid (I/O) at the left bottom of the fabric
 *
 * As such, the range of CHANX coordinate ends to x = width - 2, y = height - 2
 * which is the grid at the top right of the core fabric
 * Note that the I/O ring is
 *
 *                             TOP SIDE OF FPGA
 *
 *           +-------------+       +-------------+        +---------------------+
 *           |     Grid    |       |     Grid    |   ...  |    Grid             |
 *           |    [1][0]   |       |    [2][0]   |        | [width-2][height-1] |
 *           +-------------+       +-------------+        +---------------------+
 *
 *           +-------------+       +-------------+        +---------------------+
 *           |  X-Channel  |       |  X-Channel  |   ...  |  X-Channel          |
 *           |    [1][0]   |       |   [2][0]    |        | [width-2][height-2] |
 *           +-------------+       +-------------+        +---------------------+
 *
 *           +-------------+       +-------------+        +---------------------+
 *           |     Grid    |       |     Grid    |   ...  |    Grid             |
 *           |    [1][0]   |       |    [2][0]   |        | [width-2][height-2] |
 *           +-------------+       +-------------+        +---------------------+
 *
 *                 ...                   ...                    ...
 *
 *           +-------------+       +-------------+        +--------------+
 *           |  X-Channel  |       |  X-Channel  |   ...  |  X-Channel   |
 *           |    [1][1]   |       |   [2][1]    |        | [width-2][1] |
 *           +-------------+       +-------------+        +--------------+
 *
 *  LEFT     +-------------+       +-------------+        +--------------+          RIGHT
 *  SIDE     |     Grid    |       |     Grid    |   ...  |    Grid      |          SIDE
 *  GRID     |    [1][1]   |       |    [2][1]   |        | [width-2][1] |          GRID
 *           +-------------+       +-------------+        +--------------+
 *
 *           +-------------+       +-------------+        +--------------+
 *           |  X-Channel  |       |  X-Channel  |   ...  |  X-Channel   |
 *           |    [1][0]   |       |   [2][0]    |        | [width-2][0] |
 *           +-------------+       +-------------+        +--------------+
 *
 *           +-------------+       +-------------+        +--------------+
 *           |     Grid    |       |     Grid    |   ...  |    Grid      |
 *           |    [1][0]   |       |    [2][0]   |        | [width-2][0] |
 *           +-------------+       +-------------+        +--------------+
 *
 *                                 BOTTOM SIDE OF FPGA
 *
 *  The figure above describe how the X-direction routing channels are
 *  organized in a homogeneous FPGA fabric
 *  Note that we talk about general-purpose uni-directional routing architecture here
 *  It means that a routing track may span across multiple grids
 *  However, the hard limits are as follows
 *  All the routing tracks will start at the most LEFT routing channel
 *  All the routing tracks will end at the most RIGHT routing channel
 *
 *  Things will become more complicated in terms of track starting and end
 *  in the context of heterogeneous FPGAs
 *  We may have a grid which span multiple column and rows, as exemplified in the figure below
 *  In such case,
 *  all the routing tracks [x-1][y] at the left side of the grid [x][y] are forced to end
 *  all the routing tracks [x+2][y] at the right side of the grid [x][y] are forced to start
 *  And there are no routing tracks inside the grid[x][y]
 *  It means that X-channel [x][y] & [x+1][y] will no exist
 *
 *   +------------+     +-------------+       +-------------+        +--------------+
 *   | X-Channel  |     |  X-Channel  |       |  X-Channel  |        |  X-Channel   |
 *   | [x-1][y+2] |     |   [x][y+2]  |       | [x+1][y+2]  |        |  [x+2][y+2]  |
 *   +------------+     +-------------+       +-------------+        +--------------+
 *
 *   +------------+     +-----------------------------------+        +--------------+
 *   |    Grid    |     |                                   |        |    Grid      |
 *   | [x-1][y+1] |     |                                   |        |  [x+2][y+1]  |
 *   +------------+     |                                   |        +--------------+
 *                      |                                   |
 *   +------------+     |                                   |        +--------------+
 *   | X-channel  |     |               Grid                |        |  X-Channel   |
 *   | [x-1][y]   |     |        [x][y] - [x+1][y+1]        |        |   [x+2][y]   |
 *   +------------+     |                                   |        +--------------+
 *                      |                                   |
 *   +------------+     |                                   |        +--------------+
 *   |   Grid     |     |                                   |        |    Grid      |
 *   |  [x-1][y]  |     |                                   |        |   [x+2][y]   |
 *   +------------+     +-----------------------------------+        +--------------+
 *
 *
 *
 ***********************************************************************/
static size_t estimate_num_chanx_rr_nodes(const DeviceGrid& grids,
                                          const size_t& layer,
                                          const size_t& chan_width,
                                          const std::vector<t_segment_inf>& segment_infs,
                                          const DeviceGridAnnotation& device_grid_annotation,
                                          const bool& shrink_boundary,
                                          const bool& through_channel) {
    size_t num_chanx_rr_nodes = 0;

    for (size_t iy = 0; iy < grids.height() - 1; ++iy) {
        for (size_t ix = 1; ix < grids.width() - 1; ++ix) {
            vtr::Point<size_t> chanx_coord(ix, iy);

            /* Bypass if the routing channel does not exist when through channels are not allowed */
            if ((false == through_channel)
                && (false == is_chanx_exist(grids, layer, chanx_coord))) {
                continue;
            }
            /* Bypass if the routing channel does not exist when a shrink boundary is considered */
            if (shrink_boundary && !device_grid_annotation.is_chanx_exist(chanx_coord)) {
                continue;
            }

            bool force_start = false;
            bool force_end = false;

            /* All the tracks have to start when
             *  - the routing channel touch the RIGHT side a heterogeneous block
             *  - the routing channel touch the LEFT side of FPGA
             */
            if (true == is_chanx_right_to_multi_height_grid(grids, layer, chanx_coord, through_channel)) {
                force_start = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chanx_start(chanx_coord)) {
                force_start = true;
            }

            /* All the tracks have to end when
             *  - the routing channel touch the LEFT side a heterogeneous block
             *  - the routing channel touch the RIGHT side of FPGA
             */
            if (true == is_chanx_left_to_multi_height_grid(grids, layer, chanx_coord, through_channel)) {
                force_end = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chanx_end(chanx_coord)) {
                force_end = true;
            }

            /* Evaluate if the routing channel locates in the middle of a grid */
            ChanNodeDetails chanx_details = build_unidir_chan_node_details(chan_width, grids.width() - 2, force_start, force_end, segment_infs);
            /* When an INC_DIRECTION CHANX starts, we need a new rr_node */
            num_chanx_rr_nodes += chanx_details.get_num_starting_tracks(Direction::INC);
            /* When an DEC_DIRECTION CHANX ends, we need a new rr_node */
            num_chanx_rr_nodes += chanx_details.get_num_ending_tracks(Direction::DEC);
        }
    }

    return num_chanx_rr_nodes;
}

/************************************************************************
 * Estimate the number of CHANY rr_nodes for Y-direction routing channels
 * The technical rationale is very similar to the X-direction routing channel
 * Refer to the detailed explanation there
 ***********************************************************************/
static size_t estimate_num_chany_rr_nodes(const DeviceGrid& grids,
                                          const size_t& layer,
                                          const size_t& chan_width,
                                          const std::vector<t_segment_inf>& segment_infs,
                                          const DeviceGridAnnotation& device_grid_annotation,
                                          const bool& shrink_boundary,
                                          const bool& through_channel) {
    size_t num_chany_rr_nodes = 0;

    for (size_t ix = 0; ix < grids.width() - 1; ++ix) {
        for (size_t iy = 1; iy < grids.height() - 1; ++iy) {
            vtr::Point<size_t> chany_coord(ix, iy);

            /* Bypass if the routing channel does not exist when through channel are not allowed */
            if ((false == through_channel)
                && (false == is_chany_exist(grids, layer, chany_coord))) {
                continue;
            }

            /* Bypass if the routing channel does not exist when a shrink boundary is considered */
            if (shrink_boundary && !device_grid_annotation.is_chany_exist(chany_coord)) {
                continue;
            }

            bool force_start = false;
            bool force_end = false;

            /* All the tracks have to start when
             *  - the routing channel touch the TOP side a heterogeneous block
             *  - the routing channel touch the BOTTOM side of FPGA
             */
            if (true == is_chany_top_to_multi_width_grid(grids, layer, chany_coord, through_channel)) {
                force_start = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chany_start(chany_coord)) {
                force_start = true;
            }

            /* All the tracks have to end when
             *  - the routing channel touch the BOTTOM side a heterogeneous block
             *  - the routing channel touch the TOP side of FPGA
             */
            if (true == is_chany_bottom_to_multi_width_grid(grids, layer, chany_coord, through_channel)) {
                force_end = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chany_end(chany_coord)) {
                force_end = true;
            }

            ChanNodeDetails chany_details = build_unidir_chan_node_details(chan_width, grids.height() - 2, force_start, force_end, segment_infs);
            /* When an INC_DIRECTION CHANX starts, we need a new rr_node */
            num_chany_rr_nodes += chany_details.get_num_starting_tracks(Direction::INC);
            /* When an DEC_DIRECTION CHANX ends, we need a new rr_node */
            num_chany_rr_nodes += chany_details.get_num_ending_tracks(Direction::DEC);
        }
    }

    return num_chany_rr_nodes;
}

/************************************************************************
 * Estimate the number of nodes by each type in a routing resource graph
 ***********************************************************************/
static std::vector<size_t> estimate_num_rr_nodes(const DeviceGrid& grids,
                                                 const size_t& layer,
                                                 const vtr::Point<size_t>& chan_width,
                                                 const std::vector<t_segment_inf>& segment_inf_x,
                                                 const std::vector<t_segment_inf>& segment_inf_y,
                                                 const DeviceGridAnnotation& device_grid_annotation,
                                                 const bool& shrink_boundary,
                                                 const bool& through_channel) {
    /* Reset the OPIN, IPIN, SOURCE, SINK counter to be zero */
    std::vector<size_t> num_rr_nodes_per_type(NUM_RR_TYPES, 0);

    /**
     * 1 Find number of rr nodes related to grids
     */
    num_rr_nodes_per_type[OPIN] = estimate_num_grid_rr_nodes_by_type(grids, layer, OPIN);
    num_rr_nodes_per_type[IPIN] = estimate_num_grid_rr_nodes_by_type(grids, layer, IPIN);
    num_rr_nodes_per_type[SOURCE] = estimate_num_grid_rr_nodes_by_type(grids, layer, SOURCE);
    num_rr_nodes_per_type[SINK] = estimate_num_grid_rr_nodes_by_type(grids, layer, SINK);

    /**
     * 2. Assign the segments for each routing channel,
     *    To be specific, for each routing track, we assign a routing segment.
     *    The assignment is subject to users' specifications, such as
     *    a. length of each type of segment
     *    b. frequency of each type of segment.
     *    c. routing channel width
     *
     *    SPECIAL for fringes:
     *    All segments will start and ends with no exception
     *
     *    IMPORTANT: we should be aware that channel width maybe different
     *    in X-direction and Y-direction channels!!!
     *    So we will load segment details for different channels
     */
    num_rr_nodes_per_type[CHANX] = estimate_num_chanx_rr_nodes(grids, layer,
                                                               chan_width.x(),
                                                               segment_inf_x,
                                                               device_grid_annotation,
                                                               shrink_boundary,
                                                               through_channel);
    num_rr_nodes_per_type[CHANY] = estimate_num_chany_rr_nodes(grids, layer,
                                                               chan_width.y(),
                                                               segment_inf_y,
                                                               device_grid_annotation,
                                                               shrink_boundary,
                                                               through_channel);

    return num_rr_nodes_per_type;
}

/************************************************************************
 * Allocate rr_nodes to a rr_graph object
 * This function just allocate the memory and ensure its efficiency
 * It will NOT fill detailed information for each node!!!
 *
 * Note: ensure that there are NO nodes in the rr_graph
 ***********************************************************************/
void alloc_tileable_rr_graph_nodes(RRGraphBuilder& rr_graph_builder,
                                   vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                   const DeviceGrid& grids,
                                   const size_t& layer,
                                   const vtr::Point<size_t>& chan_width,
                                   const std::vector<t_segment_inf>& segment_inf_x,
                                   const std::vector<t_segment_inf>& segment_inf_y,
                                   const DeviceGridAnnotation& device_grid_annotation,
                                   const bool& shrink_boundary,
                                   const bool& through_channel) {
    VTR_ASSERT(0 == rr_graph_builder.rr_nodes().size());

    std::vector<size_t> num_rr_nodes_per_type = estimate_num_rr_nodes(grids,
                                                                      layer,
                                                                      chan_width,
                                                                      segment_inf_x,
                                                                      segment_inf_y,
                                                                      device_grid_annotation,
                                                                      shrink_boundary,
                                                                      through_channel);

    /* Reserve the number of node to be memory efficient */
    size_t num_nodes = 0;
    for (const size_t& num_node_per_type : num_rr_nodes_per_type) {
        num_nodes += num_node_per_type;
    }

    rr_graph_builder.reserve_nodes(num_nodes);

    rr_node_driver_switches.resize(num_nodes);
}

/************************************************************************
 * Configure OPIN rr_nodes for this grid
 * coordinates: xlow, ylow, xhigh, yhigh,
 * features: capacity, ptc_num (pin_num),
 *
 * Note: this function should be applied ONLY to grid with 0 width offset and 0 height offset!!!
 ***********************************************************************/
static void load_one_grid_opin_nodes_basic_info(RRGraphBuilder& rr_graph_builder,
                                                vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                                std::vector<t_rr_rc_data>& rr_rc_data,
                                                const size_t& layer,
                                                const vtr::Point<size_t>& grid_coordinate,
                                                const DeviceGrid& grids,
                                                const e_side& io_side,
                                                const RRSwitchId& delayless_switch) {
    SideManager io_side_manager(io_side);

    /* Walk through the width height of each grid,
     * get pins and configure the rr_nodes
     */
    t_physical_tile_type_ptr phy_tile_type = grids.get_physical_type(t_physical_tile_loc(grid_coordinate.x(), grid_coordinate.y(), layer));
    for (int width = 0; width < phy_tile_type->width; ++width) {
        for (int height = 0; height < phy_tile_type->height; ++height) {
            /* Walk through sides */
            for (e_side side : SIDES) {
                SideManager side_manager(side);
                /* skip unwanted sides */
                if ((true == is_io_type(phy_tile_type))
                    && (side != io_side_manager.to_size_t()) && (NUM_SIDES != io_side)) {
                    continue;
                }
                /* Find OPINs */
                /* Configure pins by pins */
                std::vector<int> opin_list = get_grid_side_pins(grids, layer, grid_coordinate.x(), grid_coordinate.y(), DRIVER, side_manager.get_side(),
                                                                width, height);
                for (const int& pin_num : opin_list) {
                    /* Create a new node and fill information */
                    RRNodeId node = rr_graph_builder.create_node(layer, grid_coordinate.x() + width, grid_coordinate.y() + height, OPIN, pin_num, side);

                    /* node bounding box */
                    rr_graph_builder.set_node_coordinates(node, grid_coordinate.x() + width,
                                                          grid_coordinate.y() + height,
                                                          grid_coordinate.x() + width,
                                                          grid_coordinate.y() + height);
                    rr_graph_builder.add_node_side(node, side_manager.get_side());
                    rr_graph_builder.set_node_pin_num(node, pin_num);

                    rr_graph_builder.set_node_capacity(node, 1);
                    rr_graph_builder.set_node_layer(node, layer);

                    /* cost index is a FIXED value for OPIN */
                    rr_graph_builder.set_node_cost_index(node, RRIndexedDataId(OPIN_COST_INDEX));

                    /* Switch info */
                    rr_node_driver_switches[node] = delayless_switch;

                    /* RC data */
                    rr_graph_builder.set_node_rc_index(node, NodeRCIndex(find_create_rr_rc_data(0., 0., rr_rc_data)));

                } /* End of loading OPIN rr_nodes */
            }     /* End of side enumeration */
        }         /* End of height enumeration */
    }             /* End of width enumeration */
}

/************************************************************************
 * Configure IPIN rr_nodes for this grid
 * coordinates: xlow, ylow, xhigh, yhigh,
 * features: capacity, ptc_num (pin_num),
 *
 * Note: this function should be applied ONLY to grid with 0 width offset and 0 height offset!!!
 ***********************************************************************/
static void load_one_grid_ipin_nodes_basic_info(RRGraphBuilder& rr_graph_builder,
                                                vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                                std::vector<t_rr_rc_data>& rr_rc_data,
                                                const size_t& layer,
                                                const vtr::Point<size_t>& grid_coordinate,
                                                const DeviceGrid& grids,
                                                const e_side& io_side,
                                                const RRSwitchId& wire_to_ipin_switch) {
    SideManager io_side_manager(io_side);

    /* Walk through the width and height of each grid,
     * get pins and configure the rr_nodes
     */
    t_physical_tile_type_ptr phy_tile_type = grids.get_physical_type(t_physical_tile_loc(grid_coordinate.x(), grid_coordinate.y(), layer));
    for (int width = 0; width < phy_tile_type->width; ++width) {
        for (int height = 0; height < phy_tile_type->height; ++height) {
            /* Walk through sides */
            for (e_side side : SIDES) {
                SideManager side_manager(side);
                /* skip unwanted sides */
                if ((true == is_io_type(phy_tile_type))
                    && (side != io_side_manager.to_size_t()) && (NUM_SIDES != io_side)) {
                    continue;
                }

                /* Find IPINs */
                /* Configure pins by pins */
                std::vector<int> ipin_list = get_grid_side_pins(grids, layer, grid_coordinate.x(), grid_coordinate.y(), RECEIVER, side_manager.get_side(), width, height);
                for (const int& pin_num : ipin_list) {
                    /* Create a new node and fill information */
                    RRNodeId node = rr_graph_builder.create_node(layer, grid_coordinate.x() + width, grid_coordinate.y() + height, IPIN, pin_num, side);

                    /* node bounding box */
                    rr_graph_builder.set_node_coordinates(node, grid_coordinate.x() + width,
                                                          grid_coordinate.y() + height,
                                                          grid_coordinate.x() + width,
                                                          grid_coordinate.y() + height);
                    rr_graph_builder.add_node_side(node, side_manager.get_side());
                    rr_graph_builder.set_node_pin_num(node, pin_num);

                    rr_graph_builder.set_node_capacity(node, 1);
                    rr_graph_builder.set_node_layer(node, layer);

                    /* cost index is a FIXED value for OPIN */
                    rr_graph_builder.set_node_cost_index(node, RRIndexedDataId(IPIN_COST_INDEX));

                    /* Switch info */
                    rr_node_driver_switches[node] = wire_to_ipin_switch;

                    /* RC data */
                    rr_graph_builder.set_node_rc_index(node, NodeRCIndex(find_create_rr_rc_data(0., 0., rr_rc_data)));

                } /* End of loading IPIN rr_nodes */
            }     /* End of side enumeration */
        }         /* End of height enumeration */
    }             /* End of width enumeration */
}

/************************************************************************
 * Configure SOURCE rr_nodes for this grid
 * coordinates: xlow, ylow, xhigh, yhigh,
 * features: capacity, ptc_num (pin_num),
 *
 * Note: this function should be applied ONLY to grid with 0 width offset and 0 height offset!!!
 ***********************************************************************/
static void load_one_grid_source_nodes_basic_info(RRGraphBuilder& rr_graph_builder,
                                                  vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                                  std::vector<t_rr_rc_data>& rr_rc_data,
                                                  const size_t& layer,
                                                  const vtr::Point<size_t>& grid_coordinate,
                                                  const DeviceGrid& grids,
                                                  const e_side& io_side,
                                                  const RRSwitchId& delayless_switch) {
    SideManager io_side_manager(io_side);

    /* Set a SOURCE rr_node for each DRIVER class */
    t_physical_tile_loc tile_loc(grid_coordinate.x(), grid_coordinate.y(), layer);
    t_physical_tile_type_ptr phy_tile_type = grids.get_physical_type(tile_loc);
    for (size_t iclass = 0; iclass < phy_tile_type->class_inf.size(); ++iclass) {
        /* Set a SINK rr_node for the OPIN */
        if (DRIVER != phy_tile_type->class_inf[iclass].type) {
            continue;
        }

        /* Create a new node and fill information */
        RRNodeId node = rr_graph_builder.create_node(layer, grid_coordinate.x(), grid_coordinate.y(), SOURCE, iclass);

        /* node bounding box */
        rr_graph_builder.set_node_coordinates(node, grid_coordinate.x(),
                                              grid_coordinate.y(),
                                              grid_coordinate.x() + phy_tile_type->width - 1,
                                              grid_coordinate.y() + phy_tile_type->height - 1);
        rr_graph_builder.set_node_class_num(node, iclass);
        rr_graph_builder.set_node_layer(node, (int)layer);

        /* The capacity should be the number of pins in this class*/
        rr_graph_builder.set_node_capacity(node, phy_tile_type->class_inf[iclass].num_pins);

        /* cost index is a FIXED value for SOURCE */
        rr_graph_builder.set_node_cost_index(node, RRIndexedDataId(SOURCE_COST_INDEX));

        /* Switch info */
        rr_node_driver_switches[node] = delayless_switch;

        /* RC data */
        rr_graph_builder.set_node_rc_index(node, NodeRCIndex(find_create_rr_rc_data(0., 0., rr_rc_data)));

    } /* End of class enumeration */
}

/************************************************************************
 * Configure SINK rr_nodes for this grid
 * coordinates: xlow, ylow, xhigh, yhigh,
 * features: capacity, ptc_num (pin_num),
 *
 * Note: this function should be applied ONLY to grid with 0 width offset and 0 height offset!!!
 ***********************************************************************/
static void load_one_grid_sink_nodes_basic_info(RRGraphBuilder& rr_graph_builder,
                                                vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                                std::vector<t_rr_rc_data>& rr_rc_data,
                                                const size_t& layer,
                                                const vtr::Point<size_t>& grid_coordinate,
                                                const DeviceGrid& grids,
                                                const e_side& io_side,
                                                const RRSwitchId& delayless_switch) {
    SideManager io_side_manager(io_side);

    /* Set a SINK rr_node for each RECEIVER class */
    t_physical_tile_loc tile_loc(grid_coordinate.x(), grid_coordinate.y(), layer);
    t_physical_tile_type_ptr phy_tile_type = grids.get_physical_type(tile_loc);
    for (size_t iclass = 0; iclass < phy_tile_type->class_inf.size(); ++iclass) {
        /* Set a SINK rr_node for the OPIN */
        if (RECEIVER != phy_tile_type->class_inf[iclass].type) {
            continue;
        }

        /* Create a new node and fill information */
        RRNodeId node = rr_graph_builder.create_node(layer, grid_coordinate.x(), grid_coordinate.y(), SINK, iclass);

        /* node bounding box */
        rr_graph_builder.set_node_coordinates(node, grid_coordinate.x(),
                                              grid_coordinate.y(),
                                              grid_coordinate.x() + phy_tile_type->width - 1,
                                              grid_coordinate.y() + phy_tile_type->height - 1);
        rr_graph_builder.set_node_class_num(node, iclass);
        rr_graph_builder.set_node_layer(node, layer);

        rr_graph_builder.set_node_capacity(node, 1);

        /* The capacity should be the number of pins in this class*/
        rr_graph_builder.set_node_capacity(node, phy_tile_type->class_inf[iclass].num_pins);

        /* cost index is a FIXED value for SINK */
        rr_graph_builder.set_node_cost_index(node, RRIndexedDataId(SINK_COST_INDEX));

        /* Switch info */
        rr_node_driver_switches[node] = delayless_switch;

        /* RC data */
        rr_graph_builder.set_node_rc_index(node, NodeRCIndex(find_create_rr_rc_data(0., 0., rr_rc_data)));

    } /* End of class enumeration */
}

/************************************************************************
 * Create all the rr_nodes for grids
 ***********************************************************************/
static void load_grid_nodes_basic_info(RRGraphBuilder& rr_graph_builder,
                                       vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                       std::vector<t_rr_rc_data>& rr_rc_data,
                                       const DeviceGrid& grids,
                                       const size_t& layer,
                                       const RRSwitchId& wire_to_ipin_switch,
                                       const RRSwitchId& delayless_switch) {
    for (size_t iy = 0; iy < grids.height(); ++iy) {
        for (size_t ix = 0; ix < grids.width(); ++ix) {
            t_physical_tile_loc tile_loc(ix, iy, layer);
            /* Skip EMPTY tiles */
            if (true == is_empty_type(grids.get_physical_type(tile_loc))) {
                continue;
            }

            /* We only build rr_nodes for grids with width_offset = 0 and height_offset = 0 */
            if ((0 < grids.get_width_offset(tile_loc))
                || (0 < grids.get_height_offset(tile_loc))) {
                continue;
            }

            vtr::Point<size_t> grid_coordinate(ix, iy);
            enum e_side io_side = NUM_SIDES;

            std::vector<e_side> wanted_sides{TOP, RIGHT, BOTTOM, LEFT};

            /* If this is the block on borders, we consider IO side */
            if (true == is_io_type(grids.get_physical_type(tile_loc))) {
                vtr::Point<size_t> io_device_size(grids.width() - 1, grids.height() - 1);
                io_side = determine_io_grid_pin_side(io_device_size, grid_coordinate);
                wanted_sides.clear();
                wanted_sides.push_back(io_side);
            }

            for (e_side side : wanted_sides) {
                for (int width_offset = 0; width_offset < grids.get_physical_type(tile_loc)->width; ++width_offset) {
                    int x_tile = ix + width_offset;
                    for (int height_offset = 0; height_offset < grids.get_physical_type(tile_loc)->height; ++height_offset) {
                        int y_tile = iy + height_offset;
                        rr_graph_builder.node_lookup().reserve_nodes(layer, x_tile, y_tile, OPIN, grids.get_physical_type(tile_loc)->num_pins, side);
                        rr_graph_builder.node_lookup().reserve_nodes(layer, x_tile, y_tile, IPIN, grids.get_physical_type(tile_loc)->num_pins, side);
                    }
                }
            }

            /* Configure source rr_nodes for this grid */
            load_one_grid_source_nodes_basic_info(rr_graph_builder,
                                                  rr_node_driver_switches,
                                                  rr_rc_data,
                                                  layer, grid_coordinate,
                                                  grids,
                                                  io_side,
                                                  delayless_switch);

            /* Configure sink rr_nodes for this grid */
            load_one_grid_sink_nodes_basic_info(rr_graph_builder,
                                                rr_node_driver_switches,
                                                rr_rc_data,
                                                layer, grid_coordinate,
                                                grids,
                                                io_side,
                                                delayless_switch);

            /* Configure opin rr_nodes for this grid */
            load_one_grid_opin_nodes_basic_info(rr_graph_builder,
                                                rr_node_driver_switches,
                                                rr_rc_data,
                                                layer, grid_coordinate,
                                                grids,
                                                io_side,
                                                delayless_switch);

            /* Configure ipin rr_nodes for this grid */
            load_one_grid_ipin_nodes_basic_info(rr_graph_builder,
                                                rr_node_driver_switches,
                                                rr_rc_data,
                                                layer, grid_coordinate,
                                                grids,
                                                io_side,
                                                wire_to_ipin_switch);
        }
    }
    //Copy the SOURCE/SINK nodes to all offset positions for blocks with width > 1 and/or height > 1
    // This ensures that look-ups on non-root locations will still find the correct SOURCE/SINK
    for (size_t x = 0; x < grids.width(); x++) {
        for (size_t y = 0; y < grids.height(); y++) {
            t_physical_tile_loc tile_loc(x, y, 0);
            int width_offset = grids.get_width_offset(tile_loc);
            int height_offset = grids.get_height_offset(tile_loc);
            if (width_offset != 0 || height_offset != 0) {
                int root_x = x - width_offset;
                int root_y = y - height_offset;

                rr_graph_builder.node_lookup().mirror_nodes(0,
                                                            vtr::Point<int>(root_x, root_y),
                                                            vtr::Point<int>(x, y),
                                                            SOURCE,
                                                            SIDES[0]);
                rr_graph_builder.node_lookup().mirror_nodes(0,
                                                            vtr::Point<int>(root_x, root_y),
                                                            vtr::Point<int>(x, y),
                                                            SINK,
                                                            SIDES[0]);
            }
        }
    }
}

/************************************************************************
 * Initialize the basic information of routing track rr_nodes
 * coordinates: xlow, ylow, xhigh, yhigh,
 * features: capacity, track_ids, ptc_num, direction
 ***********************************************************************/
static void load_one_chan_rr_nodes_basic_info(const RRGraphView& rr_graph,
                                              RRGraphBuilder& rr_graph_builder,
                                              vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                              std::map<RRNodeId, std::vector<size_t>>& rr_node_track_ids,
                                              const size_t& layer,
                                              const vtr::Point<size_t>& chan_coordinate,
                                              const t_rr_type& chan_type,
                                              ChanNodeDetails& chan_details,
                                              const std::vector<t_segment_inf>& segment_infs,
                                              const t_unified_to_parallel_seg_index& seg_index_map,
                                              const int& cost_index_offset) {
    /* Check each node_id(potential ptc_num) in the channel :
     * If this is a starting point, we set a new rr_node with xlow/ylow, ptc_num
     * If this is a ending point, we set xhigh/yhigh and track_ids
     * For other nodes, we set changes in track_ids
     */
    for (size_t itrack = 0; itrack < chan_details.get_chan_width(); ++itrack) {
        /* For INC direction, a starting point requires a new chan rr_node  */
        if (((true == chan_details.is_track_start(itrack))
             && (Direction::INC == chan_details.get_track_direction(itrack)))
            /* For DEC direction, an ending point requires a new chan rr_node  */
            || ((true == chan_details.is_track_end(itrack))
                && (Direction::DEC == chan_details.get_track_direction(itrack)))) {
            /* Create a new chan rr_node  */
            RRNodeId node = rr_graph_builder.create_node(layer, chan_coordinate.x(), chan_coordinate.y(), chan_type, itrack);

            rr_graph_builder.set_node_direction(node, chan_details.get_track_direction(itrack));
            rr_graph_builder.add_node_track_num(node, chan_coordinate, itrack);
            rr_node_track_ids[node].push_back(itrack);

            rr_graph_builder.set_node_capacity(node, 1);
            rr_graph_builder.set_node_layer(node, layer);

            /* assign switch id */
            size_t seg_id = chan_details.get_track_segment_id(itrack);
            e_parallel_axis wanted_axis = chan_type == CHANX ? X_AXIS : Y_AXIS;
            size_t parallel_seg_id = find_parallel_seg_index(seg_id, seg_index_map, wanted_axis);
            rr_node_driver_switches[node] = RRSwitchId(segment_infs[parallel_seg_id].arch_opin_switch);

            /* Update chan_details with node_id */
            chan_details.set_track_node_id(itrack, size_t(node));

            /* cost index depends on the segment index */
            rr_graph_builder.set_node_cost_index(node, RRIndexedDataId(cost_index_offset + parallel_seg_id));
            /* Finish here, go to next */
        }

        /* For INC direction, an ending point requires an update on xhigh and yhigh  */
        if (((true == chan_details.is_track_end(itrack))
             && (Direction::INC == chan_details.get_track_direction(itrack)))
            ||
            /* For DEC direction, an starting point requires an update on xlow and ylow  */
            ((true == chan_details.is_track_start(itrack))
             && (Direction::DEC == chan_details.get_track_direction(itrack)))) {
            /* Get the node_id */
            const RRNodeId& rr_node_id = RRNodeId(chan_details.get_track_node_id(itrack));

            /* Do a quick check, make sure we do not mistakenly modify other nodes */
            VTR_ASSERT(chan_type == rr_graph.node_type(rr_node_id));
            VTR_ASSERT(chan_details.get_track_direction(itrack) == rr_graph.node_direction(rr_node_id));

            /* set xhigh/yhigh and push changes to track_ids */
            rr_graph_builder.set_node_coordinates(rr_node_id, rr_graph.node_xlow(rr_node_id),
                                                  rr_graph.node_ylow(rr_node_id),
                                                  chan_coordinate.x(),
                                                  chan_coordinate.y());

            /* Do not update track_ids for length-1 wires, they should have only 1 track_id */
            if ((rr_graph.node_xhigh(rr_node_id) > rr_graph.node_xlow(rr_node_id))
                || (rr_graph.node_yhigh(rr_node_id) > rr_graph.node_ylow(rr_node_id))) {
                rr_node_track_ids[rr_node_id].push_back(itrack);
                rr_graph_builder.add_node_track_num(rr_node_id, chan_coordinate, itrack);
            }
            /* Finish here, go to next */
        }

        /* Finish processing starting and ending tracks */
        if ((true == chan_details.is_track_start(itrack))
            || (true == chan_details.is_track_end(itrack))) {
            /* Finish here, go to next */
            continue;
        }

        /* For other nodes, we get the node_id and just update track_ids */
        /* Ensure those nodes are neither starting nor ending points */
        VTR_ASSERT((false == chan_details.is_track_start(itrack))
                   && (false == chan_details.is_track_end(itrack)));

        /* Get the node_id */
        const RRNodeId& rr_node_id = RRNodeId(chan_details.get_track_node_id(itrack));

        /* Do a quick check, make sure we do not mistakenly modify other nodes */
        VTR_ASSERT(chan_type == rr_graph.node_type(rr_node_id));
        VTR_ASSERT(chan_details.get_track_direction(itrack) == rr_graph.node_direction(rr_node_id));

        /* Deposit xhigh and yhigh using the current chan_coordinate
         * We will update when this track ends
         */
        rr_graph_builder.set_node_coordinates(rr_node_id, rr_graph.node_xlow(rr_node_id),
                                              rr_graph.node_ylow(rr_node_id),
                                              chan_coordinate.x(),
                                              chan_coordinate.y());

        /* Update track_ids */
        rr_node_track_ids[rr_node_id].push_back(itrack);
        rr_graph_builder.add_node_track_num(rr_node_id, chan_coordinate, itrack);
        /* Finish here, go to next */
    }
}

/************************************************************************
 * Initialize the basic information of X-channel rr_nodes:
 * coordinates: xlow, ylow, xhigh, yhigh,
 * features: capacity, track_ids, ptc_num, direction
 * grid_info : pb_graph_pin
 ***********************************************************************/
static void load_chanx_rr_nodes_basic_info(const RRGraphView& rr_graph,
                                           RRGraphBuilder& rr_graph_builder,
                                           vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                           std::map<RRNodeId, std::vector<size_t>>& rr_node_track_ids,
                                           const DeviceGrid& grids,
                                           const size_t& layer,
                                           const size_t& chan_width,
                                           const std::vector<t_segment_inf>& segment_infs,
                                           const t_unified_to_parallel_seg_index& segment_index_map,
                                           const DeviceGridAnnotation& device_grid_annotation,
                                           const bool& shrink_boundary,
                                           const bool& through_channel) {
    /* For X-direction Channel: CHANX */
    for (size_t iy = 0; iy < grids.height() - 1; ++iy) {
        /* Keep a vector of node_ids for the channels, because we will rotate them when walking through ix */
        std::vector<size_t> track_node_ids;

        for (size_t ix = 1; ix < grids.width() - 1; ++ix) {
            vtr::Point<size_t> chanx_coord(ix, iy);

            /* Bypass if the routing channel does not exist when through channels are not allowed */
            if ((false == through_channel)
                && (false == is_chanx_exist(grids, layer, chanx_coord))) {
                continue;
            }
            /* Bypass if the routing channel does not exist when a shrink boundary is considered */
            if (shrink_boundary && !device_grid_annotation.is_chanx_exist(chanx_coord)) {
                continue;
            }

            bool force_start = false;
            bool force_end = false;

            /* All the tracks have to start when
             *  - the routing channel touch the RIGHT side a heterogeneous block
             *  - the routing channel touch the LEFT side of FPGA
             */
            if (true == is_chanx_right_to_multi_height_grid(grids, layer, chanx_coord, through_channel)) {
                force_start = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chanx_start(chanx_coord)) {
                force_start = true;
            }

            /* All the tracks have to end when
             *  - the routing channel touch the LEFT side a heterogeneous block
             *  - the routing channel touch the RIGHT side of FPGA
             */
            if (true == is_chanx_left_to_multi_height_grid(grids, layer, chanx_coord, through_channel)) {
                force_end = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chanx_end(chanx_coord)) {
                force_end = true;
            }

            ChanNodeDetails chanx_details = build_unidir_chan_node_details(chan_width, grids.width() - 2,
                                                                           force_start, force_end, segment_infs);
            /* Force node_ids from the previous chanx */
            if (0 < track_node_ids.size()) {
                /* Rotate should be done based on a typical case of routing tracks.
                 * Tracks on the borders are not regularly started and ended,
                 * which causes the node_rotation malfunction
                 */
                ChanNodeDetails chanx_details_tt = build_unidir_chan_node_details(chan_width, grids.width() - 2,
                                                                                  false, false, segment_infs);
                chanx_details_tt.set_track_node_ids(track_node_ids);

                /* TODO:
                 * Do NOT rotate the tracks when the routing channel
                 * locates inside a multi-height and multi-width grid
                 * Let the routing channel passing through the grid (if through channel is allowed!)
                 * An example:
                 *
                 *               +------------------------------
                 *               |                             |
                 *               |          Grid               |
                 *  track0 ----->+-----------------------------+----> track0
                 *               |                             |
                 */
                if (true == is_chanx_exist(grids, layer, chanx_coord, through_channel)) {
                    /* Rotate the chanx_details by an offset of ix - 1, the distance to the most left channel */
                    /* For INC_DIRECTION, we use clockwise rotation
                     * node_id A ---->   -----> node_id D
                     * node_id B ---->  / ----> node_id A
                     * node_id C ----> /  ----> node_id B
                     * node_id D ---->    ----> node_id C
                     */
                    chanx_details_tt.rotate_track_node_id(1, Direction::INC, true);
                    /* For DEC_DIRECTION, we use clockwise rotation
                     * node_id A <-----    <----- node_id B
                     * node_id B <----- \  <----- node_id C
                     * node_id C <-----  \ <----- node_id D
                     * node_id D <-----    <----- node_id A
                     */
                    chanx_details_tt.rotate_track_node_id(1, Direction::DEC, false);
                }

                track_node_ids = chanx_details_tt.get_track_node_ids();
                chanx_details.set_track_node_ids(track_node_ids);
            }

            /* Configure CHANX in this channel */
            load_one_chan_rr_nodes_basic_info(rr_graph,
                                              rr_graph_builder,
                                              rr_node_driver_switches,
                                              rr_node_track_ids,
                                              layer, chanx_coord, CHANX,
                                              chanx_details,
                                              segment_infs,
                                              segment_index_map,
                                              CHANX_COST_INDEX_START);
            /* Get a copy of node_ids */
            track_node_ids = chanx_details.get_track_node_ids();
        }
    }
}

/************************************************************************
 * Initialize the basic information of Y-channel rr_nodes:
 * coordinates: xlow, ylow, xhigh, yhigh,
 * features: capacity, track_ids, ptc_num, direction
 ***********************************************************************/
static void load_chany_rr_nodes_basic_info(const RRGraphView& rr_graph,
                                           RRGraphBuilder& rr_graph_builder,
                                           vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                           std::map<RRNodeId, std::vector<size_t>>& rr_node_track_ids,
                                           const DeviceGrid& grids,
                                           const size_t& layer,
                                           const size_t& chan_width,
                                           const std::vector<t_segment_inf>& segment_infs,
                                           const size_t& num_segment_x,
                                           const t_unified_to_parallel_seg_index& seg_index_map,
                                           const DeviceGridAnnotation& device_grid_annotation,
                                           const bool& shrink_boundary,
                                           const bool& through_channel) {
    /* For Y-direction Channel: CHANY */
    for (size_t ix = 0; ix < grids.width() - 1; ++ix) {
        /* Keep a vector of node_ids for the channels, because we will rotate them when walking through ix */
        std::vector<size_t> track_node_ids;

        for (size_t iy = 1; iy < grids.height() - 1; ++iy) {
            vtr::Point<size_t> chany_coord(ix, iy);

            /* Bypass if the routing channel does not exist when through channel are not allowed */
            if ((false == through_channel)
                && (false == is_chany_exist(grids, layer, chany_coord))) {
                continue;
            }
            /* Bypass if the routing channel does not exist when a shrink boundary is considered */
            if (shrink_boundary && !device_grid_annotation.is_chany_exist(chany_coord)) {
                continue;
            }

            bool force_start = false;
            bool force_end = false;

            /* All the tracks have to start when
             *  - the routing channel touch the TOP side a heterogeneous block
             *  - the routing channel touch the BOTTOM side of FPGA
             */
            if (true == is_chany_top_to_multi_width_grid(grids, layer, chany_coord, through_channel)) {
                force_start = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chany_start(chany_coord)) {
                force_start = true;
            }

            /* All the tracks have to end when
             *  - the routing channel touch the BOTTOM side a heterogeneous block
             *  - the routing channel touch the TOP side of FPGA
             */
            if (true == is_chany_bottom_to_multi_width_grid(grids, layer, chany_coord, through_channel)) {
                force_end = true;
            }
            if (shrink_boundary && device_grid_annotation.is_chany_end(chany_coord)) {
                force_end = true;
            }

            ChanNodeDetails chany_details = build_unidir_chan_node_details(chan_width, grids.height() - 2,
                                                                           force_start, force_end, segment_infs);
            /* Force node_ids from the previous chany
             * This will not be applied when the routing channel is cut off (force to start)
             */
            if (0 < track_node_ids.size()) {
                /* Rotate should be done based on a typical case of routing tracks.
                 * Tracks on the borders are not regularly started and ended,
                 * which causes the node_rotation malfunction
                 */
                ChanNodeDetails chany_details_tt = build_unidir_chan_node_details(chan_width, grids.height() - 2,
                                                                                  false, false, segment_infs);

                chany_details_tt.set_track_node_ids(track_node_ids);

                /* TODO:
                 * Do NOT rotate the tracks when the routing channel
                 * locates inside a multi-height and multi-width grid
                 * Let the routing channel passing through the grid (if through channel is allowed!)
                 * An example:
                 *
                 *               +------------------------------
                 *               |                             |
                 *               |          Grid               |
                 *  track0 ----->+-----------------------------+----> track0
                 *               |                             |
                 * we should rotate only once at the bottom side of a grid
                 */
                if (true == is_chany_exist(grids, layer, chany_coord, through_channel)) {
                    /* Rotate the chany_details by an offset of 1*/
                    /* For INC_DIRECTION, we use clockwise rotation
                     * node_id A ---->   -----> node_id D
                     * node_id B ---->  / ----> node_id A
                     * node_id C ----> /  ----> node_id B
                     * node_id D ---->    ----> node_id C
                     */
                    chany_details_tt.rotate_track_node_id(1, Direction::INC, true);
                    /* For DEC_DIRECTION, we use clockwise rotation
                     * node_id A <-----    <----- node_id B
                     * node_id B <----- \  <----- node_id C
                     * node_id C <-----  \ <----- node_id D
                     * node_id D <-----    <----- node_id A
                     */
                    chany_details_tt.rotate_track_node_id(1, Direction::DEC, false);
                }

                track_node_ids = chany_details_tt.get_track_node_ids();
                chany_details.set_track_node_ids(track_node_ids);
            }
            /* Configure CHANX in this channel */
            load_one_chan_rr_nodes_basic_info(rr_graph,
                                              rr_graph_builder,
                                              rr_node_driver_switches,
                                              rr_node_track_ids,
                                              layer, chany_coord, CHANY,
                                              chany_details,
                                              segment_infs,
                                              seg_index_map,
                                              CHANX_COST_INDEX_START + num_segment_x);
            /* Get a copy of node_ids */
            track_node_ids = chany_details.get_track_node_ids();
        }
    }
}

/************************************************************************
 * Reverse the track_ids of CHANX and CHANY nodes in DEC_DIRECTION
 * This is required as the track ids are allocated in the sequence
 * of incrementing x and y
 * However, DEC direction routing tracks should have a reversed sequence in
 * track ids
 ***********************************************************************/
static void reverse_dec_chan_rr_node_track_ids(const RRGraphView& rr_graph,
                                               std::map<RRNodeId, std::vector<size_t>>& rr_node_track_ids) {
    // this should call rr_graph_builder to do the job
    for (const RRNodeId& node : rr_graph.nodes()) {
        /* Bypass condition: only focus on CHANX and CHANY in DEC_DIRECTION */
        if (CHANX != rr_graph.node_type(node) && CHANY != rr_graph.node_type(node)) {
            continue;
        }
        /* Reach here, we must have a node of CHANX or CHANY */
        if (Direction::DEC != rr_graph.node_direction(node)) {
            continue;
        }
        std::reverse(rr_node_track_ids[node].begin(),
                     rr_node_track_ids[node].end());
    }
}

/************************************************************************
 * Create all the rr_nodes covering both grids and routing channels
 ***********************************************************************/
void create_tileable_rr_graph_nodes(const RRGraphView& rr_graph,
                                    RRGraphBuilder& rr_graph_builder,
                                    vtr::vector<RRNodeId, RRSwitchId>& rr_node_driver_switches,
                                    std::map<RRNodeId, std::vector<size_t>>& rr_node_track_ids,
                                    std::vector<t_rr_rc_data>& rr_rc_data,
                                    const DeviceGrid& grids,
                                    const size_t& layer,
                                    const vtr::Point<size_t>& chan_width,
                                    const std::vector<t_segment_inf>& segment_inf_x,
                                    const std::vector<t_segment_inf>& segment_inf_y,
                                    const t_unified_to_parallel_seg_index& segment_index_map,
                                    const RRSwitchId& wire_to_ipin_switch,
                                    const RRSwitchId& delayless_switch,
                                    const DeviceGridAnnotation& device_grid_annotation,
                                    const bool& shrink_boundary,
                                    const bool& through_channel) {
    /* Allocates and loads all the structures needed for fast lookups of the   *
     * index of an rr_node.  rr_node_indices is a matrix containing the index  *
     * of the *first* rr_node at a given (i,j) location.                       */

    /* Alloc the lookup table 
     * .. warning: It is mandatory. There are bugs in resize() when called incrementally in RRSpatialLookup.
     *             When comment the following block out, you will see errors */
    for (t_rr_type rr_type : RR_TYPES) {
        if (rr_type == CHANX) {
            rr_graph_builder.node_lookup().resize_nodes(layer, grids.height(), grids.width(), rr_type, NUM_SIDES);
        } else {
            rr_graph_builder.node_lookup().resize_nodes(layer, grids.width(), grids.height(), rr_type, NUM_SIDES);
        }
    }

    load_grid_nodes_basic_info(rr_graph_builder,
                               rr_node_driver_switches,
                               rr_rc_data,
                               grids, layer,
                               wire_to_ipin_switch,
                               delayless_switch);

    load_chanx_rr_nodes_basic_info(rr_graph,
                                   rr_graph_builder,
                                   rr_node_driver_switches,
                                   rr_node_track_ids,
                                   grids, layer,
                                   chan_width.x(),
                                   segment_inf_x,
                                   segment_index_map,
                                   device_grid_annotation,
                                   shrink_boundary,
                                   through_channel);

    load_chany_rr_nodes_basic_info(rr_graph,
                                   rr_graph_builder,
                                   rr_node_driver_switches,
                                   rr_node_track_ids,
                                   grids, layer,
                                   chan_width.y(),
                                   segment_inf_y,
                                   segment_inf_x.size(),
                                   segment_index_map,
                                   device_grid_annotation,
                                   shrink_boundary,
                                   through_channel);

    reverse_dec_chan_rr_node_track_ids(rr_graph,
                                       rr_node_track_ids);

    /* Update node look-up for CHANX and CHANY nodes */
    for (const RRNodeId& rr_node_id : rr_graph.nodes()) {
        if (CHANX == rr_graph.node_type(rr_node_id) || CHANY == rr_graph.node_type(rr_node_id)) {
            rr_graph_builder.add_track_node_to_lookup(rr_node_id);
        }
    }
}
