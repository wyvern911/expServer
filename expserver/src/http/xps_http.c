#include "xps_http.h"
#include "xps_http_req.h"
#include <string.h>
#include <strings.h> // For strcasecmp
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// Utility to compare u_char string with a const char method string
bool http_strcmp(u_char *str, const char *method, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (str[i] != (u_char)method[i]) {
            return false;
        }
    }
    return true;
}

int xps_http_parse_request_line(xps_http_req_t *http_req, xps_buffer_t *buff) {
    u_char *p_ch = buff->pos;
    xps_http_parser_state_t parser_state = http_req->parser_state;

    // Traverse through buffer and increment buffer position
    for (; p_ch < buff->data + buff->len; p_ch++) {
        char ch = *p_ch;

        switch (parser_state) {
            case RL_START:
                if (ch == CR || ch == LF) {
                    continue; // Ignore leading CR or LF
                }
                if (ch >= 'A' && ch <= 'Z') {
                    http_req->request_line_start = p_ch;
                    http_req->method_start = p_ch;
                    parser_state = RL_METHOD;
                } else {
                    return E_FAIL; // Fails if not upper case
                }
                break;

            case RL_METHOD:
                if (ch == ' ') {
                    http_req->method_end = p_ch;
                    size_t method_len = p_ch - http_req->method_start;
                    
                    // Match the method
                    if (method_len == 3 && http_strcmp(http_req->method_start, "GET", 3)) http_req->method_n = HTTP_GET;
                    else if (method_len == 4 && http_strcmp(http_req->method_start, "POST", 4)) http_req->method_n = HTTP_POST;
                    else if (method_len == 3 && http_strcmp(http_req->method_start, "PUT", 3)) http_req->method_n = HTTP_PUT;
                    else if (method_len == 4 && http_strcmp(http_req->method_start, "HEAD", 4)) http_req->method_n = HTTP_HEAD;
                    else if (method_len == 6 && http_strcmp(http_req->method_start, "DELETE", 6)) http_req->method_n = HTTP_DELETE;
                    // Note: Expand with OPTIONS, TRACE, CONNECT if needed
                    
                    parser_state = RL_SP_AFTER_METHOD;
                } else if (ch < 'A' || ch > 'Z') {
                    return E_FAIL;
                }
                break;

            case RL_SP_AFTER_METHOD:
                if (ch == '/') {
                    // Absolute path without schema (e.g. GET /index.html)
                    http_req->uri_start = p_ch;
                    http_req->path_start = p_ch;
                    http_req->pathname_start = p_ch;
                    parser_state = RL_PATH;
                } else {
                    char c = ch | 0x20; // convert to lower case
                    if (c >= 'a' && c <= 'z') {
                        // Full URL with schema (e.g. GET http://...)
                        http_req->schema_start = p_ch;
                        http_req->uri_start = p_ch;
                        parser_state = RL_SCHEMA;
                    } else if (ch != ' ') {
                        return E_FAIL;
                    }
                }
                break;

            case RL_SCHEMA:
                if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') {
                    // Schema can have lower case alphabets
                } else if (ch == ':') {
                    http_req->schema_end = p_ch;
                    parser_state = RL_SCHEMA_SLASH;
                } else {
                    return E_FAIL;
                }
                break;

            case RL_SCHEMA_SLASH:
                if (ch == '/') parser_state = RL_SCHEMA_SLASH_SLASH;
                else return E_FAIL;
                break;

            case RL_SCHEMA_SLASH_SLASH:
                if (ch == '/') {
                    parser_state = RL_HOST;
                    http_req->host_start = p_ch + 1;
                } else {
                    return E_FAIL;
                }
                break;

            case RL_HOST:
                if (((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.') {
                    // Valid host characters
                } else if (ch == ':') {
                    http_req->host_end = p_ch;
                    http_req->port_start = p_ch + 1;
                    parser_state = RL_PORT;
                } else if (ch == '/') {
                    http_req->host_end = p_ch;
                    http_req->path_start = p_ch;
                    http_req->pathname_start = p_ch;
                    parser_state = RL_PATH;
                } else if (ch == ' ') {
                    http_req->host_end = p_ch;
                    http_req->uri_end = p_ch;
                    http_req->path_start = p_ch;
                    http_req->path_end = p_ch;
                    http_req->pathname_start = p_ch;
                    http_req->pathname_end = p_ch;
                    parser_state = RL_VERSION_START;
                } else {
                    return E_FAIL;
                }
                break;

            case RL_PORT:
                if (ch >= '0' && ch <= '9') {
                    // Port can only have numbers
                } else if (ch == '/') {
                    http_req->port_end = p_ch;
                    http_req->path_start = p_ch;
                    http_req->pathname_start = p_ch;
                    parser_state = RL_PATH;
                } else if (ch == ' ') {
                    http_req->port_end = p_ch;
                    http_req->uri_end = p_ch;
                    http_req->path_start = p_ch;
                    http_req->path_end = p_ch;
                    http_req->pathname_start = p_ch;
                    http_req->pathname_end = p_ch;
                    parser_state = RL_VERSION_START;
                } else {
                    return E_FAIL;
                }
                break;

            case RL_PATH:
                if (ch == ' ') {
                    http_req->path_end = p_ch;
                    http_req->pathname_end = p_ch;
                    http_req->uri_end = p_ch;
                    parser_state = RL_VERSION_START;
                } else if (ch == '?' || ch == '&' || ch == '=' || ch == '#') {
                    // The base pathname ends here; transition to process query params
                    http_req->pathname_end = p_ch;
                    parser_state = RL_PATHNAME;
                } else if (ch == CR || ch == LF) {
                    return E_FAIL;
                }
                break;

            case RL_PATHNAME:
                if (ch == ' ') {
                    // Full path ends here
                    http_req->path_end = p_ch;
                    http_req->uri_end = p_ch;
                    parser_state = RL_VERSION_START;
                } else if (ch == CR || ch == LF) {
                    return E_FAIL;
                }
                break;

            case RL_VERSION_START:
                if (ch == 'H') parser_state = RL_VERSION_H;
                else if (ch != ' ') return E_FAIL;
                break;

            case RL_VERSION_H:
                if (ch == 'T') parser_state = RL_VERSION_HT;
                else return E_FAIL;
                break;

            case RL_VERSION_HT:
                if (ch == 'T') parser_state = RL_VERSION_HTT;
                else return E_FAIL;
                break;

            case RL_VERSION_HTT:
                if (ch == 'P') parser_state = RL_VERSION_HTTP;
                else return E_FAIL;
                break;

            case RL_VERSION_HTTP:
                if (ch == '/') parser_state = RL_VERSION_HTTP_SLASH;
                else return E_FAIL;
                break;

            case RL_VERSION_HTTP_SLASH:
                if (ch == '1') {
                    http_req->http_major = p_ch;
                    parser_state = RL_VERSION_MAJOR;
                } else {
                    return E_FAIL;
                }
                break;

            case RL_VERSION_MAJOR:
                if (ch == '.') parser_state = RL_VERSION_DOT;
                else return E_FAIL;
                break;

            case RL_VERSION_DOT:
                if (ch == '0' || ch == '1') {
                    http_req->http_minor = p_ch;
                    parser_state = RL_VERSION_MINOR;
                } else {
                    return E_FAIL;
                }
                break;

            case RL_VERSION_MINOR:
                if (ch == CR) {
                    parser_state = RL_CR;
                } else if (ch == LF) {
                    parser_state = RL_LF;
                } else {
                    return E_FAIL;
                }
                break;

            case RL_CR:
                if (ch == LF) parser_state = RL_LF;
                else return E_FAIL;
                break;

            case RL_LF:
                if (http_req->request_line_end == NULL) {
                    http_req->request_line_end = p_ch;
                }
                http_req->parser_state = H_START;
                buff->pos = p_ch; // Leave position here, next read starts headers
                return OK;

            default:
                return E_FAIL;
        }
    }

    // Save state in case buffer ran out and we need more data
    http_req->parser_state = parser_state;
    return E_AGAIN;
}


int xps_http_parse_header_line(xps_http_req_t *http_req, xps_buffer_t *buff) {
    assert(http_req != NULL);
    assert(buff != NULL);
    u_char *p_ch = buff->pos;
    xps_http_parser_state_t parser_state = http_req->parser_state;

    for (; p_ch < buff->data + buff->len; p_ch++) {
        char ch = *p_ch;

        switch (parser_state) {
            case H_START:
                if (ch == CR) {
                    parser_state = H_LF_CR; // Encountered empty line (end of headers)
                } else if (ch == LF) {
                    parser_state = H_LF_LF;
                } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
                    http_req->header_key_start = p_ch;
                    parser_state = H_NAME;
                } else {
                    return E_FAIL;
                }
                break;

            case H_NAME:
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-') {
                    // Valid header name characters
                } else if (ch == ':') {
                    http_req->header_key_end = p_ch;
                    parser_state = H_COLON;
                } else {
                    return E_FAIL;
                }
                break;

            case H_COLON:
                if (ch == ' ') {
                    parser_state = H_SP_AFTER_COLON;
                } else if (ch != CR && ch != LF) {
                    http_req->header_val_start = p_ch;
                    parser_state = H_VAL;
                } else {
                    return E_FAIL;
                }
                break;

            case H_SP_AFTER_COLON:
                if (ch == ' ') {
                    // Keep skipping spaces
                } else if (ch != CR && ch != LF) {
                    http_req->header_val_start = p_ch;
                    parser_state = H_VAL;
                } else {
                    return E_FAIL;
                }
                break;

            case H_VAL:
                if (ch == CR || ch == LF) {
                    http_req->header_val_end = p_ch;
                    parser_state = (ch == CR) ? H_CR : H_LF;
                }
                break;

            case H_CR:
                if (ch == LF) {
                    parser_state = H_LF;
                } else {
                    return E_FAIL;
                }
                break;

            case H_LF:
                if (ch == LF) {
                    parser_state = H_LF_LF;
                } else if (ch == CR) {
                    parser_state = H_LF_CR;
                } else {
                    // Another header starts here! 
                    buff->pos = p_ch; 
                    http_req->parser_state = H_START;
                    return E_NEXT; 
                }
                break;

            case H_LF_LF:
                buff->pos = p_ch;
                http_req->parser_state = H_START;
                return OK; // HTTP complete header section done

            case H_LF_CR:
                if (ch == LF) {
                    buff->pos = p_ch + 1; // Consume final LF
                    http_req->parser_state = H_START;
                    return OK; // HTTP complete header section done
                } else {
                    return E_FAIL;
                }
                break;

            default:
                return E_FAIL;
        }
    }
    
    http_req->parser_state = parser_state;
    return E_AGAIN;
}

const char *xps_http_get_header(vec_void_t *headers, const char *key) {
    if (!headers || !key) return NULL;
    
    for (int i = 0; i < headers->length; i++) {
        xps_keyval_t *header = (xps_keyval_t *)headers->data[i];
        // strcasecmp fulfills TRY #1: Case Insensitive header checking
        if (strcasecmp(header->key, key) == 0) {
            return header->val;
        }
    }
    return NULL;
}

xps_buffer_t *xps_http_serialize_headers(vec_void_t *headers) {
    assert(headers != NULL);
    
    size_t initial_size = DEFAULT_BUFFER_SIZE;
    xps_buffer_t *buff = xps_buffer_create(0, initial_size, NULL);
    if (!buff) return NULL;
    
    // Initialize first byte to null terminator for strcat
    buff->data[0] = '\0';
    buff->len = 0;

    for (int i = 0; i < headers->length; i++) {
        xps_keyval_t *header = (xps_keyval_t *)headers->data[i];
        
        // Calculate length: Key + Value + 4 characters for ": \r\n" + 1 for null term '\0' = +5
        size_t header_str_len = strlen(header->key) + strlen(header->val) + 5;
        char header_str[header_str_len];
        
        // Format exactly as requested
        sprintf(header_str, "%s: %s\r\n", header->key, header->val);
        
        // Check if buffer is large enough; Realloc if small
        if ((buff->size - buff->len) < header_str_len) {
            size_t new_size = buff->size * 2;
            while (new_size - buff->len < header_str_len) {
                new_size *= 2;
            }
            
            u_char *new_data = realloc(buff->data, new_size);
            if (new_data == NULL) {
                logger(LOG_ERROR, "xps_http_serialize_headers()", "realloc() failed");
                xps_buffer_destroy(buff);
                return NULL;
            }
            buff->data = new_data;
            buff->size = new_size;
        }
        
        strcat((char *)buff->data, header_str);
        buff->len = strlen((char *)buff->data);
    }
    
    return buff;
}