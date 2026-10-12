#include "xps_connection.h"

void connection_source_handler(void *ptr);
void connection_source_close_handler(void *ptr);
void connection_sink_handler(void *ptr);
void connection_sink_close_handler(void *ptr);

void connection_loop_read_handler(void *ptr);
void connection_loop_write_handler(void *ptr);
void connection_loop_close_handler(void *ptr);

void connection_close(xps_connection_t *connection, bool peer_closed);

xps_connection_t *xps_connection_create(xps_core_t *core, u_int sock_fd) {
    // validate params
    assert(core != NULL);

    // Create & allocate memory for a connection instance
    xps_connection_t *connection = malloc(sizeof(xps_connection_t));
    if (connection == NULL) {
        logger(LOG_ERROR, "xps_connection_create()", "malloc() failed for 'connection'");
        return NULL;
    }

    // Create source instance
    xps_pipe_source_t *source = xps_pipe_source_create(connection, connection_source_handler, connection_source_close_handler);
    if (source == NULL) {
        logger(LOG_ERROR, "xps_connection_create()", "xps_pipe_source_create() failed");
        free(connection);
        return NULL;
    }

    // Create sink instance
    xps_pipe_sink_t *sink = xps_pipe_sink_create(connection, connection_sink_handler, connection_sink_close_handler);
    if (sink == NULL) {
        logger(LOG_ERROR, "xps_connection_create()", "xps_pipe_sink_create() failed");
        xps_pipe_source_destroy(source);
        free(connection);
        return NULL;
    }

    // Init values
    connection->core = core;
    connection->sock_fd = sock_fd;
    connection->listener = NULL;
    connection->remote_ip = get_remote_ip(sock_fd);
    connection->source = source;
    connection->sink = sink;

    // attach sock_fd to epoll
    if (xps_loop_attach(core->loop, sock_fd, EPOLLIN | EPOLLOUT | EPOLLET, connection, 
                        connection_loop_read_handler, connection_loop_write_handler, connection_loop_close_handler) != OK) {
        logger(LOG_ERROR, "xps_connection_create()", "xps_loop_attach() failed");
        xps_pipe_source_destroy(source);
        xps_pipe_sink_destroy(sink);
        if (connection->remote_ip != NULL) free(connection->remote_ip);
        free(connection);
        return NULL;
    }

    // add connection to 'connections' list
    vec_push(&core->connections, connection);
    
    logger(LOG_DEBUG, "xps_connection_create()", "created connection");
    return connection;
}

void xps_connection_destroy(xps_connection_t *connection) {
    // validate params
    assert(connection != NULL);

    // detach connection from loop
    xps_loop_detach(connection->core->loop, connection->sock_fd);

    // set connection to NULL in 'connections' list
    for (int i = 0; i < connection->core->connections.length; i++) {
        if (connection->core->connections.data[i] == connection) {
            connection->core->connections.data[i] = NULL;
            connection->core->n_null_connections++;
            break;
        }
    }

    // destroy source and sink
    if (connection->source != NULL) xps_pipe_source_destroy(connection->source);
    if (connection->sink != NULL) xps_pipe_sink_destroy(connection->sink);

    // close connection socket FD
    close(connection->sock_fd);

    // free connection->remote_ip
    if (connection->remote_ip != NULL) {
        free(connection->remote_ip);
    }

    // free connection instance
    free(connection);
    
    logger(LOG_DEBUG, "xps_connection_destroy()", "destroyed connection");
}

void connection_source_handler(void *ptr) {
    // validate params
    assert(ptr != NULL);
    xps_pipe_source_t *source = ptr;
    xps_connection_t *connection = source->ptr;

    // Create a buffer instance. Pass NULL so xps_buffer allocates its own memory
    xps_buffer_t *buff = xps_buffer_create(DEFAULT_BUFFER_SIZE, DEFAULT_BUFFER_SIZE, NULL);
    if (buff == NULL) {
        logger(LOG_ERROR, "connection_source_handler()", "xps_buffer_create() failed");
        return;
    }

    // update the recv() function parameter to limit the data read up to pipe threshold limit
    size_t available = source->pipe->buff_thresh - source->pipe->buff_list->len;
    size_t read_limit = (DEFAULT_BUFFER_SIZE < available) ? DEFAULT_BUFFER_SIZE : available;

    // read data from client using recv()
    long read_n = recv(connection->sock_fd, buff->data, read_limit, 0);
    buff->len = (read_n > 0) ? read_n : 0;

    if (read_n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // There is nothing to read, ie, receive buffer is empty. So make source ready false
        xps_buffer_destroy(buff);
        source->ready = false;
        return;
    }

    if (read_n < 0) {
        // Some crash has occured
        xps_buffer_destroy(buff);
        logger(LOG_ERROR, "connection_source_handler()", "recv() failed");
        connection_close(connection, false);
        return;
    }

    if (read_n == 0) {
        // Peer closed connection
        xps_buffer_destroy(buff);
        connection_close(connection, true);
        return;
    }

    // write buffer to pipe
    if (xps_pipe_source_write(source, buff) != OK) {
        logger(LOG_ERROR, "connection_source_handler()", "xps_pipe_source_write() failed");
        xps_buffer_destroy(buff);
        connection_close(connection, false);
        return;
    }

    // Destroy the temp buffer instance
    xps_buffer_destroy(buff);
}

void connection_source_close_handler(void *ptr) {
    // validate params
    assert(ptr != NULL);
    xps_pipe_source_t *source = ptr;
    xps_connection_t *connection = source->ptr;

    // if both the source and sink not active, close the connection
    if (!connection->source->active && !connection->sink->active) {
        connection_close(connection, false);
    }
}

void connection_sink_handler(void *ptr) {
    // validate params
    assert(ptr != NULL);
    xps_pipe_sink_t *sink = ptr;
    xps_connection_t *connection = sink->ptr;

    // chunk limit check
    size_t read_len = sink->pipe->buff_list->len;
    if (read_len > DEFAULT_BUFFER_SIZE) {
        read_len = DEFAULT_BUFFER_SIZE;
    }

    // read from the pipe
    xps_buffer_t *buff = xps_pipe_sink_read(sink, read_len);
    if (buff == NULL) {
        logger(LOG_ERROR, "connection_sink_handler()", "xps_pipe_sink_read() failed");
        return;
    }

    // Send to the client
    long write_n = send(connection->sock_fd, buff->data, buff->len, MSG_NOSIGNAL);
    
    // Destroy the temp buffer instance
    xps_buffer_destroy(buff);

    if (write_n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Since kernel buffer is full, socket is not ready to write, so make sink ready false
        sink->ready = false;
        return;
    }

    if (write_n < 0) {
        // Some crash occured
        logger(LOG_ERROR, "connection_sink_handler()", "send() failed");
        connection_close(connection, false);
        return;
    }

    // EARLY RETURN
    if (write_n == 0) return;

    // Clear the amount of data sent from the pipe buffer
    if (xps_pipe_sink_clear(sink, write_n) != OK) {
        logger(LOG_ERROR, "connection_sink_handler()", "failed to clear sink");
    }
}

void connection_sink_close_handler(void *ptr) {
    // validate params
    assert(ptr != NULL);
    xps_pipe_sink_t *sink = ptr;
    xps_connection_t *connection = sink->ptr;

    // if both the source and sink not active, close the connection
    if (!connection->source->active && !connection->sink->active) {
        connection_close(connection, false);
    }
}

void connection_loop_read_handler(void *ptr) {
    assert(ptr != NULL);
    xps_connection_t *connection = ptr;
    // indicate the source is ready to receive data and write to pipe
    connection->source->ready = true;
}

void connection_loop_write_handler(void *ptr) {
    assert(ptr != NULL);
    xps_connection_t *connection = ptr;
    // indicate the sink is ready to read data from pipe and send
    connection->sink->ready = true;
}

void connection_loop_close_handler(void *ptr) {
    // Destroy the connection instance
    assert(ptr != NULL);
    xps_connection_t *connection = ptr;
    connection_close(connection, true);
}

void connection_close(xps_connection_t *connection, bool peer_closed) {
    assert(connection != NULL);
    logger(LOG_INFO, "connection_close()", peer_closed ? "peer closed connection" : "closing connection");
    // destroy connection
    xps_connection_destroy(connection);
}