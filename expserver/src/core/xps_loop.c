#include "xps_loop.h"

loop_event_t *loop_event_create(u_int fd, void *ptr, xps_handler_t read_cb, xps_handler_t write_cb, xps_handler_t close_cb) {
  assert(ptr != NULL);

  // Alloc memory for 'event' instance
  loop_event_t *event = malloc(sizeof(loop_event_t));
  if (event == NULL) {
    logger(LOG_ERROR, "event_create()", "malloc() failed for 'event'");
    return NULL;
  }

  // set fd, ptr, read_cb fields of event
  event->fd = fd;
  event->ptr = ptr;
  event->read_cb = read_cb;
  event->write_cb = write_cb;
  event->close_cb = close_cb;

  logger(LOG_DEBUG, "event_create()", "created event");
  return event;
}

void loop_event_destroy(loop_event_t *event) {
  assert(event != NULL);
  free(event);
  logger(LOG_DEBUG, "event_destroy()", "destroyed event");
}

// --- Pipe Handlers ---

bool handle_pipes(xps_loop_t *loop) {
    assert(loop != NULL);

    // Pass 1: Execute callbacks for ready pipes
    for (int i = 0; i < loop->core->pipes.length; i++) {
        xps_pipe_t *pipe = loop->core->pipes.data[i];
        if (pipe == NULL) continue;

        // Destroy the pipe if it has no source and sink and continue
        if (pipe->source == NULL && pipe->sink == NULL) {
            xps_pipe_destroy(pipe);
            continue;
        }

        // Pipe has source AND source is ready AND pipe is writable
        if (pipe->source != NULL && pipe->source->ready && xps_pipe_is_writable(pipe)) {
            pipe->source->handler_cb(pipe->source); // call connection_source_handler to write into pipe
        }
        
        // Pipe has sink AND sink is ready AND pipe is readable
        if (pipe->sink != NULL && pipe->sink->ready && xps_pipe_is_readable(pipe)) {
            pipe->sink->handler_cb(pipe->sink); // call connection_sink_handler to read from pipe
        }
        
        // Pipe has source and no sink
        if (pipe->source != NULL && pipe->sink == NULL) {
            pipe->source->active = false;
            if (pipe->source->close_cb) pipe->source->close_cb(pipe->source);
        }
        
        // Pipe has sink and no source and pipe is not readable
        if (pipe->sink != NULL && pipe->source == NULL && !xps_pipe_is_readable(pipe)) {
            pipe->sink->active = false;
            if (pipe->sink->close_cb) pipe->sink->close_cb(pipe->sink);
        }
    }

    // Pass 2: Check for any remaining ready state
    for (int i = 0; i < loop->core->pipes.length; i++) {
        xps_pipe_t *pipe = loop->core->pipes.data[i];
        if (pipe == NULL) {
            continue;
        }

        if (pipe->source != NULL && pipe->source->ready && xps_pipe_is_writable(pipe)) {
            return true;
        }
        if (pipe->sink != NULL && pipe->sink->ready && xps_pipe_is_readable(pipe)) {
            return true;
        }
        if (pipe->source != NULL && pipe->sink == NULL) {
            return true;
        }
        if (pipe->sink != NULL && pipe->source == NULL && !xps_pipe_is_readable(pipe)) {
            return true;
        }
    }
    
    return false;
}

void filter_nulls(xps_core_t *core) {
    if (core->loop->n_null_events > DEFAULT_NULLS_THRESH) {
        vec_filter_null(&core->loop->events);
        core->loop->n_null_events = 0;
    }
    if (core->n_null_listeners > DEFAULT_NULLS_THRESH) {
        vec_filter_null(&core->listeners);
        core->n_null_listeners = 0;
    }
    if (core->n_null_connections > DEFAULT_NULLS_THRESH) {
        vec_filter_null(&core->connections);
        core->n_null_connections = 0;
    }
    if (core->n_null_pipes > DEFAULT_NULLS_THRESH) {
        vec_filter_null(&core->pipes);
        core->n_null_pipes = 0;
    }
    if (core->n_null_sessions > DEFAULT_NULLS_THRESH) {
        vec_filter_null(&core->sessions);
        core->n_null_sessions = 0;
    }
}

// --- Epoll Event Handler ---

void handle_epoll_events(xps_loop_t *loop, int n_events) {
    logger(LOG_DEBUG, "handle_epoll_events()", "handling %d events", n_events);

    for (int i = 0; i < n_events; i++) {
        logger(LOG_DEBUG, "handle_epoll_events()", "handling event no. %d", i + 1);

        struct epoll_event curr_epoll_event = loop->epoll_events[i];
        loop_event_t *curr_event = curr_epoll_event.data.ptr;

        // Check if event still exists. Could have been destroyed due to prev event
        int curr_event_idx = -1;
        for (int j = 0; j < loop->events.length; j++) {
            if (loop->events.data[j] == curr_event) {
                curr_event_idx = j;
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
                curr_event->close_cb(curr_event->ptr);
            }
        }

        // Re-check existence
        curr_event_idx = -1;
        for (int j = 0; j < loop->events.length; j++) {
            if (loop->events.data[j] == curr_event) {
                curr_event_idx = j;
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
                curr_event->read_cb(curr_event->ptr);
            }
        }

        // Re-check existence
        curr_event_idx = -1;
        for (int j = 0; j < loop->events.length; j++) {
            if (loop->events.data[j] == curr_event) {
                curr_event_idx = j;
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
                curr_event->write_cb(curr_event->ptr);
            }
        }
    }
}

// --- Main loop functions ---

xps_loop_t *xps_loop_create(xps_core_t *core) {
  assert(core != NULL);

  // Create epoll instance
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    logger(LOG_ERROR, "xps_loop_create()", "epoll_create1() failed");
    return NULL;
  }

  xps_loop_t *loop = (xps_loop_t *)malloc(sizeof(xps_loop_t));
  if (loop == NULL) {
    logger(LOG_ERROR, "xps_loop_create()", "malloc() failed");
    return NULL;
  }

  // Initialize values
  loop->core = core;
  loop->epoll_fd = epoll_fd;
  vec_init(&(loop->events));
  loop->n_null_events = 0;

  logger(LOG_DEBUG, "xps_loop_create()", "created loop instance");
  return loop;
}

void xps_loop_destroy(xps_loop_t *loop) {
  assert(loop != NULL);

  // Destroy all remaining loop_event_t instances in loop->events
  for (int i = 0; i < loop->events.length; i++) {
    loop_event_t *event = loop->events.data[i];
    if (event != NULL) {
        loop_event_destroy(event);
    }
  }

  vec_deinit(&(loop->events));
  close(loop->epoll_fd);
  free(loop);

  logger(LOG_DEBUG, "xps_loop_destroy()", "destroyed loop instance");
}

int xps_loop_attach(xps_loop_t *loop, u_int fd, int event_flags, void *ptr, xps_handler_t read_cb, xps_handler_t write_cb, xps_handler_t close_cb) {
  assert(loop != NULL);
  assert(ptr != NULL);

  // Create the loop_event_t container
  loop_event_t *event = loop_event_create(fd, ptr, read_cb, write_cb, close_cb);
  if (event == NULL) {
    logger(LOG_ERROR, "xps_loop_attach", "loop_event_create() failed");
    return E_FAIL;
  }

  // Configure epoll event
  struct epoll_event ep_event;
  ep_event.events = event_flags;
  ep_event.data.ptr = event;

  // Add to epoll instance
  if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ep_event) == -1) {
    logger(LOG_ERROR, "xps_loop_attach()", "epoll_ctl ADD failed for fd: %d", fd);
    loop_event_destroy(event);
    return E_FAIL;
  }

  // Add the pointer to loop_event_t to the events list
  vec_push(&(loop->events), event);

  logger(LOG_DEBUG, "xps_loop_attach()", "attached fd %d to loop", fd);
  return OK;
}

int xps_loop_detach(xps_loop_t *loop, u_int fd) {
  assert(loop != NULL);

  // Detach from epoll
  if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
    logger(LOG_ERROR, "xps_loop_detach()", "epoll_ctl DEL failed for fd: %d", fd);
    return E_FAIL;
  }

  // Find the instance of loop_event_t from loop->events that matches fd
  for (int i = 0; i < loop->events.length; i++) {
    loop_event_t *event = loop->events.data[i];
    if (event && event->fd == fd) {
        loop_event_destroy(event);
        loop->events.data[i] = NULL;
        (loop->n_null_events)++;
        logger(LOG_DEBUG, "xps_loop_detach()", "detached fd %d", fd);
        return OK;
    }
  }

  logger(LOG_WARNING, "xps_loop_detach()", "event for fd %d not found in list", fd);
  return E_FAIL;
}

void xps_loop_run(xps_loop_t *loop) {
  assert(loop != NULL);
  logger(LOG_DEBUG, "xps_loop_run()", "starting to run loop");

  while (1) {
    logger(LOG_DEBUG, "xps_loop_run()", "loop top");

    // Handle pipes
    bool has_ready_pipes = handle_pipes(loop);
    int timeout = (has_ready_pipes) ? 0 : -1;

    logger(LOG_DEBUG, "xps_loop_run()", "epoll waiting");

    // Epoll Wait
    int n_events = epoll_wait(loop->epoll_fd, loop->epoll_events, MAX_EPOLL_EVENTS, timeout);
    
    logger(LOG_DEBUG, "xps_loop_run()", "epoll wait over");

    if (n_events < 0) {
        logger(LOG_ERROR, "xps_loop_run()", "epoll_wait() error");
        break; // or continue, depending on error handling strategy
    }

    // Handle epoll events
    if (n_events > 0) {
        handle_epoll_events(loop, n_events);
    }

    // Filter NULLs from vec lists
    filter_nulls(loop->core);
  }
}