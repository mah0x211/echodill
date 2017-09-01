#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "libdill.h"


#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_PORT    5000
#define DEFAULT_NPROC   1
#define DEFAULT_NCONN   100
#define DEFAULT_NMSG    5000
#define DEFAULT_MSGLEN  6


static const int B_ERROR = -1;
static const int B_READY = -2;
static char *MESSAGE = NULL;
static size_t MESSAGELEN = DEFAULT_MSGLEN;


typedef struct {
    char *host;
    int port;
    int nproc;
    int nconn;
    int nmsg;
} bench_opts_t;


#define TOSTR(v)        TOSTRX(v)
#define TOSTRX(v)       #v


#define log(fmt,...) do{                                    \
    fprintf(stderr, "[%d:%s:%d] "fmt,                       \
            getpid(), __FILE__, __LINE__, ##__VA_ARGS__);   \
    if(errno){                                              \
        perror(" ");                                        \
    }                                                       \
    else {                                                  \
        fprintf(stderr,"\n");                               \
    }                                                       \
}while(0)


#define term(...) do{   \
    log(__VA_ARGS__);   \
    exit(EXIT_FAILURE); \
}while(0)


static inline int newch( size_t bytes )
{
    int ch = chmake( bytes );

    if(ch == -1){
        term("failed to chmake()");
    }

    return ch;
}


coroutine void client(int out, int trg, struct ipaddr *addr, int nmsg)
{
    int s = 0;
    int sent = 0;
    int val = 0;
    char *buf = malloc(sizeof(char) * MESSAGELEN);

    if(!buf){
        log("failed to malloc()");
        chsend(out, (void*)&B_ERROR, sizeof(int), -1);
        return;
    }
    else if((s = tcp_connect(addr, -1)) == -1){
        log("failed to tcp_connect()");
        chsend(out, (void*)&B_ERROR, sizeof(int), -1);
        return;
    }

    // notify established
    if(chsend(out, (void*)&B_READY, sizeof(int), -1) == -1){
        log("failed to chsend()");
        goto fail;
    }
    // get number of messages via channel
    else if(chrecv(trg, (void*)&val, sizeof(int), -1) == -1 &&
            (errno != EPIPE && errno != EBADF)){
        log("failed to chrecv()");
        goto fail;
    }

    // send echo messages
    for(int i = 0; i < nmsg; i++)
    {
        if(bsend(s, MESSAGE, MESSAGELEN, -1) == -1){
            log("failed to bsend()");
            break;
        }

        if(brecv(s, buf, MESSAGELEN, -1) == -1){
            log("failed to brecv()");
            break;
        }
        sent++;
    }

fail:
    chsend(out, (void*)&sent, sizeof(int), -1);
    tcp_close(s, -1);
}


static void worker(int s, struct ipaddr *addr, int nconn, int nmsg)
{
    int in = newch(sizeof(int));
    int trg = newch(sizeof(int));
    int nsent = 0;
    int sent = 0;
    int op = 0;

    for(int i = 0; i < nconn; i++){
        go(client(in, trg, addr, nmsg));
    }

    // wait until all connections established
    for(int i = 0; i < nconn; i++)
    {
        if(chrecv(in, (void*)&op, sizeof(int), -1) == -1){
            log("failed to chrecv()");
            goto fail;
        }
        else if(op != B_READY){
            log("failed to connect(): %d - %d/%d", op, i, nconn);
            goto fail;
        }
    }

    // log("%d connection established", nconn);

    // send ready message to main process
    if(send(s, (void*)&B_READY, sizeof(int), 0) == -1){
        log("failed to send()");
        goto fail;
    }
    raise(SIGSTOP);

    // run all coroutines
    hclose(trg);
    // receive results
    for(int i = 0; i < nconn; i++)
    {
        if(chrecv(in, (void*)&sent, sizeof(int), -1) == -1){
            log("failed to chrecv()");
            goto fail;
        }
        nsent += sent;
    }

fail:
    if(send(s, (void*)&nsent, sizeof(int), 0) == -1){
        log("failed to send()");
    }

    exit(EXIT_SUCCESS);
}


#define spawn(s, addr, nconn, nmsg) switch(fork()){ \
    case -1:                                        \
        term("failed to spawn()");                  \
    case 0:                                         \
        return worker(s, addr, nconn, nmsg);        \
}


static inline int setrcvtimeo( int s, double deadline )
{
    double sec = 0;
    double usec = modf(deadline, &sec);
    struct timeval tv = {
        .tv_sec = sec,
        .tv_usec = usec * 1000000
    };

    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const void*)&tv, sizeof(tv));
}


static inline int newipc( int *s, double deadline )
{
    return -(socketpair(AF_UNIX, SOCK_STREAM, 0, s) ||
             setrcvtimeo(s[0], deadline) || setrcvtimeo(s[1], deadline));
}


static void benchmark(struct ipaddr *addr, bench_opts_t *opts)
{
    int s[2];
    int nproc = opts->nproc;
    int nmsg_total = opts->nconn * opts->nmsg;
    int pconn = opts->nconn / nproc;
    int pconn_last = pconn + opts->nconn % nproc;
    int sent = 0;
    int nsent = 0;
    int64_t start = 0;
    int64_t end = 0;
    int64_t elapsed = 0;
    int op = 0;

    printf(
        "Destination: %s:%d\n"
        "    Workers: %d\n"
        "Connections: %d (connections per worker: %d - %d)\n"
        "   Messages: %d (connections * %d)\n"
        "Total Bytes: %ld (Messages * %ld bytes per message)\n"
        "--------------------------------------------------------------------------------\n",
        opts->host, opts->port,
        nproc,
        opts->nconn, pconn, pconn_last,
        nmsg_total, opts->nmsg,
        nmsg_total * MESSAGELEN, MESSAGELEN
    );

    if(newipc(s, 10) == -1){
        term("failed to newipc()");
    }

    // spawn workers
    for(int i = 1; i < nproc; i++){
        spawn(s[1], addr, pconn, opts->nmsg);
    }
    spawn(s[1], addr, pconn_last, opts->nmsg);

    // wait ready
    for(int i = 0; i < nproc; i++)
    {
        if(recv(*s, (void*)&op, sizeof(int), 0) == -1){
            term("failed to establish connections");
        }
        else if(op != B_READY){
            term("invalid message received %d", op);
        }
    }

    // unset timeout
    if(setrcvtimeo(*s, 0) == -1){
        term("failed to setrcvtimeo");
    }
    // wake workers after 1000 ms
    if(kill(0, SIGCONT) != 0){
        term("failed to kill()");
    }

    // run benchmark
    start = now();
    for(int i = 0; i < nproc; i++)
    {
        if(recv(*s, (void*)&sent, sizeof(int), 0) == -1){
            term("failed to recv()");
        }
        nsent += sent;
    }
    end = now();
    elapsed = end - start;

    printf(
        "   Message: %d/%d\n"
        "   Elapsed: %lld ms (end:%lld - start:%lld\n"
        "Throughput: %.f [#/sec]\n",
        nsent, nmsg_total,
        elapsed, end, start,
        nsent * 1000.0 / elapsed
    );
}



int main(int argc, char *argv[])
{
    bench_opts_t opts = {
        .host = DEFAULT_HOST,
        .port = DEFAULT_PORT,
        .nproc = sysconf(_SC_NPROCESSORS_ONLN),
        .nconn = DEFAULT_NCONN,
        .nmsg = DEFAULT_NMSG
    };
    size_t msglen = DEFAULT_MSGLEN;
    struct ipaddr addr;
    int opt;

    while((opt = getopt(argc, argv, "w:c:m:p:l:")) != -1)
    {
        switch(opt){
            // number of workers
            case 'w':
                opts.nproc = atoi(optarg);
                break;
            // number of connections
            case 'c':
                opts.nconn = atoi(optarg);
                break;
            // number of messages per connection
            case 'm':
                opts.nmsg = atoi(optarg);
                break;
            // port number
            case 'p':
                opts.port = atoi(optarg);
                break;

            // message length
            case 'l':
                msglen = atoi(optarg);
                break;

            default:
                printf(
                    "Usage: bench [-w nworker] [-c clients] [-m messages per client] [-l message length] [-p port] host\n"
                    "default values\n"
                    "            host | " TOSTR(DEFAULT_HOST) "\n"
                    "            port | " TOSTR(DEFAULT_PORT) "\n"
                    "         nworker | %d\n"
                    "         clients | " TOSTR(DEFAULT_NCONN) "\n"
                    "        messages | " TOSTR(DEFAULT_NMSG) "\n"
                    "  message length | " TOSTR(DEFAULT_MSGLEN) "\n",
                    opts.nproc
                );
                exit(EXIT_FAILURE);
            }
    }

    if (optind < argc) {
        opts.host = argv[optind];
    }

    MESSAGELEN = msglen;
    if(!(MESSAGE = malloc(sizeof(char) * MESSAGELEN))){
        term("failed to malloc()");
    }
    else if( ipaddr_remote( &addr, opts.host, opts.port, 0, -1) != 0 ){
        term("failed to ipaddr_remote()");
    }

    memset(MESSAGE, 1, MESSAGELEN);
    benchmark(&addr, &opts);

    return 0;
}
