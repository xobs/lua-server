#include <stdio.h>
#include <stdlib.h>
#include <fcgiapp.h>
#include <pthread.h>
#include <unistd.h>

#define FCGI_SOCKET "/tmp/lua.socket"
#define THREAD_COUNT 20

static int
handle_request(FCGX_Request *request)
{
    return 0;
}

static void *
request_thread(void *s_ptr)
{
    int rc;
    int counter = 0;
    FCGX_Request request;

    if (FCGX_InitRequest(&request, *((int *)s_ptr), 0)) {
        perror("Unable to init request");
        return NULL;
    }

    while (1) {
        static pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER;
        static pthread_mutex_t counts_mutex = PTHREAD_MUTEX_INITIALIZER;

        pthread_mutex_lock(&accept_mutex);
        rc = FCGX_Accept_r(&request);
        pthread_mutex_unlock(&accept_mutex);

        handle_request(&request);
        FCGX_FPrintF(request.out, "\r\n\r\nRequest count: %d (wait...)\n", counter++);
        sleep(5);
        FCGX_FPrintF(request.out, "OK");
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
