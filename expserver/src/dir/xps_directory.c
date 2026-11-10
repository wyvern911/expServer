#include <fcntl.h>    // For open()
#include <time.h>     // For strftime() and localtime()
#include "xps_directory.h"

xps_buffer_t *xps_directory_browsing(const char *dir_path, const char *pathname) {
    // 1. Allocate a sufficiently large buffer for the HTML page (e.g., 64KB)
    char *buff = malloc(65536); 
    if (buff == NULL) {
        logger(LOG_ERROR, "xps_directory_browsing()", "malloc() failed for 'buff'");
        return NULL;
    }
    
    // Clear the buffer to ensure strlen works correctly
    memset(buff, 0, 65536); 

    // Create the html body
     sprintf(buff,"<html><head lang='en'><meta http-equiv='Content-Type' "
              "content='text/html; "
              "charset=UTF-8'><meta name='viewport' content='width=device-width, "
              "initial-scale=1.0'><title>Directory: "
              "%s</title><style>body{font-family: monospace; "
              "font-size: 15px;}td {padding: 1.5px 6px; padding-right: 20px;} "
              "h1{font-family: serif; "
              "margin: 0;} h3{font-family: serif;margin: 12px 0px; "
              "background-color: rgba(0,0,0,0.1); "
              "padding: 4px 0px;}</style></head><body><h1>eXpServer</h1><h3>Index "
              "of&nbsp;%s</h3><hr><table>",
              pathname, pathname);

    DIR *dir = opendir(dir_path);
    if(dir == NULL){
        logger(LOG_ERROR, "xps_directory_browsing()", "opendir() failed");
        free(buff);
        return NULL;
    }

    struct dirent *dir_entry;

    while((dir_entry = readdir(dir)) != NULL) {
        if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0)
            continue;

        char full_path[1024];
        // Format the full path
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir_entry->d_name);

        // Open the file to get its file descriptor
        int file_fd = open(full_path, O_RDONLY);
        if(file_fd == -1){
            logger(LOG_ERROR, "xps_directory_browsing()", "failed to open file");
            continue;
        }

        struct stat file_stat;
        if(fstat(file_fd, &file_stat) == -1){
            logger(LOG_ERROR, "xps_directory_browsing()", "fstat() failed");
            close(file_fd);
            continue;
        }

        if(S_ISREG(file_stat.st_mode) || S_ISDIR(file_stat.st_mode)){
            char *is_dir = S_ISDIR(file_stat.st_mode) ? "/" : "";
            char *temp_pathname = str_create(pathname);
            if (temp_pathname[strlen(temp_pathname) - 1] == '/')
                temp_pathname[strlen(temp_pathname) - 1] = '\0';

            // EXPERIMENT #1: Format the Date and Size
            char time_buff[20];
            strftime(time_buff, sizeof(time_buff), "%Y-%m-%d %H:%M:%S", localtime(&file_stat.st_mtime));
            
            if (S_ISREG(file_stat.st_mode)) {
                // File: Show Size (in KB) then Date
                double size_kb = (double)file_stat.st_size / 1024.0;
                sprintf(buff + strlen(buff),
                        "<tr><td><a href='%s/%s'>%s%s</a></td><td>%.2fKB</td><td>%s</td></tr>\n",
                        temp_pathname, dir_entry->d_name, dir_entry->d_name, is_dir, size_kb, time_buff);
            } else {
                // Directory: Leave Size blank, then show Date
                sprintf(buff + strlen(buff),
                        "<tr><td><a href='%s/%s'>%s%s</a></td><td></td><td>%s</td></tr>\n",
                        temp_pathname, dir_entry->d_name, dir_entry->d_name, is_dir, time_buff);
            }
            free(temp_pathname);
        }
        close(file_fd);
    }
    closedir(dir);

    sprintf(buff + strlen(buff), "</table></body></html>");

    // Package it into your buffer struct
    xps_buffer_t *directory_browsing = xps_buffer_create(strlen(buff), strlen(buff), buff);

    if (directory_browsing == NULL) {
        logger(LOG_ERROR, "xps_directory_browsing()","xps_buffer_create() returned NULL");
        return NULL;
    }
    return directory_browsing;
}