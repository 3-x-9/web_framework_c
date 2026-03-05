#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <Python.h>
#include <signal.h>

#define DEFAULT_PORT  8000
#define BUFFER_SIZE   4096

typedef enum { GET, POST, PUT, DELETE, PATCH, UNKNOWN } http_method;

typedef struct {
    http_method method;
    char       *path_start;
    size_t      path_length;
    char       *body_start;
    size_t      body_length;
    int         client_fd;
} Request;

typedef struct {
    int        client_fd;
    PyObject  *routes;
} ClientContext;


http_method parse_method(char *buf) {
    if (strncmp(buf, "GET",    3) == 0) return GET;
    if (strncmp(buf, "POST",   4) == 0) return POST;
    if (strncmp(buf, "PUT",    3) == 0) return PUT;
    if (strncmp(buf, "DELETE", 6) == 0) return DELETE;
    if (strncmp(buf, "PATCH",  5) == 0) return PATCH;
    return UNKNOWN;
}

void parse_path(char *buf, Request *req) {
    char *s1 = strchr(buf, ' ');
    if (!s1) return;
    char *path = s1 + 1;
    char *s2 = strchr(path, ' ');
    if (!s2) return;
    req->path_start  = path;
    req->path_length = s2 - path;
}

void parse_body(char *buf, Request *req) {
    char *cl = strstr(buf, "Content-Length: ");
    if (!cl) return;
    req->body_length = strtol(cl + 16, NULL, 10);
    char *sep = strstr(buf, "\r\n\r\n");
    req->body_start  = sep ? sep + 4 : NULL;
}

Request parse_request(char *buf, int fd) {
    Request req = {0};
    req.client_fd = fd;
    parse_path(buf, &req);
    req.method = parse_method(buf);
    parse_body(buf, &req);
    return req;
}


const char *get_mime_type(const char *ext) {
    if (strcmp(ext, "html") == 0) return "text/html";
    if (strcmp(ext, "css")  == 0) return "text/css";
    if (strcmp(ext, "js")   == 0) return "application/javascript";
    if (strcmp(ext, "png")  == 0) return "image/png";
    if (strcmp(ext, "jpg")  == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "json") == 0) return "application/json";
    return "text/plain";
}

void send_file(int fd, const char *path) {
    const char *dot  = strrchr(path, '.');
    const char *mime = get_mime_type(dot ? dot + 1 : "bin");
    char header[BUFFER_SIZE];
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        snprintf(header, sizeof(header),
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>404 Not Found</h1>");
        send(fd, header, strlen(header), 0);
        return;
    }
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n\r\n", mime);
    send(fd, header, strlen(header), 0);
    char buf[BUFFER_SIZE];
    ssize_t received;
    while ((received = read(file_fd, buf, sizeof(buf))) > 0)
        send(fd, buf, received, 0);
    close(file_fd);
}

void send_text(int fd, const char *text) {
    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n");
    send(fd, header, strlen(header), 0);
    send(fd, text, strlen(text), 0);
}

void build_response(Request *req, PyObject *result) {
    PyObject *type_obj = PyDict_GetItemString(result, "type");
    if (!type_obj) { PyErr_Print(); return; }
    const char *type = PyUnicode_AsUTF8(type_obj);

    if (strcmp(type, "file") == 0) {
        PyObject *path_obj = PyDict_GetItemString(result, "path");
        if (!path_obj) {PyErr_Print(); return; }
        const char *file_path = PyUnicode_AsUTF8(path_obj);
        send_file(req->client_fd, file_path);
    } else if (strcmp(type, "text") == 0) {
        PyObject *body_obj = PyDict_GetItemString(result, "body");
        if (!body_obj) {PyErr_Print(); return;}
        const char *body = PyUnicode_AsUTF8(body_obj);
        send_text(req->client_fd, body);
    }
}

void dispatch_request(PyObject *routes, Request *req, const char *method_str) {
    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject *key     = PyUnicode_FromFormat("%s:%.*s",
                            method_str, (int)req->path_length, req->path_start);
    PyObject *handler = PyDict_GetItem(routes, key);
    Py_DECREF(key);

    if (!handler) {
        send_text(req->client_fd, "<h1>404 Not Found</h1>");
        PyGILState_Release(gstate);
        return;
    }
    PyObject *result = PyObject_CallNoArgs(handler);

    if (!result) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return;
    }

    build_response(req, result);
    Py_DECREF(result);
    PyGILState_Release(gstate);
}

void *handle_client(void *arg) {
    ClientContext *ctx = (ClientContext *)arg;
    int       client_fd = ctx->client_fd;
    PyObject *routes    = ctx->routes;
    free(arg);

    char    buf[BUFFER_SIZE];
    ssize_t received = recv(client_fd, buf, BUFFER_SIZE - 1, 0);
    if (received > 0) {
        buf[received] = '\0';
        Request req = parse_request(buf, client_fd);
        switch (req.method) {
            case GET:    dispatch_request(routes, &req, "GET");    break;
            case POST:   dispatch_request(routes, &req, "POST");   break;
            case PUT:    dispatch_request(routes, &req, "PUT");    break;
            case DELETE: dispatch_request(routes, &req, "DELETE"); break;
            case PATCH:  dispatch_request(routes, &req, "PATCH");  break;
            default: break;
        }
    }
    close(client_fd);
    return NULL;
}

void accept_loop(PyObject *routes, int server_fd) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_addr_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        ClientContext *ctx = malloc(sizeof(ClientContext));
        ctx->client_fd = client_fd;
        ctx->routes    = routes;

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, ctx);
        pthread_detach(thread_id);
    }
}

int create_server(int port) {
    struct sockaddr_in addr = {0};
    int server_fd  = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed"); return -1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen failed"); return -1;
    }
    return server_fd;
}

typedef struct {
    PyObject_HEAD
    PyObject *routes;    // structure is: "METHOD:/path"
    int       server_fd;
} PyServerObject;


typedef struct {
    PyObject_HEAD
    PyServerObject *server;
    //PyObject       *params;
    PyObject       *key;      
} RouteDecoratorObject;

static PyObject *
route_decorator_call(RouteDecoratorObject *self, PyObject *args, PyObject *kwargs) {
    PyObject *func;
    if (!PyArg_ParseTuple(args, "O:__call__", &func)) return NULL;
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "Route handler must be callable");
        return NULL;
    }
    PyDict_SetItem(self->server->routes, self->key, func);
    Py_INCREF(func);
    return func; 
}

static void
route_decorator_dealloc(RouteDecoratorObject *self) {
    Py_XDECREF(self->key);
    //Py_XDECREF(self->params);
    Py_XDECREF(self->server);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject RouteDecoratorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "myframework.RouteDecorator",
    .tp_basicsize = sizeof(RouteDecoratorObject),
    .tp_call      = (ternaryfunc)route_decorator_call,
    .tp_dealloc   = (destructor)route_decorator_dealloc,
    .tp_new       = PyType_GenericNew,
};


static int
server_init(PyServerObject *self, PyObject *args, PyObject *kwargs) {
    int port = DEFAULT_PORT;
    static char *kwlist[] = {"port", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i", kwlist, &port))
        return -1;

    self->routes = PyDict_New();
    if (!self->routes) return -1;

    self->server_fd = create_server(port);
    if (self->server_fd < 0) {
        PyErr_SetString(PyExc_OSError, "Failed to bind server socket");
        return -1;
    }
    return 0;
}

static void
server_dealloc(PyServerObject *self) {
    Py_XDECREF(self->routes);
    if (self->server_fd >= 0) close(self->server_fd);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
make_route_decorator(PyServerObject *self, const char *method, PyObject *args) {
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path)) return NULL;

    RouteDecoratorObject *dec = PyObject_New(RouteDecoratorObject, &RouteDecoratorType);
    if (!dec) return NULL;

    


    dec->key    = PyUnicode_FromFormat("%s:%s", method, path);
    //dec->params = PyDict_New();
    dec->server = self;
    Py_INCREF(self); 
    return (PyObject *)dec;
}

static PyObject *server_get   (PyServerObject *self, PyObject *args) { return make_route_decorator(self, "GET",    args); }
static PyObject *server_post  (PyServerObject *self, PyObject *args) { return make_route_decorator(self, "POST",   args); }
static PyObject *server_put   (PyServerObject *self, PyObject *args) { return make_route_decorator(self, "PUT",    args); }
static PyObject *server_delete(PyServerObject *self, PyObject *args) { return make_route_decorator(self, "DELETE", args); }
static PyObject *server_patch (PyServerObject *self, PyObject *args) { return make_route_decorator(self, "PATCH",  args); }

static PyObject *
server_run(PyServerObject *self, PyObject *args) {
    printf("Server listening on port %d\n", DEFAULT_PORT);
    fflush(stdout);

    Py_BEGIN_ALLOW_THREADS
    accept_loop(self->routes, self->server_fd);
    Py_END_ALLOW_THREADS

    Py_RETURN_NONE;
}

static PyMethodDef PyServerMethods[] = {
    {"get",    (PyCFunction)server_get,    METH_VARARGS, "Register a GET route"},
    {"post",   (PyCFunction)server_post,   METH_VARARGS, "Register a POST route"},
    {"put",    (PyCFunction)server_put,    METH_VARARGS, "Register a PUT route"},
    {"delete", (PyCFunction)server_delete, METH_VARARGS, "Register a DELETE route"},
    {"patch",  (PyCFunction)server_patch,  METH_VARARGS, "Register a PATCH route"},
    {"run",    (PyCFunction)server_run,    METH_NOARGS,  "Start the server"},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject PyServerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "myframework.Server",
    .tp_basicsize = sizeof(PyServerObject),
    .tp_init      = (initproc)server_init,
    .tp_dealloc   = (destructor)server_dealloc,
    .tp_methods   = PyServerMethods,
    .tp_new       = PyType_GenericNew,
};


PyMODINIT_FUNC PyInit_myframework(void) {
    if (PyType_Ready(&RouteDecoratorType) < 0) return NULL;
    if (PyType_Ready(&PyServerType)       < 0) return NULL;

    static PyModuleDef module_def = {
        PyModuleDef_HEAD_INIT,
        "myframework",
        "A lightweight C-backed HTTP framework",
        -1,
        NULL
    };

    PyObject *m = PyModule_Create(&module_def);
    if (!m) return NULL;

    Py_INCREF(&PyServerType);
    if (PyModule_AddObject(m, "Server", (PyObject *)&PyServerType) < 0) {
        Py_DECREF(&PyServerType);
        Py_DECREF(m);
        return NULL;
    }

    if (signal(SIGINT, SIG_DFL) == SIG_ERR) {
        perror("signal SIGINT");
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
