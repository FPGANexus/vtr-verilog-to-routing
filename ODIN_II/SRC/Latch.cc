/**
 * Copyright (c) 2021 Seyed Alireza Damghani (sdamghann@gmail.com)
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "Latch.hh"
#include "node_creation_library.h"
#include "odin_util.h"
#include "netlist_utils.h"
#include "vtr_memory.h"

/**
 * (function: resolve_dlatch_node)
 * 
 * @brief split the dlatch node read from yosys blif to
 * latch nodes with input/output width one
 * 
 * @param node pointing to the dlatch node 
 * @param traverse_mark_number unique traversal mark for blif elaboration pass
 * @param netlist pointer to the current netlist file
 */
void resolve_dlatch_node(nnode_t* node, uintptr_t traverse_mark_number, netlist_t* netlist) {
    oassert(node->traverse_visited == traverse_mark_number);
    oassert(node->num_input_port_sizes == node->num_output_port_sizes + 1);

    /* making sure the enable input is 1 bit */
    oassert(node->input_port_sizes[1] == 1);

    /**
     * D:   input port 0
     * EN:  input port 1
     * Q:   output port 0
    */
    int i;
    int D_width = node->input_port_sizes[0];
    // int EN_width = node->input_port_sizes[1]; // == 1

    signal_list_t* possible_clks = init_signal_list();
    add_pin_to_signal_list(possible_clks, node->input_pins[D_width]);
    signal_list_t* new_clk_output = create_single_clk_pin(possible_clks, node, netlist);
    npin_t* new_clk = new_clk_output->pins[0];

    for (i = 0; i < D_width; i++) {
        /*****************************************************************************************/
        /**************************************** FF_NODE ****************************************/
        /*****************************************************************************************/
        /* remapping the D inputs to [1..n] */
        make_ff_node(node->input_pins[i],      // D
                     copy_input_npin(new_clk), // clk
                     node->output_pins[i],     // Q
                     node,                     // node
                     netlist                   // netlist
        );
    }

    // CLEAN UP
    free_signal_list(new_clk_output);
    free_signal_list(possible_clks);
    free_nnode(node);
}

/**
 * (function: resolve_adlatch_node)
 * 
 * @brief split the adlatch node read from yosys blif to
 * latch nodes with input/output width one
 * 
 * @param node pointing to the adlatch node 
 * @param traverse_mark_number unique traversal mark for blif elaboration pass
 * @param netlist pointer to the current netlist file
 */
void resolve_adlatch_node(nnode_t* node, uintptr_t traverse_mark_number, netlist_t* netlist) {
    oassert(node->traverse_visited == traverse_mark_number);
    oassert(node->num_input_port_sizes == 3);
    oassert(node->num_output_port_sizes == 1);

    /* making sure the arst input is 1 bit */
    oassert(node->input_port_sizes[0] == 1);

    /* making sure the en input is 1 bit */
    oassert(node->input_port_sizes[2] == 1);

    /**
     * ARST: input port 0
     * D:    input port 1
     * EN:   input port 2
     * Q:    output port 0
    */
    int i;
    int ARST_width = node->input_port_sizes[0]; // == 1
    int D_width = node->input_port_sizes[1];

    /*****************************************************************************************/
    /**************************************** EN_CHECK ***************************************/
    /*****************************************************************************************/
    /* store enable sensitivity in enable pin */
    node->input_pins[ARST_width + D_width]->sensitivity = node->attributes->enable_polarity;
    /* keep record of enable pin */
    npin_t* select_enable = copy_input_npin(node->input_pins[ARST_width + D_width]);
    /* delete to avoid mem leaks */
    delete_npin(node->input_pins[ARST_width + D_width]);

    /*****************************************************************************************/
    /**************************************** RST_CHECK **************************************/
    /*****************************************************************************************/
    /* store areset sensitivity in areset pin */
    node->input_pins[0]->sensitivity = node->attributes->areset_polarity;
    /* keep record of areset pin */
    npin_t* select_areset = copy_input_npin(node->input_pins[0]);
    signal_list_t* areset_value = create_constant_signal(node->attributes->areset_value, D_width, netlist);

    /*****************************************************************************************/
    /***************************************** FF CLK ****************************************/
    /*****************************************************************************************/
    signal_list_t* possible_clks = init_signal_list();
    add_pin_to_signal_list(possible_clks, copy_input_npin(select_enable));
    add_pin_to_signal_list(possible_clks, copy_input_npin(select_areset));
    /* create single bit clk */
    signal_list_t* adlatch_clk = create_single_clk_pin(possible_clks, node, netlist);
    npin_t* new_clk = adlatch_clk->pins[0];

    /* creating a internal nodes to initialize the value of D register */
    for (i = 0; i < D_width; i++) {
        /*****************************************************************************************/
        /*************************************** EN_MUX *****************************************/
        /*****************************************************************************************/
        nnode_t* EN_mux = smux_with_sel_polarity(get_pad_pin(netlist),             // pad while not enable
                                                 node->input_pins[i + ARST_width], // D[i]
                                                 copy_input_npin(select_enable),   // enable selector
                                                 node                              // node
        );

        npin_t* EN_muxes_output_pin = EN_mux->output_pins[0]->net->fanout_pins[0];
        /*****************************************************************************************/
        /*************************************** RST_MUX *****************************************/
        /*****************************************************************************************/
        nnode_t* ARST_mux = smux_with_sel_polarity(EN_muxes_output_pin,            // D[i]
                                                   areset_value->pins[i],          // areset value
                                                   copy_input_npin(select_areset), // areset selector
                                                   node                            // node
        );

        npin_t* ARST_muxes_output_pin = ARST_mux->output_pins[0]->net->fanout_pins[0];

        /*****************************************************************************************/
        /*************************************** FF_NODE *****************************************/
        /*****************************************************************************************/
        make_ff_node(ARST_muxes_output_pin,    // D
                     copy_input_npin(new_clk), // clk
                     node->output_pins[i],     // Q
                     node,                     // node
                     netlist                   // netlist
        );
    }

    // CLEAN UP
    free_signal_list(possible_clks);
    free_signal_list(adlatch_clk);
    free_nnode(node);
}

/**
 * (function: resolve_sr_node)
 * 
 * @brief resolving the sr node using 
 * 
 * @param node pointing to a sr node 
 * @param traverse_mark_number unique traversal mark for blif elaboration pass
 * @param netlist pointer to the current netlist file
 */
void resolve_sr_node(nnode_t* node, uintptr_t traverse_mark_number, netlist_t* netlist) {
    oassert(node->traverse_visited == traverse_mark_number);
    oassert(node->num_input_port_sizes == 2);
    oassert(node->num_output_port_sizes == 1);

    /**
     * CLR: input port 0
     * Q:   output port 0
     * SET: input port 1
    */
    int width = node->num_output_pins;
    int CLR_width = node->input_port_sizes[0];

    int i;
    for (i = 0; i < width; i++) {
        /*****************************************************************************************/
        /**************************************** SET_CHECK **************************************/
        /*****************************************************************************************/
        /* store set sensitivity in set pin */
        node->input_pins[CLR_width + i]->sensitivity = node->attributes->set_polarity;
        /* keep record of set pin */
        npin_t* select_set = copy_input_npin(node->input_pins[CLR_width + i]);
        /* delete to avoid mem leaks */
        delete_npin(node->input_pins[CLR_width + i]);

        /*****************************************************************************************/
        /**************************************** SET_MUXES **************************************/
        /*****************************************************************************************/
        nnode_t* SET_mux = smux_with_sel_polarity(get_pad_pin(netlist),        // PAD
                                                  get_one_pin(netlist),        // VCC (set value)
                                                  copy_input_npin(select_set), // set selector
                                                  node                         // node
        );

        // specify SET_muxes[i] output pin
        npin_t* SET_muxes_output_pin = SET_mux->output_pins[0]->net->fanout_pins[0];

        /*****************************************************************************************/
        /*************************************** CLR_CHECK ***************************************/
        /*****************************************************************************************/
        /* store reset sensitivity in rst pin */
        node->input_pins[i]->sensitivity = node->attributes->clr_polarity;
        /* keep record of clr pin */
        npin_t* select_clr = copy_input_npin(node->input_pins[i]);
        /* delete to avoid mem leaks */
        delete_npin(node->input_pins[i]);

        /*****************************************************************************************/
        /*************************************** CLR_MUXES ***************************************/
        /*****************************************************************************************/
        nnode_t* CLR_mux = smux_with_sel_polarity(SET_muxes_output_pin,        // set mux output
                                                  get_zero_pin(netlist),       // GND (clr value)
                                                  copy_input_npin(select_clr), // clr selector
                                                  node                         // node
        );

        // specify CLR_muxes[i] output pin
        npin_t* CLR_muxes_output_pin = CLR_mux->output_pins[0]->net->fanout_pins[0];

        /*****************************************************************************************/
        /**************************************** BUF_NODE ***************************************/
        /*****************************************************************************************/
        /* create buf node for $sr output */
        nnode_t* BUF_node = make_1port_gate(BUF_NODE, 1, 1, node, traverse_mark_number);

        /**
         * remap D[i] from the sr node to the buf node 
         * since we do not need it in dbuf node anymore 
         **/
        add_input_pin_to_node(BUF_node, CLR_muxes_output_pin, 0);

        // remap node's output pin to CLR_muxes
        remap_pin_to_new_node(node->output_pins[i], BUF_node, 0);
    }

    // CLEAN UP
    free_nnode(node);
}
