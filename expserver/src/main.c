#include "xps.h"

xps_cliargs_t *cliargs;
xps_config_t *config;
xps_core_t *core;

void sigint_handler(int signum);
int core_create(xps_config_t *config);
void core_destroy(xps_core_t *core);

int main(int argc, char *argv[]) {
  signal(SIGINT, sigint_handler); // for handling ctrl+c

  cliargs = xps_cliargs_create(argc, argv); // get commandline arguments
  if (cliargs == NULL) {
    exit(EXIT_FAILURE);
  }

  /* create config, create core, start core */
  config = xps_config_create(cliargs->config_path);
  if (config == NULL) {
    xps_cliargs_destroy(cliargs);
    exit(EXIT_FAILURE);
  }

  if (core_create(config) != OK) {
    xps_config_destroy(config);
    xps_cliargs_destroy(cliargs);
    exit(EXIT_FAILURE);
  }

  xps_core_start(core);

  return EXIT_SUCCESS;
}

int core_create(xps_config_t *config) {
  assert(config != NULL);

  core = xps_core_create(config);
  if (core == NULL) {
    return E_FAIL;
  }

  /* Create listeners */
  vec_void_t listeners;
  vec_init(&listeners);

  for (int i = 0; i < config->_all_listeners.length; i++) {
    xps_config_listener_t *conf = config->_all_listeners.data[i];
    xps_listener_t *listener = xps_listener_create(conf->host, conf->port);

    if (listener == NULL) {
      for (int j = 0; j < listeners.length; j++) {
        xps_listener_destroy(listeners.data[j]);
      }
      vec_deinit(&listeners);
      xps_core_destroy(core);
      return E_FAIL;
    }
    vec_push(&listeners, listener);
  }

  /* Duplicate (use dup(fd) to duplicate file descriptor) and add listeners to cores */
  for (int i = 0; i < listeners.length; i++) {
    xps_listener_t *listener = listeners.data[i];

    int dup_sock_fd = dup(listener->sock_fd);
    if (dup_sock_fd < 0) {
      logger(LOG_ERROR, "core_create()", "dup() failed");
      perror("Error message");
      continue;
    }

    xps_listener_t *dup_listener = malloc(sizeof(xps_listener_t));
    if (dup_listener == NULL) {
      logger(LOG_ERROR, "core_create()", "malloc() failed for 'dup_listener'");
      close(dup_sock_fd);
      continue;
    }

    /* Initialize dup_listener values */
    dup_listener->core = core;
    dup_listener->host = listener->host;
    dup_listener->port = listener->port;
    dup_listener->sock_fd = dup_sock_fd;

    /* Attach dup_listener to loop */
    xps_loop_attach(core->loop, dup_listener->sock_fd, EPOLLIN | EPOLLET, dup_listener, listener_connection_handler, NULL, NULL);

    /* Add listener to 'listeners' list of core */
    vec_push(&core->listeners, dup_listener);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Server listening on http://%s:%u", dup_listener->host, dup_listener->port);
    logger(LOG_INFO, "core_create()", log_msg);
  }

  /* Destroy listeners */
  for (int i = 0; i < listeners.length; i++) {
    xps_listener_destroy(listeners.data[i]);
  }
  vec_deinit(&listeners);

  return OK;
}

void core_destroy(xps_core_t *core) {
  if (core != NULL) {
    xps_core_destroy(core);
  }
}

void sigint_handler(int signum) {
  logger(LOG_WARNING, "sigint_handler()", "SIGINT received");

  core_destroy(core);
  if (config != NULL) xps_config_destroy(config);
  if (cliargs != NULL) xps_cliargs_destroy(cliargs);

  exit(EXIT_SUCCESS);
}