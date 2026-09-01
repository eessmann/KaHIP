include_guard(GLOBAL)

function(kahip_append_consumer_cache_argument command_variable name value)
    string(REPLACE ";" "\\;" escaped_value "${value}")
    set(command "${${command_variable}}")
    list(APPEND command "-D${name}=${escaped_value}")
    set(${command_variable} "${command}" PARENT_SCOPE)
endfunction()
