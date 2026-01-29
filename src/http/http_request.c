#include "http_request.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/* Case-insensitive string comparison */
int http_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/* Case-insensitive string comparison with length limit */
static int strncasecmp_custom(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/* Skip whitespace */
static const char* skip_whitespace(const char *str, const char *end) {
    while (str < end && (*str == ' ' || *str == '\t')) {
        str++;
    }
    return str;
}

/* Find CRLF in buffer */
static const char* find_crlf(const char *data, size_t len) {
    if (len < 2) return NULL;
    
    for (size_t i = 0; i < len - 1; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return data + i;
        }
    }
    return NULL;
}

/* Find double CRLF (end of headers) */
static const char* find_header_end(const char *data, size_t len) {
    if (len < 4) return NULL;
    
    for (size_t i = 0; i < len - 3; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return data + i;
        }
    }
    return NULL;
}

/* ============================================================================
 * METHOD PARSING
 * ============================================================================
 */

const char* http_method_to_string(http_method_t method) {
    switch (method) {
        case HTTP_METHOD_GET:     return "GET";
        case HTTP_METHOD_HEAD:    return "HEAD";
        case HTTP_METHOD_POST:    return "POST";
        case HTTP_METHOD_PUT:     return "PUT";
        case HTTP_METHOD_DELETE:  return "DELETE";
        case HTTP_METHOD_PATCH:   return "PATCH";
        case HTTP_METHOD_OPTIONS: return "OPTIONS";
        case HTTP_METHOD_TRACE:   return "TRACE";
        case HTTP_METHOD_CONNECT: return "CONNECT";
        default:                  return "UNKNOWN";
    }
}

http_method_t http_parse_method(const char *str, size_t len) {
    /* Fast path for common methods */
    if (len == 3 && strncasecmp_custom(str, "GET", 3) == 0) {
        return HTTP_METHOD_GET;
    }
    if (len == 4 && strncasecmp_custom(str, "POST", 4) == 0) {
        return HTTP_METHOD_POST;
    }
    if (len == 4 && strncasecmp_custom(str, "HEAD", 4) == 0) {
        return HTTP_METHOD_HEAD;
    }
    if (len == 3 && strncasecmp_custom(str, "PUT", 3) == 0) {
        return HTTP_METHOD_PUT;
    }
    if (len == 6 && strncasecmp_custom(str, "DELETE", 6) == 0) {
        return HTTP_METHOD_DELETE;
    }
    if (len == 5 && strncasecmp_custom(str, "PATCH", 5) == 0) {
        return HTTP_METHOD_PATCH;
    }
    if (len == 7 && strncasecmp_custom(str, "OPTIONS", 7) == 0) {
        return HTTP_METHOD_OPTIONS;
    }
    if (len == 5 && strncasecmp_custom(str, "TRACE", 5) == 0) {
        return HTTP_METHOD_TRACE;
    }
    if (len == 7 && strncasecmp_custom(str, "CONNECT", 7) == 0) {
        return HTTP_METHOD_CONNECT;
    }
    
    return HTTP_METHOD_UNKNOWN;
}

/* ============================================================================
 * REQUEST LINE PARSING
 * ============================================================================
 */

/* Parse request line: "GET /path HTTP/1.1\r\n" */
static int parse_request_line(http_request_t *req, const char *line, size_t len) {
    const char *p = line;
    const char *end = line + len;
    
    /* Parse method */
    const char *method_start = p;
    while (p < end && *p != ' ') p++;
    if (p >= end) return -1;  /* No space after method */
    
    size_t method_len = p - method_start;
    if (method_len >= MAX_METHOD_LEN) return -1;
    
    memcpy(req->method_str, method_start, method_len);
    req->method_str[method_len] = '\0';
    req->method = http_parse_method(method_start, method_len);
    
    /* Skip whitespace */
    p = skip_whitespace(p, end);
    if (p >= end) return -1;
    
    /* Parse path */
    const char *path_start = p;
    while (p < end && *p != ' ') p++;
    if (p >= end) return -1;  /* No space after path */
    
    size_t path_len = p - path_start;
    if (path_len >= MAX_PATH_LEN) return -1;
    
    memcpy(req->path, path_start, path_len);
    req->path[path_len] = '\0';
    
    /* Skip whitespace */
    p = skip_whitespace(p, end);
    if (p >= end) return -1;
    
    /* Parse version */
    if (end - p >= 8 && strncasecmp_custom(p, "HTTP/1.1", 8) == 0) {
        req->version = HTTP_VERSION_11;
    } else if (end - p >= 8 && strncasecmp_custom(p, "HTTP/1.0", 8) == 0) {
        req->version = HTTP_VERSION_10;
    } else {
        req->version = HTTP_VERSION_UNKNOWN;
        return -1;
    }
    
    return 0;
}

/* ============================================================================
 * HEADER PARSING
 * ============================================================================
 */

/* Parse single header line: "Name: Value\r\n" */
static int parse_header(http_request_t *req, const char *line, size_t len) {
    if (req->header_count >= MAX_HEADERS) {
        return -1;  /* Too many headers */
    }
    
    /* Find colon separator */
    const char *colon = memchr(line, ':', len);
    if (!colon) return -1;
    
    size_t name_len = colon - line;
    if (name_len >= MAX_HEADER_NAME_LEN) return -1;
    
    /* Extract name (trim trailing whitespace) */
    while (name_len > 0 && (line[name_len - 1] == ' ' || line[name_len - 1] == '\t')) {
        name_len--;
    }
    
    /* Skip colon and leading whitespace in value */
    const char *value_start = colon + 1;
    const char *value_end = line + len;
    value_start = skip_whitespace(value_start, value_end);
    
    /* Trim trailing whitespace from value */
    while (value_end > value_start && 
           (value_end[-1] == ' ' || value_end[-1] == '\t' || 
            value_end[-1] == '\r' || value_end[-1] == '\n')) {
        value_end--;
    }
    
    size_t value_len = value_end - value_start;
    if (value_len >= MAX_HEADER_VALUE_LEN) return -1;
    
    /* Store header */
    http_header_t *header = &req->headers[req->header_count];
    memcpy(header->name, line, name_len);
    header->name[name_len] = '\0';
    memcpy(header->value, value_start, value_len);
    header->value[value_len] = '\0';
    req->header_count++;
    
    /* Cache important headers */
    if (strncasecmp_custom(header->name, "Host", 4) == 0) {
        snprintf(req->host, MAX_HOST_LEN, "%s", header->value);
    } else if (strncasecmp_custom(header->name, "Content-Length", 14) == 0) {
        req->content_length = atoll(header->value);
    } else if (strncasecmp_custom(header->name, "Connection", 10) == 0) {
        if (strncasecmp_custom(header->value, "keep-alive", 10) == 0) {
            req->keep_alive = 1;
        } else if (strncasecmp_custom(header->value, "close", 5) == 0) {
            req->keep_alive = 0;
        }
    } else if (strncasecmp_custom(header->name, "Transfer-Encoding", 17) == 0) {
        if (strncasecmp_custom(header->value, "chunked", 7) == 0) {
            req->chunked = 1;
        }
    }
    
    return 0;
}

/* ============================================================================
 * MAIN PARSING FUNCTION
 * ============================================================================
 */

void http_request_init(http_request_t *req) {
    memset(req, 0, sizeof(http_request_t));
    req->content_length = -1;  /* -1 means "not specified" */
    req->version = HTTP_VERSION_11;  /* Default to HTTP/1.1 */
    req->keep_alive = 1;  /* HTTP/1.1 defaults to keep-alive */
}

int http_request_parse(http_request_t *req, const char *data, size_t len) {
    /* Already parsed? Don't parse again. */
    if (req->is_complete) {
        return 1;
    }
    
    /* Store raw data pointer */
    req->raw_data = data;
    req->raw_data_len = len;
    
    /* Find end of headers (double CRLF) */
    const char *header_end = find_header_end(data, len);
    if (!header_end) {
        /* Haven't received full headers yet */
        return 0;
    }
    
    req->headers_end_offset = (header_end - data) + 4;  /* +4 for \r\n\r\n */
    
    /* Parse request line */
    const char *line_start = data;
    const char *crlf = find_crlf(data, len);
    if (!crlf) return -1;
    
    if (parse_request_line(req, line_start, crlf - line_start) != 0) {
        return -1;
    }
    
    /* Parse headers */
    line_start = crlf + 2;  /* Skip \r\n */
    
    while (line_start < header_end) {
        crlf = find_crlf(line_start, header_end - line_start);
        if (!crlf) break;
        
        size_t line_len = crlf - line_start;
        if (line_len == 0) {
            /* Empty line = end of headers */
            break;
        }
        
        if (parse_header(req, line_start, line_len) != 0) {
            return -1;
        }
        
        line_start = crlf + 2;  /* Skip \r\n */
    }
    
    /* Set keep-alive default based on HTTP version */
    if (req->version == HTTP_VERSION_10) {
        /* HTTP/1.0 defaults to close unless explicitly keep-alive */
        const char *conn = http_request_get_header(req, "Connection");
        req->keep_alive = (conn && http_strcasecmp(conn, "keep-alive") == 0);
    } else {
        /* HTTP/1.1 defaults to keep-alive unless explicitly close */
        const char *conn = http_request_get_header(req, "Connection");
        req->keep_alive = !(conn && http_strcasecmp(conn, "close") == 0);
    }
    
    /* Calculate total request length */
    if (req->chunked) {
        /* Can't determine length until we parse chunks - not supported yet */
        req->total_length = req->headers_end_offset;
        req->is_complete = 1;  /* Forward headers, let backend handle chunked body */
    } else if (req->content_length >= 0) {
        /* Have explicit Content-Length */
        req->total_length = req->headers_end_offset + req->content_length;
        
        /* Check if we have the full body */
        if (len >= req->total_length) {
            req->is_complete = 1;
        }
    } else {
        /* No body for GET/HEAD/DELETE */
        if (req->method == HTTP_METHOD_GET ||
            req->method == HTTP_METHOD_HEAD ||
            req->method == HTTP_METHOD_DELETE) {
            req->total_length = req->headers_end_offset;
            req->is_complete = 1;
        } else {
            /* POST/PUT without Content-Length is malformed */
            return -1;
        }
    }
    
    return req->is_complete ? 1 : 0;
}

/* ============================================================================
 * QUERY FUNCTIONS
 * ============================================================================
 */

const char* http_request_get_header(const http_request_t *req, const char *name) {
    for (int i = 0; i < req->header_count; i++) {
        if (http_strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

int http_request_is_valid(const http_request_t *req) {
    /* Must have valid method */
    if (req->method == HTTP_METHOD_UNKNOWN) {
        return 0;
    }
    
    /* Must have non-empty path */
    if (req->path[0] == '\0') {
        return 0;
    }
    
    /* Must have valid HTTP version */
    if (req->version == HTTP_VERSION_UNKNOWN) {
        return 0;
    }
    
    /* Content-Length must be reasonable (< 100MB) */
    if (req->content_length > 100 * 1024 * 1024) {
        return 0;
    }
    
    return 1;
}

/* ============================================================================
 * ERROR RESPONSES
 * ============================================================================
 */

const char* http_get_status_line(int status_code) {
    switch (status_code) {
        case 200: return "HTTP/1.1 200 OK\r\n";
        case 400: return "HTTP/1.1 400 Bad Request\r\n";
        case 404: return "HTTP/1.1 404 Not Found\r\n";
        case 413: return "HTTP/1.1 413 Request Entity Too Large\r\n";
        case 500: return "HTTP/1.1 500 Internal Server Error\r\n";
        case 502: return "HTTP/1.1 502 Bad Gateway\r\n";
        case 503: return "HTTP/1.1 503 Service Unavailable\r\n";
        default:  return "HTTP/1.1 500 Internal Server Error\r\n";
    }
}