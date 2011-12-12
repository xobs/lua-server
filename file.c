
#define PROJECT_DIR "/Users/smc/Sites/dev/luaed/projects"

#include <dirent.h>
#include <fcgiapp.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* plugin config for all request/connections */

enum file_operation {
    PROJ_CREATE,    // create a new project
    PROJ_LIST,      // list all projects
    PROJ_DELETE,    // delete the project and all files
    PROJ_UNKNOWN,   // Who knows?
    FILE_LIST,      // list files in a project
    FILE_CREATE,    // Creating a new file
    FILE_FETCH,     // Fetch the contents of a file
    FILE_DELETE,    // Delete a file
    FILE_RENAME,    // Rename a file
    FILE_LINK,      // Hardlink a file between projects
    FILE_METADATA,  // Handle metadata about a file
    FILE_UNKNOWN,
};



static const char *
op_to_str(enum file_operation op)
{
    if (op == PROJ_CREATE)
        return "PROJ_CREATE";
    if (op == PROJ_LIST)
        return "PROJ_LIST";
    if (op == PROJ_DELETE)
        return "PROJ_DELETE";
    if (op == PROJ_UNKNOWN)
        return "PROJ_UNKNWON";
    if (op == FILE_LIST)
        return "FILE_LIST";
    if (op == FILE_CREATE)
        return "FILE_CREATE";
    if (op == FILE_FETCH)
        return "FILE_FETCH";
    if (op == FILE_DELETE)
        return "FILE_DELETE";
    if (op == FILE_RENAME)
        return "FILE_RENAME";
    if (op == FILE_LINK)
        return "FILE_LINK";
    if (op == FILE_METADATA)
        return "FILE_METADATA";
    if (op == FILE_UNKNOWN)
        return "FILE_UNKNOWN";
    return "Please update op_to_str()";
}


/* Replace all instances of the character 'src' with 'dst' in string 'c' */
static int
strrep(char *c, char src, char dst)
{
    for(; *c; c++)
        if (*c == src)
            *c = dst;
    return 0;
}

static int
make_error(FCGX_Request *request, char *msg, int err)
{
    FCGX_FPrintF(request->out,
            "Content-type: text/html\r\n"
            "Status: 500\r\n"
            "\r\n"
            "%s: %s\r\n", msg, strerror(err));
    return 0;
}




static int
parse_file_uri(char *uri, char *cmd, char *token, char *arg, char *arg2, int sz)
{
    char *uri_tmp;
    char *slashes;
    int len;

    bzero(cmd, sz);
    bzero(token, sz);
    bzero(arg, sz);
    bzero(arg2, sz);

    slashes = strchr(uri, '/');
    uri_tmp = uri;

    len = sz-1;
    if (slashes && slashes-uri_tmp < len)
        len = slashes-uri_tmp;
    strncpy(cmd, uri_tmp, len);

    if (slashes) {
        uri_tmp = slashes+1;
        slashes = strchr(uri_tmp, '/');

        if (slashes) {
            len = sz-1;
            if (slashes-uri_tmp < len)
                len = slashes-uri_tmp;
            strncpy(token, uri_tmp, len);

            uri_tmp = slashes+1;
            slashes = strchr(uri_tmp, '/');

            if (slashes) {
                len = sz-1;
                if (slashes-uri_tmp < len)
                    len = slashes-uri_tmp;
                strncpy(arg, uri_tmp, len);

                uri_tmp = slashes+1;
                slashes = strchr(uri_tmp, '/');

                if (slashes) {
                    len = sz-1;
                    if (slashes-uri_tmp < len)
                        len = slashes-uri_tmp;
                    strncpy(arg2, uri_tmp, len);
                }
                else if(*uri_tmp)
                    strncpy(arg2, uri_tmp, sz);
            }
            else if(*uri_tmp)
                strncpy(arg, uri_tmp, sz);
        }
        else if(*uri_tmp)
            strncpy(token, uri_tmp, sz);
    }

    strrep(cmd, '/', '\0');
    strrep(token, '/', '\0');
    strrep(arg, '/', '\0');

    return 0;
}

static enum file_operation
determine_file_operation(char *cmd, char *token, char *arg, char *method)
{
    if (!cmd || !cmd[0])
        return PROJ_LIST;

    else if (!token || !token[0])
        return FILE_LIST;

    else if (!strcmp(token, ".create"))
        return PROJ_CREATE;

    else if (!strcmp(token, ".delete"))
        return PROJ_DELETE;

    else if (!arg || !arg[0]) {
        if (!strcmp(method, "POST"))
            return FILE_CREATE;
        else
            return FILE_FETCH;
    }

    else if (!strcmp(arg, "delete"))
        return FILE_DELETE;

    else if (!strcmp(arg, "metadata"))
        return FILE_METADATA;

    else if (!strcmp(arg, "link"))
        return FILE_LINK;

    else if (!strcmp(arg, "rename"))
        return FILE_RENAME;

    return FILE_UNKNOWN;
}


#if 0
static int
handle_lua_uri(server *srv, connection *con, char *uri)
{
    char cmd[1024];
    char token[1024];
    char arg[1024];
    enum lua_operation lo;
    buffer *b;
    int id; // token ID, parsed as an int

    parse_lua_uri(uri, cmd, token, arg, sizeof(cmd));
    b = chunkqueue_get_append_buffer(con->write_queue);


    con->file_started  = 1;
    con->file_finished = 1;

    /* Overwrite lighttpd's use of default error handlers */
    con->mode = EXTERNAL;


    lo = determine_lua_operation(cmd, token, arg);
    if (lo != LUA_STDIO)
    log_error_write(srv, __FILE__, __LINE__, "sssssssssds",
            "Raw URI:", uri,
            " command name:", cmd,
            " token:", token,
            " arg:", arg,
            " lua operation:", lo, lop_to_str(lo));


    if (lo == LUA_EVAL)
        return nlua_eval(srv, con);

    else if(lo == LUA_OPEN) {
        char data[4096];

        id = nlua_open(srv, con, token, arg);
        if (id < 0)
            return make_error(request, "Unable to open LUA environment", -id);

        snprintf(data, sizeof(data)-1, "%d\n", id);
        buffer_copy_string(b, data);
    }

    else if (lo == LUA_CLOSE) {
        id = strtoul(token, NULL, 16);
        if (id > MAX_THREAD_ID)
            return make_error(request, "Invalid thread ID specified", EINVAL);
        nlua_close(srv, con, id);
        buffer_copy_string(b, "OK\n");
    }

    else if (lo == LUA_STDIO) {
        int timeout = DEFAULT_STDIO_TIMEOUT;
        id = strtoul(token, NULL, 0);
        if (id > MAX_THREAD_ID)
            return make_error(request, "Invalid thread ID specified", EINVAL);

        if (*arg) {
            timeout = strtoul(arg, NULL, 0);
            if (timeout < 0 || timeout > MAX_STDIO_TIMEOUT)
                timeout = DEFAULT_STDIO_TIMEOUT;
        }

        if (!nlua_pool_status[id])
            return make_error(request, "Thread not running", EINVAL);

        return nlua_stdio(srv, con, arg, id, timeout);
    }

    else if (lo == LUA_LIST) {
        for (id=0; id<MAX_THREAD_ID; id++) {
            if (nlua_pool_status[id]) {
                buffer_append_long(b, id);
                buffer_append_string(b, "\n");
            }
        }
    }


    else if (lo == LUA_UNKNOWN)
        return make_error(request, "No valid LUA command specified", EINVAL);

    return HANDLER_FINISHED;
}
#endif



int
handle_file_uri(FCGX_Request *request)
{
    char project_name[1024];
    char file_name[1024];
    char file2_name[1024];
    char file3_name[1024];
    enum file_operation op;
    char *uri = FCGX_GetParam("REQUEST_URI", request->envp) + strlen("/file/");
    char *method = FCGX_GetParam("REQUEST_METHOD", request->envp);

    parse_file_uri(uri, project_name, file_name, file2_name, file3_name,
                   sizeof(project_name));
    op = determine_file_operation(project_name, file_name, file2_name, method);


    if (strstr(project_name, "..")
     || strstr(file_name, "..")
     || strstr(file2_name, "..")
     || strstr(file3_name, ".."))
        return make_error(request, "Invalid project name", EISDIR);

    fprintf(stderr, "Raw URI: %s  Project name: %s  Filename: %s  Filename 2: %s  Filename3: %s  Operation: %s\n", uri, project_name, file_name, file2_name, file3_name, op_to_str(op));

    if (op == PROJ_CREATE) {
        int res;
        char bfr[1024];

        bzero(bfr, sizeof(bfr));
        snprintf(bfr, sizeof(bfr)-1, "%s/%s", PROJECT_DIR, project_name);

        res = mkdir(bfr, 0755);
        if (-1 == res)
            return make_error(request, "Unable to create project", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        FCGX_FPrintF(request->out, "OK\n");
    }

    else if (op == PROJ_LIST) {
        DIR *proj_dir;
        struct dirent *de;

        proj_dir = opendir(PROJECT_DIR);
        if (!proj_dir)
            return make_error(request, "Unable to open project dir", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        while ((de = readdir(proj_dir)) != NULL) {

            /* Only accept files */
            if ((de->d_type != DT_DIR)) {
                continue;
            }
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }

            FCGX_FPrintF(request->out, "%s\n", de->d_name);
        }
        closedir(proj_dir);
    }

    else if (op == PROJ_DELETE) {
        struct dirent *de;
        DIR *proj_dir;
        char proj[2048];

        snprintf(proj, sizeof(proj)-1, "%s/%s", PROJECT_DIR, project_name);

        proj_dir = opendir(proj);
        if (!proj_dir)
            return make_error(request, "Unable to open project dir", errno);

        while ((de = readdir(proj_dir)) != NULL) {
            char entry[2048];

            /* Only accept files */
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            snprintf(entry, sizeof(entry)-1, "%s/%s/%s", PROJECT_DIR,
                    project_name, de->d_name);
            if (unlink(entry))
                return make_error(request,
                                  "Unable to remove file from project",
                                  errno);
        }
        closedir(proj_dir);

        if (rmdir(proj) < 0)
            return make_error(request, "Unable to remove project", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        FCGX_FPrintF(request->out, "OK\n");
    }
    else if (op == FILE_LIST) {
        struct dirent *de;
        DIR *proj_dir;
        char proj[2048];

        snprintf(proj, sizeof(proj)-1, "%s/%s", PROJECT_DIR, project_name);

        proj_dir = opendir(proj);
        if (!proj_dir)
            return make_error(request, "Unable to get file listing", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        while ((de = readdir(proj_dir)) != NULL) {

            /* Only accept files */
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;

            FCGX_FPrintF(request->out, "%s\n", de->d_name);
        }
        closedir(proj_dir);
    }

    else if (op == FILE_CREATE) {
        char full_path[2048];
        char bfr[8192];
        int fd;
        int bytes_read, bytes_written;

        bzero(full_path, sizeof(full_path));

        snprintf(full_path, sizeof(full_path)-1,
                 "%s/%s/%s", PROJECT_DIR, project_name, file_name);

        fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (-1 == fd)
            return make_error(request, "Unable to create file", errno);

        while ((bytes_read = FCGX_GetStr(bfr, sizeof(bfr), request->in)) > 0) {
            if ((bytes_written = write(fd, bfr, bytes_read)) < 0) {
                switch(errno) {
                    case ENOSPC:
                        close(fd);
                        return make_error(request, "Disk is full", errno);

                    case EINTR:
                        continue;

                    default:
                        close(fd);
                        return make_error(request, "Unable to write file", errno);
                }
            }
		}

		close(fd);
        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        FCGX_FPrintF(request->out, "OK\n");
    }

    else if (op == FILE_RENAME) {
        char full_path[2048];
        char full_path2[2048];
        bzero(full_path, sizeof(full_path));
        bzero(full_path2, sizeof(full_path2));

        snprintf(full_path, sizeof(full_path)-1,
                 "%s/%s/%s", PROJECT_DIR, project_name, file_name);
        snprintf(full_path2, sizeof(full_path2)-1,
                 "%s/%s/%s", PROJECT_DIR, project_name, file3_name);

        if (rename(full_path, full_path2) < 0)
            return make_error(request, "Unable to rename file", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        FCGX_FPrintF(request->out, "OK\n");
    }

    else if (op == FILE_LINK) {
        char full_path[2048];
        char project2_name[2048];
        bzero(full_path, sizeof(full_path));
        bzero(project2_name, sizeof(project2_name));

        snprintf(full_path, sizeof(full_path)-1,
                 "%s/%s/%s", PROJECT_DIR, project_name, file_name);
        snprintf(project2_name, sizeof(project2_name)-1,
                 "%s/%s/%s", PROJECT_DIR, file2_name, file_name);

        if (link(full_path, project2_name) < 0)
            return make_error(request, "Unable to link file", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        FCGX_FPrintF(request->out, "OK\n");
    }

    else if (op == FILE_FETCH) {
        char full_path[2048];
        char bfr[8192];
        int bytes_read;
        int fd;
        struct stat st;

        bzero(full_path, sizeof(full_path));
        snprintf(full_path, sizeof(full_path)-1,
                 "%s/%s/%s", PROJECT_DIR, project_name, file_name);
        if (lstat(full_path, &st) < 0 || !(st.st_mode | S_IFREG))
            return make_error(request, "Unable to read file", errno);

        fd = open(full_path, O_RDONLY);
        if (fd < 0)
            return make_error(request, "Unable to open file", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");

        while ( (bytes_read = read(fd, bfr, sizeof(bfr))) > 0)
            FCGX_PutStr(bfr, bytes_read, request->out);
    }

    else if (op == FILE_DELETE) {
        char full_path[2048];
        bzero(full_path, sizeof(full_path));

        snprintf(full_path, sizeof(full_path)-1,
                 "%s/%s/%s", PROJECT_DIR, project_name, file_name);

        if (unlink(full_path) < 0)
            return make_error(request, "Unable to remove file", errno);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n");
        FCGX_FPrintF(request->out, "OK\n");
    }

    return 0;
}


