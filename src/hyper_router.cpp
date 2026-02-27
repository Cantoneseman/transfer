#include "topology_manager.hpp"

#include <iostream>
#include <string>

// ============================================================================
// HYPER-TRANSFER v2.0 - Router CLI Tool
// ============================================================================
// CLI utility for querying optimal network paths.
// Used by the Python controller to determine routing decisions.
//
// Usage: ./hyper_router <src_node> <dst_node>
// Output: Node IDs separated by spaces (e.g., "GZ WH BJ")
// Exit codes: 0 = success, 1 = error/no path
// ============================================================================

using namespace hyper;

/**
 * @brief Setup the demo network topology.
 * 
 * Topology:
 *   GZ ---(slow)--> BJ     (100 Mbps, 50ms, 1% loss)
 *   GZ ---(fast)--> WH     (1000 Mbps, 5ms, 0% loss)
 *   WH ---(fast)--> BJ     (1000 Mbps, 5ms, 0% loss)
 */
void setup_demo_topology(TopologyManager& topo) {
    // Direct Link (Slow): GZ -> BJ
    topo.add_link("GZ", "BJ", LinkMetric(100.0, 50.0, 0.01));
    
    // Indirect Links (Fast): GZ -> WH -> BJ
    topo.add_link("GZ", "WH", LinkMetric(1000.0, 5.0, 0.0));
    topo.add_link("WH", "BJ", LinkMetric(1000.0, 5.0, 0.0));
    
    // Add reverse links for bidirectional routing
    topo.add_link("BJ", "GZ", LinkMetric(100.0, 50.0, 0.01));
    topo.add_link("WH", "GZ", LinkMetric(1000.0, 5.0, 0.0));
    topo.add_link("BJ", "WH", LinkMetric(1000.0, 5.0, 0.0));
}

void print_usage(const char* prog_name) {
    std::cerr << "HYPER-TRANSFER v2.0 Router\n";
    std::cerr << "Usage: " << prog_name << " <src_node> <dst_node>\n";
    std::cerr << "\nAvailable nodes: GZ, WH, BJ\n";
    std::cerr << "Output: Space-separated node IDs on stdout\n";
}

int main(int argc, char* argv[]) {
    // Validate arguments
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string src_node = argv[1];
    std::string dst_node = argv[2];
    
    // Setup topology
    TopologyManager topo;
    setup_demo_topology(topo);
    
    // Validate nodes exist
    if (!topo.has_node(src_node)) {
        std::cerr << "Error: Unknown source node '" << src_node << "'\n";
        return 1;
    }
    
    if (!topo.has_node(dst_node)) {
        std::cerr << "Error: Unknown destination node '" << dst_node << "'\n";
        return 1;
    }
    
    // Find best path
    NetworkPath path = topo.find_best_path(src_node, dst_node);
    
    // Check if path was found
    if (!path.is_valid()) {
        std::cerr << "Error: No path found from " << src_node 
                  << " to " << dst_node << "\n";
        return 1;
    }
    
    // Output path to stdout (space-separated node IDs)
    for (size_t i = 0; i < path.nodes.size(); ++i) {
        std::cout << path.nodes[i];
        if (i < path.nodes.size() - 1) {
            std::cout << " ";
        }
    }
    std::cout << "\n";
    
    // Debug info to stderr
    std::cerr << "[router] Path cost: " << path.total_cost 
              << ", hops: " << path.hop_count() << "\n";
    
    return 0;
}
