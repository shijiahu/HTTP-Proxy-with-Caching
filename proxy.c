#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>

#define BUFSIZE 8192
#define BILLION  1000000L
#define CACHE_SIZE 30
#define MAXNAMELEN 256


//cache
char * content[CACHE_SIZE];
char * chostname[CACHE_SIZE];
char * ctarget[CACHE_SIZE];
size_t clength[CACHE_SIZE];
int frequency[CACHE_SIZE];
int use[CACHE_SIZE];
long current;
long Limit;	

struct client_state
{
    int fd;  
    char ipBuffer[INET_ADDRSTRLEN];          
    pthread_t thread; 
};



int find_index(size_t l)
{
    int i = 0;
    int min = 0;
    int min_i = -1;

    for(i = 0; i < CACHE_SIZE; i++)
    {
        if(use[i]==1) 
            {   min_i =i;
                min = frequency[i];
                break;
            }
    }

    if(min_i < 0) return 0;

    while(current + l > Limit)
    {
        if(l > Limit) break;
        if(current > Limit) break;
        //we need to remove some one now.
        for(i = 0; i < CACHE_SIZE; i++)
        {
            if(use[i] == 1 && frequency[i] < min)
            {
                min_i = i;
                min = frequency[i];
            }
        }

        if(use[min_i]){
        current -= clength[min_i];
        free(content[min_i]);
        free(chostname[min_i]);
        free(ctarget[min_i]);
        clength[min_i] = 0;
        frequency[min_i] = 0;
        use[min_i] = 0; 
        }
    }

    for(i = 0; i < CACHE_SIZE; i++)
    {
        if(use[i]==0) return i;
    }

    return 0;

}

void extract_match(char * dest, const char * src, regmatch_t match)
{
    regoff_t o;
    for (o=0; o < match.rm_eo - match.rm_so; o++)
    {
        dest[o] = src[o + match.rm_so];
    }
    dest[o] = 0;
}

void * make_request(char * hostname, char * request, char * resourceName, size_t * length)
{
    //  resolve the hostname to an IP
    struct addrinfo hints;  //  ai_family, ai_socktype, ai_protocol
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    struct addrinfo *result = 0;

	getaddrinfo(hostname, "80", &hints, &result);
    
    //  we assume first result is a TCP/IPv4 address
    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    
    //  connect to server
	connect(sock, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    //  write the request
	write(sock, request, strlen(request)) != strlen(request);
    
    char buffer[BUFSIZE];  //  storage for read() operations
    void * data = 0;    //  final data downloaded
    size_t count = 0;

    //  we use a polling time of 500ms for any read events
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.events = POLLIN | POLLRDNORM | POLLRDBAND;
    pfd.fd = sock;
    
    while (poll(&pfd, 1, 500) > 0)
    {
        ssize_t L = read(sock, buffer, sizeof(buffer));
        if (L > 0)
        {
            if (!data)
            {
                data = malloc((unsigned long)L);
            }
            else
            {
                data = realloc(data, count + L);
            }
            
            memcpy(data + count, buffer, L);
            count += (size_t)L;
        } else
            break;
    }
    
    //  cleanup socket
    close(sock);    
    //  done
    *length = count;
    return data;
}


void * request_handler(void * param)
{  
	
	struct timespec start, finish;
	clock_gettime( CLOCK_REALTIME, &start);

    struct client_state * state = (struct client_state *)param;
 

    char buffer[BUFSIZE];
    memset(buffer, 0, sizeof(buffer));
    size_t count = recv(state->fd, buffer, BUFSIZE - 1, 0);  //  leave space for 0 terminator
    
    //  this regexp will match a GET request
    regex_t reg;
	regcomp(&reg, "GET +([^ \t]+) +HTTP/[0-9.]+ *[\r|\n]", REG_ICASE | REG_EXTENDED);
    
    //  find a single match
    regmatch_t matches[2];
    regexec(&reg, buffer, 2, matches, 0);
    regfree(&reg);

    //  this is a get request, let's pull out the target address

    char * requestTarget= (char*)malloc(BUFSIZE *sizeof(char));
    extract_match(requestTarget, buffer, matches[1]);
    
    //  hit the cache for the resource
    size_t dataLength;
    void * data;
  
    //  cache contained no resource, so we lookup the host and make a request
    //  a regexp to find the hostname
	regcomp(&reg, "Host:[ \t]+([^ \t\r\n]+)[\r|\n]", REG_ICASE | REG_EXTENDED);
    regexec(&reg, buffer, 2, matches, 0);
    regfree(&reg);
        
    //  extract the hostname
    char * hostname = (char*)malloc(BUFSIZE *sizeof(char));
    extract_match(hostname, buffer, matches[1]);

    int marker = 0;
    int i = 0;
    for(i = 0; i < CACHE_SIZE; i++)
    {
    	if(use[i] == 1 && strcmp(hostname, chostname[i]) == 0 && strcmp(requestTarget, ctarget[i])== 0)
    	{
    		marker = 1;
    		break;
    	}
    }

    if(marker)
    {
    	data = content[i];
    	dataLength = clength[i];
    }

    else{

    	data = make_request(hostname, buffer, requestTarget, &dataLength); 
    	i = find_index(dataLength);
    	content[i] = data;
    	clength[i] = dataLength;
    	chostname[i] = hostname;
    	ctarget[i] = requestTarget;
    	use[i] = 1;
    	frequency[i] += 1; 
    	current += dataLength;
    }    
  
    //  we downloaded some data, send it back to the client and finish
    write(state->fd, data, dataLength);
    close(state->fd);
    clock_gettime(CLOCK_REALTIME, &finish); 

    long seconds = finish.tv_sec - start.tv_sec; 
    long ns = finish.tv_nsec - start.tv_nsec; 
    
    if (start.tv_nsec > finish.tv_nsec) { // clock underflow 
	--seconds; 
	ns += 1000000000; 
    } 
    long accum = seconds*1000 + ns/1000000;
    if(marker) printf("%s|%s|CACHE_HIT|%d|%d\n", state->ipBuffer, requestTarget, dataLength, accum );
    else printf("%s|%s|CACHE_MISS|%d|%d\n", state->ipBuffer, requestTarget, dataLength, accum );

    return 0;
}

int main(int argc, const char * argv[])
{
    //It should create a TCP server socket to listen for incoming TCP connections on an unused port, and output
	//the host name and port number the proxy is running on.
	ushort data_port ;
    char * data_host;
    struct hostent *hp;

    int proxy = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    
    struct sockaddr_in proxyAddress;
    memset(&proxyAddress, 0, sizeof(proxyAddress));
 
    proxyAddress.sin_family = AF_INET;
    proxyAddress.sin_port = htons(0);
    proxyAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    
    //  bind
	bind(proxy, (const struct sockaddr*)&proxyAddress, sizeof(proxyAddress));
        
	listen(proxy, SOMAXCONN);



/*
     FILL HERE
     figure out the full host name (servhost)
     use gethostname() and gethostbyname()
     full host name is remote**.cs.binghamton.edu
     */
     data_host = malloc(MAXNAMELEN);
     if( gethostname(data_host, MAXNAMELEN) < 0 )
            return -1;
     if( (hp = gethostbyname(data_host)) < 0 )
            return -1;

     strcpy(data_host, hp->h_name);


 //    //get host name
 //    char * data_host = (char*)malloc(BUFSIZE *sizeof(char));
    socklen_t Length = sizeof(proxyAddress);

	// gethostname(data_host, BUFSIZE);

    /*
     FILL HERE
     figure out the port assigned to this server (servport)
     use getsockname()
     */
    if (getsockname(proxy, (struct sockaddr *)&proxyAddress, &Length) < 0){
        perror("Error: getsockname() error");
        exit(1);
    }

    data_port = ntohs(proxyAddress.sin_port);

    printf("My hostname: %s, data port: %d\n", data_host, data_port);

    //initial cache
    int i = 0;
    for(;i < CACHE_SIZE; i++)
    {
    	use[i] = 0;
    	frequency[i] = 0;
    	Limit = 5000000;
    	if(argc>1)
    	Limit = atoi(argv[1]);
    	current = 0;
    }

    while (1)
    {      
        int client = accept(proxy, (struct sockaddr*)&proxyAddress, &Length);
        struct client_state * state = malloc(sizeof(struct client_state));
		inet_ntop(proxyAddress.sin_family, &proxyAddress.sin_addr, state->ipBuffer, sizeof(state->ipBuffer));
        state->fd = client;       
        int status = pthread_create(&state->thread, NULL, request_handler, state);       
        pthread_join(state->thread,NULL);
        free(state);
    }
    
    close(proxy);
    free(data_host);
    return 0;
}
