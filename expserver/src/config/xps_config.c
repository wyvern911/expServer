#include "xps_config.h"

// Forward declarations for helper parsing functions
int parse_server(JSON_Object *server_object, xps_config_server_t *server);
int parse_listener(JSON_Object *listener_object, xps_config_listener_t *listener);
int parse_route(JSON_Object *route_object, xps_config_route_t *route);
void parse_all_listeners(xps_config_t *config);

xps_config_t *xps_config_create(const char *config_path) {
  assert(config_path != NULL);

  // Allocate mem for config
  xps_config_t *config = malloc(sizeof(xps_config_t));
  if (config == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "malloc() failed for 'config'");
    return NULL;
  }

  // Get config_json using json_parse_file
  JSON_Value *config_json = json_parse_file(config_path);
  if (config_json == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "json_parse_file() failed for '%s'", config_path);
    free(config);
    return NULL;
  }

  // Initialize fields of config object
  config->config_path = config_path;
  config->_config_json = config_json;
  vec_init(&config->servers);
  vec_init(&config->_all_listeners);

  JSON_Object *root_object = json_value_get_object(config_json);
  if (root_object == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "failed to parse root_object");
    xps_config_destroy(config);
    return NULL;
  }

  // Initialize server_name and servers fields
  const char *server_name = json_object_get_string(root_object, "server_name");
  if (server_name == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "missing 'server_name' in config");
    xps_config_destroy(config);
    return NULL;
  }
  config->server_name = server_name;

  JSON_Array *servers = json_object_get_array(root_object, "servers");
  if (servers == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "missing 'servers' array in config");
    xps_config_destroy(config);
    return NULL;
  }

  for (size_t i = 0; i < json_array_get_count(servers); i++) {
    JSON_Object *server_object = json_array_get_object(servers, i);
    xps_config_server_t *server = malloc(sizeof(xps_config_server_t));
    if (server == NULL) {
      logger(LOG_ERROR, "xps_config_create()", "malloc() failed for 'server'");
      xps_config_destroy(config);
      return NULL;
    }

    // Initialize and parse the server and push to servers list of config
    if (parse_server(server_object, server) != OK) {
      logger(LOG_ERROR, "xps_config_create()", "parse_server() failed");
      free(server);
      xps_config_destroy(config);
      return NULL;
    }

    vec_push(&config->servers, server);
  }

  // Initialize and set up the _all_listeners array
  parse_all_listeners(config);

  logger(LOG_DEBUG, "xps_config_create()", "created config from %s", config_path);
  return config;
}

void xps_config_destroy(xps_config_t *config) {
  assert(config != NULL);

  // Deallocate the servers and the corresponding listeners and routes
  for (int i = 0; i < config->servers.length; i++) {
    xps_config_server_t *server = config->servers.data[i];
    if (server == NULL) continue;

    for (int j = 0; j < server->listeners.length; j++) {
      if (server->listeners.data[j] != NULL) {
        free(server->listeners.data[j]);
      }
    }
    vec_deinit(&server->listeners);

    vec_deinit(&server->hostnames);

    for (int j = 0; j < server->routes.length; j++) {
      xps_config_route_t *route = server->routes.data[j];
      if (route != NULL) {
        vec_deinit(&route->index);
        vec_deinit(&route->upstreams);
        free(route);
      }
    }
    vec_deinit(&server->routes);

    free(server);
  }
  vec_deinit(&config->servers);
  vec_deinit(&config->_all_listeners);

  if (config->_config_json != NULL) {
    json_value_free(config->_config_json);
  }

  free(config);
  logger(LOG_DEBUG, "xps_config_destroy()", "destroyed config");
}

int parse_server(JSON_Object *server_object, xps_config_server_t *server) {
  assert(server_object != NULL);
  assert(server != NULL);

  vec_init(&server->listeners);
  vec_init(&server->hostnames);
  vec_init(&server->routes);

  // Extract and initialize server listeners
  JSON_Array *listeners = json_object_get_array(server_object, "listeners");
  if (listeners == NULL || json_array_get_count(listeners) == 0) {
    logger(LOG_ERROR, "parse_server()", "missing or empty 'listeners' in server block");
    return E_FAIL;
  }

  for (size_t i = 0; i < json_array_get_count(listeners); i++) {
    JSON_Object *listener_object = json_array_get_object(listeners, i);
    xps_config_listener_t *listener = malloc(sizeof(xps_config_listener_t));
    if (listener == NULL) {
      logger(LOG_ERROR, "parse_server()", "malloc() failed for 'listener'");
      return E_FAIL;
    }

    if (parse_listener(listener_object, listener) != OK) {
      free(listener);
      return E_FAIL;
    }
    vec_push(&server->listeners, listener);
  }

  // Extract and initialize server hostnames (optional)
  JSON_Array *hostnames = json_object_get_array(server_object, "hostnames");
  if (hostnames != NULL) {
    for (size_t i = 0; i < json_array_get_count(hostnames); i++) {
      const char *hostname = json_array_get_string(hostnames, i);
      if (hostname != NULL) {
        vec_push(&server->hostnames, (void *)hostname);
      }
    }
  }

  // Extract and initialize server routes
  JSON_Array *routes = json_object_get_array(server_object, "routes");
  if (routes == NULL || json_array_get_count(routes) == 0) {
    logger(LOG_ERROR, "parse_server()", "missing or empty 'routes' in server block");
    return E_FAIL;
  }

  for (size_t i = 0; i < json_array_get_count(routes); i++) {
    JSON_Object *route_object = json_array_get_object(routes, i);
    xps_config_route_t *route = malloc(sizeof(xps_config_route_t));
    if (route == NULL) {
      logger(LOG_ERROR, "parse_server()", "malloc() failed for 'route'");
      return E_FAIL;
    }

    if (parse_route(route_object, route) != OK) {
      free(route);
      return E_FAIL;
    }
    vec_push(&server->routes, route);
  }

  return OK;
}

int parse_listener(JSON_Object *listener_object, xps_config_listener_t *listener) {
  assert(listener_object != NULL);
  assert(listener != NULL);

  const char *host = json_object_get_string(listener_object, "host");
  u_int port = (u_int)json_object_get_number(listener_object, "port");

  if (host == NULL || !is_valid_port(port) || port == 0) {
    logger(LOG_ERROR, "parse_listener()", "invalid host or port in listener config");
    return E_FAIL;
  }

  listener->host = host;
  listener->port = port;

  return OK;
}

int parse_route(JSON_Object *route_object, xps_config_route_t *route) {
  assert(route_object != NULL);
  assert(route != NULL);

  memset(route, 0, sizeof(xps_config_route_t));
  vec_init(&route->index);
  vec_init(&route->upstreams);
  vec_init(&route->ip_whitelist);
  vec_init(&route->ip_blacklist);

  route->req_path = json_object_get_string(route_object, "req_path");
  route->type = json_object_get_string(route_object, "type");

  if (route->req_path == NULL || route->type == NULL) {
    logger(LOG_ERROR, "parse_route()", "missing 'req_path' or 'type' in route");
    return E_FAIL;
  }

  if (strcmp(route->type, "file_serve") == 0) {
    route->dir_path = json_object_get_string(route_object, "dir_path");
    if (route->dir_path == NULL) {
      logger(LOG_ERROR, "parse_route()", "missing 'dir_path' for file_serve route");
      return E_FAIL;
    }

    JSON_Array *index_arr = json_object_get_array(route_object, "index");
    if (index_arr != NULL) {
      for (size_t i = 0; i < json_array_get_count(index_arr); i++) {
        const char *index_file = json_array_get_string(index_arr, i);
        if (index_file != NULL) {
          vec_push(&route->index, (void *)index_file);
        }
      }
    }
  } else if (strcmp(route->type, "reverse_proxy") == 0) {
    JSON_Array *upstreams_arr = json_object_get_array(route_object, "upstreams");
    if (upstreams_arr == NULL || json_array_get_count(upstreams_arr) == 0) {
      logger(LOG_ERROR, "parse_route()", "missing 'upstreams' for reverse_proxy route");
      return E_FAIL;
    }

    for (size_t i = 0; i < json_array_get_count(upstreams_arr); i++) {
      const char *upstream = json_array_get_string(upstreams_arr, i);
      if (upstream != NULL) {
        vec_push(&route->upstreams, (void *)upstream);
      }
    }
  } else if (strcmp(route->type, "redirect") == 0) {
    route->http_status_code = (u_int)json_object_get_number(route_object, "http_status_code");
    route->redirect_url = json_object_get_string(route_object, "redirect_url");
    if (route->http_status_code == 0 || route->redirect_url == NULL) {
      logger(LOG_ERROR, "parse_route()", "missing 'http_status_code' or 'redirect_url' for redirect route");
      return E_FAIL;
    }
  }

  JSON_Array *ip_whitelist = json_object_get_array(route_object, "ip_whitelist");
    JSON_Array *ip_blacklist = json_object_get_array(route_object, "ip_blacklist");

    if (ip_whitelist && !ip_blacklist) {
        for (size_t i = 0; i < json_array_get_count(ip_whitelist); i++) {
            const char *ip = json_array_get_string(ip_whitelist, i);
            if (ip != NULL) vec_push(&(route->ip_whitelist), (void *)ip);
        }
    } else if (ip_blacklist && !ip_whitelist) {
        for (size_t i = 0; i < json_array_get_count(ip_blacklist); i++) {
            const char *ip = json_array_get_string(ip_blacklist, i);
            if (ip != NULL) vec_push(&(route->ip_blacklist), (void *)ip);
        }
    } else if (ip_whitelist && ip_blacklist) {
        // Whitelist takes priority: Filter out any IPs that are also on the blacklist
        for (size_t i = 0; i < json_array_get_count(ip_whitelist); i++) {
            const char *ip_w = json_array_get_string(ip_whitelist, i);
            if (ip_w == NULL) continue;
            
            bool is_blacklisted = false;
            for (size_t j = 0; j < json_array_get_count(ip_blacklist); j++) {
                const char *ip_b = json_array_get_string(ip_blacklist, j);
                if (ip_b != NULL && strcmp(ip_w, ip_b) == 0) {
                    is_blacklisted = true;
                    break;
                }
            }
            if (!is_blacklisted) {
                vec_push(&(route->ip_whitelist), (void *)ip_w);
            }
        }
    }

  return OK;
}

void parse_all_listeners(xps_config_t *config) {
  assert(config != NULL);

  for (int i = 0; i < config->servers.length; i++) {
    xps_config_server_t *server = config->servers.data[i];
    for (int j = 0; j < server->listeners.length; j++) {
      xps_config_listener_t *listener = server->listeners.data[j];

      // Check whether each listener already exists in _all_listeners
      bool exists = false;
      for (int k = 0; k < config->_all_listeners.length; k++) {
        xps_config_listener_t *existing = config->_all_listeners.data[k];
        if (strcmp(existing->host, listener->host) == 0 && existing->port == listener->port) {
          exists = true;
          break;
        }
      }

      if (!exists) {
        vec_push(&config->_all_listeners, listener);
      }
    }
  }
}

xps_config_lookup_t *xps_config_lookup(xps_config_t *config, xps_http_req_t *http_req, xps_connection_t *client, int *error) {
  assert(config != NULL);
  assert(http_req != NULL);
  assert(client != NULL);
  assert(error != NULL);

  *error = E_FAIL;

  // Get host, accept encoding, pathname from http_req
  const char *host = http_req->host ? http_req->host : xps_http_get_header(&http_req->headers, "Host");
  const char *accept_encoding = xps_http_get_header(&http_req->headers, "Accept-Encoding");
  (void)accept_encoding;
  const char *pathname = http_req->pathname ? http_req->pathname : http_req->path;

  if (pathname == NULL) {
    return NULL;
  }

  // Step 1: Find matching server block
  int target_server_index = -1;
  for (int i = 0; i < config->servers.length; i++) {
    xps_config_server_t *server = config->servers.data[i];

    // Check if client listener is present in server
    bool has_matching_listener = false;
    for (int j = 0; j < server->listeners.length; j++) {
      xps_config_listener_t *listener = server->listeners.data[j];
      if (client->listener != NULL &&
          strcmp(listener->host, client->listener->host) == 0 &&
          listener->port == client->listener->port) {
        has_matching_listener = true;
        break;
      }
    }

    if (!has_matching_listener)
      continue;

    // Check if host header matches any hostname
    // NOTE: if no hostnames provided, it is considered a match
    bool has_matching_hostname = (server->hostnames.length == 0);
    if (!has_matching_hostname && host != NULL) {
      // Strip port from Host header if present (e.g., localhost:8001 -> localhost)
      char host_only[256];
      strncpy(host_only, host, sizeof(host_only) - 1);
      host_only[sizeof(host_only) - 1] = '\0';
      char *colon = strchr(host_only, ':');
      if (colon != NULL) *colon = '\0';

      for (int j = 0; j < server->hostnames.length; j++) {
        const char *hostname = server->hostnames.data[j];
        if (strcasecmp(hostname, host) == 0 || strcasecmp(hostname, host_only) == 0) {
          has_matching_hostname = true;
          break;
        }
      }
    }

    if (has_matching_hostname) {
      target_server_index = i;
      break;
    }
  }

  if (target_server_index == -1) {
    *error = E_NOTFOUND;
    return NULL;
  }

  xps_config_server_t *server = config->servers.data[target_server_index];

  /* Find matching route block */
  // Route matching uses prefix matching with longest-match-first strategy.
  xps_config_route_t *route = NULL;
  ssize_t best_match_len = -1; // Track the longest matching route path

  for (int i = 0; i < server->routes.length; i++) {
    xps_config_route_t *current_route = server->routes.data[i];
    size_t route_path_len = strlen(current_route->req_path);

    // Check if this route's path is a prefix of the request path
    if (str_starts_with(pathname, current_route->req_path)) {
      // If this is a longer match than we've found so far, use it
      if ((ssize_t)route_path_len > best_match_len) {
        best_match_len = (ssize_t)route_path_len;
        route = current_route;
      }
    }
  }

  if (route == NULL) {
    *error = E_NOTFOUND; // No matching route found - 404
    return NULL;
  }

  /* Init values of lookup */
  xps_config_lookup_t *lookup = malloc(sizeof(xps_config_lookup_t));
  if (lookup == NULL) {
    logger(LOG_ERROR, "xps_config_lookup()", "malloc() failed for 'lookup'");
    *error = E_FAIL;
    return NULL;
  }

  lookup->type = REQ_INVALID;
  lookup->file_path = NULL;
  lookup->dir_path = NULL;
  lookup->upstream = NULL;
  lookup->http_status_code = 0;
  lookup->redirect_url = NULL;
  vec_init(&lookup->ip_whitelist);
  vec_init(&lookup->ip_blacklist);

  if (strcmp(route->type, "file_serve") == 0) {
    lookup->type = REQ_FILE_SERVE;
  } else if (strcmp(route->type, "reverse_proxy") == 0) {
    lookup->type = REQ_REVERSE_PROXY;
  } else if (strcmp(route->type, "redirect") == 0) {
    lookup->type = REQ_REDIRECT;
  } else if (strcmp(route->type, "metrics") == 0) {
    lookup->type = REQ_METRICS;
  }

  // Transfer Whitelist
    for (int i = 0; i < route->ip_whitelist.length; i++) {
        vec_push(&lookup->ip_whitelist, route->ip_whitelist.data[i]);
    }
    // Transfer Blacklist
    for (int i = 0; i < route->ip_blacklist.length; i++) {
        vec_push(&lookup->ip_blacklist, route->ip_blacklist.data[i]);
    }

  // File serve
  if (lookup->type == REQ_FILE_SERVE) {
    const char *rel_path = pathname + strlen(route->req_path);
    char *resource_path = path_join(route->dir_path, rel_path);

    if (!is_abs_path(resource_path)) {
      /* we require absolute path so we need to see if the current path is absolute or not */
      char *config_dir_temp = str_create(config->config_path);
      char *last_slash = strrchr(config_dir_temp, '/');
      if (last_slash != NULL) {
        *last_slash = '\0';
      } else {
        strcpy(config_dir_temp, ".");
      }

      char *joined_with_config = path_join(config_dir_temp, resource_path);
      free(config_dir_temp);

      char *abs_path = realpath(joined_with_config, NULL);
      free(joined_with_config);

      if (abs_path == NULL) {
        // Fallback to resolving relative to current working directory
        abs_path = realpath(resource_path, NULL);
      }

      free(resource_path);
      resource_path = abs_path;
    } else {
      char *abs_path = realpath(resource_path, NULL);
      free(resource_path);
      resource_path = abs_path;
    }

    if (resource_path == NULL) {
      *error = OK;
      return lookup;
    }

    // is file
    if (is_file(resource_path)) {
      lookup->file_path = resource_path;
    } else if (is_dir(resource_path)) { // is directory
      /* If request is for a directory, serve the index file (e.g. index.html) instead of showing the directory listing. */
      bool index_file_found = false;
      for (int i = 0; i < route->index.length; i++) {
        char *index_file = path_join(resource_path, route->index.data[i]);
        if (is_file(index_file)) {
          lookup->file_path = realpath(index_file, NULL);
          free(index_file);
          index_file_found = true;
          break;
        }
        free(index_file);
      }
      if (!index_file_found) {
        /* If no index file, save the directory path for directory browsing */
        lookup->dir_path = resource_path; 
      } else {
        free(resource_path);
      }

    } else {
      /* no matching type so free resource_path */
      free(resource_path);
    }

    *error = OK;
    return lookup;
  } else if (lookup->type == REQ_REVERSE_PROXY) {
    if (route->upstreams.length > 0) {
      lookup->upstream = route->upstreams.data[0];
    }
    *error = OK;
    return lookup;
  } else if (lookup->type == REQ_REDIRECT) {
    lookup->http_status_code = route->http_status_code;
    lookup->redirect_url = route->redirect_url;
    *error = OK;
    return lookup;
  }

  *error = OK;
  return lookup;
}

void xps_config_lookup_destroy(xps_config_lookup_t *config_lookup, xps_core_t *core) {
  assert(config_lookup != NULL);
  (void)core;

  if (config_lookup->file_path != NULL) {
    free(config_lookup->file_path);
  }
  if (config_lookup->dir_path != NULL) {
    free(config_lookup->dir_path);
  }

  vec_deinit(&config_lookup->ip_whitelist);
  vec_deinit(&config_lookup->ip_blacklist);

  free(config_lookup);
}