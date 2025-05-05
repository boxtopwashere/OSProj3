 #include "io_helper.h"
 #include "request.h"
 
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <fcntl.h>
 #include <unistd.h>
 #include <sys/mman.h> 
 #include <sys/stat.h>
 #include <pthread.h>
 #include <semaphore.h>
 #include <time.h>
 #include <errno.h>
 
 #define MAXBUF 8192
 
 #define AGING_THRESHOLD 10
 
 // below default values are defined in 'request.h'
 int num_threads = DEFAULT_THREADS;
 int buffer_max_size = DEFAULT_BUFFER_SIZE;
 int scheduling_algo = DEFAULT_SCHED_ALGO;  

 
 // ------------------------- Global shared buffer for HTTP requests -------------------------
 
 // holds the connection file descriptor and metadata needed for scheduling
 typedef struct request_node {
     int fd;
     char method[MAXBUF];
     char uri[MAXBUF];
     char version[MAXBUF];
     char filename[MAXBUF];
     char cgiargs[MAXBUF];
     size_t file_size;      // SFF policy
     time_t arrival;        // aging
     struct request_node *next;
 } request_node;
 
 // The global linked list for buffer
 request_node *req_head = NULL;
 request_node *req_tail = NULL;
 int req_count = 0;  // curr num of enqueued requests
 
 // ------------------------- Synchronization primitives -------------------------
 pthread_mutex_t req_mutex = PTHREAD_MUTEX_INITIALIZER;
 pthread_cond_t req_not_full = PTHREAD_COND_INITIALIZER;  // buff has space
 sem_t req_sem;  // counts pending requests num
 
 // ------------------------- Helper: Pick a request from the queue --------------------------
 //
 request_node *pick_request() {
     request_node *selected = NULL;
     request_node *selected_prev = NULL;
     
     // FIFO
     if (scheduling_algo == 0) { 
         selected = req_head;
         req_head = (req_head ? req_head->next : NULL);
         if (req_head == NULL)
             req_tail = NULL;
     }
     // SFF
     else if (scheduling_algo == 1) {
         time_t now = time(NULL);
         request_node *cur = req_head;
         request_node *prev = NULL;
         selected = req_head;
         selected_prev = NULL;
         while (cur != NULL) {
             // If the request has waited longer than the aging threshold/if its file size is smaller than the current candidate... CHOOSE IT -- Mitigate starvation
             if ( (now - cur->arrival >= AGING_THRESHOLD) ||
                  (cur->file_size < selected->file_size) ) {
                 selected = cur;
                 selected_prev = prev;
             }
             prev = cur;
             cur = cur->next;
         }
         // Remove selected node from the list
         if (selected_prev == NULL) {
             req_head = selected->next;
         } else {
             selected_prev->next = selected->next;
         }
         if (selected == req_tail)
             req_tail = selected_prev;
     }
     // RANDOM
     else if (scheduling_algo == 2) {
         int count = 0;
         request_node *cur = req_head;
         while (cur) {
             count++;
             cur = cur->next;
         }
         int r = rand() % count;
         cur = req_head;
         request_node *prev = NULL;
         for (int i = 0; i < r; i++) {
             prev = cur;
             cur = cur->next;
         }
         selected = cur;
         if (prev == NULL)
             req_head = selected->next;
         else
             prev->next = selected->next;
         if (selected == req_tail)
             req_tail = prev;
     }
     
     return selected;
 }
 
 // ------------------------- OG -------------------------
 
 void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
     char buf[MAXBUF], body[MAXBUF];
     
     // Create the body of the error message
     sprintf(body,
         "<!doctype html>\r\n"
         "<head>\r\n"
         "  <title>CYB-3053 WebServer Error</title>\r\n"
         "</head>\r\n"
         "<body>\r\n"
         "  <h2>%s: %s</h2>\r\n" 
         "  <p>%s: %s</p>\r\n"
         "</body>\r\n"
         "</html>\r\n", errnum, shortmsg, longmsg, cause);
         
     // Write out response headers
     sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
     write_or_die(fd, buf, strlen(buf));
     
     sprintf(buf, "Content-Type: text/html\r\n");
     write_or_die(fd, buf, strlen(buf));
     
     sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
     write_or_die(fd, buf, strlen(buf));
     
     // Write out the body
     write_or_die(fd, body, strlen(body));
     
     // Close the connection
     close_or_die(fd);
 }
 
 void request_read_headers(int fd) {
     char buf[MAXBUF];
     readline_or_die(fd, buf, MAXBUF);
     while (strcmp(buf, "\r\n")) {
         readline_or_die(fd, buf, MAXBUF);
     }
     return;
 }
 
 int request_parse_uri(char *uri, char *filename, char *cgiargs) {
     char *ptr;
     if (!strstr(uri, "cgi")) { 
         // static content
         strcpy(cgiargs, "");
         sprintf(filename, ".%s", uri);
         if (uri[strlen(uri)-1] == '/')
             strcat(filename, "index.html");
         return 1;
     } else { 
         // dynamic content (not handled)
         ptr = index(uri, '?');
         if (ptr) {
             strcpy(cgiargs, ptr+1);
             *ptr = '\0';
         } else {
             strcpy(cgiargs, "");
         }
         sprintf(filename, ".%s", uri);
         return 0;
     }
 }
 
 void request_get_filetype(char *filename, char *filetype) {
     if (strstr(filename, ".html"))
         strcpy(filetype, "text/html");
     else if (strstr(filename, ".gif"))
         strcpy(filetype, "image/gif");
     else if (strstr(filename, ".jpg"))
         strcpy(filetype, "image/jpeg");
     else 
         strcpy(filetype, "text/plain");
 }
 
 void request_serve_static(int fd, char *filename, int filesize) {
     int srcfd;
     char *srcp, filetype[MAXBUF], buf[MAXBUF];
     
     request_get_filetype(filename, filetype);
     srcfd = open_or_die(filename, O_RDONLY, 0);
     
     // Memory-map the file
     srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
     close_or_die(srcfd);
     
     // Form the HTTP response header
     snprintf(buf, MAXBUF,
        "HTTP/1.0 200 OK\r\n"
        "Server: OSTEP WebServer\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: %.128s\r\n\r\n",
        filesize, filetype);
     write_or_die(fd, buf, strlen(buf));
     
     // Write out the file contents
     write_or_die(fd, srcp, filesize);
     munmap_or_die(srcp, filesize);
 }
 
 // ------------------------- Multithreading: Worker Thread -------------------------
 
 void* thread_request_serve_static(void* arg) {
     (void)arg;  
     
     while (1) {
         // Block until at least one req in the queue
         sem_wait(&req_sem);
         
         // Lock the buffer to dequeue req.
         pthread_mutex_lock(&req_mutex);
         request_node *req = pick_request();
         req_count--;  
         pthread_cond_signal(&req_not_full);
         pthread_mutex_unlock(&req_mutex);
         
         if (req) {
             // process the HTTP request and parse URI
             request_serve_static(req->fd, req->filename, (int)req->file_size);
             close_or_die(req->fd);
             free(req);
         }
     }
     return NULL;
 }
 
 // ------------------------- Initialization for the Request System -------------------------
 
 // Initializes synchronization primitives and spawns a pool of worker threads
 void init_request_system() {
     if (sem_init(&req_sem, 0, 0) != 0) {
         perror("sem_init");
         exit(1);
     }
     
     srand(time(NULL));
     
     pthread_t tid;
     for (int i = 0; i < num_threads; i++) {
         if (pthread_create(&tid, NULL, thread_request_serve_static, NULL) != 0) {
             perror("pthread_create");
             exit(1);
         }
         pthread_detach(tid);
     }
 }
 
 // ------------------------- Entry Point for Incoming Requests -------------------------
 
 void request_handle(int fd) {
     int is_static;
     struct stat sbuf;
     char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
     char filename[MAXBUF], cgiargs[MAXBUF];
    
     // Read the first line of the HTTP request
     readline_or_die(fd, buf, MAXBUF);
     sscanf(buf, "%s %s %s", method, uri, version);
     printf("method:%s uri:%s version:%s\n", method, uri, version);
     
     // Only the GET method is implemented
     if (strcasecmp(method, "GET")) {
         request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
         return;
     }
     request_read_headers(fd);
     
     is_static = request_parse_uri(uri, filename, cgiargs);
     
     // Check if requested file exists
     if (stat(filename, &sbuf) < 0) {
         request_error(fd, filename, "404", "Not found", "server could not find this file");
         return;
     }
     
     if (is_static) {
         // Only serve static files that are regular files and readable
         if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
             request_error(fd, filename, "403", "Forbidden", "server could not read this file");
             return;
         }
         
         // Reject any URI that contains ".." to avoid serving files outside the intended directory
         if (strstr(uri, "..") != NULL) {
             request_error(fd, uri, "403", "Forbidden", "directory traversal attempt detected");
             return;
         }
         
         // Prepare a new request node with all the data needed
         request_node *req_new = malloc(sizeof(request_node));
         if (!req_new) {
             perror("malloc");
             close(fd);
             return;
         }
         req_new->fd = fd;
         strncpy(req_new->method, method, MAXBUF);
         strncpy(req_new->uri, uri, MAXBUF);
         strncpy(req_new->version, version, MAXBUF);
         strncpy(req_new->filename, filename, MAXBUF);
         strncpy(req_new->cgiargs, cgiargs, MAXBUF);
         req_new->file_size = sbuf.st_size;
         req_new->arrival = time(NULL);
         req_new->next = NULL;
         
         // Enqueue the request into the global buffer
         pthread_mutex_lock(&req_mutex);
         while (req_count >= buffer_max_size)
             pthread_cond_wait(&req_not_full, &req_mutex);
         
         if (req_head == NULL) {
             req_head = req_new;
             req_tail = req_new;
         } else {
             req_tail->next = req_new;
             req_tail = req_new;
         }
         req_count++;
         pthread_mutex_unlock(&req_mutex);
         // Signal new request is available
         sem_post(&req_sem);
     }
     else {
         request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
     }
 }