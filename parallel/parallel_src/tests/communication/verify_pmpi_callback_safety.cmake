cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED SOURCE_FILE OR SOURCE_FILE STREQUAL "")
    message(FATAL_ERROR "SOURCE_FILE is required")
endif()
if(NOT EXISTS "${SOURCE_FILE}")
    message(FATAL_ERROR "PMPI callback source does not exist: ${SOURCE_FILE}")
endif()
if(NOT DEFINED PROFILE OR PROFILE STREQUAL "")
    set(PROFILE projection)
endif()

file(READ "${SOURCE_FILE}" callback_source)
set(begin_marker "// KAHIP_PMPI_CALLBACK_REGION_BEGIN")
set(end_marker "// KAHIP_PMPI_CALLBACK_REGION_END")

string(REGEX MATCHALL "${begin_marker}" begin_matches "${callback_source}")
string(REGEX MATCHALL "${end_marker}" end_matches "${callback_source}")
list(LENGTH begin_matches begin_count)
list(LENGTH end_matches end_count)
if(NOT begin_count EQUAL 1 OR NOT end_count EQUAL 1)
    message(
        FATAL_ERROR
        "${PROFILE} callback audit requires exactly one ordered marker pair; found begin=${begin_count}, end=${end_count}"
    )
endif()

string(FIND "${callback_source}" "${begin_marker}" begin_index)
string(FIND "${callback_source}" "${end_marker}" end_index)
if(begin_index GREATER_EQUAL end_index)
    message(FATAL_ERROR "${PROFILE} callback audit markers are reversed")
endif()
string(LENGTH "${begin_marker}" begin_marker_length)
math(EXPR content_begin "${begin_index} + ${begin_marker_length}")
math(EXPR content_length "${end_index} - ${content_begin}")
string(
    SUBSTRING "${callback_source}" ${content_begin} ${content_length}
    callback_region
)

set(audit_errors "")
function(reject_callback_pattern label pattern)
    string(REGEX MATCHALL "${pattern}" matches "${callback_region}")
    if(matches)
        list(LENGTH matches match_count)
        string(APPEND audit_errors "\n- ${label}: ${match_count} match(es)")
        set(audit_errors "${audit_errors}" PARENT_SCOPE)
    endif()
endfunction()

reject_callback_pattern(
    "Catch assertion macro reachable from an extern-C MPI wrapper"
    [=[(^|[^A-Za-z0-9_])((STATIC_)?(REQUIRE|CHECK)|(UNSCOPED_)?INFO|FAIL|CAPTURE|WARN|SUCCEED|DYNAMIC_SECTION|SECTION)[A-Z0-9_]*[ \t\r\n]*\(]=]
)
reject_callback_pattern(
    "C++ throw expression reachable from an extern-C MPI wrapper"
    [=[(^|[^A-Za-z0-9_])throw([^A-Za-z0-9_]|$)]=]
)
reject_callback_pattern(
    "dynamic standard container, callable, or string in callback state/helper code"
    [=[std::(vector|deque|list|map|set|unordered_map|unordered_set|function)[ \t\r\n]*<|std::(basic_string|string)([^A-Za-z0-9_]|$)]=]
)
reject_callback_pattern(
    "allocation-capable operation in callback state/helper code"
    [=[(^|[^A-Za-z0-9_])(new|malloc|calloc|realloc|aligned_alloc)([^A-Za-z0-9_]|$)|std::(allocator|allocator_traits)[ \t\r\n]*<|[.:](allocate|allocate_at_least|push_back|emplace_back|reserve|resize|insert|assign|append|replace)[ \t\r\n]*\(]=]
)
reject_callback_pattern(
    "smart-pointer allocation in callback state/helper code"
    [=[std::make_unique[ \t\r\n]*<|std::make_shared[ \t\r\n]*<|std::allocate_shared[ \t\r\n]*<]=]
)
reject_callback_pattern(
    "allocation-capable formatting or logging in callback code"
    [=[(fmt|spdlog)::|std::(format|vformat|to_string|print|println)[ \t\r\n]*\(|std::((basic_)?(ostringstream|stringstream|istringstream|fstream|ofstream|ifstream)|ostream|iostream|cout|cerr|clog)([^A-Za-z0-9_]|$)]=]
)

if(PROFILE STREQUAL "projection")
    set(
        required_noexcept_patterns
        [=[auto[ \t\r\n]+dense_payload_collective_calls\(\)[ \t\r\n]+noexcept[ \t\r\n]+->[ \t\r\n]+int]=]
        [=[void[ \t\r\n]+mutate_received_payload\([^)]*\)[ \t\r\n]+noexcept]=]
    )
    set(
        required_compile_time_check_patterns
        [=[static_assert *\( *noexcept *\( *protocol_probe::dense_payload_collective_calls *\( *\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *protocol_probe::mutate_received_payload<int, *int> *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *protocol_probe::mutate_received_payload<MPI_Count, *MPI_Aint> *\([^)]*\) *\) *\) *[;]]=]
    )
elseif(PROFILE STREQUAL "ghost-label")
    set(
        required_noexcept_patterns
        [=[void[ \t\r\n]+corrupt_neighbor_payload\([^)]*\)[ \t\r\n]+noexcept]=]
        [=[void[ \t\r\n]+corrupt_legacy_payload\([^)]*\)[ \t\r\n]+noexcept]=]
        [=[void[ \t\r\n]+observe_immediate_payload\([^)]*\)[ \t\r\n]+noexcept]=]
    )
    set(
        required_compile_time_check_patterns
        [=[static_assert *\( *noexcept *\( *ghost_label_probe::corrupt_neighbor_payload<int, *int> *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *ghost_label_probe::corrupt_neighbor_payload<MPI_Count, *MPI_Aint> *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *ghost_label_probe::corrupt_legacy_payload *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *ghost_label_probe::observe_immediate_payload<int> *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *ghost_label_probe::observe_immediate_payload<MPI_Count> *\([^)]*\) *\) *\) *[;]]=]
    )
elseif(PROFILE STREQUAL "ghost-label-failure")
    set(
        required_noexcept_patterns
        [=[void[ \t\r\n]+track_incremental_payload\([^)]*\)[ \t\r\n]+noexcept]=]
        [=[void[ \t\r\n]+corrupt_completed_incremental_payload\(\)[ \t\r\n]+noexcept]=]
        [=[auto[ \t\r\n]+expected_abort_state\(\)[ \t\r\n]+noexcept[ \t\r\n]+->[ \t\r\n]+bool]=]
    )
    set(
        required_compile_time_check_patterns
        [=[static_assert *\( *noexcept *\( *ghost_failure_probe::track_incremental_payload<int, *int> *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *ghost_failure_probe::track_incremental_payload<MPI_Count, *MPI_Aint> *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *ghost_failure_probe::corrupt_completed_incremental_payload *\( *\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *ghost_failure_probe::expected_abort_state *\( *\) *\) *\) *[;]]=]
    )
elseif(PROFILE STREQUAL "population-size")
    set(
        required_noexcept_patterns
        [=[void[ \t\r\n]+reset\([^)]*\)[ \t\r\n]+noexcept]=]
        [=[void[ \t\r\n]+observe_broadcast\([^)]*\)[ \t\r\n]+noexcept]=]
    )
    set(
        required_compile_time_check_patterns
        [=[static_assert *\( *noexcept *\( *population_size_probe::reset *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *population_size_probe::observe_broadcast *\([^)]*\) *\) *\) *[;]]=]
    )
elseif(PROFILE STREQUAL "population-size-failure")
    set(
        required_noexcept_patterns
        [=[void[ \t\r\n]+write_text\([^)]*\)[ \t\r\n]+noexcept]=]
        [=[void[ \t\r\n]+observed_abort\([^)]*\)[ \t\r\n]+noexcept]=]
    )
    set(
        required_compile_time_check_patterns
        [=[static_assert *\( *noexcept *\( *population_failure_probe::write_text *\([^)]*\) *\) *\) *[;]]=]
        [=[static_assert *\( *noexcept *\( *population_failure_probe::observed_abort *\([^)]*\) *\) *\) *[;]]=]
    )
else()
    message(FATAL_ERROR "unknown PMPI callback audit PROFILE: ${PROFILE}")
endif()
foreach(required_pattern IN LISTS required_noexcept_patterns)
    string(REGEX MATCH "${required_pattern}" required_match "${callback_region}")
    if(NOT required_match)
        string(
            APPEND audit_errors
            "\n- callback helper is missing its required noexcept contract: ${required_pattern}"
        )
    endif()
endforeach()

string(REGEX REPLACE "[ \t\r\n]+" " " normalized_callback_region "${callback_region}")
foreach(required_check_pattern IN LISTS required_compile_time_check_patterns)
    string(
        REGEX MATCH "${required_check_pattern}" required_check_match
        "${normalized_callback_region}"
    )
    if(NOT required_check_match)
        string(
            APPEND audit_errors
            "\n- missing compile-time callback noexcept proof: ${required_check_pattern}"
        )
    endif()
endforeach()

if(NOT audit_errors STREQUAL "")
    message(
        FATAL_ERROR
        "${PROFILE} PMPI callback-safety audit failed:${audit_errors}"
    )
endif()

message(STATUS "${PROFILE} PMPI callback-safety audit passed")
