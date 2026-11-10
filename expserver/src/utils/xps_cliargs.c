#include "../xps.h"

xps_cliargs_t *xps_cliargs_create(int argc, char *argv[]) {
  if (argc < 2) {
    logger(LOG_DEBUG, "xps_cliargs_create()", "arguments count not matching");
    return NULL;
  }

  xps_cliargs_t *cliargs = malloc(sizeof(xps_cliargs_t));
  if (cliargs == NULL) {
    logger(LOG_ERROR, "xps_cliargs_create()", "malloc() failed for 'cliargs'");
    return NULL;
  }

  if (is_abs_path(argv[1]))
    cliargs->config_path = str_create(argv[1]);
  else
    cliargs->config_path = realpath(argv[1], NULL);

  if (cliargs->config_path == NULL) {
    logger(LOG_ERROR, "xps_cliargs_create()", "failed to resolve config_path");
    free(cliargs);
    return NULL;
  }

  return cliargs;
}

void xps_cliargs_destroy(xps_cliargs_t *cliargs) {
  assert(cliargs != NULL);
  free(cliargs->config_path);
  free(cliargs);
}