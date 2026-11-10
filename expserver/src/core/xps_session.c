#include "xps_session.h"

// Forward declarations of handler functions
void session_process_request(xps_session_t *session);
void client_source_handler(void *ptr);
void client_source_close_handler(void *ptr);
void client_sink_handler(void *ptr);
void client_sink_close_handler(void *ptr);
void upstream_source_handler(void *ptr);
void upstream_source_close_handler(void *ptr);
void upstream_sink_handler(void *ptr);
void upstream_sink_close_handler(void *ptr);
void file_sink_handler(void *ptr);
void file_sink_close_handler(void *ptr);
void upstream_error_res(xps_session_t *session);
void set_to_client_buff(xps_session_t *session, xps_buffer_t *buff);
void set_from_client_buff(xps_session_t *session, xps_buffer_t *buff);
void session_check_destroy(xps_session_t *session);

xps_session_t *xps_session_create(xps_core_t *core, xps_connection_t *client) {
  /* validate parameters */
  assert(core != NULL && client != NULL);

  // Allocates memory and initializes session instance
  xps_session_t *session = malloc(sizeof(xps_session_t));
  if (session == NULL) {
    logger(LOG_ERROR, "xps_session_create()", "malloc() failed for 'session'");
    return NULL;
  }

  // Create pipe sources and sinks for the session
  session->client_source = xps_pipe_source_create(session, client_source_handler, client_source_close_handler);
  session->client_sink = xps_pipe_sink_create(session, client_sink_handler, client_sink_close_handler);
  session->upstream_source = xps_pipe_source_create(session, upstream_source_handler, upstream_source_close_handler);
  session->upstream_sink = xps_pipe_sink_create(session, upstream_sink_handler, upstream_sink_close_handler);
  session->file_sink = xps_pipe_sink_create(session, file_sink_handler, file_sink_close_handler);

  if (!(session->client_source && session->client_sink && session->upstream_source &&
        session->upstream_sink && session->file_sink)) {
    logger(LOG_ERROR, "xps_session_create()", "failed to create some sources/sinks");
    if (session->client_source) xps_pipe_source_destroy(session->client_source);
    if (session->client_sink) xps_pipe_sink_destroy(session->client_sink);
    if (session->upstream_source) xps_pipe_source_destroy(session->upstream_source);
    if (session->upstream_sink) xps_pipe_sink_destroy(session->upstream_sink);
    if (session->file_sink) xps_pipe_sink_destroy(session->file_sink);
    free(session);
    return NULL;
  }

  // Init values
  session->core = core;
  session->client = client;
  session->upstream = NULL;

  session->http_req = NULL;
  session->lookup = NULL;
  
  session->upstream_connected = false;
  session->upstream_error_res_set = false;
  session->upstream_write_bytes = 0;
  session->file = NULL;
  session->to_client_buff = NULL;
  session->from_client_buff = NULL;

  session->client_sink->ready = true;
  session->upstream_sink->ready = true;
  session->file_sink->ready = true;

  // Add the session instance to core's session list
  vec_push(&core->sessions, session);

  // Create pipe between session->client_source and client->sink, as well as between client->source and session->client_sink
  if (xps_pipe_create(core, DEFAULT_PIPE_BUFF_THRESH, client->source, session->client_sink) == NULL ||
      xps_pipe_create(core, DEFAULT_PIPE_BUFF_THRESH, session->client_source, client->sink) == NULL) {
    logger(LOG_ERROR, "xps_session_create()", "failed to create client pipes");
    if (session->client_source) xps_pipe_source_destroy(session->client_source);
    if (session->client_sink) xps_pipe_sink_destroy(session->client_sink);
    if (session->upstream_source) xps_pipe_source_destroy(session->upstream_source);
    if (session->upstream_sink) xps_pipe_sink_destroy(session->upstream_sink);
    if (session->file_sink) xps_pipe_sink_destroy(session->file_sink);
    free(session);
    return NULL;
  }

  logger(LOG_DEBUG, "xps_session_create()", "created session");

  return session;
}

// To process http request
void session_process_request(xps_session_t *session) {
    assert(session != NULL);
    xps_http_res_t *res = NULL;

    // Bad Request
    if (session->http_req == NULL || session->http_req->path == NULL) {
        res = xps_http_res_create(session->core, HTTP_BAD_REQUEST);
        xps_buffer_t *buff = xps_http_res_serialize(res);
        set_to_client_buff(session, buff);
        xps_http_res_destroy(res);
        return;
    }

    /* config_lookup called */
    int lookup_error;
    xps_config_lookup_t *lookup = xps_config_lookup(session->core->config, session->http_req, session->client, &lookup_error);
    session->lookup = lookup;

    /* handle lookup error with appropriate response (eg: internal_server_error, not found) */
    if (lookup == NULL) {
        if (lookup_error == E_NOTFOUND) {
            res = xps_http_res_create(session->core, HTTP_NOT_FOUND);
        } else {
            res = xps_http_res_create(session->core, HTTP_INTERNAL_SERVER_ERROR);
        }
        xps_buffer_t *buff = xps_http_res_serialize(res);
        set_to_client_buff(session, buff);
        xps_http_res_destroy(res);
        return;
    }

    // 1. Check Whitelist
    if (lookup->ip_whitelist.length > 0) {
        const char *client_ip = session->client->remote_ip;
        bool is_allowed = false;
        
        for (size_t i = 0; i < lookup->ip_whitelist.length; i++) {
            const char *ip_w = lookup->ip_whitelist.data[i];
            if (strcmp(client_ip, ip_w) == 0) {
                is_allowed = true;
                break;
            }
        }
        
        if (!is_allowed) {
            logger(LOG_DEBUG, "session_process_request()", "client ip %s is not whitelisted", client_ip);
            res = xps_http_res_create(session->core, HTTP_FORBIDDEN);
            xps_buffer_t *http_res_buff = xps_http_res_serialize(res);
            set_to_client_buff(session, http_res_buff);
            xps_http_res_destroy(res);
            return;
        }
    }

    // 2. Check Blacklist
    if (lookup->ip_blacklist.length > 0) {
        const char *client_ip = session->client->remote_ip;
        
        for (size_t i = 0; i < lookup->ip_blacklist.length; i++) {
            const char *ip_b = lookup->ip_blacklist.data[i];
            if (strcmp(client_ip, ip_b) == 0) {
                logger(LOG_DEBUG, "session_process_request()", "client ip %s is blacklisted", client_ip);
                res = xps_http_res_create(session->core, HTTP_FORBIDDEN);
                xps_buffer_t *http_res_buff = xps_http_res_serialize(res);
                set_to_client_buff(session, http_res_buff);
                xps_http_res_destroy(res);
                return;
            }
        }
    }

    if (lookup->type == REQ_FILE_SERVE) {
        if (lookup->file_path) {
            int error;
            session->file = xps_file_create(session->core, lookup->file_path, &error);

            // File Errors
            if (session->file == NULL) {
                if (error == E_PERMISSION) {
                    res = xps_http_res_create(session->core, HTTP_FORBIDDEN);
                } else {
                    res = xps_http_res_create(session->core, HTTP_NOT_FOUND);
                }
                xps_buffer_t *buff = xps_http_res_serialize(res);
                set_to_client_buff(session, buff);
                xps_http_res_destroy(res);
                return;
            }

            // Success (200 OK)
            res = xps_http_res_create(session->core, HTTP_OK);

            // MIME type fallback (Fix for Tests 1 & 2)
            const char *mime = session->file->mime_type;
            if (!mime) {
                if (strstr(lookup->file_path, ".jpg") || strstr(lookup->file_path, ".jpeg")) mime = "image/jpeg";
                else if (strstr(lookup->file_path, ".pdf")) mime = "application/pdf";
                else if (strstr(lookup->file_path, ".txt")) mime = "text/plain";
            }

            if (mime) {
                xps_http_set_header(&res->headers, "Content-Type", mime);
            }

            xps_buffer_t *buff = xps_http_res_serialize(res);
            set_to_client_buff(session, buff);
            xps_http_res_destroy(res);

            xps_pipe_create(session->core, DEFAULT_PIPE_BUFF_THRESH, session->file->source, session->file_sink);
        } else if (lookup->dir_path) {
            xps_buffer_t *dir_html = xps_directory_browsing(lookup->dir_path, session->http_req->path);
        
            // Create the response object
            xps_http_res_t *res;
            if (dir_html != NULL) {
                res = xps_http_res_create(session->core, HTTP_OK);
                xps_http_set_header(&res->headers, "Content-Type", "text/html");
                xps_http_res_set_body(res, dir_html);
            } else {
                res = xps_http_res_create(session->core, HTTP_INTERNAL_SERVER_ERROR);
            }

            // Serialize and prep to send
            xps_buffer_t *http_res_buff = xps_http_res_serialize(res);
            set_to_client_buff(session, http_res_buff);
            
            xps_http_res_destroy(res);
            return;
        } else {
            res = xps_http_res_create(session->core, HTTP_NOT_FOUND);
            xps_buffer_t *buff = xps_http_res_serialize(res);
            set_to_client_buff(session, buff);
            xps_http_res_destroy(res);
            return;
        }
    } else if (lookup->type == REQ_REVERSE_PROXY) {
        char host[256];
        u_int port = 0;
        if (lookup->upstream == NULL || sscanf(lookup->upstream, "%[^:]:%u", host, &port) != 2) {
            res = xps_http_res_create(session->core, HTTP_BAD_GATEWAY);
            xps_buffer_t *buff = xps_http_res_serialize(res);
            set_to_client_buff(session, buff);
            xps_http_res_destroy(res);
            return;
        }

        /* create upstream connection */
        session->upstream = xps_upstream_create(session->core, host, port);

        /* handle errors */
        if (session->upstream == NULL) {
            res = xps_http_res_create(session->core, HTTP_BAD_GATEWAY);
            xps_buffer_t *buff = xps_http_res_serialize(res);
            set_to_client_buff(session, buff);
            xps_http_res_destroy(res);
            return;
        }

        /* attach pipes */
        xps_pipe_create(session->core, DEFAULT_PIPE_BUFF_THRESH, session->upstream_source, session->upstream->sink);
        xps_pipe_create(session->core, DEFAULT_PIPE_BUFF_THRESH, session->upstream->source, session->upstream_sink);
    } else if (lookup->type == REQ_REDIRECT) {
        xps_http_res_t *http_res =
            xps_http_res_create(session->core, lookup->http_status_code);
        xps_http_set_header(&http_res->headers, "Location", lookup->redirect_url);
        /* serialize the response */
        xps_buffer_t *http_res_buff = xps_http_res_serialize(http_res);
        set_to_client_buff(session, http_res_buff);
        /* destroy the response object after use */
        xps_http_res_destroy(http_res);
        return;
    } else {
        logger(LOG_ERROR, "session_process_request()", "invalid lookup type");
        /* destroy session */
        xps_session_destroy(session);
        return;
    }
}

// Used while writing data from session instance to client connection instance
void client_source_handler(void *ptr) {
  /* validate parameters */
  assert(ptr != NULL);
  xps_pipe_source_t *source = ptr;
  xps_session_t *session = source->ptr;

  // write to session->to_client_buff
  if (xps_pipe_source_write(source, session->to_client_buff) != OK) {
    logger(LOG_ERROR, "client_source_handler()", "xps_pipe_source_write() failed");
    return;
  }
  
  // After successful write, the session->to_client_buff is set to NULL.
  xps_buffer_destroy(session->to_client_buff);
  set_to_client_buff(session, NULL);
  session_check_destroy(session);
}

void client_source_close_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_source_t *source = ptr;
  xps_session_t *session = source->ptr;
  session_check_destroy(session);
}

// Used when data is read from client connection instance to session instance
void client_sink_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  size_t read_len = sink->pipe->buff_list->len;
  if (read_len > DEFAULT_BUFFER_SIZE) read_len = DEFAULT_BUFFER_SIZE;

  xps_buffer_t *buff = xps_pipe_sink_read(sink, read_len);
  if (buff == NULL) {
    logger(LOG_ERROR, "client_sink_handler()", "xps_pipe_sink_read() failed");
    return;
  }

  // Intercept data and parse it as HTTP
  if (session->http_req == NULL) { // HTTP request hasn't been parsed yet
    int error;
    
    // Try to create and parse the HTTP request from the buffer
    xps_http_req_t *http_req = xps_http_req_create(session->core, buff, &error);
    
    if (error == E_FAIL) {
      // Parsing failed (bad syntax). Process to trigger 400 Bad Request.
      session_process_request(session);
      xps_buffer_destroy(buff);
      xps_pipe_sink_clear(sink, read_len);
      return;
    }
    
    if (error == E_AGAIN) {
      // Need more data (e.g., partial header received). 
      // Do not clear pipe, let data accumulate and try again on next Epoll event.
      xps_buffer_destroy(buff);
      return;
    }

    // Parsing Successful
    session->http_req = http_req;
    
    // Serialize http_req into a clean buffer (used for downstream proxy features later)
    xps_buffer_t *http_req_buff = xps_http_req_serialize(http_req);
    
    // Send serialized HTTP buffer toward the proxy route
    set_from_client_buff(session, http_req_buff);
    
    xps_pipe_sink_clear(sink, http_req->header_len);
    xps_buffer_destroy(buff);

    // Formulate HTTP response and serve file
    session_process_request(session);
  } else {
    // If the HTTP head is already parsed (e.g., this is just body data like a file upload),
    // act like a normal proxy and pass it straight through.
    set_from_client_buff(session, buff);
    xps_pipe_sink_clear(sink, read_len);
  }
}

void client_sink_close_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;
  session_check_destroy(session);
}

// Used when data is written from session to upstream
void upstream_source_handler(void *ptr) {
  /*assert*/
  assert(ptr != NULL);
  /*set ptr as source*/
  xps_pipe_source_t *source = ptr;
  /*set session as source->ptr*/
  xps_session_t *session = source->ptr;

  if (xps_pipe_source_write(source, session->from_client_buff) != OK) {
    logger(LOG_ERROR, "upstream_source_handler()", "xps_pipe_source_write() failed");
    return;
  }

  // Checking if upstream is connected
  // The upstream_connected flag is set to true, if some data has been moved from session to upstream.
  if (session->upstream_connected == false) {
    session->upstream_write_bytes += session->from_client_buff->len;
    if (session->upstream_write_bytes > source->pipe->buff_thresh)
      session->upstream_connected = true;
  }

  // After successful write the session->from_client_buff is set to NULL.
  xps_buffer_destroy(session->from_client_buff);
  set_from_client_buff(session, NULL);
  session_check_destroy(session);
}

// In the upstream source/sink close handlers, if upstream is not connected and upstream_error_res_set flag is not set, then upstream_error_res_set is set to true.
void upstream_source_close_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_source_t *source = ptr;
  xps_session_t *session = source->ptr;

  if (!session->upstream_connected && !session->upstream_error_res_set) {
    upstream_error_res(session);
  }
  session_check_destroy(session);
}

// Deals with reading of data from upstream to session. 
void upstream_sink_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  // Here the session->upstream_connected is set to true.
  session->upstream_connected = true;
  
  size_t read_len = sink->pipe->buff_list->len;
  if (read_len > DEFAULT_BUFFER_SIZE) read_len = DEFAULT_BUFFER_SIZE;

  xps_buffer_t *buff = xps_pipe_sink_read(sink, read_len);
  if (buff == NULL) {
    logger(LOG_ERROR, "upstream_sink_handler()", "xps_pipe_sink_read() failed");
    return;
  }

  // After successfully reading the data, it is then stored in the session->to_client_buff.
  set_to_client_buff(session, buff);
  xps_pipe_sink_clear(sink, read_len);
}

void upstream_sink_close_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  if (!session->upstream_connected && !session->upstream_error_res_set) {
    upstream_error_res(session);
  }
  session_check_destroy(session);
}

// Used to set the session->upstream_error_res_set as true.
void upstream_error_res(xps_session_t *session) {
  assert(session != NULL);
  session->upstream_error_res_set = true;

  xps_http_res_t *http_res = xps_http_res_create(session->core, HTTP_BAD_GATEWAY);
  xps_buffer_t *http_res_buff = xps_http_res_serialize(http_res);
  set_to_client_buff(session, http_res_buff);
  xps_http_res_destroy(http_res);
}

// Similar to upstream_sink_handler(). Here data is read from file to session and it is stored in session->to_client_buff.
void file_sink_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  size_t read_len = sink->pipe->buff_list->len;
  if (read_len > DEFAULT_BUFFER_SIZE) read_len = DEFAULT_BUFFER_SIZE;

  xps_buffer_t *buff = xps_pipe_sink_read(sink, read_len);
  if (buff == NULL) {
    logger(LOG_ERROR, "file_sink_handler()", "xps_pipe_sink_read() failed");
    return;
  }

  set_to_client_buff(session, buff);
  xps_pipe_sink_clear(sink, read_len);
}

void file_sink_close_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;
  session_check_destroy(session);
}

// Plays an important role in setting the ready flags for source and sink for the session, upstream and file.
void set_to_client_buff(xps_session_t *session, xps_buffer_t *buff) {
  /* validate parameters */
  assert(session != NULL);
  session->to_client_buff = buff;

  // If the session->to_client_buff is found to be null, then it implies that data has been already written into the pipe. 
  // So now session is ready to receive data from upstream or file.
  if (buff == NULL) {
    session->client_source->ready = false;
    session->upstream_sink->ready = true;
    session->file_sink->ready = true;
  } else {
    session->client_source->ready = true;
    session->upstream_sink->ready = false;
    session->file_sink->ready = false;
  }
}

// This function also sets the source and sink ready flags. 
void set_from_client_buff(xps_session_t *session, xps_buffer_t *buff) {
  /* validate parameters */
  assert(session != NULL);
  session->from_client_buff = buff;
  
  // if the session->from_client_buf is found to be null, then it implies that no data has been read from client to session. 
  // Session is now ready to accept data from client.
  if (buff == NULL) {
    session->client_sink->ready = true;
    session->upstream_source->ready = false;
  } else {
    session->client_sink->ready = false;
    session->upstream_source->ready = true;
  }
}

// This function checks whether the session instance is working properly or whether it has to be destroyed.
void session_check_destroy(xps_session_t *session) {
  /* validate parameters */
  assert(session != NULL);
  
  // 1. client to upstream flow : If session->upstream_source is active and either session - >client_sink 
  // is active or if there is some data in from_client_buff, then it implies there can be a data flow
  bool c2u_flow = session->upstream_source->active && (session->client_sink->active || session->from_client_buff);
  
  // 2. upstream to client flow : If session->client_source is active and either session->upstream_sink 
  // is active or there is some data in to_client_buff, then there is a data flow possible
  bool u2c_flow = session->client_source->active && (session->upstream_sink->active || session->to_client_buff);
  
  // 3. file to client flow : If session->client_source is active and either session->file_sink 
  // is active or there is some data in to_client_buff, then there is a data flow possible
  bool f2c_flow = session->client_source->active && (session->file_sink->active || session->to_client_buff);
  
  bool flowing = c2u_flow || u2c_flow || f2c_flow;
  
  // If none of these flows are present then session instance gets destroyed.
  if (!flowing)
    xps_session_destroy(session);
}

// This function is responsible for destroying the session instance.
void xps_session_destroy(xps_session_t *session) {
  /* validate parameters */
  assert(session != NULL);

  // Destroy the source and sink associated with session->client , session->upstream and session->file .
  if (session->client_source) xps_pipe_source_destroy(session->client_source);
  if (session->client_sink) xps_pipe_sink_destroy(session->client_sink);
  if (session->upstream_source) xps_pipe_source_destroy(session->upstream_source);
  if (session->upstream_sink) xps_pipe_sink_destroy(session->upstream_sink);
  if (session->file_sink) xps_pipe_sink_destroy(session->file_sink);

  // Destroy the buffers, session->to_client_buff and session->from_client_buff .
  if (session->to_client_buff != NULL)
    xps_buffer_destroy(session->to_client_buff);
  if (session->from_client_buff != NULL)
    xps_buffer_destroy(session->from_client_buff);

  // Free the parsed HTTP request memory
  if (session->http_req != NULL) {
    xps_http_req_destroy(session->core, session->http_req);
  }

  // Free the lookup structure
  if (session->lookup != NULL) {
    xps_config_lookup_destroy(session->lookup, session->core);
  }
  
  // Set the entry for session in core->sessions as NULL.
  for (int i = 0; i < session->core->sessions.length; i++) {
    if (session->core->sessions.data[i] == session) {
      session->core->sessions.data[i] = NULL;
      session->core->n_null_sessions++;
      break;
    }
  }

  // Free the session instance.
  free(session);
  logger(LOG_DEBUG, "xps_session_destroy()", "destroyed session");
}