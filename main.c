#include "proxy.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* ============================================================================
 * USAGE AND HELP
 * ============================================================================
 */

static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\n");
    printf("High-performance TCP proxy using epoll and edge-triggered I/O.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -l, --listen ADDR    Listen address (default: 0.0.0.0)\n");
    printf("  -p, --port PORT      Listen port (default: 8080)\n");
    printf("  -b, --backend ADDR   Backend address (default: 127.0.0.1)\n");
    printf("  -P, --backend-port PORT  Backend port (default: 8081)\n");
    printf("  -h, --help           Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # Forward port 8080 to localhost:8081\n");
    printf("  %s\n", program_name);
    printf("\n");
    printf("  # Forward external port 80 to backend server\n");
    printf("  %s -l 0.0.0.0 -p 80 -b 192.168.1.100 -P 8080\n", program_name);
    printf("\n");
    printf("Performance Notes:\n");
    printf("  - Uses edge-triggered epoll for maximum efficiency\n");
    printf("  - Supports up to %d concurrent connections\n", MAX_CONNECTIONS);
    printf("  - Zero-copy forwarding between client and backend\n");
    printf("  - Non-blocking I/O throughout\n");
    printf("\n");
}

/* ============================================================================
 * ARGUMENT PARSING
 * ============================================================================
 */

typedef struct {
    const char *listen_addr;
    uint16_t listen_port;
    const char *backend_addr;
    uint16_t backend_port;
} args_t;

static int parse_args(int argc, char **argv, args_t *args) {
    /* Set defaults */
    args->listen_addr = "0.0.0.0";
    args->listen_port = 8080;
    args->backend_addr = "127.0.0.1";
    args->backend_port = 8081;
    
    /* Long options for getopt_long */
    static struct option long_options[] = {
        {"listen",       required_argument, 0, 'l'},
        {"port",         required_argument, 0, 'p'},
        {"backend",      required_argument, 0, 'b'},
        {"backend-port", required_argument, 0, 'P'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "l:p:b:P:h", 
                             long_options, &option_index)) != -1) {
        switch (opt) {
            case 'l':
                args->listen_addr = optarg;
                break;
                
            case 'p': {
                /* Parse port number with validation */
                char *endptr;
                long port = strtol(optarg, &endptr, 10);
                
                if (*endptr != '\0' || port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid port number: %s\n", optarg);
                    return -1;
                }
                
                args->listen_port = (uint16_t)port;
                break;
            }
            
            case 'b':
                args->backend_addr = optarg;
                break;
                
            case 'P': {
                /* Parse backend port */
                char *endptr;
                long port = strtol(optarg, &endptr, 10);
                
                if (*endptr != '\0' || port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid backend port number: %s\n", optarg);
                    return -1;
                }
                
                args->backend_port = (uint16_t)port;
                break;
            }
            
            case 'h':
                print_usage(argv[0]);
                exit(0);
                
            case '?':
                /* getopt_long already printed error message */
                print_usage(argv[0]);
                return -1;
                
            default:
                fprintf(stderr, "Unknown option: %c\n", opt);
                print_usage(argv[0]);
                return -1;
        }
    }
    
    /* Check for extra arguments */
    if (optind < argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        print_usage(argv[0]);
        return -1;
    }
    
    return 0;
}

/* ============================================================================
 * CONFIGURATION VALIDATION
 * ============================================================================
 */

static int validate_config(const args_t *args) {
    /* Check if listening on same address:port as backend.
     * This would create a forwarding loop.
     */
    if (strcmp(args->listen_addr, args->backend_addr) == 0 &&
        args->listen_port == args->backend_port) {
        fprintf(stderr, "Error: Listen and backend cannot be the same address:port\n");
        fprintf(stderr, "This would create an infinite forwarding loop.\n");
        return -1;
    }
    
    /* Warn if listening on privileged port without root */
    if (args->listen_port < 1024) {
        fprintf(stderr, "Warning: Port %d requires root privileges.\n", 
                args->listen_port);
        fprintf(stderr, "If bind() fails, try running with sudo or use a port >= 1024.\n");
    }
    
    return 0;
}

/* ============================================================================
 * MAIN
 * ============================================================================
 */

int main(int argc, char **argv) {
    args_t args;
    proxy_config_t *config;
    int ret;
    
    /* Allocate config on heap (it's large - contains all connection structs) */
    config = calloc(1, sizeof(proxy_config_t));
    if (config == NULL) {
        fprintf(stderr, "Failed to allocate memory for proxy config\n");
        return EXIT_FAILURE;
    }
    
    /* Print banner */
    printf("╔════════════════════════════════════════╗\n");
    printf("║   High-Performance Epoll TCP Proxy    ║\n");
    printf("║   Edge-Triggered | Non-Blocking I/O   ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\n");
    
    /* Parse command-line arguments */
    if (parse_args(argc, argv, &args) == -1) {
        return EXIT_FAILURE;
    }
    
    /* Validate configuration */
    if (validate_config(&args) == -1) {
        return EXIT_FAILURE;
    }
    
    /* Print configuration */
    printf("Configuration:\n");
    printf("  Listen:  %s:%d\n", args.listen_addr, args.listen_port);
    printf("  Backend: %s:%d\n", args.backend_addr, args.backend_port);
    printf("  Max connections: %d\n", MAX_CONNECTIONS);
    printf("  Buffer size: %d bytes\n", BUFFER_SIZE);
    printf("\n");
    
    /* Initialize proxy */
    ret = proxy_init(config, 
                    args.listen_addr, args.listen_port,
                    args.backend_addr, args.backend_port);
    if (ret == -1) {
        fprintf(stderr, "Failed to initialize proxy\n");
        free(config);
        return EXIT_FAILURE;
    }
    
    /* Run the event loop.
     * This blocks until interrupted (Ctrl-C) or error occurs.
     */
    ret = proxy_run(config);
    
    /* Cleanup */
    proxy_cleanup(config);
    
    /* Free config memory */
    free(config);
    
    if (ret == -1) {
        fprintf(stderr, "Proxy terminated with error\n");
        return EXIT_FAILURE;
    }
    
    printf("Proxy terminated gracefully\n");
    return EXIT_SUCCESS;
}