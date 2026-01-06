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



void *handle_client(void *newSocket){
    int client_socketID = *(int*)newSocket;
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
    // tempReq[strlen(buffer)] = '\0';
    struct cache_element* temp = find(tempReq);
    if(temp!=NULL){
        int size = temp->len/sizeof(char);
        int pos = 0;
        char response[MAX_BYTES];
        while(pos<size){
            bzero(response,MAX_BYTES);
            for(int i=0;i<MAX_BYTES;i++){
                response[i] = temp->data[i];
                pos++;
            }
            send(socket,response,MAX_BYTES,0);
        }
        printf("Data retrieved from the cache\n");
        printf("%s\n\n",response);
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
        i++;
    }
    close(proxy_socketID);
    return 0;
}