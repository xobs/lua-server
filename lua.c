#include <fcgiapp.h>

#include <unistd.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define LUA_MAXINPUT 512
#define DEFAULT_STDIO_TIMEOUT 30
#define MAX_STDIO_TIMEOUT 120


enum lua_operation {
    LUA_EVAL,
    LUA_OPEN,
    LUA_RUN,
    LUA_STDIO,
    LUA_STATE,
    LUA_PING,
    LUA_BPADD,
    LUA_BPGET,
    LUA_BPDEL,
    LUA_CLOSE,
    LUA_LIST,
    LUA_UNKNOWN,
};

enum lua_cmd {
    LC_RUN,         // LUA should run some code
    LC_EVAL,        // LUA should eval the code and return the result
    LC_PAUSE,       // LUA should pause.  A breakpoint may be specified.
    LC_CONTINUE,    // LUA should continue where it left off.
    LC_ERROR,       // Indicates an error occurred
    LC_UNKNOWN,
};

struct netv_lua_state {
    pid_t        pid;
    int          in_fd;
    int          out_fd;
    int          in_ctrl;
    int          out_ctrl;
    int          last_ping;
    char         project[1024];
    char         filename[1024];
    lua_State   *L;
};

#define MAX_THREAD_ID 32
static struct netv_lua_state nlua_states[MAX_THREAD_ID];
static char nlua_pool_status[MAX_THREAD_ID]; // TODO: Turn this into a bitmap


static const char *
op_to_str(enum lua_operation op)
{
    if (op == LUA_EVAL)
        return "LUA_EVAL";
    if (op == LUA_OPEN)
        return "LUA_OPEN";
    if (op == LUA_RUN)
        return "LUA_RUN";
    if (op == LUA_STDIO)
        return "LUA_STDIO";
    if (op == LUA_STATE)
        return "LUA_STATE";
    if (op == LUA_PING)
        return "LUA_PING";
    if (op == LUA_BPADD)
        return "LUA_BPADD";
    if (op == LUA_BPGET)
        return "LUA_BPGET";
    if (op == LUA_BPDEL)
        return "LUA_BPDEL";
    if (op == LUA_CLOSE)
        return "LUA_CLOSE";
    if (op == LUA_LIST)
        return "LUA_LIST";
    if (op == LUA_UNKNOWN)
        return "LUA_UNKNOWN";
    return "Please update lua op_to_str()";
}

/*
static const char *
lc_to_str(enum lua_cmd ls) {
    if (ls == LC_RUN)
        return "LC_RUN";
    if (ls == LC_EVAL)
        return "LC_EVAL";
    if (ls == LC_PAUSE)
        return "LC_PAUSE";
    if (ls == LC_CONTINUE)
        return "LC_CONTINUE";
    if (ls == LC_ERROR)
        return "LC_ERROR";
    if (ls == LC_UNKNOWN)
        return "LC_UNKNOWN";
    return "Please update lc_to_str()";
}
*/


static void
nlua_sig(int sig)
{
    fflush(stdout);
    exit(0);
}

#if 0
static void my_reaper(int sig) {
    pid_t pid;
    
    pid = waitpid(-1, NULL, WNOHANG);
    if (pid > 0) {
        int i;
        for (i=0; i<MAX_THREAD_ID; i++) {
            if (nlua_states[i].pid == pid) {
                /* -- let the file handles drain on their own
                close(nlua_states[i].in_fd);
                close(nlua_states[i].out_fd);
                close(nlua_states[i].in_ctrl);
                close(nlua_states[i].out_ctrl);
                */
                nlua_pool_status[i] = 0;
            }
        }
        fprintf(stderr, "Got SIGCHLD for pid %d\n", pid);
    }

}
#endif




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


/* ------- LUA INTERPRETER FUNCTIONS (mostly run in their own fork) -------- */

static lua_State *globalL = NULL;
static const char *progname = "luaed";

static void lstop(lua_State *L, lua_Debug *ar)
{
    (void)ar;  /* unused arg. */
    lua_sethook(L, NULL, 0, 0);
    /* Avoid luaL_error -- a C hook doesn't add an extra frame. */
    luaL_where(L, 0);
    lua_pushfstring(L, "%sinterrupted!", lua_tostring(L, -1));
    lua_error(L);
}

static void laction(int i)
{
    signal(i, SIG_DFL); /* if another SIGINT happens before lstop,
                           terminate process (default action) */
    lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}

static void l_message(const char *pname, const char *msg)
{
    if (pname) fprintf(stderr, "%s: ", pname);
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
}

static int report(lua_State *L, int status)
{
    if (status && !lua_isnil(L, -1)) {
        const char *msg = lua_tostring(L, -1);
        if (msg == NULL)
            msg = "(error object is not a string)";
        l_message(progname, msg);
        lua_pop(L, 1);
    }
    fflush(stdout);
    fflush(stderr);
    return status;
}

static int traceback(lua_State *L)
{
    if (!lua_isstring(L, 1))  /* 'message' not a string? */
        return 1;  /* keep it intact */
    lua_getfield(L, LUA_GLOBALSINDEX, "debug");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 1;
    }
    lua_pushvalue(L, 1);  /* pass error message */
    lua_pushinteger(L, 2);  /* skip this function and traceback */
    lua_call(L, 2, 1);  /* call debug.traceback */
    return 1;
}

static int incomplete(lua_State *L, int status)
{
    if (status == LUA_ERRSYNTAX) {
        size_t lmsg;
        const char *msg = lua_tolstring(L, -1, &lmsg);
        const char *tp = msg + lmsg - (sizeof(LUA_QL("<eof>")) - 1);
        if (strstr(msg, LUA_QL("<eof>")) == tp) {
            lua_pop(L, 1);
            return 1;
        }
    }
    return 0;  /* else... */
}

static int docall(lua_State *L, int narg, int clear)
{
    int status;
    int base = lua_gettop(L) - narg;  /* function index */
    lua_pushcfunction(L, traceback);  /* push traceback function */
    lua_insert(L, base);  /* put it under chunk and args */
    signal(SIGINT, laction);
    status = lua_pcall(L, narg, (clear ? 0 : LUA_MULTRET), base);
    signal(SIGINT, SIG_DFL);
    lua_remove(L, base);  /* remove traceback function */
    /* force a complete garbage collection in case of errors */
    if (status != 0)
        lua_gc(L, LUA_GCCOLLECT, 0);
    return status;
}


#define LUA_PROMPT  "\nlua> "
#define LUA_PROMPT2 "\n .... "
static void write_prompt(lua_State *L, int firstline)
{
    const char *p;
    lua_getfield(L, LUA_GLOBALSINDEX, firstline ? "_PROMPT" : "_PROMPT2");
    p = lua_tostring(L, -1);
    if (p == NULL)
        p = firstline ? LUA_PROMPT : LUA_PROMPT2;
    fputs(p, stdout);
    fflush(stdout);
    lua_pop(L, 1);  /* remove global */
}

static int pushline(lua_State *L, int firstline)
{
    char buf[LUA_MAXINPUT];
    write_prompt(L, firstline);
    if (fgets(buf, LUA_MAXINPUT, stdin)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n')
            buf[len-1] = '\0';
        if (firstline && buf[0] == '=')
            lua_pushfstring(L, "return %s", buf+1);
        else
            lua_pushstring(L, buf);
        return 1;
    }
    return 0;
}

static int loadline(lua_State *L)
{
    int status;
    lua_settop(L, 0);
    if (!pushline(L, 1))
        return -1;  /* no input */
    for (;;) {  /* repeat until gets a complete line */
        status = luaL_loadbuffer(L, lua_tostring(L, 1), lua_strlen(L, 1), "=stdin");
        if (!incomplete(L, status))
         break;  /* cannot try to add lines? */
        if (!pushline(L, 0))  /* no more input? */
            return -1;
        lua_pushliteral(L, "\n");  /* add a new line... */
        lua_insert(L, -2);  /* ...between the two lines */
        lua_concat(L, 3);  /* join them */
    }
    lua_remove(L, 1);  /* remove line */
    return status;
}


static void
nlua_interpret_hook(lua_State *L, lua_Debug *ar)
{
    #if 0
    printf("Executing on line %d.\n", ar->currentline);/* in file %s (%s).  "
           "Function runs from line %d to line %d.  "
           "Function is of type '%s', and is of kind '%s' named '%s'.  "
           "It has %d nups, and is event %d."
           "\n",
           ar->currentline, ar->source, "",//ar->short_src,
           ar->linedefined, ar->lastlinedefined,
           ar->what, ar->namewhat, ar->name,
           ar->nups, ar->event
           );
           */
    #endif
}



static void
nlua_thread(int pin, int pout, char *project, char *filename)
{
    char cmd[4096];
    int cmd_size;

    lua_State *L;
    L = lua_open();

    if (!L) {
        cmd[0] = LC_ERROR;
        cmd_size = snprintf(cmd+1, sizeof(cmd)-2, "Unable to open lua") + 1;
        write(1, cmd, cmd_size);
        exit(0);
    }

    luaL_openlibs(L);
    lua_sethook(L, nlua_interpret_hook, LUA_MASKLINE, 0);

    /* If a filename was specified, load it in and run it */
    if (project && *project) {
        char full_filename[2048];
        if (filename && *filename)
            snprintf(full_filename, sizeof(full_filename)-1,
                    "%s/%s/%s", PROJECT_DIR, project, filename);
        else
            snprintf(full_filename, sizeof(full_filename)-1,
                    "%s/%s", PROJECT_DIR, project);

        if (luaL_dofile(L, full_filename)) {
            cmd[0] = LC_ERROR;
            cmd_size = snprintf(cmd+1, sizeof(cmd)-2, "Unable to load file: %s",
                    lua_tostring(L, 1)) + 1;
            write(1, cmd, cmd_size);
        }
    }

    /* If no file was specified, enter REPL mode */
    else {
        printf("Entering REPL mode...\n");
        int status;
        while ((status = loadline(L)) != -1) {
            if (status == 0)
                status = docall(L, 0, 0);
            report(L, status);
            if (status == 0 && lua_gettop(L) > 0) {  /* any result to print? */
                lua_getglobal(L, "print");
                lua_insert(L, 1);
                if (lua_pcall(L, lua_gettop(L)-1, 0, 0) != 0)
                    l_message(progname,
                        lua_pushfstring(L, "error calling " LUA_QL("print") " (%s)",
                        lua_tostring(L, -1)));
            }
        }
        lua_settop(L, 0);
        fputs("\n", stdout);
        fflush(stdout);
    }

    lua_close(L);
    close(pin);
    close(pout);
    exit(0);

    return;
}


static int
tohex(char c)
{
    if (c>='0' && c<='9')
        return c-'0';
    if (c>='a' && c<='f')
        return c-'a'+10;
    if (c>='A' && c<='F')
        return c-'A'+10;
    return c;
}

static int
nlua_stdio(FCGX_Request *request, char *enc, int id, int timeout)
{
    char *method = FCGX_GetParam("REQUEST_METHOD", request->envp);

    /* If it's a POST, then write to Lua's stdin */
    if (!strcmp(method, "POST")) {
        char bfr[8192];
        int bytes_read, bytes_written;

        while ( (bytes_read = FCGX_GetStr(bfr, sizeof(bfr), request->in)) > 0) {
            char *left = bfr;

            write(STDOUT_FILENO, bfr, bytes_read);
            if (enc && !strcmp(enc, "hex")) {
                char *s = bfr;
                int input=0, output=0;
                while (input < bytes_read) {
                    s[output] = (tohex(s[input++])<<4)&0xf0;
                    if (s[input])
                        s[output] |= tohex(s[input++])&0x0f;
                    output++;
                }
                s[output] = '\0';
                bytes_read /= 2;
            }

            while (bytes_read > 0) {
                bytes_written = write(nlua_states[id].in_fd, left, bytes_read);
                if (bytes_written < 0) {
                    perror("Unable to write file");
                    return make_error(request, "Unable to write file", errno);
                }
                left += bytes_written;
                bytes_read -= bytes_written;
            }
        }
        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n\r\n");
    }

    /* If it's not a POST, then read from stdout / stderr */
    else if (!strcmp(method, "GET")) {
        fd_set s;
        struct timeval t = {timeout, 20000};
        int i;

        FD_ZERO(&s);
        FD_SET(nlua_states[id].out_fd, &s);

        i = select(nlua_states[id].out_fd+1, &s, NULL, NULL, &t);
        if (i > 0) {
            char bfr[4096];
            i = read(nlua_states[id].out_fd, bfr, sizeof(bfr));
            if (i == -1 && (errno == EINTR || errno == EAGAIN)) {
                
                /* Interrupted, but try again */
                FCGX_FPrintF(request->out, "Content-Type: text/html\r\n\r\n");
                return 0;
            }

            else if (i == -1) {
                /* Unrecoverable error */
                kill(nlua_states[id].pid, SIGKILL);
                kill(nlua_states[id].pid, SIGTERM);
                close(nlua_states[id].out_fd);
                nlua_states[id].out_fd = -1;
                nlua_pool_status[id] = 0;
                return make_error(request, "Unable to write to stdin", errno);
            }

            else if (i == 0) {
                /* Connection closed */
                close(nlua_states[id].out_fd);
                nlua_states[id].out_fd = -1;
                nlua_pool_status[id] = 0;
                FCGX_FPrintF(request->out, "Content-Type: text/plain\r\n"
                                           "Status: 204\r\n"
                                           "\r\n");
            }
            else {
                FCGX_FPrintF(request->out, "Content-Type: text/plain\r\n"
                                           "\r\n");
                FCGX_PutStr(bfr, i, request->out);
            }
        }

        else if (!i || (i == -1 && (errno == EINTR || errno == EAGAIN))) {
            /* No data to read */
            FCGX_FPrintF(request->out, "Content-Type: text/html\r\n\r\n");
        }
        else {
            /* Error occurred */
            kill(nlua_states[id].pid, SIGKILL);
            kill(nlua_states[id].pid, SIGTERM);
            close(nlua_states[id].out_fd);
            nlua_states[id].out_fd = -1;
            nlua_pool_status[id] = 0;
            return make_error(request, "Unable to read from stdout", errno);
        }
    }
    else {
        return make_error(request, "Unrecognized http method", 0);
    }

    return 0;
}

/* Open a LUA state.  Forks a new process and sets it to be idle */
static int
nlua_open(FCGX_Request *request, char *project, char *filename)
{
    int thread_id = -1;
    int i;
    int p[4][2];

    signal(SIGTERM, nlua_sig);

    for(i=0; i<MAX_THREAD_ID && thread_id < 0; i++) 
        if (!nlua_pool_status[i])
            thread_id = i;
    if (thread_id < 0)
        return -ENOSPC;


    nlua_pool_status[thread_id] = 1;


    if (-1 == pipe(p[0])) {
        nlua_pool_status[thread_id] = 0;
        return -errno;
    }

    if (-1 == pipe(p[1])) {
        close(p[0][0]);
        close(p[0][1]);
        nlua_pool_status[thread_id] = 0;
        return -errno;
    }
    pipe(p[2]);
    pipe(p[3]);

    nlua_states[thread_id].pid = fork();

    if (-1 == nlua_states[thread_id].pid) {
        close(p[0][0]);
        close(p[0][1]);
        close(p[1][0]);
        close(p[1][1]);
        close(p[2][0]);
        close(p[2][1]);
        close(p[3][0]);
        close(p[3][1]);
        nlua_pool_status[thread_id] = 0;
        return -errno;
    }

    else if(!nlua_states[thread_id].pid) {
        /* Make p[1][1] be stdin */
        if (p[1][1] != 1) {
            dup2(p[1][1], fileno(stdout));
            dup2(p[1][1], fileno(stderr));
            close(p[1][1]);
        }

        /* Make p[0][0] be stdin */
        if (p[0][0] != 0) {
            dup2(p[0][0], fileno(stdin));
            close(p[0][0]);
        }

        close(p[0][1]);
        close(p[1][0]);
        close(p[2][1]);
        close(p[3][0]);

        /* Reset buffering on the new descriptors */
        setlinebuf(stdin);
        setlinebuf(stdout);
        setlinebuf(stderr);

        nlua_thread(p[2][0], p[3][1], project, filename);
        exit(0);
    }

    close(p[0][0]);
    close(p[1][1]);
    close(p[2][0]);
    close(p[3][1]);

    /* We're running as parent process */
    nlua_states[thread_id].in_fd    = p[0][1];
    nlua_states[thread_id].out_fd   = p[1][0];
    nlua_states[thread_id].in_ctrl  = p[2][1];
    nlua_states[thread_id].out_ctrl = p[3][0];

    return thread_id;
}


static int
nlua_close(FCGX_Request *request, int thread_id)
{

    if (!nlua_pool_status[thread_id])
        return 0;

    kill(nlua_states[thread_id].pid, SIGTERM);
    kill(nlua_states[thread_id].pid, SIGKILL);
    nlua_pool_status[thread_id] = 0;
    return 0;
}


static int
nlua_eval(FCGX_Request *request)
{
    return make_error(request, "Unimplemented", ENOSYS);
#if 0
    char *program;
    int prog_ptr = 0;
    lua_State *lua;
    int streamer[2];
    int in, out;
    int content_length;
    char *tmp = FCGX_GetParam("CONTENT_LENGTH", request->envp);

    close(0);
    content_length = strtoul(content_length, 0, NULL);

    /* If there's no program, there's nothing to do */
    if (content_length <= 0) {
        FCGX_FPrintF(request, "Content-Type: text/plain\r\n\r\n");
        return 0;
    }

    program = malloc(con->request.content_length+1);
    if (!program)
        return make_error(request, "Program size to large", ENOMEM);

    program[content_length] = '\0';

    /* there is content to eval */
    for (c = cq->first; c; c = cq->first) {
        int r = 0;

        /* copy all chunks */
        switch(c->type) {
        case FILE_CHUNK:

            if (c->file.mmap.start == MAP_FAILED) {
                if (-1 == c->file.fd &&  /* open the file if not already open */
                    -1 == (c->file.fd = open(c->file.name->ptr, O_RDONLY))) {
                    //log_error_write(srv, __FILE__, __LINE__, "ss", "open failed: ", strerror(errno));

                    free(program);
                    return -1;
                }

                c->file.mmap.length = c->file.length;

                if (MAP_FAILED == (c->file.mmap.start = mmap(0,  c->file.mmap.length, PROT_READ, MAP_SHARED, c->file.fd, 0))) {
                    //log_error_write(srv, __FILE__, __LINE__, "ssbd", "mmap failed: ", strerror(errno), c->file.name,  c->file.fd);

                    free(program);
                    return -1;
                }

                close(c->file.fd);
                c->file.fd = -1;

                /* chunk_reset() or chunk_free() will cleanup for us */
            }

            memcpy(program+prog_ptr, c->file.mmap.start+c->offset,
                    c->file.length - c->offset);
            r = c->file.length - c->offset;
            break;
        case MEM_CHUNK:
            memcpy(program+prog_ptr,
                    c->mem->ptr + c->offset,
                    c->mem->used - c->offset - 1);
            r = c->mem->used - c->offset - 1;
            break;
        case UNUSED_CHUNK:
            break;
        }

        c->offset += r;
        cq->bytes_out += r;
        prog_ptr += r;
        chunkqueue_remove_finished_chunks(cq);
    }

    lua = lua_open();
    if (!lua)
        return make_error(con, "Unable to open lua", errno);

    luaL_openlibs(lua);

    pipe(streamer);
    out = dup2(streamer[1], 0);
    in = streamer[0];

    if (streamer[1] != out)
        close(streamer[1]);

    if (luaL_dostring(lua, program)) {
        char errmsg[2048];
        snprintf(errmsg, sizeof(errmsg)-1,
                "LUA program \"%s\" encountered an error: %s", program, lua_tostring(lua, 1));
        make_error(con, errmsg, 1);
        con->http_status = 200;
    }
    else {
        char data[4096];
        int len;
        bzero(data, sizeof(data));
        len = read(in, data, sizeof(data));
        b = chunkqueue_get_append_buffer(con->write_queue);
        buffer_copy_string_len(b, data, len);
        con->http_status = 200;
    }
    lua_close(lua);
    free(program);

    close(streamer[1]);
    close(streamer[0]);
    close(out);
    close(in);

    return HANDLER_FINISHED;
#endif
}






/* init the plugin data */
/*
INIT_FUNC(mod_netv_init) {
	plugin_data *p;
    int i;

	p = calloc(1, sizeof(*p));

	p->match_buf = buffer_init();
    for (i=0; i<MAX_THREAD_ID; i++) {
        nlua_states[i].in_fd    = -1;
        nlua_states[i].out_fd   = -1;
        nlua_states[i].in_ctrl  = -1;
        nlua_states[i].out_ctrl = -1;
    }

	return p;
}
*/


static int
parse_lua_uri(char *uri, char *cmd, char *token, char *arg, int sz)
{
    char *uri_tmp;
    char *slashes;
    int len;

    bzero(cmd, sz);
    bzero(token, sz);
    bzero(arg, sz);

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

static enum lua_operation
determine_lua_operation(char *cmd, char *token, char *arg)
{
    if (!strcmp(cmd, "list"))
        return LUA_LIST;

    else if(!strcmp(cmd, "eval"))
        return LUA_EVAL;

    else if(!strcmp(cmd, "close"))
        return LUA_CLOSE;

    else if(!strcmp(cmd, "bpdel"))
        return LUA_BPDEL;

    else if(!strcmp(cmd, "bpget"))
        return LUA_BPGET;

    else if(!strcmp(cmd, "bpadd"))
        return LUA_BPADD;

    else if(!strcmp(cmd, "ping"))
        return LUA_PING;

    else if(!strcmp(cmd, "state"))
        return LUA_STATE;

    else if(!strcmp(cmd, "stdio"))
        return LUA_STDIO;

    else if(!strcmp(cmd, "run"))
        return LUA_RUN;

    else if(!strcmp(cmd, "open"))
        return LUA_OPEN;

    return LUA_UNKNOWN;
}


int
handle_lua_uri(FCGX_Request *request)
{
    char cmd[1024];
    char token[1024];
    char arg[1024];
    enum lua_operation lo;
    int id; // token ID, parsed as an int
    char *uri = FCGX_GetParam("REQUEST_URI", request->envp) + strlen("/lua/");


    parse_lua_uri(uri, cmd, token, arg, sizeof(cmd));

    lo = determine_lua_operation(cmd, token, arg);
    fprintf(stderr, "Raw URI: %s  Command: %s  Token: %s  Arg: %s Lua Operation: %s\n", uri, cmd, token, arg, op_to_str(lo));


    if (lo == LUA_EVAL)
        return nlua_eval(request);

    else if(lo == LUA_OPEN) {
        id = nlua_open(request, token, arg);
        if (id < 0)
            return make_error(request, "Unable to open LUA environment", -id);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n"
                                   "%d\n", id);
    }

    else if (lo == LUA_CLOSE) {
        id = strtoul(token, NULL, 16);
        if (id > MAX_THREAD_ID)
            return make_error(request, "Invalid thread ID specified", EINVAL);

        nlua_close(request, id);

        FCGX_FPrintF(request->out, "Content-Type: text/html\r\n"
                                   "\r\n"
                                   "OK\n");
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

        return nlua_stdio(request, arg, id, timeout);
    }

    else if (lo == LUA_LIST) {
        FCGX_FPrintF(request->out, "Content-Type: text/plain\r\n\r\n");
        for (id=0; id<MAX_THREAD_ID; id++)
            if (nlua_pool_status[id])
                FCGX_FPrintF(request->out, "%d\r\n", id);
    }


    else if (lo == LUA_UNKNOWN)
        return make_error(request, "No valid LUA command specified", EINVAL);

    return 0;
}

