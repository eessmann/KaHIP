#include "parhip_interface.h"

#include <mpi.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <ranges>
#include <span>
#include <streambuf>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/mpi_failure.h"
#include "communication/mpi_fixed_reduction.h"
#include "communication/mpi_handles.h"
#include "configuration.h"
#include "distributed_partitioning/distributed_partitioner.h"
#include "parallel_graph_io.h"
#include "random_functions.h"
#include "tools/distributed_quality_metrics.h"

namespace {
using parhip::EdgeID;
using parhip::EdgeWeight;
using parhip::NodeID;
using parhip::NodeWeight;
using parhip::PartitionID;
using parhip::PEID;
using parhip::mpi::communicator_view;
inline constexpr auto pristine_communication_rounds = parhip::ULONG{128};

void require_collectively(
    bool local_condition,
    communicator_view communicator,
    std::string_view diagnostic,
    std::string_view collective_context =
        "MPI_Allreduce(ParHIP input validation)") noexcept {
  auto const local = local_condition ? 1 : 0;
  auto global = 0;
  parhip::mpi::check_or_abort(
      MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN,
                    communicator.native_handle()),
      communicator.native_handle(), collective_context);
  if (global == 0) {
    parhip::mpi::abort_on_programming_error(communicator.native_handle(),
                                            diagnostic);
  }
}

void require_capacity_collectively(bool local_condition,
                                   communicator_view communicator,
                                   std::string_view diagnostic) noexcept {
  auto const local = local_condition ? 1 : 0;
  auto global = 0;
  parhip::mpi::check_or_abort(
      MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN,
                    communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allreduce(ParHIP capacity validation)");
  if (global == 0) {
    parhip::mpi::abort_on_capacity_failure(communicator.native_handle(),
                                           "ParHIPPartitionKWay", diagnostic);
  }
}

template <typename T>
[[nodiscard]] auto agree_integral(T local_value,
                                  communicator_view communicator,
                                  std::string_view diagnostic) noexcept -> T {
  static_assert(parhip::mpi::mpi_integral_reduction_datatype<T>);
  auto minimum = T{};
  auto maximum = T{};
  auto const local = std::array<T, 1>{local_value};
  auto minimum_span = std::span<T>{&minimum, 1};
  auto maximum_span = std::span<T>{&maximum, 1};
  parhip::mpi::all_reduce_bounded(std::span<T const>{local}, minimum_span,
                                  parhip::mpi::reduction_kind::minimum,
                                  communicator,
                                  "MPI_Allreduce(ParHIP common input minimum)");
  parhip::mpi::all_reduce_bounded(std::span<T const>{local}, maximum_span,
                                  parhip::mpi::reduction_kind::maximum,
                                  communicator,
                                  "MPI_Allreduce(ParHIP common input maximum)");
  if (minimum != maximum) {
    parhip::mpi::abort_on_programming_error(communicator.native_handle(),
                                            diagnostic);
  }
  return minimum;
}

[[nodiscard]] auto agree_double(double local_value,
                                communicator_view communicator,
                                std::string_view diagnostic) noexcept
    -> double {
  auto const canonical = local_value == 0.0 ? 0.0 : local_value;
  auto const bits = std::bit_cast<std::uint64_t>(canonical);
  static_cast<void>(agree_integral(bits, communicator, diagnostic));
  return canonical;
}

[[nodiscard]] auto validated_distribution(idxtype const* distribution,
                                          PEID size,
                                          communicator_view communicator)
    -> std::vector<NodeID> {
  require_capacity_collectively(
      size < std::numeric_limits<int>::max(), communicator,
      "communicator size cannot be represented as a distribution extent");
  auto local = std::vector<NodeID>(distribution, distribution + size + 1);
  auto minimum = std::vector<NodeID>(local.size());
  auto maximum = std::vector<NodeID>(local.size());
  parhip::mpi::all_reduce_bounded(
      std::span<NodeID const>{local}, std::span<NodeID>{minimum},
      parhip::mpi::reduction_kind::minimum, communicator,
      "MPI_Allreduce(ParHIP vertex distribution minimum)");
  parhip::mpi::all_reduce_bounded(
      std::span<NodeID const>{local}, std::span<NodeID>{maximum},
      parhip::mpi::reduction_kind::maximum, communicator,
      "MPI_Allreduce(ParHIP vertex distribution maximum)");
  require_collectively(
      minimum == maximum, communicator,
      "ParHIP vertex distribution differs across communicator");
  require_collectively(
      local.front() == 0 && std::ranges::is_sorted(local), communicator,
      "ParHIP vertex distribution must start at zero and be monotone");
  return local;
}

[[nodiscard]] auto checked_collective_sum(NodeWeight local_value,
                                          communicator_view communicator,
                                          std::string_view diagnostic)
    -> NodeWeight {
  auto const local = std::array<NodeWeight, 1>{local_value};
  auto global = std::array<NodeWeight, 1>{};
  parhip::mpi::all_reduce_checked_sum(
      std::span<NodeWeight const>{local}, std::span<NodeWeight>{global},
      communicator, "MPI_Allreduce(ParHIP checked scalar sum)",
      "ParHIPPartitionKWay", diagnostic);
  return global.front();
}

class null_streambuf final : public std::streambuf {
 protected:
  auto overflow(traits_type::int_type character)
      -> traits_type::int_type override {
    return traits_type::not_eof(character);
  }
};

class scoped_output_suppression final {
 public:
  explicit scoped_output_suppression(bool suppress)
      : backup_(std::cout.rdbuf()) {
    if (suppress) {
      std::cout.rdbuf(&sink_);
    }
  }

  ~scoped_output_suppression() { std::cout.rdbuf(backup_); }

  scoped_output_suppression(scoped_output_suppression const&) = delete;
  auto operator=(scoped_output_suppression const&)
      -> scoped_output_suppression& = delete;

 private:
  std::streambuf* backup_;
  null_streambuf sink_;
};

[[nodiscard]] auto exact_upper_bound(NodeWeight global_weight,
                                     PartitionID block_count,
                                     unsigned imbalance_percent,
                                     communicator_view communicator)
    -> NodeWeight {
  auto const quotient = global_weight / block_count;
  auto const remainder = global_weight % block_count;
  require_capacity_collectively(
      remainder == 0 || quotient < std::numeric_limits<NodeWeight>::max(),
      communicator, "partition upper bound exceeds the graph-weight domain");
  auto const ceiling = quotient + (remainder == 0 ? 0 : 1);
  auto const imbalance = static_cast<NodeWeight>(imbalance_percent);
  auto const whole_hundreds = ceiling / 100;
  require_capacity_collectively(
      whole_hundreds == 0 ||
          imbalance <= std::numeric_limits<NodeWeight>::max() / whole_hundreds,
      communicator, "partition upper bound exceeds the graph-weight domain");
  auto const whole_extra = whole_hundreds * imbalance;
  auto const remaining_hundredths = ceiling % 100;
  require_capacity_collectively(
      remaining_hundredths == 0 ||
          imbalance <=
              std::numeric_limits<NodeWeight>::max() / remaining_hundredths,
      communicator, "partition upper bound exceeds the graph-weight domain");
  auto const fractional_extra = remaining_hundredths * imbalance / 100;
  require_capacity_collectively(
      fractional_extra <=
              std::numeric_limits<NodeWeight>::max() - whole_extra &&
          whole_extra + fractional_extra <=
              std::numeric_limits<NodeWeight>::max() - ceiling,
      communicator, "partition upper bound exceeds the graph-weight domain");
  return ceiling + whole_extra + fractional_extra;
}

void require_valid_partition(parhip::parallel_graph_access& graph,
                             std::span<NodeWeight const> vertex_weights,
                             parhip::PPartitionConfig const& config,
                             communicator_view communicator) {
  auto local_block_weights =
      std::vector<NodeWeight>(static_cast<std::size_t>(config.k), 0);
  auto local_valid = true;
  for (NodeID node = 0; node < graph.number_of_local_nodes(); ++node) {
    auto const block = graph.getNodeLabel(node);
    if (block >= config.k) {
      local_valid = false;
      continue;
    }
    auto& block_weight = local_block_weights[static_cast<std::size_t>(block)];
    auto const node_weight = vertex_weights[static_cast<std::size_t>(node)];
    if (node_weight > std::numeric_limits<NodeWeight>::max() - block_weight) {
      parhip::mpi::abort_on_capacity_failure(
          communicator.native_handle(), "ParHIPPartitionKWay",
          "local partition block weight exceeds the graph-weight domain");
    }
    block_weight += node_weight;
  }
  require_collectively(
      local_valid, communicator,
      "ParHIP produced a partition label outside the block domain");

  auto global_block_weights =
      std::vector<NodeWeight>(local_block_weights.size());
  parhip::mpi::all_reduce_checked_sum(
      std::span<NodeWeight const>{local_block_weights},
      std::span<NodeWeight>{global_block_weights}, communicator,
      "MPI_Allreduce(ParHIP partition block weights)", "ParHIPPartitionKWay",
      "global partition block-weight sum exceeds the graph-weight domain");
  require_collectively(
      std::ranges::all_of(global_block_weights,
                          [&](NodeWeight weight) {
                            return weight <= config.upper_bound_partition;
                          }),
      communicator,
      "ParHIP produced a partition that exceeds the configured block-weight "
      "upper bound");
}

// 3% imbalance is specified as imbalance = 0.03.
void parhip_partition_kway(idxtype const* vtxdist,
                           idxtype const* xadj,
                           idxtype const* adjncy,
                           idxtype const* vwgt,
                           idxtype const* adjwgt,
                           int const* nparts,
                           double const* imbalance,
                           bool suppress_output,
                           int seed,
                           int mode,
                           int* edgecut,
                           idxtype* part,
                           communicator_view communicator) {
  using namespace parhip;

  auto const rank = communicator.rank();
  auto const size = communicator.size();

  require_collectively(
      vtxdist != nullptr && xadj != nullptr && nparts != nullptr &&
          imbalance != nullptr && edgecut != nullptr,
      communicator, "ParHIP required input pointers are invalid");

  auto const block_count = agree_integral(
      *nparts, communicator, "ParHIP block count differs across communicator");
  require_collectively(block_count > 0, communicator,
                       "ParHIP block count must be greater than zero");
  require_collectively(std::isfinite(*imbalance) && *imbalance >= 0.0,
                       communicator,
                       "ParHIP imbalance must be finite and nonnegative");
  auto const common_imbalance = agree_double(
      *imbalance, communicator, "ParHIP imbalance differs across communicator");
  static_cast<void>(agree_integral(seed, communicator,
                                   "ParHIP seed differs across communicator"));
  auto const common_mode = agree_integral(
      mode, communicator, "ParHIP mode differs across communicator");
  constexpr auto supported_modes = std::array{
      ULTRAFASTMESH, FASTMESH, ECOMESH, ULTRAFASTSOCIAL, FASTSOCIAL, ECOSOCIAL};
  require_collectively(
      std::ranges::find(supported_modes, common_mode) != supported_modes.end(),
      communicator, "ParHIP mode is outside the supported domain");
  static_cast<void>(
      agree_integral(suppress_output ? 1 : 0, communicator,
                     "ParHIP output suppression differs across communicator"));
  static_cast<void>(agree_integral(
      vwgt == nullptr ? 0 : 1, communicator,
      "ParHIP optional vertex-weight presence differs across communicator"));
  static_cast<void>(agree_integral(
      adjwgt == nullptr ? 0 : 1, communicator,
      "ParHIP optional edge-weight presence differs across communicator"));

  auto vertex_dist = validated_distribution(vtxdist, size, communicator);
  auto const first = vertex_dist[static_cast<std::size_t>(rank)];
  auto const next = vertex_dist[static_cast<std::size_t>(rank) + 1];
  auto const local_number_of_nodes = next - first;
  require_capacity_collectively(
      std::in_range<std::size_t>(local_number_of_nodes), communicator,
      "local vertex count exceeds addressable storage");
  auto const local_node_count = static_cast<std::size_t>(local_number_of_nodes);

  auto const offsets_valid =
      xadj[0] == 0 && std::ranges::is_sorted(
                          std::span<idxtype const>{xadj, local_node_count + 1});
  require_collectively(offsets_valid, communicator,
                       "ParHIP local CSR offsets are invalid");
  auto const local_number_of_edges = xadj[local_node_count];
  require_capacity_collectively(
      std::in_range<std::size_t>(local_number_of_edges), communicator,
      "local edge count exceeds addressable storage");
  auto const local_edge_count = static_cast<std::size_t>(local_number_of_edges);
  require_collectively(
      local_edge_count == 0 || adjncy != nullptr, communicator,
      "ParHIP adjacency pointer is missing for nonempty edge storage");
  require_collectively(
      local_node_count == 0 || part != nullptr, communicator,
      "ParHIP partition output pointer is missing for nonempty local storage");

  auto const global_number_of_nodes = vertex_dist.back();
  auto const neighbors = local_edge_count == 0 ? std::span<idxtype const>{}
                                               : std::span<idxtype const>{
                                                     adjncy, local_edge_count};
  require_collectively(
      std::ranges::all_of(
          neighbors,
          [&](idxtype neighbor) {
            auto const is_nonnegative = []<typename T>(T value) {
              if constexpr (std::is_signed_v<T>) {
                return value >= 0;
              }
              return true;
            }(neighbor);
            return is_nonnegative && neighbor < global_number_of_nodes;
          }),
      communicator,
      "ParHIP adjacency contains a vertex outside the global domain");

  auto vertex_weights = std::vector<NodeWeight>(local_node_count, 1);
  auto local_overall_node_weight = NodeWeight{local_number_of_nodes};
  if (vwgt != nullptr) {
    local_overall_node_weight = 0;
    auto local_weight_valid = true;
    for (auto index = std::size_t{0}; index < local_node_count; ++index) {
      vertex_weights[index] = vwgt[index];
      if (vwgt[index] >
          std::numeric_limits<NodeWeight>::max() - local_overall_node_weight) {
        local_weight_valid = false;
      } else {
        local_overall_node_weight += vwgt[index];
      }
    }
    require_capacity_collectively(
        local_weight_valid, communicator,
        "local vertex-weight sum exceeds the graph-weight domain");
  }
  auto const global_node_weight = checked_collective_sum(
      local_overall_node_weight, communicator,
      "global vertex-weight sum exceeds the graph-weight domain");
  auto const global_number_of_edges =
      checked_collective_sum(local_number_of_edges, communicator,
                             "global edge count exceeds the graph-size domain");

  auto suppression = scoped_output_suppression{suppress_output};
  // start_construction consumes this process-global tuning value before the
  // per-configuration value is installed below. Restore the pristine upstream
  // entry state so a previous C API call cannot change this call's semantics.
  parallel_graph_access::set_comm_rounds(pristine_communication_rounds);
  parallel_graph_access graph{communicator.native_handle()};
  graph.start_construction(local_number_of_nodes, local_number_of_edges,
                           global_number_of_nodes, global_number_of_edges);
  graph.set_range(first, local_number_of_nodes == 0 ? first : next - 1);
  graph.set_range_array(vertex_dist);
  for (NodeID local_node = 0; local_node < local_number_of_nodes;
       ++local_node) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node,
                        vertex_weights[static_cast<std::size_t>(local_node)]);
    graph.setNodeLabel(node, first + node);
    graph.setSecondPartitionIndex(node, 0);
    for (EdgeID edge_index = xadj[local_node];
         edge_index < xadj[local_node + 1]; ++edge_index) {
      auto const edge = graph.new_edge(node, adjncy[edge_index]);
      graph.setEdgeWeight(edge, adjwgt == nullptr ? 1 : adjwgt[edge_index]);
    }
  }
  graph.finish_construction();

  PPartitionConfig partition_config;
  configuration config;
  config.standard(partition_config);
  switch (common_mode) {
    case FASTMESH:
      config.fast(partition_config);
      partition_config.cluster_coarsening_factor = 20000;
      break;
    case ULTRAFASTMESH:
      config.ultrafast(partition_config);
      partition_config.cluster_coarsening_factor = 20000;
      break;
    case ECOMESH:
      config.eco(partition_config, communicator);
      partition_config.cluster_coarsening_factor = 20000;
      break;
    case FASTSOCIAL:
      config.fast(partition_config);
      break;
    case ECOSOCIAL:
      config.eco(partition_config, communicator);
      break;
    case ULTRAFASTSOCIAL:
      config.ultrafast(partition_config);
      break;
    default:
      parhip::mpi::abort_on_programming_error(
          communicator.native_handle(),
          "ParHIP mode escaped collective domain validation");
  }

  partition_config.k = static_cast<PartitionID>(block_count);
  partition_config.seed = seed;
  partition_config.stop_factor /= block_count;
  auto const derived_seed = rank == 0
                                ? static_cast<std::int64_t>(seed)
                                : static_cast<std::int64_t>(seed) * size + rank;
  require_capacity_collectively(
      std::in_range<int>(derived_seed), communicator,
      "rank-derived random seed exceeds the int domain");
  partition_config.seed = static_cast<int>(derived_seed);

  std::srand(partition_config.seed);
  random_functions::setSeed(partition_config.seed);
  parallel_graph_access::set_comm_rounds(partition_config.comm_rounds / size);
  parallel_graph_access::set_comm_rounds_up(partition_config.comm_rounds /
                                            size);
  distributed_partitioner::generate_random_choices(partition_config,
                                                    communicator);

  require_capacity_collectively(
      common_imbalance <=
          static_cast<double>(std::numeric_limits<unsigned>::max()) / 100.0,
      communicator, "imbalance percentage exceeds the unsigned int domain");
  partition_config.inbalance = static_cast<unsigned>(100.0 * common_imbalance);
  partition_config.number_of_overall_nodes = graph.number_of_global_nodes();
  partition_config.upper_bound_partition =
      exact_upper_bound(global_node_weight, partition_config.k,
                        partition_config.inbalance, communicator);

  timer runtime;
  distributed_partitioner partitioner;
  partitioner.perform_partitioning(communicator.native_handle(),
                                   partition_config, graph);
  parhip::mpi::check_or_abort(MPI_Barrier(communicator.native_handle()),
                              communicator.native_handle(),
                              "MPI_Barrier(ParHIP partition completion)");
  auto const running_time = runtime.elapsed();

  require_valid_partition(graph, vertex_weights, partition_config,
                          communicator);
  distributed_quality_metrics metrics;
  auto const global_edge_cut =
      metrics.edge_cut(graph, communicator.native_handle());
  require_capacity_collectively(
      std::in_range<int>(global_edge_cut), communicator,
      "global edge cut exceeds the C interface int domain");

  *edgecut = static_cast<int>(global_edge_cut);
  for (NodeID local_node = 0; local_node < local_number_of_nodes;
       ++local_node) {
    part[local_node] = graph.getNodeLabel(local_node);
  }

  if (!suppress_output) {
    auto const balance =
        metrics.balance(partition_config, graph, communicator.native_handle());
    if (rank == 0) {
      std::cout << "log>=====================================\n"
                << "log>============AND WE R DONE============\n"
                << "log>=====================================\n"
                << "log>total partitioning time elapsed " << running_time
                << '\n'
                << "log>final edge cut " << *edgecut << '\n'
                << "log>final balance " << balance << std::endl;
    }
  }
}
}  // namespace

extern "C" void ParHIPPartitionKWay(idxtype* vtxdist,
                                    idxtype* xadj,
                                    idxtype* adjncy,
                                    idxtype* vwgt,
                                    idxtype* adjwgt,
                                    int* nparts,
                                    double* imbalance,
                                    bool suppress_output,
                                    int seed,
                                    int mode,
                                    int* edgecut,
                                    idxtype* part,
                                    MPI_Comm* comm) noexcept {
  using parhip::mpi::abort_on_exception;
  using parhip::mpi::abort_on_programming_error;
  using parhip::mpi::communicator;
  using parhip::mpi::communicator_view;
  using parhip::mpi::run_with_exception_barrier;

  if (comm == nullptr) {
    abort_on_programming_error(MPI_COMM_WORLD,
                               "ParHIP communicator pointer is null");
  }
  auto const caller = *comm;
  run_with_exception_barrier(
      [&] {
        auto owned = communicator{communicator_view{caller}};
        auto const affected = owned.view();
        run_with_exception_barrier(
            [&] {
              parhip_partition_kway(vtxdist, xadj, adjncy, vwgt, adjwgt, nparts,
                                    imbalance, suppress_output, seed, mode,
                                    edgecut, part, affected);
            },
            [affected](std::exception_ptr failure) noexcept {
              abort_on_exception(affected.native_handle(),
                                 "ParHIPPartitionKWay", failure);
            });
      },
      [caller](std::exception_ptr failure) noexcept {
        abort_on_exception(
            caller, "ParHIPPartitionKWay communicator acquisition", failure);
      });
}
