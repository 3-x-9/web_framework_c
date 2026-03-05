#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <Python.h>

#define PORT 9090
#define BUFFER_SIZE 1024
#define MAX_ROUTES 10

typedef enum {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    UNKNOWN
} http_method;

typedef struct {
    PyObject_HEAD
    PyObject *router;
} RouterObject;

typedef struct {
    http_method method;
    char *path_start;
    size_t path_length; 
    char *body_start;
    size_t body_length;
    int client_fd;
} Request;

typedef struct {
    int client_fd;
    PyObject *router;
} ClientContext;

http_method parse_method(char *buffer) {
    if (strncmp(buffer, "GET", 3) == 0) {
        return GET;
    }
    if (strncmp(buffer, "POST", 4) == 0) {
        return POST;
    }
    if (strncmp(buffer, "PUT", 3) == 0) {
        return PUT;
    }
    if (strncmp(buffer, "DELETE", 6) == 0) {
        return DELETE;
    }
    if (strncmp(buffer, "PATCH", 5) == 0) {
        return PATCH;
    }
    return UNKNOWN;
}

void parse_path(char *buffer, Request *req) {
    char *first_space = strchr(buffer, ' ');
    if (!first_space) return;
    
    char *path_start = first_space + 1;
    
    char *second_space = strchr(path_start, ' ');
    if (!second_space) return;
    
    size_t path_length = second_space - path_start;

    req->path_length = path_length;
    req->path_start = path_start;

    return;
}

const char *get_mime_type(const char *file_extension) {
    if (strcmp(file_extension, "html") == 0) return "text/html";
    if (strcmp(file_extension, "css") == 0) return "text/css";
    if (strcmp(file_extension, "js") == 0) return "application/javascript";
    if (strcmp(file_extension, "png") == 0) return "image/png";
    if (strcmp(file_extension, "jpg") == 0 || strcmp(file_extension, "jpeg") == 0) return "image/jpeg";
    if (strcmp(file_extension, "json") == 0) return "application/json";
    if (strcmp(file_extension, "txt") == 0) return "text/plain";
    if (strcmp(file_extension, "pdf") == 0) return "application/pdf";
    if (strcmp(file_extension, "doc") == 0 || strcmp(file_extension, "docx") == 0) return "application/msword";
    if (strcmp(file_extension, "xls") == 0 || strcmp(file_extension, "xlsx") == 0) return "application/vnd.ms-excel";
    if (strcmp(file_extension, "ppt") == 0 || strcmp(file_extension, "pptx") == 0) return "application/vnd.ms-powerpoint";
    if (strcmp(file_extension, "zip") == 0) return "application/zip";
    if (strcmp(file_extension, "tar") == 0) return "application/x-tar";
    if (strcmp(file_extension, "gz") == 0) return "application/gzip";

    return "text/plain";
}

void send_file(int client_fd, const char *file_path) {
    const char *last_dot = strrchr(file_path, '.');
    const char *file_extension = (last_dot) ? last_dot + 1 : "binary";
    const char *mime_type = get_mime_type(file_extension);

    char header[BUFFER_SIZE];
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        snprintf(header, sizeof(header), "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>404 Not Found</h1>");    
        send(client_fd, header, strlen(header), 0);
        return;
    }

    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n\r\n", mime_type);
    send(client_fd, header, strlen(header), 0);
    
    char file_buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, file_buffer, sizeof(file_buffer))) > 0) {
        send(client_fd, file_buffer, bytes_read, 0);
        printf("%s", file_buffer);
    }
    close(file_fd);
    return;
}

void send_text(int client_fd, const char *text) {
    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n");
    send(client_fd, header, strlen(header), 0);
    send(client_fd, text, strlen(text), 0);
    return;
}

void build_response(Request *req, PyObject *result) {
    PyObject *type_obj = PyDict_GetItemString(result, "type");
    if (!type_obj) {
        PyErr_Print();
        return;
    }
    const char *type = PyUnicode_AsUTF8(type_obj);
    if (strcmp(type, "file") == 0) {
        PyObject *path_obj = PyDict_GetItemString(result, "path");
        if (!path_obj) {
            PyErr_Print();
            return;
        }
        const char *file_path = PyUnicode_AsUTF8(path_obj);
        send_file(req->client_fd, file_path);
    } else if (strcmp(type, "text") == 0) {
        PyObject *body_obj = PyDict_GetItemString(result, "body");
        if (!body_obj) {
            PyErr_Print();
            return;
        }
        const char *body = PyUnicode_AsUTF8(body_obj);
        send_text(req->client_fd, body);
    }   
}

void parse_body(char *buffer, Request *req) {

    char *content_length_start = strstr(buffer, "Content-Length: ");
    if (!content_length_start) return;
    
    content_length_start += 16;

    int content_length = strtol(content_length_start, NULL, 10);

    req->body_start = strstr(buffer, "\r\n\r\n") + 4;
    req->body_length = content_length;
    return;
}

Request parse_request(char *buffer, int client_fd) {
    Request req;

    req.client_fd = client_fd;

    parse_path(buffer, &req);

    http_method method = parse_method(buffer);
    req.method = method;

    parse_body(buffer, &req);

    return req;
}

static PyObject *
get(PyObject *callable, PyObject *args) {
    
}

void *handle_get(PyObject *router, Request *req) {
    PyGILState_STATE state = PyGILState_Ensure();
    PyObject *result = PyObject_CallMethod(router, "handle_request","s#", req->path_start, req->path_length);
    if (!result) {
        PyErr_Print();
        PyGILState_Release(state);
        return NULL;
    }
    build_response(req, result);
    PyGILState_Release(state);
    
}

void handle_post(PyObject *router, Request *req) {
    
}

void handle_put(PyObject *router, Request *req) {
    
}

void handle_delete(PyObject *router, Request *req) {
    
}

void handle_patch(PyObject *router, Request *req) {
    
}

void *handle_client(void *arg) {
    ClientContext *ctx = (ClientContext *)arg;
    int client_fd = ctx->client_fd;
    PyObject *router = ctx->router;
    free(arg);

    char buffer[BUFFER_SIZE];

    ssize_t bytes_recieved = recv(client_fd, buffer, BUFFER_SIZE, 0);
    if (bytes_recieved > 0) {
        Request req = parse_request(buffer, client_fd);
        switch (req.method) {
            case GET:    
                printf("GET request\n");
                handle_get(router, &req);
                break;
            case POST:
                printf("POST request\n");
                handle_post(router, &req);
                break;
            case PUT:
                handle_put(router, &req);
                break;
            case DELETE:
                handle_delete(router, &req);
                break;
            case PATCH:
                handle_patch(router, &req);
                break;
            default:
                break;
        }
    }
    close(client_fd);
    return NULL;
}

void accept_loop(PyObject *router, int server_fd) {
    while (1) {
        struct sockaddr_in client_address;
        socklen_t client_address_len = sizeof(client_address);
        int *client_fd = malloc(sizeof(int));
        
        if ((*client_fd = accept(server_fd, (struct sockaddr *)&client_address, &client_address_len)) < 0) {
            perror("accept failed");
            continue;
        }
        
        ClientContext *ctx = malloc(sizeof(ClientContext));
        ctx->client_fd = *client_fd;
        ctx->router = router;

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, (void *)ctx);
        pthread_detach(thread_id);
    }
}

int create_server() {
    struct sockaddr_in address;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return -1;
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        return -1;
    }
    return server_fd;
}

int main() {
    Py_Initialize();

    PyRun_SimpleString("import sys; sys.path.insert(0, '.')");

    PyObject *module_name = PyUnicode_FromString("main");
    PyObject *module = PyImport_Import(module_name);
    Py_DECREF(module_name);

    if (!module) {
        PyErr_Print();
        fprintf(stderr, "Failed to import Python module 'main'\n");
        Py_Finalize();
        return 1;
    }

    PyObject *router = PyObject_GetAttrString(module, "router");
    if (!router) {
        PyErr_Print();
        fprintf(stderr, "Failed to get 'router' from module\n");
        Py_DECREF(module);
        Py_Finalize();
        return 1;
    }

    int server_fd = create_server();
    if (server_fd == -1) return 1;

    PyEval_SaveThread();

    accept_loop(router, server_fd);
    Py_DECREF(router);
    Py_DECREF(module);
    Py_Finalize();

    
    return 0;
}

