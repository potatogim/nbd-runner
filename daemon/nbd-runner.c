/*
  Copyright (c) 2019 Red Hat, Inc. <http://www.redhat.com>
  This file is part of nbd-runner.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#define   _GNU_SOURCE

#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <rpc/pmap_clnt.h>
#include <signal.h>
#include <errno.h>
#include <event.h>
#include <arpa/inet.h>

#include "utils.h"
#include "version.h"
#include "nbd-log.h"
#include "rpc_nbd.h"
#include "rpc_nbd_svc.h"
#include "nbd-common.h"

#define NBD_LOCK_FILE "/run/nbd-runner.lock"
#define NBD_MIN_THREADS  1
#define NBD_DEF_THREADS  1
#define NBD_MAX_THREADS  16

static char *iohost;

struct io_thread_data {
    int threads;
    int sockfd;
};

extern int io_port;

static void usage(void)
{
    _nbd_out("Usage:\n"
             "\tnbd-runner [<args>]\n\n"
             "Commands:\n"
             "\thelp\n"
             "\t\tDisplay help for nbd-runner command\n\n"
             "\tthreads <NUMBER>\n"
             "\t\tSpecify the IO thread number for each mapped backstore, 1 as default\n\n"
             "\trpchost <CONTROL_HOST>\n"
             "\t\tSpecify the listenning IP for the nbd-runner server to receive/reply the control\n"
             "\t\tcommands(create/delete/map/unmap/list, etc) from nbd-cli, INADDR_ANY as default\n\n"
             "\tiohost <IO_HOST>\n"
             "\t\tSpecify the listenning IP for the nbd-runner server to receive/reply the NBD device's\n"
             "\t\tIO operations(WRITE/READ/FLUSH/TRIM, etc), INADDR_ANY as default\n\n"
             "\tversion\n"
             "\t\tShow version info and exit.\n\n"
             "\tNOTE:\n"
             "\t\tThe CONTROL_HOST and the IO_HOST will be useful if you'd like the control commands\n"
             "\t\troute different from the IOs route via different NICs, or just omit them as default\n"
            );
}

static void *worker_thread(void *arg)
{
    struct io_thread_data *data = arg;

    if (nbd_handle_request(data->sockfd, data->threads))
        return NULL;

    return NULL;
}

static void event_handler(int listenfd, short event, void *arg)
{
    pthread_t thread_id;
    struct sockaddr_storage addr_in;
    socklen_t sin_size = sizeof(addr_in);
    struct io_thread_data data = {0, };
    int acceptfd;

    acceptfd = accept(listenfd, (struct sockaddr*)&addr_in, &sin_size);
    if(acceptfd < 0) {
        nbd_err("error occure in accept: %s\n", strerror(errno));
        return;
    }

    data.threads = *(int *)arg;
    data.sockfd = acceptfd;

    if (pthread_create(&thread_id, NULL, worker_thread, &data) != 0)
        nbd_err("failed to create thread: %s!\n", strerror(errno));

    if (pthread_detach(thread_id) != 0)
        nbd_err("failed to detach thread: %s!\n", strerror(errno));
}

static void *nbd_map_svc_thread_start(void *arg)
{
    struct sockaddr_in sin = {0,};
    struct event_base *base;
    struct event listen_ev;
    int listenfd;
    int ret = 0;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd < 0){
        nbd_err("failed to create socket: %s\n", strerror(errno));
        return NULL;
    }

    sin.sin_family = AF_INET;
    if (iohost) {
        if (inet_pton(AF_INET, iohost, (void *)&sin.sin_addr.s_addr) < 0)
        {
            nbd_err("failed to convert %s to binary form!\n", iohost);
            goto err;
        }
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }

again:
    sin.sin_port = htons(io_port);

    if (bind(listenfd, (struct sockaddr*)&sin, sizeof(struct sockaddr)) < 0) {
        nbd_out("bind on port %d failed, %s\n", io_port, strerror(errno));
        nbd_out("will try to use port %d!\n", ++io_port);
        goto again;
    }

    if (listen(listenfd, 16) < 0) {
        nbd_err("failed to start listening on a socket: %s\n", strerror(errno));
        goto err;
    }

    evutil_make_socket_nonblocking(listenfd);

    base = event_base_new();
    if (!base) {
        nbd_err("failed to create event base: %s\n", strerror(errno));
        goto err;
    }

    event_set(&listen_ev, listenfd, EV_READ|EV_PERSIST, event_handler, arg);
    event_base_set(base, &listen_ev);
    event_add(&listen_ev, NULL);
    event_base_dispatch(base);

    nbd_out("nbd server exits!\n");

    event_del(&listen_ev);
    event_base_free(base);

err:
    close(listenfd);
    return NULL;
}

static void *nbd_rpc_svc_thread_start(void *arg)
{
    register SVCXPRT *transp = NULL;
    struct sockaddr_in sin = {0, };
    int listenfd = RPC_ANYSOCK;
    char *rpchost = arg;
    int opt = 1;
    int ret;

    listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenfd < 0) {
        nbd_err("socket creation failed, %s\n", strerror(errno));
        return NULL;
    }

    ret = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret < 0) {
        nbd_err("setsocket set option to re-use address failed, %s\n",
                strerror(errno));
        goto out;
    }

    sin.sin_family = AF_INET;
    if (rpchost) {
        if (inet_pton(AF_INET, rpchost, (void *)&sin.sin_addr.s_addr) < 0)
        {
            nbd_err("failed to convert %s to binary form!\n", rpchost);
            goto out;
        }
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    sin.sin_port = htons(NBD_RPC_SVC_PORT);

    ret = bind(listenfd, (struct sockaddr *) &sin, sizeof (sin));
    if (ret < 0) {
        nbd_err("bind on port %d failed, %s\n", NBD_RPC_SVC_PORT,
                strerror(errno));
        goto out;
    }

#if HAVE_TIRPC
    if (listen(listenfd, 16) < 0) {
        nbd_err("failed to start listening on a socket: %s\n",
                strerror(errno));
        goto out;
    }
#endif

    transp = svctcp_create(listenfd, 0, 0);
    if (!transp) {
        nbd_err("svctcp_create failed, %s\n", strerror(errno));
        goto out;
    }

    if (!svc_register(transp, RPC_NBD, RPC_NBD_VERS, rpc_nbd_1, IPPROTO_TCP)) {
        nbd_err("Please check if rpcbind service is running.");
        goto out;
    }

    svc_run();

out:
    if (transp)
        svc_destroy(transp);

    if (listenfd != RPC_ANYSOCK)
        close(listenfd);

    return NULL;
}

int main (int argc, char **argv)
{
    int lockfd = -1;
    pthread_t rpc_svc_threadid;
    pthread_t map_svc_threadid;
    struct flock lock = {0, };
    char *rpchost = NULL;
    int threads = NBD_DEF_THREADS;
    int ret;
    int ind;

    nbd_service_init();

    ret = nbd_log_init();
    if (ret < 0) {
        nbd_err("nbd_log_init failed!\n");
        goto out;
    }

    ind = 1;
    while (ind < argc) {
        if (!strcmp("iohost", argv[ind])) {
            if (ind + 1 >= argc) {
                nbd_err("Invalid argument '<iohost IO_HOST>'!\n\n");
                goto out;
            }

            iohost = strdup(argv[ind + 1]);
            if (!iohost) {
                nbd_err("No memory for host!\n");
                goto out;
            }

            if (!nbd_init_iohost(iohost, AF_INET))
                goto out;

            ind += 2;
        } else if (!strcmp("rpchost", argv[ind])) {
            if (ind + 1 >= argc) {
                nbd_err("Invalid argument 'rpchost <CONTROL_HOST>'!\n\n");
                goto out;
            }

            rpchost = strdup(argv[ind + 1]);
            if (!rpchost) {
                nbd_err("No memory for host!\n");
                goto out;
            }

            ind += 2;
        } else if (!strcmp("threads", argv[ind])) {
            if (ind + 1 >= argc) {
                nbd_err("Invalid argument 'threads <NUM>'!\n\n");
                goto out;
            }

            threads = atoi(argv[ind + 1]);
            if (threads < NBD_MIN_THREADS) {
                nbd_err("Currently the min threads are %d, will set it to %d!\n",
                        NBD_MIN_THREADS);
                threads = NBD_MIN_THREADS;
            }

            if (threads > NBD_MAX_THREADS) {
                nbd_err("Currently the max threads are %d, will set it to %d!\n",
                        NBD_MAX_THREADS, NBD_MAX_THREADS);
                threads = NBD_MAX_THREADS;
            }

            ind += 2;
        } else if (!strcmp("version", argv[ind])) {
            _nbd_out("nbd-runner (%d.%d)\n\n", NBD_VERSION_MAJ, NBD_VERSION_MIN);
            _nbd_out("%s\n", NBD_LICENSE_INFO);
            goto out;
        } else if (!strcmp("help", argv[ind])) {
            usage();
            goto out;
        } else {
            nbd_err("Invalid argument '%s'!\n\n", argv[ind]);
            usage();
            goto out;
        }
    }

    /* make sure only one nbd-runner daemon is running */
    lockfd = creat(NBD_LOCK_FILE, S_IRUSR | S_IWUSR);
    if (lockfd == -1) {
        nbd_err("create lock file :%s failed, %s!\n", NBD_LOCK_FILE,
                strerror(errno));
        goto out;
    }

    lock.l_type = F_WRLCK;
    if (fcntl(lockfd, F_SETLK, &lock) == -1) {
        nbd_err("nbd-runner service is already running...\n");
        goto out;
    }

    /* set signal */
    signal(SIGPIPE, SIG_IGN);

    pmap_unset(RPC_NBD, RPC_NBD_VERS);

    pthread_create(&rpc_svc_threadid, NULL, nbd_rpc_svc_thread_start, rpchost);
    pthread_create(&map_svc_threadid, NULL, nbd_map_svc_thread_start, &threads);
    pthread_join(map_svc_threadid, NULL);
    pthread_join(rpc_svc_threadid, NULL);

    nbd_err("svc_run returned %s\n", strerror (errno));

    lock.l_type = F_UNLCK;
    if (fcntl(lockfd, F_SETLK, &lock) == -1) {
        nbd_err("fcntl unlock pidfile %s failed, %s\n", NBD_LOCK_FILE,
                strerror(errno));
    }

out:
    free(rpchost);
    nbd_fini_iohost();
    if (lockfd != -1)
        close(lockfd);
    nbd_log_destroy();
    nbd_service_fini();
    exit (EXIT_FAILURE);
}
