#ifndef VTR_WEST_FIRST_ROUTING_H
#define VTR_WEST_FIRST_ROUTING_H

#include "turn_model_routing.h"

class WestFirstRouting : public TurnModelRouting {
  public:
    WestFirstRouting(const NocStorage& noc_model,
                     const std::optional<std::reference_wrapper<const NocVirtualBlockStorage>>& noc_virtual_blocks);
    ~WestFirstRouting() override;

  private:
    const std::vector<TurnModelRouting::Direction>& get_legal_directions(NocRouterId src_router_id,
                                                                         NocRouterId curr_router_id,
                                                                         NocRouterId dst_router_id) override;

    TurnModelRouting::Direction select_next_direction(const std::vector<TurnModelRouting::Direction>& legal_directions,
                                                      NocRouterId src_router_id,
                                                      NocRouterId dst_router_id,
                                                      NocRouterId curr_router_id,
                                                      NocTrafficFlowId traffic_flow_id) override;
};

#endif //VTR_WEST_FIRST_ROUTING_H
