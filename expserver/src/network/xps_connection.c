#include "xps_connection.h"

void connection_read_handler(void* ptr);
void connection_write_handler(void* ptr);
void connection_loop_read_handler(void* ptr);
void connection_loop_write_handler(void* ptr);
void connection_loop_close_handler(void* ptr);

xps_connection_t *xps_connection_create(xps_core_t* core, u_int sock_fd) {
  xps_connection_t *connection = malloc(sizeof(xps_connection_t));
  if (connection == NULL) {
    logger(LOG_ERROR, "xps_connection_create()", "malloc() failed for 'connection'");
    return NULL;
  }

  // attach sock_fd to epoll
  xps_loop_attach(core->loop, sock_fd, EPOLLIN | EPOLLOUT | EPOLLET, connection, connection_loop_read_handler, connection_loop_write_handler, connection_loop_close_handler);

  // Init values
  connection->core = core;
  connection->sock_fd = sock_fd;
  connection->listener = NULL;
  connection->write_buff_list=xps_buffer_list_create();
  connection->remote_ip = get_remote_ip(sock_fd);
  connection->read_ready=false;
  connection->write_ready=false;
  connection->recv_handler=connection_read_handler;
  connection->send_handler=connection_write_handler;

  // add connection to 'connections' list
  vec_push(&core->connections, connection);
  
  logger(LOG_DEBUG, "xps_connection_create()", "created connection");
  return connection;
}

void xps_connection_destroy(xps_connection_t *connection) {
  // validate params
  assert(connection != NULL);

  // set connection to NULL in 'connections' list
  for (int i = 0; i < connection->core->connections.length; i++) {
    xps_connection_t *curr = connection->core->connections.data[i];
    if (curr == connection) {
      connection->core->connections.data[i] = NULL;
      connection->core->n_null_connections++;
      break;
    }
  }

  // detach connection from loop
  xps_loop_detach(connection->core->loop, connection->sock_fd);

  // close connection socket FD
  close(connection->sock_fd);

  // free connection->remote_ip
  if (connection->remote_ip != NULL) {
      free(connection->remote_ip);
  }

  // Destroy buffer list if exists
  if (connection->write_buff_list != NULL) {
    xps_buffer_list_destroy(connection->write_buff_list);
  }

  // free connection instance
  free(connection);
  
  logger(LOG_DEBUG, "xps_connection_destroy()", "destroyed connection");
}

void connection_read_handler(void* ptr) {
  // validate params
  assert(ptr != NULL);

  xps_connection_t* connection=ptr;
  
  u_char buff[DEFAULT_BUFFER_SIZE];

  // read data from client using recv()
  long read_n = recv(connection->sock_fd, buff, DEFAULT_BUFFER_SIZE - 1, 0);
  
  if (read_n < 0) {

    if (errno==EAGAIN || errno==EWOULDBLOCK) { // There is nothing to read, ie, receive buffer is empty. So make read_ready false
      connection->read_ready=false;
      return;

    } else { // Some crash has occured
      logger(LOG_ERROR, "xps_connection_read_handler()", "recv() failed");
      perror("Error message");
      xps_connection_destroy(connection);
      return;

    }
  }

  if (read_n == 0) {
    logger(LOG_INFO, "connection_read_handler()", "peer closed connection");
    xps_connection_destroy(connection);
    return;
  }

  buff[read_n] = '\0';
  
  // print client message
  logger(LOG_DEBUG, "xps_connection_read_handler()", "Client Message: %s", buff);

  // Reverse client message (ignoring the single trailing '\n')
  long str_len = read_n - 1; 

  for (long i = 0; i < str_len / 2; i++) {
      u_char temp = buff[i];
      buff[i] = buff[str_len - 1 - i];
      buff[str_len - 1 - i] = temp;
  }

  // Create a buffer instance. Pass NULL so xps_buffer allocates its own memory
  xps_buffer_t* buffer=xps_buffer_create(read_n, read_n, NULL);
  if (buffer==NULL) {
    logger(LOG_ERROR, "xps_connection_read_handler()", "xps_buffer_create() failed");
    perror("Error Message");
    return;
  }

  // Copy the reversed data into the new buffer's memory
  memcpy(buffer->data, buff, read_n);

  // Append buffer to buffer list
  xps_buffer_list_append(connection->write_buff_list, buffer);
}

void connection_loop_read_handler(void* ptr) {
  assert(ptr!=NULL);

  xps_connection_t* connection=ptr;
  connection->read_ready=true;
}

void connection_write_handler(void* ptr) {
  assert(ptr!=NULL);

  xps_connection_t* connection=ptr;

  // EARLY RETURN: To resolve the assert(len>0) in xps_buffer_list_read()
  if (connection->write_buff_list->len == 0) {
      return;
  }

  // Read from the buffer list
  xps_buffer_t* buffer=xps_buffer_list_read(connection->write_buff_list, connection->write_buff_list->len);
  if (buffer==NULL) {
    logger(LOG_ERROR, "xps_connection_write_handler()", "xps_buffer_list_read() failed");
    perror("Error Message");
    return;
  }

  // Send to the client
  long write_n=send(connection->sock_fd, buffer->data, connection->write_buff_list->len, 0);
  if (write_n>0) { // Some part is sent
    // Clear the amount of data sent
    xps_buffer_list_clear(connection->write_buff_list, write_n);

    // Destroy the temp buffer instance
    xps_buffer_destroy(buffer);

  } else if (write_n==-1) {
    
    if (errno==EAGAIN || errno==EWOULDBLOCK) { // Kernel buffer is full

      // Since kernel buffer is full, socket is not ready to write, so make write_ready false
      connection->write_ready=false;

      xps_buffer_destroy(buffer);
      return;

    } else { // Some crash occured

      // Destroy temp buffer and connection too
      xps_buffer_destroy(buffer);
      xps_connection_destroy(connection);
      return;
    }
  }
}

void connection_loop_write_handler(void* ptr) {
  assert(ptr!=NULL);

  xps_connection_t* connection=ptr;
  connection->write_ready=true;
}

void connection_loop_close_handler(void* ptr) {
  // Destroy the connection instance
  xps_connection_t* connection=ptr;
  xps_connection_destroy(connection);
}