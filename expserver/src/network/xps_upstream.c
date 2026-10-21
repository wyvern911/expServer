#include "xps_upstream.h"

xps_connection_t *xps_upstream_create(xps_core_t *core, const char *host, u_int port) {
    // validate parameter
    assert(core != NULL);
    assert(host != NULL);
    assert(is_valid_port(port));

    // Create a socket with non-blocking flag
    int sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock_fd < 0) {
        logger(LOG_ERROR, "xps_upstream_create()", "socket() failed");
        perror("Error message");
        return NULL;
    }

    // Resolve host and port using xps_getaddrinfo
    struct addrinfo *addr_info = xps_getaddrinfo(host, port);
    if (addr_info == NULL) {
        logger(LOG_ERROR, "xps_upstream_create()", "xps_getaddrinfo() failed");
        close(sock_fd);
        return NULL;
    }

    // Connect to the upstream server
    int connect_error = connect(sock_fd, addr_info->ai_addr, addr_info->ai_addrlen);
    
    // Free the addrinfo object after use
    freeaddrinfo(addr_info);

    // Because the socket is non-blocking, connect() usually returns EINPROGRESS. 
    // We treat 0 (success) or EINPROGRESS as valid connection states.
    if (!(connect_error == 0 || errno == EINPROGRESS)) {
        logger(LOG_ERROR, "xps_upstream_create()", "connect() failed");
        perror("Error message");
        close(sock_fd);
        return NULL;
    }

    // create a connection to upstream with core and sock_fd
    xps_connection_t *connection = xps_connection_create(core, sock_fd);
    if (connection == NULL) {
        logger(LOG_ERROR, "xps_upstream_create()", "xps_connection_create() failed");
        close(sock_fd);
        return NULL;
    }

    logger(LOG_DEBUG, "xps_upstream_create()", "created upstream connection");
    return connection;
}