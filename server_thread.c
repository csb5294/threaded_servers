#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

//Mutex declaration
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


void *handle_connection(void *arg){

	//Create timespec structs for measuring the time
	struct timespec start, end;

	//Measure starting time
	clock_gettime(CLOCK_MONOTONIC, &start);

	//Thread variables
	int client_socket = *((int*)arg);
	char buffer[1024];
	char filename[1024];
	struct stat file_stats;
	FILE *f;

	memset(buffer, sizeof(buffer), 0);
	memset(filename, sizeof(buffer), 0);

	//In HTTP, the client speaks first. So we recv their message
	//into out buffer
	int amt = recv(client_socket, buffer, sizeof(buffer), 0);

	//We only can handle HTTP GET requests for files served
	//from the current working directory, which becomes the website root
	if(sscanf(buffer, "GET /%s", filename)<1){
		fprintf(stderr, "Bad HTTP request\n");
		close(client_socket);
		pthread_exit(NULL);
	}

	//If the HTTP request is bigger than our buffer can hold, we need to call
	//recv() until we have no more data to read, otherwise it will be
	//there waiting for us on the next call to recv(). So we'll just
	//read it and discard it. GET should be the first 3 bytes, and we'll
	//assume paths that are smaller than about 1000 characters.
	if(amt == sizeof(buffer)){
		//if recv returns as much as we asked for, there may be more data
		while(recv(client_socket, buffer, sizeof(buffer), 0) == sizeof(buffer))
			/*discard*/;
	}

	//if we don't open for binary mode, line ending conversion may occur.
	//this will make a liar our of our file size.
	f = fopen(filename, "rb");
	if(f == NULL){
		//Assume that failure to open the file means it doesn't exist
		strcpy(buffer, "HTTP/1.1 404 Not Found (open)\n\n");
		send(client_socket, buffer, strlen(buffer), 0);
	}
	else
	{
		int size;
		char response[1024];

		strcpy(response, "HTTP/1.1 200 OK\n");
		send(client_socket, response, strlen(response), 0);

		time_t now;
		time(&now);
		//How convenient that the HTTP Date headerfield is exactly
		//in the format of the asctime() library function.
		//
		//asctime adds a newline for some dumb reason.
		sprintf(response, "Date: %s", asctime(gmtime(&now)));
		send(client_socket, response, strlen(response), 0);

		//Get the file size via the stat system call
		fstat(fileno(f), &file_stats);
		sprintf(response, "Content-Length: %d\n", file_stats.st_size);
		send(client_socket, response, strlen(response), 0);

		//Tell the client we won't reuse this connection for other files
		strcpy(response, "Connection: close\n");
		send(client_socket, response, strlen(response), 0);

		//Send out MIME type and a blank line
		strcpy(response, "Content-Type: text/html\n\n");
		send(client_socket, response, strlen(response), 0);

		fprintf(stderr, "File: %s\n", filename);

		int bytes_read = 0;
		do
		{
			//read response amount of data at a time.
			//Note that sizeof() in C can only tell us the number of
			//elements in an array that is declared in scope. If you
			//move the declaration elsewhere, it will degrade into
			//the sizeof a pointer instead.
			bytes_read = fread(response, 1, sizeof(response), f);

			//if we read anything, send it
			if(bytes_read > 0)
			{
				int sent = send(client_socket, response, bytes_read, 0);
				//It's possible that send wasn't able to send all of
				//our response in one call. It will return how much it
				//actually sent. Keep calling send until all of it is
				//sent.
				while(sent < bytes_read)
				{
					sent += send(client_socket, response+sent, bytes_read-sent, 0);
				}
			}
		}while(bytes_read > 0 && bytes_read == sizeof(response));

		//Close file
		fclose(f);
	}

	//Close client socket
	close(client_socket);

	//Measure ending time
	clock_gettime(CLOCK_MONOTONIC, &end);

	//Compute elapsed time
	float e_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000000.0;

	//Lock mutex
	pthread_mutex_lock(&mutex);

	//Open stats file for writing
	FILE *statfd;
	statfd = fopen("stats_thread.txt", "w");
	if(statfd == NULL){
		perror("Opening stats file failed");
		exit(EXIT_FAILURE);
	}

	//Write the stats to the stat file
	fprintf(statfd,"%s\t%d\t%.4f", filename, file_stats.st_size, e_time);

	//Close stat file
	fclose(statfd);

	//Unlock mutex
	pthread_mutex_unlock(&mutex);

	//Exit thread
	pthread_exit(NULL);
}

int main(){

	//Thread id
	pthread_t thread_id;

	//Sockets represent potential connections
	//We make an internet socket
	int sfd = socket(PF_INET, SOCK_STREAM, 0);
	if(-1 == sfd)
	{
		perror("Cannot create socket\n");
		exit(EXIT_FAILURE);
	}

	//We will configure it to use this machine's IP, or
	//for us, localhost (127.0.0.1)
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	//Web servers always listen on port 80
	addr.sin_port = htons(80);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	//So we bind our socket to port 80
	if(-1 == bind(sfd, (struct sockaddr *) &addr, sizeof(addr)))
	{
		perror("Bind failed");
		exit(EXIT_FAILURE);
	}

	//And set it up as a listening socket with a backlog of 10 pending connections
	if(-1 == listen(sfd, 10)){
		perror("Listen failed");
		exit(EXIT_FAILURE);
	}

	//Initialize mutex
	if(pthread_mutex_init(&mutex, NULL) != 0){
		perror("Mutex initialization failed");
		exit(EXIT_FAILURE);
	}

	//A server's gotta serve...
	for(;;)
	{
		//accept() blocks until a client connects. When it returns,
		//we have a client that we can do client stuff with.
		int connfd = accept(sfd, NULL, NULL);
		if(connfd < 0)
		{
			perror("Accept failed");
			continue;
		}

		//Start thread
		if(pthread_create(&thread_id, NULL, handle_connection, &connfd) != 0){
			perror("Creating thread failed");
			close(connfd);
			continue;
		}

		//Detach thread
		pthread_detach(thread_id);
	}
	close(sfd);
	return 0;
}
