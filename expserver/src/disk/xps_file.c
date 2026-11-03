#include "xps_file.h"

void file_source_handler(void *ptr);
void file_source_close_handler(void *ptr);

xps_file_t *xps_file_create(xps_core_t *core, const char *file_path, int *error) {
    assert(core != NULL);
    assert(file_path != NULL);

    *error = E_FAIL;

    // Check if file is inside the public directory
    char *resolved_path = realpath(file_path, NULL);
    char *resolved_public = realpath("../public", NULL);

    if (resolved_path == NULL || resolved_public == NULL) {
        logger(LOG_ERROR, "xps_file_create()", "realpath() failed for %s", file_path);
        if (resolved_path) free(resolved_path);
        if (resolved_public) free(resolved_public);
        return NULL;
    }

    size_t public_len = strlen(resolved_public);
    if (strncmp(resolved_path, resolved_public, public_len) != 0) {
        logger(LOG_WARNING, "xps_file_create()", "file requested is outside of public directory");
        *error = E_PERMISSION;
        free(resolved_path);
        free(resolved_public);
        return NULL;
    }
    
    free(resolved_path);
    free(resolved_public);

    // Check if others have read permission
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        logger(LOG_ERROR, "xps_file_create()", "stat() failed");
        perror("Error message");
        return NULL;
    }

    if (!(file_stat.st_mode & S_IROTH)) {
        logger(LOG_WARNING, "xps_file_create()", "others do not have read permission");
        *error = E_PERMISSION;
        return NULL;
    }

    // Getting size of file from stat (already called above)
    long temp_size = file_stat.st_size;

    // Opening file
    FILE *file_struct = fopen(file_path, "rb");
    if (file_struct == NULL) {
        logger(LOG_ERROR, "xps_file_create()", "fopen() failed");
        if (errno == ENOENT) *error = E_NOTFOUND;
        else if (errno == EACCES) *error = E_PERMISSION;
        return NULL;
    }

    const char *mime_type = xps_get_mime(file_path);

    // Alloc memory for instance of xps_file_t
    xps_file_t *file = malloc(sizeof(xps_file_t));
    if (file == NULL) {
        logger(LOG_ERROR, "xps_file_create()", "malloc() failed for file");
        fclose(file_struct);
        return NULL;
    }

    xps_pipe_source_t *source = xps_pipe_source_create((void *)file, file_source_handler, file_source_close_handler);
    if (source == NULL) {
        logger(LOG_ERROR, "xps_file_create()", "xps_pipe_source_create() failed");
        fclose(file_struct);
        free(file);
        return NULL;
    }

    // Init values
    source->ready = true;
    source->active = true;

    file->core = core;
    file->file_path = file_path;
    file->source = source;
    file->file_struct = file_struct;
    file->size = temp_size;
    file->mime_type = mime_type;

    *error = OK;
    logger(LOG_DEBUG, "xps_file_create()", "created file");
    return file;
}

void xps_file_destroy(xps_file_t *file) {
    assert(file != NULL);

    if (file->file_struct) {
        fclose(file->file_struct);
    }
    
    if (file->source) {
        xps_pipe_source_destroy(file->source);
    }
    
    free(file);
    logger(LOG_DEBUG, "xps_file_destroy()", "destroyed file struct");
}

void file_source_handler(void *ptr) {
    assert(ptr != NULL);
    
    xps_pipe_source_t *source = ptr;
    xps_file_t *file = source->ptr;

    xps_buffer_t *buff = xps_buffer_create(DEFAULT_BUFFER_SIZE, DEFAULT_BUFFER_SIZE, NULL);
    if (buff == NULL) {
        logger(LOG_ERROR, "file_source_handler()", "xps_buffer_create() failed");
        return;
    }

    // Limit read space to available pipe capacity
    size_t available = source->pipe->buff_thresh - source->pipe->buff_list->len;
    size_t read_limit = (DEFAULT_BUFFER_SIZE < available) ? DEFAULT_BUFFER_SIZE : available;

    // Read from file
    size_t read_n = fread(buff->data, 1, read_limit, file->file_struct);
    buff->len = read_n;

    // Checking for read errors
    if (ferror(file->file_struct)) {
        logger(LOG_ERROR, "file_source_handler()", "ferror() reported error");
        xps_buffer_destroy(buff);
        source->active = false;
        if (source->close_cb) source->close_cb(source);
        return;
    }

    // If end of file reached
    if (read_n == 0 && feof(file->file_struct)) {
        logger(LOG_DEBUG, "file_source_handler()", "EOF reached");
        xps_buffer_destroy(buff);
        source->active = false;
        if (source->close_cb) source->close_cb(source);
        return;
    }

    // Write to pipe from buff
    if (read_n > 0) {
        if (xps_pipe_source_write(source, buff) != OK) {
            logger(LOG_ERROR, "file_source_handler()", "xps_pipe_source_write() failed");
            source->active = false;
            if (source->close_cb) source->close_cb(source);
        }
    }

    xps_buffer_destroy(buff);
}

void file_source_close_handler(void *ptr) {
    assert(ptr != NULL);
    xps_pipe_source_t *source = ptr;
    xps_file_t *file = source->ptr;

    // Deallocate file object
    xps_file_destroy(file);
}