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
    printf("High-performance proxy using epoll and edge-triggered I/O.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -l, --listen ADDR    Listen address (default: 0.0.0.0)\n");
    printf("  -p, --port PORT      Listen port (default: 8080)\n");
    printf("  -b, --backend ADDR   Backend address (default: 127.0.0.1)\n");
    printf("  -P, --backend-port PORT  Backend port (default: 8081)\n");
    printf("  -m, --mode MODE      Proxy mode: tcp or http (default: http)\n");
    printf("  -h, --help           Show this help message\n");
    printf("\n");
    printf("Modes:\n");
    printf("  tcp  - Raw TCP proxy (fast, no protocol awareness)\n");
    printf("  http - HTTP-aware proxy (supports keep-alive, validation)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # HTTP proxy (recommended)\n");
    printf("  %s -m http\n", program_name);
    printf("\n");
    printf("  # TCP proxy (for non-HTTP protocols)\n");
    printf("  %s -m tcp -p 3306 -P 3307\n", program_name);
    printf("\n");
    printf("Performance:\n");
    printf("  - Supports up to %d concurrent connections\n", MAX_CONNECTIONS);
    printf("  - Edge-triggered epoll for maximum efficiency\n");
    printf("  - Zero-copy forwarding\n");
    printf("  - HTTP keep-alive support\n");
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
    const char *mode;
} args_t;

static int parse_args(int argc, char **argv, args_t *args) {
    args->listen_addr = "0.0.0.0";
    args->listen_port = 8080;
    args->backend_addr = "127.0.0.1";
    args->backend_port = 8081;
    args->mode = "http";  /* Default to HTTP mode */
    
    static struct option long_options[] = {
        {"listen",       required_argument, 0, 'l'},
        {"port",         required_argument, 0, 'p'},
        {"backend",      required_argument, 0, 'b'},
        {"backend-port", required_argument, 0, 'P'},
        {"mode",         required_argument, 0, 'm'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "l:p:b:P:m:h", 
                             long_options, &option_index)) != -1) {
        switch (opt) {
            case 'l':
                args->listen_addr = optarg;
                break;
                
            case 'p': {
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
                char *endptr;
                long port = strtol(optarg, &endptr, 10);
                
                if (*endptr != '\0' || port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid backend port number: %s\n", optarg);
                    return -1;
                }
                
                args->backend_port = (uint16_t)port;
                break;
            }
            
            case 'm':
                if (strcmp(optarg, "tcp") != 0 && strcmp(optarg, "http") != 0) {
                    fprintf(stderr, "Invalid mode: %s (must be 'tcp' or 'http')\n", optarg);
                    return -1;
                }
                args->mode = optarg;
                break;
            
            case 'h':
                print_usage(argv[0]);
                exit(0);
                
            case '?':
                print_usage(argv[0]);
                return -1;
                
            default:
                fprintf(stderr, "Unknown option: %c\n", opt);
                print_usage(argv[0]);
                return -1;
        }
    }
    
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
    if (strcmp(args->listen_addr, args->backend_addr) == 0 &&
        args->listen_port == args->backend_port) {
        fprintf(stderr, "Error: Listen and backend cannot be the same address:port\n");
        return -1;
    }
    
    if (args->listen_port < 1024) {
        fprintf(stderr, "Warning: Port %d requires root privileges.\n", 
                args->listen_port);
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
    
    config = calloc(1, sizeof(proxy_config_t));
    if (config == NULL) {
        fprintf(stderr, "Failed to allocate memory for proxy config\n");
        return EXIT_FAILURE;
    }
    
    printf("╔════════════════════════════════════════╗\n");
    printf("║   High-Performance Epoll Proxy        ║\n");
    printf("║   Edge-Triggered | Non-Blocking I/O   ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\n");
    
    if (parse_args(argc, argv, &args) == -1) {
        free(config);
        return EXIT_FAILURE;
    }
    
    if (validate_config(&args) == -1) {
        free(config);
        return EXIT_FAILURE;
    }
    
    printf("Configuration:\n");
    printf("  Mode:    %s\n", args.mode);
    printf("  Listen:  %s:%d\n", args.listen_addr, args.listen_port);
    printf("  Backend: %s:%d\n", args.backend_addr, args.backend_port);
    printf("  Max connections: %d\n", MAX_CONNECTIONS);
    printf("  Buffer size: %d bytes\n", BUFFER_SIZE);
    printf("\n");
    
    /* Initialize based on mode */
    if (strcmp(args.mode, "http") == 0) {
        ret = proxy_init_http(config, 
                             args.listen_addr, args.listen_port,
                             args.backend_addr, args.backend_port);
    } else {
        ret = proxy_init(config, 
                        args.listen_addr, args.listen_port,
                        args.backend_addr, args.backend_port);
    }
    
    if (ret == -1) {
        fprintf(stderr, "Failed to initialize proxy\n");
        free(config);
        return EXIT_FAILURE;
    }
    
    ret = proxy_run(config);
    
    proxy_cleanup(config);
    free(config);
    
    if (ret == -1) {
        fprintf(stderr, "Proxy terminated with error\n");
        return EXIT_FAILURE;
    }
    
    printf("Proxy terminated gracefully\n");
    return EXIT_SUCCESS;
}