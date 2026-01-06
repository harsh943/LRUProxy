#include<stdio.h>
#include "proxy_parse.h"
#include<stdlib.h>
#include<string.h>
#include<pthread.h>
#include<semaphore.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netdb.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<fcntl.h>
#include<sys/wait.h>
#include<errno.h>
#include<time.h>

#define MAX_BYTES 4096
#define MAX_CLIENTS 10
#define MAX_ELEMENT_SIZE 10*(1<<10)
#define MAX_SIZE 200*(1<<20)
typedef struct cache_element cache_element;
struct cache_element{
    char *data;
    int len;
    char *url;
    time_t lru_time;
    cache_element *next;
};
cache_element *find(char *url);
int add_cache_element(char *url, char *data, int len);
void remove_cache_element();

int port = 8080;
int proxy_socketID;
pthread_t tid[MAX_CLIENTS];
sem_t semaphore;
pthread_mutex_t lock;

cache_element *head;
int cache_size;

// Check if HTTP version is valid (HTTP/1.0 or HTTP/1.1)
int checkHTTPVersion(char *version) {
    if (version == NULL) {
        return -1;
    }
    if (strcmp(version, "HTTP/1.0") == 0 || strcmp(version, "HTTP/1.1") == 0) {
        return 1;
    }
    return -1;
}
void sendErrorMessage(int socket, int status_code) {
    char str[1024];
    char currentTime[50];
    time_t now = time(0);
    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    const char *status_text;
    const char *body_message;

    switch (status_code) {
        case 400:
            status_text = "Bad Request";
            body_message = "The server could not understand the request due to invalid syntax.";
            break;
        case 403:
            status_text = "Forbidden";
            body_message = "You don't have permission to access this resource.";
            break;
        case 404:
            status_text = "Not Found";
            body_message = "The requested URL was not found on this server.";
            break;
        case 500:
            status_text = "Internal Server Error";
            body_message = "The server encountered an internal error and was unable to complete your request.";
            break;
        case 501:
            status_text = "Not Implemented";
            body_message = "The server does not support the functionality required to fulfill the request.";
            break;
        case 502:
            status_text = "Bad Gateway";
            body_message = "The server received an invalid response from the upstream server.";
            break;
        case 503:
            status_text = "Service Unavailable";
            body_message = "The server is currently unavailable. Please try again later.";
            break;
        case 505:
            status_text = "HTTP Version Not Supported";
            body_message = "The server does not support the HTTP protocol version used in the request.";
            break;
        default:
            status_text = "Internal Server Error";
            body_message = "An unexpected error occurred.";
            status_code = 500;
            break;
    }

    snprintf(str, sizeof(str),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>%d %s</title></head>\n"
        "<body>\n"
        "<h1>%d %s</h1>\n"
        "<p>%s</p>\n"
        "<hr>\n"
        "<p><em>Proxy Server</em></p>\n"
        "</body>\n"
        "</html>\n",
        status_code, status_text,
        currentTime,
        status_code, status_text,
        status_code, status_text,
        body_message
    );

    send(socket, str, strlen(str), 0);
}

int connectRemoteServer(char *host_addr, int port){
    struct addrinfo hints, *res, *p;
    int remoteSocketID = -1;
    char port_str[10];
    
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      // Use IPv4
    hints.ai_socktype = SOCK_STREAM;
    
    int status = getaddrinfo(host_addr, port_str, &hints, &res);
    if(status != 0){
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(status));
        return -1;
    }
    
    // Try each address until we successfully connect
    for(p = res; p != NULL; p = p->ai_next){
        remoteSocketID = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if(remoteSocketID < 0){
            continue;
        }
        
        if(connect(remoteSocketID, p->ai_addr, p->ai_addrlen) < 0){
            close(remoteSocketID);
            remoteSocketID = -1;
            continue;
        }
        
        break; // Successfully connected
    }
    
    freeaddrinfo(res);
    
    if(remoteSocketID < 0){
        perror("Error in connecting to remote server");
        return -1;
    }
    
    return remoteSocketID;
}
int handle_request(int clientSocketID, struct ParsedRequest *request, char *tempReq){
    char *buffer = (char *)malloc(MAX_BYTES*sizeof(char));
    strcpy(buffer,"GET "); 
    strcat(buffer,request->path);
    strcat(buffer," ");
    strcat(buffer,request->version);
    strcat(buffer,"\r\n");
    
    size_t len = strlen(buffer);
    if(ParsedHeader_set(request,"Connection","close")<0){
        perror("ParsedHeader_set failed\n");
        exit(1);
    }
    if(ParsedHeader_get(request,"Host")==NULL){
        if(ParsedHeader_set(request,"Host",request->host)<0){
            perror("ParsedHeader_set failed\n");
            exit(1);
        }
    }
    if(ParsedRequest_unparse_headers(request,buffer+len,(size_t)(MAX_BYTES-len))<0){
        perror("ParsedRequest_unparse_headers failed\n");
        // exit(1);
    }
    int server_port = 80;
    if(request->port!=NULL){
        server_port = atoi(request->port);
    }
    int remoteSocketID = connectRemoteServer(request->host,server_port);
    if(remoteSocketID<0){
        perror("Error in connecting to remote server\n");
        exit(1);
    }
    int byte_send = send(remoteSocketID,buffer,strlen(buffer),0);
    bzero(buffer,MAX_BYTES);

    byte_send = recv(remoteSocketID,buffer,MAX_BYTES-1,0);

    char *temp_buffer = (char*)malloc(MAX_BYTES*sizeof(char));
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;
    while(byte_send>0){
        byte_send = send(clientSocketID,buffer,byte_send,0);
        for(int i=0;i<byte_send/sizeof(char);i++){
            temp_buffer[temp_buffer_index] = buffer[i];
            temp_buffer_index++;
        }
        temp_buffer_size+=MAX_BYTES;
        temp_buffer = (char*)realloc(temp_buffer,temp_buffer_size);
        if(byte_send<0){
            perror("Error in sending data to client\n");
            break;
        }
        bzero(buffer,MAX_BYTES);
        byte_send = recv(remoteSocketID,buffer,MAX_BYTES-1,0);
    }   
    temp_buffer[temp_buffer_index] = '\0';
    free(buffer);
    add_cache_element(tempReq,temp_buffer,strlen(temp_buffer));
    free(temp_buffer);
    close(remoteSocketID);
    return 0;
}

void *handle_client(void *newSocket){
    sem_wait(&semaphore);
    int p;
    sem_getvalue(&semaphore, &p);
    printf("semaphore value is %d\n",p);
    int *t = (int *) newSocket;
    int socket = *t;
    int byte_send_client,len;

    char *buffer = (char *)calloc(MAX_BYTES,sizeof(char));
    bzero(buffer,MAX_BYTES);

    byte_send_client = recv(socket,buffer,MAX_BYTES,0);
    if(byte_send_client<0){
        perror("Receive failed\n");
        exit(1);
    }
    while(byte_send_client>0){
        len = strlen(buffer);
        if(strstr(buffer,"\r\n\r\n")==NULL){
            byte_send_client = recv(socket,buffer+len,MAX_BYTES-len,0);
        }
        else{
            break;
        }
    }
    char *tempReq = (char *)malloc(strlen(buffer)*sizeof(char)+1);
    for(int i = 0;i<strlen(buffer);i++){
        tempReq[i] = buffer[i];
    }
    tempReq[strlen(buffer)] = '\0';
    struct cache_element* temp = find(tempReq);
    if(temp!=NULL){
        printf("URL found in cache: %s\n", temp->url);
        int size = temp->len;
        int pos = 0;
        char response[MAX_BYTES];
        while(pos < size){
            bzero(response, MAX_BYTES);
            int bytes_to_send = (size - pos < MAX_BYTES) ? (size - pos) : MAX_BYTES;
            for(int i = 0; i < bytes_to_send; i++){
                response[i] = temp->data[pos + i];
            }
            send(socket, response, bytes_to_send, 0);
            pos += bytes_to_send;
        }
        printf("Data retrieved from cache\n");
    }
    // if not found in cache
    else if(byte_send_client>0){
        len = strlen(buffer);
        struct ParsedRequest *request = ParsedRequest_create();
        if(ParsedRequest_parse(request,buffer,len)<0){
            perror("Parse failed\n");
            exit(1);
        }else{
            bzero(buffer,MAX_BYTES);
            if(!strcmp(request->method,"GET")){
                if(request->host && request->path && checkHTTPVersion(request->version)==1){
                    byte_send_client = handle_request(socket,request,tempReq);
                    if(byte_send_client==-1){
                        sendErrorMessage(socket,500);
                    }
                }
                else{
                    sendErrorMessage(socket,500);
                }
            }else{
                printf("this code doesnt support any method apart from get\n");
            }
        }
        ParsedRequest_destroy(request);
    }else if(byte_send_client==0){
        printf("Client disconnected\n");
    }
    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore,&p);
    printf("semaphore post value is %d\n",p);
    free(tempReq);
    return NULL;
}
int main(int argc, char *argv[]){
    int client_socketID,client_len;
    struct sockaddr_in client_addr,server_addr;
    sem_init(&semaphore,0,MAX_CLIENTS);
    pthread_mutex_init(&lock,NULL);
    if(argc==2){
        port = atoi(argv[1]);
    }else{
        printf("Too few arguments\n");
        exit(1);
    }
    printf("Starting proxy server at port:%d\n",port);
    proxy_socketID = socket(AF_INET,SOCK_STREAM,0);
    if(proxy_socketID<0){
        perror("Socket creation failed\n");
        exit(1);
    }
    // resuing the socket again
    int reuse = 1;
    if(setsockopt(proxy_socketID,SOL_SOCKET,SO_REUSEADDR,(const char *)&reuse,sizeof(reuse))<0){
        perror("setsockopt failed\n");
        exit(1);
    }
    bzero((char*)&server_addr,sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    // to accept recieve connections we use bind
    if(bind(proxy_socketID,(struct sockaddr *)&server_addr,sizeof(server_addr))<0){
        perror("Port not available\n");
        exit(1);
    }
    printf("Binding at port:%d\n",port);
    int listen_status = listen(proxy_socketID,MAX_CLIENTS);
    if(listen_status<0){
        perror("Listen failed\n");
        exit(1);
    }
    printf("Listening at port:%d\n",port);
    int i = 0;
    int connected_socketID[MAX_CLIENTS];
    while(1){
        bzero((char*)&client_addr,sizeof(client_addr));
        client_len = sizeof(client_addr);
        // this accepts the client connection
        client_socketID = accept(proxy_socketID,(struct sockaddr *)&client_addr,(socklen_t*)&client_len);
        if(client_socketID<0){
            perror("Accept failed\n");
            exit(1);
        }
        else{
            connected_socketID[i] = client_socketID;
        }
        // printf("Connected at port:%d\n",port);
        struct sockaddr_in *client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&ip,str,INET_ADDRSTRLEN);
        printf("Client connected with port %d and ip address %s\n",ntohs(client_pt->sin_port),str);
        pthread_create(&tid[i],NULL,handle_client,(void*)&connected_socketID[i]);
        i = (i+1)%MAX_CLIENTS;
    }
    close(proxy_socketID);
    return 0;
}

cache_element *find(char *url){
    cache_element *site = NULL;
    int temp_lock = pthread_mutex_lock(&lock);
    printf("FIND cache lock acquired %d\n",temp_lock);
    if(head!=NULL){
        site = head;
        while(site!=NULL){
            if(!strcmp(site->url,url)){
                printf("LRU time track before %ld\n",site->lru_time);
                site->lru_time = time(NULL);
                printf("LRU time track after %ld\n",site->lru_time);
                break;
            }
            site = site->next;
        }
    }else{
        printf("URL not found in cache\n");
    }
    temp_lock = pthread_mutex_unlock(&lock);
    printf("FIND cache lock released %d\n",temp_lock);
    return site;
}   

// Internal version - caller must already hold the lock
static void remove_cache_element_unlocked(){
    cache_element *p;
    cache_element *q;
    cache_element *temp = head;
    if(head!=NULL){
        for(p=head,q=head;q->next!=NULL;q = q->next){
            if((q->next)->lru_time<(temp->lru_time)){
                temp = q->next;
                p = q;
            }
        }
        if(temp==head){
            head = head->next;
        }else{
            p->next = temp->next;
        }
        cache_size = cache_size -(temp->len)-sizeof(cache_element)-strlen(temp->url)-1;
        free(temp->data);
        free(temp->url);
        free(temp);
    }
}

// Public version - acquires lock
void remove_cache_element(){
    int temp_lock = pthread_mutex_lock(&lock);
    printf("Remove cache lock acquired %d\n",temp_lock);
    remove_cache_element_unlocked();
    temp_lock = pthread_mutex_unlock(&lock);
    printf("Remove cache lock released %d\n",temp_lock);
}

int add_cache_element(char *url,char *data,int len){
    int temp_lock = pthread_mutex_lock(&lock);
    printf("ADD cache lock acquired %d\n",temp_lock);
    int element_size = len+1+strlen(url)+sizeof(cache_element);
    if(element_size>MAX_ELEMENT_SIZE){
        temp_lock = pthread_mutex_unlock(&lock);
        printf("ADD cache lock released %d\n",temp_lock);
        return 0;
    }else{
        while(cache_size+element_size>MAX_SIZE){
            remove_cache_element_unlocked();  // Use unlocked version - we already hold the lock
        }
        cache_element *new_element = (cache_element *)malloc(sizeof(cache_element));
        new_element->data = (char *)malloc(len+1);
        strcpy(new_element->data,data);
        new_element->url = (char *)malloc(sizeof(char)*strlen(url)+1);
        strcpy(new_element->url,url);
        new_element->lru_time = time(NULL);
        new_element->next = head;
        new_element->len = len;
        head = new_element;
        cache_size+=element_size;
        temp_lock = pthread_mutex_unlock(&lock);
        printf("ADD cache lock released %d\n",temp_lock);
        return 1;
    }
    return 0;
    
}