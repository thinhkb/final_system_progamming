#include "config.h"
#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static server_t *active_server;

static void handle_signal(int signum) {
    (void)signum;
    if (active_server != NULL) {
        server_stop(active_server);
    }
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [-h host] [-p port] [-r doc_root] [-t threads] [-q queue] [-l access_log]\n", program);
}

static int parse_positive_int(const char *text, int *value) {
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (text == NULL || *text == '\0' || end == NULL || *end != '\0' || parsed <= 0 || parsed > 65535) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

int main(int argc, char **argv) {
    server_config_t config = {
        .host = DEFAULT_HOST,
        .port = DEFAULT_PORT,
        .thread_count = DEFAULT_THREAD_COUNT,
        .queue_capacity = DEFAULT_QUEUE_CAPACITY,
        .doc_root = DEFAULT_DOC_ROOT,
        .access_log = DEFAULT_ACCESS_LOG
    };
    server_t server;
    int option;
    int exit_code = 1;

    while ((option = getopt(argc, argv, "h:p:r:t:q:l:")) != -1) {
        switch (option) {
            case 'h':
                config.host = optarg;
                break;
            case 'p':
                if (parse_positive_int(optarg, &config.port) != 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'r':
                config.doc_root = optarg;
                break;
            case 't':
                if (parse_positive_int(optarg, &config.thread_count) != 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'q':
                if (parse_positive_int(optarg, &config.queue_capacity) != 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'l':
                config.access_log = optarg;
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (server_init(&server, &config) != 0) {
        fprintf(stderr, "failed to initialize server\n");
        return 1;
    }

    active_server = &server;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    exit_code = server_run(&server);
    server_destroy(&server);
    return exit_code;
}
