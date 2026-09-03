/*
* Source of KaHIP -- Karlsruhe High Quality Partitioning.
* Author: Daniel Seemaier <daniel.seemaier@student.kit.edu>
* Christian Schulz <christian.schulz.phone@gmail.com>
* */

#ifndef KAHIP_PARSE_DSPAC_PARAMETERS_H
#define KAHIP_PARSE_DSPAC_PARAMETERS_H

#ifndef _WIN32
#include <regex.h>
#endif
#include <string.h>
#ifdef _WIN32
#ifndef REG_EXTENDED
#define REG_EXTENDED 1
#endif
#endif
#include "configuration.h"
#include "parse_outcome.h"
namespace parhip {
struct DspacConfig {
    EdgeWeight infinity;
    PartitionID k;
};

parse_outcome parse_dspac_parameters(int argn, char **argv, PPartitionConfig &partition_config, DspacConfig &dspac_config,
                           std::string &graph_filename, std::string &out_partition_filename,
                           mpi::communicator_view communicator) {
    const char *progname = argv[0];
    auto const rank = communicator.rank();

    struct arg_lit *help = arg_lit0(NULL, "help", "Print help.");
    struct arg_str *filename = arg_str1(NULL, NULL, "FILE", "Path to graph file to partition.");
    struct arg_int *k = arg_int1(NULL, "k", NULL, "Number of blocks to partition the graph.");
    struct arg_int *seed = arg_int0(NULL, "seed", NULL, "Seed to use for PRNG.");
    struct arg_int *infinity = arg_int0(NULL, "infinity", NULL, "Infinity edge weight. Default: 1000000");
    struct arg_rex *preconfiguration = arg_rex1(NULL, "preconfiguration", "^(ecosocial|fastsocial|ultrafastsocial|ecomesh|fastmesh|ultrafastmesh)$", "VARIANT", REG_EXTENDED, "Use a preconfiguration. (Default: fast) [ecosocial|fastsocial|ultrafastsocial|ecomesh|fastmesh|ultrafastmesh]." );
    struct arg_lit *save_partition		       = arg_lit0(NULL, "save_partition","Enable this tag if you want to store the partition to disk.");
    struct arg_lit *save_partition_binary	       = arg_lit0(NULL, "save_partition_binary","Enable this tag if you want to store the partition to disk in a binary format.");
    struct arg_str *partition_filename = arg_str0(NULL, "output_filename", "FILE", "Path to where we store the computed partition if --save_partition is set.");
    struct arg_int *imbalance = arg_int0(NULL, "imbalance", NULL, "Desired imbalance. Default: 3%");
    struct arg_end *end = arg_end(100);

    // Define argtable.
    void *argtable[] = {
        help, filename, k, seed, infinity, preconfiguration, imbalance, partition_filename, save_partition, save_partition_binary, end

};

    // Parse arguments.
    int nerrors = arg_parse(argn, argv, argtable);

    // Catch case that help was requested.
    if(help->count > 0) {
        if( rank == ROOT ) {
            printf("Usage: %s", progname);
            arg_print_syntax(stdout, argtable, "\n");
            arg_print_glossary(stdout, argtable,"  %-40s %s\n");
            printf("This is the experimental parallel SPAC program.\n");
        }
        arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return parse_outcome::early_success;
    }

    if (nerrors > 0) {
        if( rank == ROOT ) {
            arg_print_errors(stderr, end, progname);
            printf("Try '%s --help' for more information.\n",progname);
        }
        arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return parse_outcome::invalid_arguments;
    }

    configuration cfg;
    cfg.standard(partition_config);

    if (k->count > 0) {
        if (k->ival[0] <= 0) {
            if (rank == ROOT) {
                fprintf(stderr, "Number of blocks must be positive.\n");
            }
            arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
            return parse_outcome::invalid_arguments;
        }
        partition_config.k = k->ival[0];
        dspac_config.k = k->ival[0];
    }

    if (filename->count > 0) {
        graph_filename = filename->sval[0];
    }

    if (seed->count > 0) {
        partition_config.seed = seed->ival[0];
    } else {
        partition_config.seed = 0;
    }

    if (partition_filename->count > 0) {
        out_partition_filename = partition_filename->sval[0];
    }


    if(preconfiguration->count > 0) {
        if (strcmp("ecosocial", preconfiguration->sval[0]) == 0) {
            cfg.eco(partition_config, communicator);
        } else if (strcmp("fastsocial", preconfiguration->sval[0]) == 0) {
            cfg.fast(partition_config);
        } else if (strcmp("ultrafastsocial", preconfiguration->sval[0]) == 0) {
            cfg.ultrafast(partition_config);
        } else if (strcmp("ecomesh", preconfiguration->sval[0]) == 0) {
            cfg.eco(partition_config, communicator);
            partition_config.cluster_coarsening_factor = 20000;
        } else if (strcmp("fastmesh", preconfiguration->sval[0]) == 0) {
            cfg.fast(partition_config);
            partition_config.cluster_coarsening_factor = 20000;
        } else if (strcmp("ultrafastmesh", preconfiguration->sval[0]) == 0) {
            cfg.ultrafast(partition_config);
            partition_config.cluster_coarsening_factor = 20000;
        } else {
            if (rank == ROOT) {
                fprintf(stderr, "Invalid preconfconfiguration variant: \"%s\"\n", preconfiguration->sval[0]);
            }
            arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
            return parse_outcome::invalid_arguments;
        }
    }

    if(save_partition->count > 0) {
        partition_config.save_partition = true;
    }

    if(save_partition_binary->count > 0) {
        partition_config.save_partition_binary = true;
    }

    if (imbalance->count > 0) {
        partition_config.epsilon = imbalance->ival[0];
        partition_config.inbalance = imbalance->ival[0];
    }

    if (infinity->count > 0) {
        if (infinity->ival[0] <= 0) {
            if (rank == ROOT) {
                fprintf(stderr, "Infinity edge weight must be positive.\n");
            }
            arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
            return parse_outcome::invalid_arguments;
        }
        dspac_config.infinity = infinity->ival[0];
    } else {
        dspac_config.infinity = 1000000;
    }

    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return parse_outcome::continue_execution;
}
}
#endif // KAHIP_PARSE_DSPAC_PARAMETERS_H
