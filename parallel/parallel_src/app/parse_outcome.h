#ifndef KAHIP_PARALLEL_PARSE_OUTCOME_H
#define KAHIP_PARALLEL_PARSE_OUTCOME_H

namespace parhip {

enum class parse_outcome {
  continue_execution,
  early_success,
  invalid_arguments,
};

}  // namespace parhip

#endif  // KAHIP_PARALLEL_PARSE_OUTCOME_H
