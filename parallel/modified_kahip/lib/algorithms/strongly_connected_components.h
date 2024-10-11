/******************************************************************************
 * strongly_connected_components.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef STRONGLY_CONNECTED_COMPONENTS_7ZJ8233R
#define STRONGLY_CONNECTED_COMPONENTS_7ZJ8233R

#include <stack>
#include <vector>

#include "data_structure/graph_access.h"
#include "definitions.h"
namespace kahip::modified {
class strongly_connected_components {
public:

  int strong_components( graph_access & G, std::vector<int> & comp_num);

  void scc_dfs(NodeID node, graph_access & G,
               std::vector<int>   & dfsnum,
               std::vector<int>   & comp_num,
               std::stack<NodeID> & unfinished,
               std::stack<NodeID> & roots);
private:
  int m_dfscount = 0;
  int m_comp_count = 0;
};
}

#endif /* end of include guard: STRONGLY_CONNECTED_COMPONENTS_7ZJ8233R */
