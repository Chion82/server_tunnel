#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>   //strlen
#include <errno.h>
#include <unistd.h>   //close
#include <arpa/inet.h>    //close
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO macros
#include <signal.h>
#include <netdb.h>
  
#define TRUE   1
#define FALSE  0

#define SERVER_PUSH_NOTIFICATION_PORT 31100
#define SERVER_TRANSACT_PORT 31200


struct conn_info {
	char* lan_server_ip;
	int lan_server_port;
	char* recognize_code;
	char* server_ip;
	int server_port;
};

int hostname_to_ip(char * hostname , char* ip);
void *run_transaction(void* ptr);
void *run_heart_beat_client(void* ptr);

void *run_notification_client(void* ptr) {
	char* server_host = (char*)ptr;
	char server_ip[100];
	struct sockaddr_in server_addr;
	char buf[128];
	int lan_server_port;
	struct conn_info tmp_conn_info;
	pthread_t tmp_trans_thread;
	pthread_t heart_beat_thread;

	while (1) {
		if (hostname_to_ip(server_host, server_ip)) {
			printf("Server hostname %s not resolved.\n", server_host);
			exit(EXIT_FAILURE);
		}
		printf("Server IP: %s\n", server_ip);
		server_addr.sin_family = AF_INET;
		inet_aton(server_ip, &(server_addr.sin_addr));
		server_addr.sin_port = htons(SERVER_PUSH_NOTIFICATION_PORT);
		int notification_socket = socket(AF_INET, SOCK_STREAM, 0);
		while(connect(notification_socket, (struct sockaddr*)&server_addr, sizeof(server_addr))<0) {
			printf("Connection failed. Retrying...\n");
			sleep(1);
		}
		printf("Server connected.\n");
		pthread_create(&heart_beat_thread, NULL, run_heart_beat_client, (void*)&notification_socket);
		pthread_detach(heart_beat_thread);
		while(1) {

			int bytes_read = recv(notification_socket, buf, 128, 0);
			if (bytes_read<=0) {
				printf("Connection lost. Re-connecting.\n");
				close(notification_socket);
				break;
			}
			usleep(5*1000);
			printf("New connection. buf=%s\n", buf);
			char* devider = strstr(buf, "|");
			char* recognize_code = (char*)malloc(devider-buf+1);
			memset(recognize_code, '\0', devider-buf+1);
			memcpy(recognize_code, buf, devider-buf);
			char* lan_server_ip = (char*)malloc(32);
			sscanf(devider+1, "%d|%s", &lan_server_port, lan_server_ip);
			tmp_conn_info.lan_server_ip = lan_server_ip;
			tmp_conn_info.lan_server_port = lan_server_port;
			tmp_conn_info.recognize_code = recognize_code;
			tmp_conn_info.server_ip = server_ip;
			tmp_conn_info.server_port = SERVER_TRANSACT_PORT;
			pthread_create(&tmp_trans_thread, NULL, run_transaction, (void*)&tmp_conn_info);
			pthread_detach(tmp_trans_thread);

			usleep(200*1000);
		}
	}
	return NULL;
}

void *run_heart_beat_client(void* ptr) {
	int notification_socket = *((int*)ptr);
	while (1) {
		printf("Sending heart beat package.\n");
		if (send(notification_socket, "", 1, MSG_NOSIGNAL)<0) {
			break;
		}
		sleep(120);
	}
	return NULL;
}

void *run_transaction(void* ptr) {
	char* lan_server_ip = ((struct conn_info*)ptr)->lan_server_ip;
	char* recognize_code = ((struct conn_info*)ptr)->recognize_code;
	int lan_server_port = ((struct conn_info*)ptr)->lan_server_port;
	char* server_ip = ((struct conn_info*)ptr)->server_ip;
	int server_port = ((struct conn_info*)ptr)->server_port;
	printf("lan_server_ip=%s, recognize_code=%s, lan_server_port=%d \n", lan_server_ip, recognize_code, lan_server_port);

	struct sockaddr_in lan_server_addr;
	lan_server_addr.sin_family = AF_INET;
	inet_aton(lan_server_ip, &(lan_server_addr.sin_addr));
	lan_server_addr.sin_port = htons(lan_server_port);

	int lan_trans_sock = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(lan_trans_sock, (struct sockaddr*)&lan_server_addr, sizeof(lan_server_addr)) < 0) {
		printf("Connection to %s:%d failed.\n", lan_server_ip, lan_server_port);
		close(lan_trans_sock);
		free(lan_server_ip);
		free(recognize_code);
		return NULL;
	}

	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	inet_aton(server_ip, &(server_addr.sin_addr));
	server_addr.sin_port = htons(server_port);

	int server_trans_sock = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(server_trans_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
		printf("Connection to %s:%d failed.\n", server_ip, server_port);
		close(server_trans_sock);
		close(lan_trans_sock);
		free(lan_server_ip);
		free(recognize_code);
		return NULL;
	}

	printf("Sending recognize_code.\n");
	if (send(server_trans_sock, recognize_code, strlen(recognize_code)+1, MSG_NOSIGNAL)<0) {
		printf("Connection to server lost.\n");
		close(server_trans_sock);
		close(lan_trans_sock);
		free(lan_server_ip);
		free(recognize_code);
		return NULL;
	}

	free(lan_server_ip);
	free(recognize_code);

	usleep(5*1000);

	fd_set trans_sock_set;

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 300*1000;

	char trans_buf[1024];
	int bytes_read = 0;

	printf("Transaction start.\n");

	while (1) {
		FD_ZERO(&trans_sock_set);
		FD_SET(lan_trans_sock, &trans_sock_set);
		FD_SET(server_trans_sock, &trans_sock_set);

		int select_result = select((lan_trans_sock>server_trans_sock?lan_trans_sock:server_trans_sock)+1, &trans_sock_set, NULL, NULL, &timeout);
		if (select_result<0) {
			printf("Select error.\n");
			close(server_trans_sock);
			close(lan_trans_sock);
			break;
		}
		

		if (FD_ISSET(server_trans_sock, &trans_sock_set)) {
			//printf("Package from server\n");
			bytes_read = read(server_trans_sock, trans_buf, 1024);
			if (bytes_read<=0) {
				printf("Connection from server closed.\n");
				close(server_trans_sock);
				close(lan_trans_sock);
				break;
			}
			if (send(lan_trans_sock, trans_buf, bytes_read, MSG_NOSIGNAL)<0) {
				printf("Connection from lan-server closed.\n");
				close(server_trans_sock);
				close(lan_trans_sock);
				break;
			}
		}

		if (FD_ISSET(lan_trans_sock, &trans_sock_set)) {
			//printf("Package from lan\n");
			bytes_read = read(lan_trans_sock, trans_buf, 1024);
			if (bytes_read<=0) {
				printf("Connection from lan-server closed.\n");
				close(server_trans_sock);
				close(lan_trans_sock);
				break;
			}
			if (send(server_trans_sock, trans_buf, bytes_read, MSG_NOSIGNAL)<0) {
				printf("Connection from server closed.");
				close(server_trans_sock);
				close(lan_trans_sock);
				break;
			}
		}

	}
	
	return NULL;
}

int hostname_to_ip(char * hostname , char* ip)
{
    struct hostent *he;
    struct in_addr **addr_list;
    int i;
         
    if ( (he = gethostbyname( hostname ) ) == NULL) 
    {
        // get the host info
        herror("gethostbyname");
        return 1;
    }
 
    addr_list = (struct in_addr **) he->h_addr_list;
     
    for(i = 0; addr_list[i] != NULL; i++) 
    {
        //Return the first one;
        strcpy(ip , inet_ntoa(*addr_list[i]) );
        return 0;
    }
     
    return 1;
}

int main(int argc, char* argv[]) {
	pthread_t notification_thread;
	pthread_create(&notification_thread, NULL, run_notification_client, (void*)(argv[1]));
	pthread_detach(notification_thread);

	while(1)
		sleep(1);
	return 0;
}
