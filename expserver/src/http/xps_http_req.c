#include "xps_http_req.h"
#include "xps_http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int http_process_request_line(xps_http_req_t *http_req, xps_buffer_t *buff) {
    int error = xps_http_parse_request_line(http_req, buff);
    if (error != OK) {
        return error;
    }

    // Extract full request line
    http_req->request_line = str_from_ptrs((char *)http_req->request_line_start, (char *)http_req->request_line_end);
    printf("REQUEST LINE: %s\n", http_req->request_line);

    // Extract method, uri, schema, host, path, pathname if pointers exist
    if (http_req->method_start && http_req->method_end) {
        http_req->method = str_from_ptrs((char *)http_req->method_start, (char *)http_req->method_end);
        printf("REQUEST LINE: %s\n", http_req->method_start);
    }
    if (http_req->uri_start && http_req->uri_end) {
        http_req->uri = str_from_ptrs((char *)http_req->uri_start, (char *)http_req->uri_end);
        printf("REQUEST LINE: %s\n", http_req->uri);
    }
    if (http_req->schema_start && http_req->schema_end) {
        http_req->schema = str_from_ptrs((char *)http_req->schema_start, (char *)http_req->schema_end);
        printf("REQUEST LINE: %s\n", http_req->schema);
    }
    if (http_req->host_start && http_req->host_end) {
        http_req->host = str_from_ptrs((char *)http_req->host_start, (char *)http_req->host_end);
        printf("REQUEST LINE: %s\n", http_req->host);
    }
    if (http_req->path_start && http_req->path_end) {
        http_req->path = str_from_ptrs((char *)http_req->path_start, (char *)http_req->path_end);
        printf("REQUEST LINE: %s\n", http_req->path);
    }
    if (http_req->pathname_start && http_req->pathname_end) {
        http_req->pathname = str_from_ptrs((char *)http_req->pathname_start, (char *)http_req->pathname_end);
        printf("REQUEST LINE: %s\n", http_req->pathname);
    }

    // Extract HTTP Version string (e.g., "1.1")
    if (http_req->http_major && http_req->http_minor) {
        // http_minor points right to the end/minor digit character
        http_req->http_version = str_from_ptrs((char *)http_req->http_major, (char *)http_req->http_minor + 1);
        printf("REQUEST LINE: %s\n", http_req->http_version);
    }

    // Resolve port number
    http_req->port = -1;
    if (http_req->port_start && http_req->port_end) {
        char *port_str = str_from_ptrs((char *)http_req->port_start, (char *)http_req->port_end);
        if (port_str != NULL) {
            http_req->port = atoi(port_str);
            free(port_str);
        }
    } else {
        // Assign default ports based on schema
        if (http_req->schema != NULL) {
            if (strcasecmp(http_req->schema, "https") == 0) {
                http_req->port = 443;
            } else if (strcasecmp(http_req->schema, "http") == 0) {
                http_req->port = 80;
            }
        }
    }

    // Milestone #1 Verification Print
    printf("REQUEST LINE: %s\n", http_req->request_line ? http_req->request_line : "(null)");
    printf("Method: %s\n", http_req->method ? http_req->method : "(null)");
    printf("URI: %s\n", http_req->uri ? http_req->uri : "(null)");
    printf("Schema: %s\n", http_req->schema ? http_req->schema : "(null)");
    printf("Host: %s\n", http_req->host ? http_req->host : "(null)");
    printf("Port: %d\n", http_req->port);
    printf("Path: %s\n", http_req->path ? http_req->path : "(null)");
    printf("Pathname: %s\n", http_req->pathname ? http_req->pathname : "(null)");
    printf("HTTP Version: %s\n", http_req->http_version ? http_req->http_version : "(null)");

    return OK;
}

int http_process_headers(xps_http_req_t *http_req, xps_buffer_t *buff) {
    assert(http_req != NULL);
    assert(buff != NULL);

    // Initialize the headers vector list
    vec_init(&http_req->headers);
    int error;

    while (1) {
        error = xps_http_parse_header_line(http_req, buff);

        if (error == E_FAIL || error == E_AGAIN) {
            // Clean up headers on failure before breaking
            for (int i = 0; i < http_req->headers.length; i++) {
                xps_keyval_t *header = (xps_keyval_t *)http_req->headers.data[i];
                if (header) {
                    free(header->key);
                    free(header->val);
                    free(header);
                }
            }
            vec_deinit(&http_req->headers);
            return error;
        }

        if (error == OK || error == E_NEXT) {
            // If pointers for key and value are set, allocate and store them
            if (http_req->header_key_start && http_req->header_key_end &&
                http_req->header_val_start && http_req->header_val_end) {
                
                xps_keyval_t *header = malloc(sizeof(xps_keyval_t));
                if (header == NULL) {
                    logger(LOG_ERROR, "http_process_headers()", "malloc() failed for 'header'");
                    return E_FAIL;
                }

                header->key = str_from_ptrs((char *)http_req->header_key_start, (char *)http_req->header_key_end);
                header->val = str_from_ptrs((char *)http_req->header_val_start, (char *)http_req->header_val_end);

                vec_push(&http_req->headers, header);
            }

            if (error == OK) {
                break; // Fully finished parsing all headers
            }
            // If E_NEXT, continue loop to parse subsequent header line
        }
    }

    // Milestone #2 Verification Print
    printf("HEADERS\n");
    for (int i = 0; i < http_req->headers.length; i++) {
        xps_keyval_t *header = (xps_keyval_t *)http_req->headers.data[i];
        printf("Key: %s | Val: %s\n", header->key, header->val);
    }

    return OK;
}

xps_buffer_t *xps_http_req_serialize(xps_http_req_t *http_req) {
    assert(http_req != NULL);

    // Serialize headers into a buffer headers_str
    xps_buffer_t *headers_str = xps_http_serialize_headers(&http_req->headers);
    if (headers_str == NULL) {
        return NULL;
    }

    // Calculate total final length: request_line + '\n' + headers_str->len + '\n' + '\0'
    size_t req_line_len = strlen(http_req->request_line);
    size_t final_len = req_line_len + 1 + headers_str->len + 1 + 1;

    xps_buffer_t *buff = xps_buffer_create(0, final_len, NULL);
    if (buff == NULL) {
        xps_buffer_destroy(headers_str);
        return NULL;
    }

    // Copy request line
    memcpy(buff->pos, http_req->request_line, req_line_len);
    buff->pos += req_line_len;
    *buff->pos++ = '\n';

    // Copy serialized headers
    memcpy(buff->pos, headers_str->data, headers_str->len);
    buff->pos += headers_str->len;
    *buff->pos++ = '\n';
    *buff->pos = '\0';

    buff->len = (size_t)(buff->pos - buff->data);

    xps_buffer_destroy(headers_str);
    return buff;
}

xps_http_req_t *xps_http_req_create(xps_core_t *core, xps_buffer_t *buff, int *error) {
    assert(core != NULL);
    assert(buff != NULL);
    assert(error != NULL);

    *error = E_FAIL;

    xps_http_req_t *http_req = malloc(sizeof(xps_http_req_t));
    if (http_req == NULL) {
        logger(LOG_ERROR, "xps_http_req_create()", "malloc() failed for 'http_req'");
        return NULL;
    }
    memset(http_req, 0, sizeof(xps_http_req_t));

    // Set initial parser state for request line
    http_req->parser_state = RL_START;

    // Process Request Line
    int ret = http_process_request_line(http_req, buff);
    if (ret != OK) {
        xps_http_req_destroy(core, http_req);
        *error = ret;
        return NULL;
    }

    // Process Headers
    ret = http_process_headers(http_req, buff);
    if (ret != OK) {
        xps_http_req_destroy(core, http_req);
        *error = ret;
        return NULL;
    }

    // Header length captured so far
    http_req->header_len = (size_t)(buff->pos - buff->data);

    // Retrieve body length from Content-Length header if present
    http_req->body_len = 0;
    const char *body_len_str = xps_http_get_header(&http_req->headers, "Content-Length");
    if (body_len_str != NULL) {
        http_req->body_len = (size_t)atoi(body_len_str);
    }

    *error = OK;
    return http_req;
}

void xps_http_req_destroy(xps_core_t *core, xps_http_req_t *http_req) {
    assert(http_req != NULL);

    // Free request line components
    if (http_req->request_line) free(http_req->request_line);
    if (http_req->method) free(http_req->method);
    if (http_req->uri) free(http_req->uri);
    if (http_req->schema) free(http_req->schema);
    if (http_req->host) free(http_req->host);
    if (http_req->path) free(http_req->path);
    if (http_req->pathname) free(http_req->pathname);
    if (http_req->http_version) free(http_req->http_version);

    // Free headers vector items
    for (int i = 0; i < http_req->headers.length; i++) {
        xps_keyval_t *header = (xps_keyval_t *)http_req->headers.data[i];
        if (header != NULL) {
            if (header->key) free(header->key);
            if (header->val) free(header->val);
            free(header);
        }
    }
    vec_deinit(&http_req->headers);

    // Free the request struct itself
    free(http_req);
}