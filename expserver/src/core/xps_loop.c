#include "xps_loop.h"

loop_event_t *loop_event_create(u_int fd, void *ptr, xps_handler_t read_cb, xps_handler_t write_cb, xps_handler_t close_cb) {
  assert(ptr != NULL);

  // Alloc memory for 'event' instance
  loop_event_t *event = malloc(sizeof(loop_event_t));
  if (event == NULL) {
    logger(LOG_ERROR, "event_create()", "malloc() failed for 'event'");
    return NULL;
  }

  /* set fd, ptr, read_cb fields of event */
  event->fd=fd;
  event->ptr=ptr;
  event->read_cb=read_cb;
  event->write_cb=write_cb;
  event->close_cb=close_cb;

  logger(LOG_DEBUG, "event_create()", "created event");
  return event;
}

void loop_event_destroy(loop_event_t *event) {
  assert(event != NULL);

  free(event);

  logger(LOG_DEBUG, "event_destroy()", "destroyed event");
}

bool handle_connections(xps_loop_t* loop) {
    for (int i=0; i<loop->core->connections.length; i++) {

        xps_connection_t* connection=loop->core->connections.data[i];

        // Check if connection is valid
        if (connection==NULL) continue;

        if (connection->read_ready) {
            connection->recv_handler(connection);
        }

        // // Connection might have been destroyed by recv_handler
        if (loop->core->connections.data[i]==NULL) continue;

        if (connection->write_ready && connection->write_buff_list->len>0) {
            connection->send_handler(connection);
        }
    }

    // Iterate through all connections once again until we find a ready connection
    for (int i=0; i<loop->core->connections.length; i++) {

        xps_connection_t* connection=loop->core->connections.data[i];

        // Event list contain null values
        if (connection==NULL) continue;

        if (connection->read_ready) return true;
        if (connection->write_ready && connection->write_buff_list->len>0) return true;
    }

    return false;
}
// ---Main loop functions---

// Creates a new event loop instance associated with the given core
xps_loop_t *xps_loop_create(xps_core_t *core) {
  assert(core != NULL);

  // Create epoll instance
  int epoll_fd=epoll_create1(0);
  if (epoll_fd<0) {
    logger(LOG_ERROR, "xps_loop_create()", "epoll_create1() failed");
    return NULL;
  }

  xps_loop_t* loop=(xps_loop_t*)malloc(sizeof(xps_loop_t));
  if (loop==NULL) {
    logger(LOG_ERROR, "xps_loop_create()", "malloc() failed");
    return NULL;
  }

  //Initialize values
  loop->core=core;
  loop->epoll_fd=epoll_fd;
  vec_init(&(loop->events));
  loop->n_null_events=0;

  logger(LOG_DEBUG, "xps_loop_create()", "created loop instance");
  return loop;

}

// Destroys the given loop instance and releases associated resources
void xps_loop_destroy(xps_loop_t *loop) {
  assert(loop != NULL);

  // Destroy all remaining loop_event_t instances in loop->events
  for (int i=0; i<loop->events.length; i++) {
    loop_event_t* event=loop->events.data[i];
    if (event!=NULL) {
        loop_event_destroy(event);
    }
  }

  vec_deinit(&(loop->events));
  close(loop->epoll_fd);
  free(loop);

  logger(LOG_DEBUG, "xps_loop_destroy()", "destroyed loop instance");

}

// Attaches a FD to be monitored using epoll
int xps_loop_attach(xps_loop_t *loop, u_int fd, int event_flags, void *ptr, xps_handler_t read_cb, xps_handler_t write_cb, xps_handler_t close_cb) {
  assert(loop != NULL);
  assert(ptr != NULL);

  // Create the loop_event_t container
  loop_event_t* event=loop_event_create(fd, ptr, read_cb, write_cb, close_cb);
  if (event==NULL) {
    logger(LOG_ERROR, "xps_loop_attach", "loop_event_create() failed");
    return E_FAIL;
  }

  // Configure epoll event
  struct epoll_event ep_event;
  ep_event.events=event_flags;
  ep_event.data.ptr=event;

  // Add to epoll instance
  if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ep_event)==-1) {
    logger(LOG_ERROR, "xps_loop_attach()", "epoll_ctl ADD failed for fd: %d", fd);
    loop_event_destroy(event);
    return E_FAIL;
  }

  // Add the pointer to loop_event_t to the events list
  vec_push(&(loop->events), event);

  logger(LOG_DEBUG, "xps_loop_attach()", "attached fd %d to loop", fd);
  return OK;

}

// Remove FD from epoll
int xps_loop_detach(xps_loop_t *loop, u_int fd) {
  assert(loop != NULL);

  // Detach from epoll
  if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL)==-1) {
    logger(LOG_ERROR, "xps_loop_detach()", "epoll_ctl DEL failed for fd: %d", fd);
    return E_FAIL;
  }

  // Find the instance of loop_event_t from loop->events that matches fd
  for (int i=0; i<loop->events.length; i++) {
    loop_event_t* event=loop->events.data[i];
    if (event && event->fd==fd) {
        loop_event_destroy(event);
        loop->events.data[i]=NULL;
        (loop->n_null_events)++;
        logger(LOG_DEBUG, "xps_loop_detach()", "detached fd %d", fd);
        return OK;
    }
  }

  logger(LOG_WARNING, "xps_loop_detach()", "event for fd %d not found in list", fd);
  return E_FAIL;
}

void xps_loop_run(xps_loop_t *loop) {
  assert(loop!=NULL);

  while (1) {
    logger(LOG_DEBUG, "xps_loop_run()", "epoll wait");

    bool has_ready_connections=handle_connections(loop);
    int timeout=(has_ready_connections) ? 0 : -1;

    // Epoll Wait
    int n_events = epoll_wait(loop->epoll_fd, loop->epoll_events, MAX_EPOLL_EVENTS, timeout);
    if (n_events==-1) {
        logger(LOG_ERROR, "xps_loop_run()", "epoll_wait failed");
        break;
    }

    logger(LOG_DEBUG, "xps_loop_run()", "epoll wait over");

    logger(LOG_DEBUG, "xps_loop_run()", "handling %d events", n_events);

    // Handle events
    for (int i = 0; i < n_events; i++) {
      logger(LOG_DEBUG, "xps_loop_run()", "handling event no. %d", i + 1);

      struct epoll_event curr_epoll_event = loop->epoll_events[i];
      loop_event_t *curr_event = curr_epoll_event.data.ptr;

      // Check if event still exists. Could have been destroyed due to prev event
      int curr_event_idx = -1;
      for (int j=0; j<loop->events.length; j++) {
        if (loop->events.data[j]==curr_event) {
            curr_event_idx=j;
            break;
        }
      }

      if (curr_event_idx == -1) {
        logger(LOG_DEBUG, "handle_epoll_events()", "event not found. skipping");
        continue;
      }

      // Close event
      if (curr_epoll_event.events & (EPOLLERR | EPOLLHUP)) {
        logger(LOG_DEBUG, "handle_epoll_events()", "EVENT / close");
        if (curr_event->close_cb != NULL) {

          // Pass the ptr from loop_event_t as a parameter to the callback
          curr_event->close_cb(curr_event->ptr);
        }
      }

      // Check if event still exists. Could have been destroyed due to prev event
      curr_event_idx = -1;
      for (int j=0; j<loop->events.length; j++) {
        if (loop->events.data[j]==curr_event) {
            curr_event_idx=j;
            break;
        }
      }

      if (curr_event_idx == -1) {
        logger(LOG_DEBUG, "handle_epoll_events()", "event not found. skipping");
        continue;
      }

      // Read event
      if (curr_epoll_event.events & EPOLLIN) {
        logger(LOG_DEBUG, "handle_epoll_events()", "EVENT / read");
        if (curr_event->read_cb != NULL) {

          // Pass the ptr from loop_event_t as a parameter to the callback
          curr_event->read_cb(curr_event->ptr);
        }
      }

      // Check if event still exists. Could have been destroyed due to prev event
      curr_event_idx = -1;
      for (int j=0; j<loop->events.length; j++) {
        if (loop->events.data[j]==curr_event) {
            curr_event_idx=j;
            break;
        }
      }

      if (curr_event_idx == -1) {
        logger(LOG_DEBUG, "handle_epoll_events()", "event not found. skipping");
        continue;
      }

      // Write event
      if (curr_epoll_event.events & EPOLLOUT) {
        logger(LOG_DEBUG, "handle_epoll_events()", "EVENT / write");
        if (curr_event->write_cb != NULL) {

          // Pass the ptr from loop_event_t as a parameter to the callback
          curr_event->write_cb(curr_event->ptr);
        }
      }
    }
  }
}