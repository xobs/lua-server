#include <stdio.h>
#include <stdlib.h>
#include <fcgiapp.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define FCGI_SOCKET "/tmp/lua.socket"
#define THREAD_COUNT 20

int handle_file_uri(FCGX_Request *request);
int handle_lua_uri(FCGX_Request *request);

static int
handle_request(FCGX_Request *request)
{
    int ret;
    char *uri;
    
    ret = -1;
    uri = FCGX_GetParam("REQUEST_URI", request->envp);

    if (!strncmp(uri, "/lua/", strlen("/lua/"))) {
        ret = handle_lua_uri(request);
    }

    else if (!strncmp(uri, "/file/", strlen("/file/"))) {
        ret = handle_file_uri(request);
    }

    else {
        fprintf(stderr, "Unrecognized URI: %s\n", uri);
    }

    return ret;
}

static void *
request_thread(void *s_ptr)
{
    int rc;
    FCGX_Request request;

    if (FCGX_InitRequest(&request, *((int *)s_ptr), 0)) {
        perror("Unable to init request");
        return NULL;
    }

    while (1) {
        static pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER;
        //static pthread_mutex_t counts_mutex = PTHREAD_MUTEX_INITIALIZER;

        pthread_mutex_lock(&accept_mutex);
        rc = FCGX_Accept_r(&request);
        pthread_mutex_unlock(&accept_mutex);

        handle_request(&request);
        FCGX_Finish_r(&request);
    }


    return NULL;
}

int
main(int argc, char **argv)
{
    int listen_socket;
    int i;
    pthread_t threads[THREAD_COUNT];

    FCGX_Init();
    listen_socket = FCGX_OpenSocket(FCGI_SOCKET, 10);
    if (listen_socket < 0) {
        perror("Unable to open listen socket");
        exit(1);
    }

    for (i=0; i<THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, request_thread, (void *)&listen_socket);
        pthread_detach(threads[i]);
    }

    request_thread((void *)&listen_socket);
    return 0;
}
