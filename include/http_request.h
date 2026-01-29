#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * HTTP METHOD TYPES
 * ============================================================================
 */
typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_TRACE,
    HTTP_METHOD_CONNECT
} http_method_t;

/* ============================================================================
 * HTTP VERSION
 * ============================================================================
 */
typedef enum {
    HTTP_VERSION_UNKNOWN = 0,
    HTTP_VERSION_10,  /* HTTP/1.0 */
    HTTP_VERSION_11   /* HTTP/1.1 */
} http_version_t;

/* ============================================================================
 * LIMITS
 * ============================================================================
 */
#define MAX_METHOD_LEN          16
#define MAX_PATH_LEN            8192
#define MAX_HOST_LEN            256
#define MAX_HEADERS             64
#define MAX_HEADER_NAME_LEN     128
#define MAX_HEADER_VALUE_LEN    8192

/* ============================================================================
 * HTTP HEADER STRUCTURE
 * ============================================================================
 */
typedef struct {
    char name[MAX_HEADER_NAME_LEN];
    char value[MAX_HEADER_VALUE_LEN];
} http_header_t;

/* ============================================================================
 * HTTP REQUEST STRUCTURE
 * ============================================================================
 */
typedef struct http_request {
    /* Request line */
    http_method_t method;
    char method_str[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    http_version_t version;
    
    /* Host */
    char host[MAX_HOST_LEN];
    
    /* Headers */
    http_header_t headers[MAX_HEADERS];
    int header_count;
    
    /* Body information */
    int64_t content_length;  /* -1 if not specified */
    int chunked;             /* 1 if Transfer-Encoding: chunked */
    
    /* Connection management */
    int keep_alive;          /* 1 for keep-alive, 0 for close */
    
    /* Parsing state */
    int is_complete;         /* 1 when full request received */
    size_t headers_end_offset;  /* Offset where headers end (\r\n\r\n) */
    size_t total_length;     /* Total length including body */
    
    /* Raw data (not owned by this struct) */
    const char *raw_data;
    size_t raw_data_len;
} http_request_t;

/* ============================================================================
 * FUNCTION DECLARATIONS
 * ============================================================================
 */

/**
 * Initialize an HTTP request structure
 * @param req Request structure to initialize
 */
void http_request_init(http_request_t *req);

/**
 * Parse HTTP request from buffer
 * @param req Request structure to fill
 * @param data Buffer containing HTTP request data
 * @param len Length of data in buffer
 * @return 1 if request complete, 0 if need more data, -1 on error
 */
int http_request_parse(http_request_t *req, const char *data, size_t len);

/**
 * Check if request is valid
 * @param req Request to validate
 * @return 1 if valid, 0 if invalid
 */
int http_request_is_valid(const http_request_t *req);

/**
 * Get header value by name (case-insensitive)
 * @param req Request to search
 * @param name Header name to find
 * @return Header value or NULL if not found
 */
const char* http_request_get_header(const http_request_t *req, const char *name);

/**
 * Convert HTTP method enum to string
 * @param method Method enum value
 * @return Method name as string
 */
const char* http_method_to_string(http_method_t method);

/**
 * Parse method string to enum
 * @param str Method string
 * @param len Length of string
 * @return Method enum value
 */
http_method_t http_parse_method(const char *str, size_t len);

/**
 * Get HTTP status line for status code
 * @param status_code HTTP status code (200, 404, etc.)
 * @return Complete HTTP status line with CRLF
 */
const char* http_get_status_line(int status_code);

/**
 * Case-insensitive string comparison
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, <0 if s1 < s2, >0 if s1 > s2
 */
int http_strcasecmp(const char *s1, const char *s2);

#endif /* HTTP_REQUEST_H */