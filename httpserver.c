#include "asgn2_helper_funcs.h"
#include "hash_table.h"
#include "queue.h"
#include "rwlock.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <regex.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#define ARRAY_SIZE(arr) (sizeof((arr)) / sizeof((arr)[0]))
#define MAXSIZE         2048

hashTable *ht;
queue_t *q;

pthread_mutex_t mutex_read_insert = PTHREAD_MUTEX_INITIALIZER;

typedef enum {
    OK,
    CREATED,
    BAD_REQUEST,
    FORBIDDEN,
    NOT_FOUND,
    INTERNAL_SERVER_ERROR,
    NOT_IMPLEMENTED,
    VERSION_NOT_SUPPORTED
} Status_response;

typedef struct Request {
    char buf[MAXSIZE + 1];
    node *node;
    char *method;
    char *path;
    char *version;
    intptr_t connection_fd;
    long length;
    size_t remaining_bytes;
    int request_id;
    uint16_t total_offset;
} Request;

typedef struct ThreadObj {
    Request *r;
    pthread_t thread;

    // ! thread_id is for debugging
    int thread_id;
} ThreadObj;

typedef ThreadObj *Thread;

void aduit_log(const char *method, const char *path, uint16_t status_code, int request_id) {
    fprintf(stderr, "%s,/%s,%u,%d\n", method, path, status_code, request_id);
}

void reponse_message(Request *r, Status_response status_code) {
    switch ((Status_response) status_code) {
    case OK: dprintf(r->connection_fd, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nOK\n"); break;
    case CREATED:
        dprintf(r->connection_fd, "HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n");
        break;
    case BAD_REQUEST:
        dprintf(r->connection_fd,
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n");
        break;
    case FORBIDDEN:
        dprintf(
            r->connection_fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n");
        break;
    case NOT_FOUND:
        dprintf(
            r->connection_fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 10\r\n\r\nNot Found\n");
        break;
    case INTERNAL_SERVER_ERROR:
        dprintf(r->connection_fd,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server "
            "Error\n");
        break;
    case NOT_IMPLEMENTED:
        dprintf(r->connection_fd,
            "HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n\r\nNot Implemented\n");
        break;
    case VERSION_NOT_SUPPORTED:
        dprintf(r->connection_fd,
            "HTTP/1.1 505 Version Not Supported\r\nContent-Length: 22\r\n\r\nVersion Not "
            "Supported\n");
        break;
    }
}

ssize_t recv_until(int socketfd, char *buf, ssize_t count, char *needle) {
    char *ret = NULL;
    ssize_t bytes_read, total_read = 0;

    while (ret == NULL && total_read < count) {
        if ((bytes_read = recv(socketfd, buf + total_read, 1, 0)) == -1)
            return (EXIT_FAILURE);
        total_read += bytes_read;
        buf[total_read] = '\0';
        ret = strstr(buf, needle);
    }

    return (EXIT_SUCCESS);
}

int is_directory(Request *r) {
    return open(r->path, O_RDONLY | O_DIRECTORY);
}

uint64_t get_file_size(int fd) {
    struct stat statbuf;
    fstat(fd, &statbuf);
    return statbuf.st_size;
}

int parsing_request(Request *r, char *buf) {
    static const char *const request_line
        = "^([a-zA-Z]{1,8}) /([a-zA-Z0-9._-]{1,64}) (HTTP/1\\.[0-9])\r\n";
    static const char *const header_field = "([a-zA-Z0-9.-]{1,128}): ([ -~]{1,128})\r\n";

    regex_t regex;
    regmatch_t pmatch[4];

    regcomp(&regex, request_line, REG_NEWLINE | REG_EXTENDED);
    if (regexec(&regex, buf, ARRAY_SIZE(pmatch), pmatch, 0)) {
        reponse_message(r, BAD_REQUEST);
        regfree(&regex);
        return (EXIT_FAILURE);
    }

    r->total_offset = 0;
    buf[pmatch[1].rm_eo] = '\0';
    buf[pmatch[2].rm_eo] = '\0';
    buf[pmatch[3].rm_eo] = '\0';

    r->method = &buf[pmatch[1].rm_so];
    r->path = &buf[pmatch[2].rm_so];
    r->version = &buf[pmatch[3].rm_so];

    r->total_offset += (pmatch[3].rm_eo + 2);
    buf += (pmatch[3].rm_eo + 2); // Move the buf to header-field.

    regcomp(&regex, header_field, REG_NEWLINE | REG_EXTENDED);
    while (regexec(&regex, buf, 3, pmatch, 0) == 0) {
        buf[pmatch[1].rm_eo] = '\0';
        if ((strncmp(buf, "Content-Length", 14)) == 0) {
            buf[pmatch[2].rm_eo] = '\0';
            r->length = strtoll(&buf[pmatch[2].rm_so], NULL, 10);
            if (errno == EINVAL) {
                reponse_message(r, BAD_REQUEST);
                regfree(&regex);
                return (EXIT_FAILURE);
            }
        } else if ((strncmp(buf, "Request-Id", 10)) == 0) {
            buf[pmatch[2].rm_eo] = '\0';
            r->request_id = strtoll(&buf[pmatch[2].rm_so], NULL, 10);
            if (errno == EINVAL || r->request_id < 0) {
                reponse_message(r, BAD_REQUEST);
                regfree(&regex);
                return (EXIT_FAILURE);
            }
        }
        buf += (pmatch[2].rm_eo + 2);
        r->total_offset += (pmatch[2].rm_eo + 2);
    }

    regfree(&regex);
    return (EXIT_SUCCESS);
}

int put_method(Request *r) {
    int fd, status_code;
    ssize_t num_bytes_passed;

    writer_lock(r->node->rwlock);

    if (r->length < 0) {
        aduit_log(r->method, r->path, 400, r->request_id);
        reponse_message(r, BAD_REQUEST);
        writer_unlock(r->node->rwlock);
        return (EXIT_FAILURE);
    }

    status_code = 201;
    if ((fd = open(r->path, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0666)) == -1) {
        if (errno == EEXIST) {
            status_code = 200;
        } else {
            reponse_message(r, INTERNAL_SERVER_ERROR);
            writer_unlock(r->node->rwlock);
            return (EXIT_FAILURE);
        }
    }

    if (status_code == 200) {
        if ((fd = open(r->path, O_WRONLY | O_TRUNC)) == -1) {
            if (errno == EACCES) {
                aduit_log(r->method, r->path, 403, r->request_id);
                reponse_message(r, FORBIDDEN);
                writer_unlock(r->node->rwlock);
                return (EXIT_FAILURE);
            } else {
                reponse_message(r, INTERNAL_SERVER_ERROR);
                writer_unlock(r->node->rwlock);
                return (EXIT_FAILURE);
            }
        }
    }

    if (is_directory(r) != -1) {
        aduit_log(r->method, r->path, 403, r->request_id);
        reponse_message(r, FORBIDDEN);
        writer_unlock(r->node->rwlock);
        return 1;
    }

    if ((num_bytes_passed = pass_n_bytes(r->connection_fd, fd, r->length)) == -1) {
        reponse_message(r, INTERNAL_SERVER_ERROR);
        close(fd);
        writer_unlock(r->node->rwlock);
        return 1;
    }

    if (status_code == 200) {
        aduit_log(r->method, r->path, 200, r->request_id);
        reponse_message(r, OK);
    } else {
        aduit_log(r->method, r->path, 201, r->request_id);
        reponse_message(r, CREATED);
    }

    close(fd);
    writer_unlock(r->node->rwlock);
    return (EXIT_SUCCESS);
}

int get_method(Request *r) {
    uint64_t st_size;
    int fd;

    reader_lock(r->node->rwlock);

    if (is_directory(r) != -1) {
        reponse_message(r, FORBIDDEN);
        reader_unlock(r->node->rwlock);
        return (EXIT_FAILURE);
    }

    if ((fd = open(r->path, O_RDONLY)) == -1) {
        if (errno == EACCES)
            reponse_message(r, FORBIDDEN);
        else if (errno == ENOENT) {
            aduit_log(r->method, r->path, 404, r->request_id);
            reponse_message(r, NOT_FOUND);
        } else
            reponse_message(r, INTERNAL_SERVER_ERROR);

        reader_unlock(r->node->rwlock);
        return (EXIT_FAILURE);
    }

    st_size = get_file_size(fd);
    aduit_log("GET", r->path, 200, r->request_id);
    dprintf(r->connection_fd, "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", st_size);
    if ((pass_n_bytes(fd, r->connection_fd, st_size)) == -1) {
        reponse_message(r, INTERNAL_SERVER_ERROR);
        close(fd);
        reader_unlock(r->node->rwlock);
        return (EXIT_FAILURE);
    }

    close(fd);
    reader_unlock(r->node->rwlock);
    return (EXIT_SUCCESS);
}

int check_version(Request *r) {
    if ((strncmp(r->version, "HTTP/1.1", 9)) != 0) {
        reponse_message(r, VERSION_NOT_SUPPORTED);
        return (EXIT_FAILURE);
    }

    return (EXIT_SUCCESS);
}

int handle_request(Request *r) {
    if (recv_until(r->connection_fd, r->buf, MAXSIZE, "\r\n\r\n") == -1) {
        fprintf(stderr, "Unable to read from socket: %d\n", errno);
        return (EXIT_FAILURE);
    }

    if (parsing_request(r, r->buf) == EXIT_FAILURE)
        return (EXIT_FAILURE);

    if (check_version(r) == EXIT_FAILURE)
        return (EXIT_FAILURE);

    pthread_mutex_lock(&mutex_read_insert);
    if ((r->node = search(ht, r->path)) == NULL)
        r->node = insert(ht, r->path);
    pthread_mutex_unlock(&mutex_read_insert);

    if ((strncmp(r->method, "GET", 3)) == 0) {
        get_method(r);
    } else if ((strncmp(r->method, "PUT", 3)) == 0)
        put_method(r);
    else {
        reponse_message(r, NOT_IMPLEMENTED);
        return (EXIT_FAILURE);
    }

    return (EXIT_SUCCESS);
}

void *workerThreads(void *args) {
    void *ele;
    Thread thread = (Thread) args;

    while (true) {
        queue_pop(q, &ele);
        thread->r->connection_fd = (intptr_t) ele;
        handle_request(thread->r);
        close(thread->r->connection_fd);
    }
    return args;
}

int process_args(int argc, char **argv, int *port_number, int *num_threads) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./httpserver [-t threads] <port>\n");
        exit(EXIT_FAILURE);
    }

    int opt;

    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
        case 't':
            *num_threads = atoi(optarg);
            if (*num_threads < 1) {
                fprintf(stderr, "Invalid number of threads\n");
                exit(EXIT_FAILURE);
            }
            break;
        default: break;
        }
    }

    if (&argv[optind] == NULL) {
        fprintf(stderr, "Expected a port number\n");
        fprintf(stderr, "Usage: ./httpserver [-t threads] <port>\n");
        exit(EXIT_FAILURE);
    }

    *port_number = strtol(argv[optind], NULL, 10);
    if (errno == EINVAL || *port_number < 1) {
        fprintf(stderr, "Invalid Port\n");
        exit(EXIT_FAILURE);
    }
    return 0;
}

int main(int argc, char **argv) {
    int port_number = 0, num_threads = 4;

    process_args(argc, *(&argv), &port_number, &num_threads);

    ht = create_ht(250);
    q = queue_new(num_threads);
    Thread threads[num_threads];

    for (int i = 0; i < num_threads; i++) {
        threads[i] = malloc(sizeof(ThreadObj));
        threads[i]->thread_id = i;
        threads[i]->r = (Request *) malloc(sizeof(Request));
        threads[i]->r->connection_fd = -1;
        pthread_create(&threads[i]->thread, NULL, workerThreads, (void *) threads[i]);
    }

    Listener_Socket socket;
    if ((listener_init(&socket, port_number)) == -1) {
        fprintf(stderr, "Invalid Port\n");
        return (EXIT_FAILURE);
    }

    uintptr_t connection_fd = -1;
    while (true) {
        connection_fd = listener_accept(&socket);
        queue_push(q, (void *) connection_fd);
    }

    delete_ht(ht);
    queue_delete(&q);

    for (int i = 0; i < num_threads; i++) {
        free(threads[i]->r);
        threads[i]->r = NULL;
        free(threads[i]);
        threads[i] = NULL;
    }

    return 0;
}
