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
  
#define TRUE   1
#define FALSE  0

#define SERVER_PUSH_NOTIFICATION_PORT 31100
#define SERVER_TRANSACT_PORT 31200

struct socket_map {
	int user_sock;
	int client_side_sock;
	char* recognize_code;
};

struct server_config {
	int user_port;
	char client_side_lan_ip[32];
	int client_side_lan_port;
};

struct transact_bundle {
	int user_sock;
	struct server_config config;
};

char *replace_str(char *str, char *orig, char *rep);
int load_server_config(struct server_config* config, char* config_file_path);
void* map_client_side_socket_thread(void* ptr);
unsigned int rand_interval(unsigned int min, unsigned int max);
void *transaction_thread(void* thread_bundle);


struct socket_map map_queue[128];

int notification_sock = 0;

void *notification_server(void* ptr) {
	int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in serv_addr;
	memset(&serv_addr, '0', sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(SERVER_PUSH_NOTIFICATION_PORT);
	bind(listen_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	listen(listen_sock, 10);
	while (1) {
		notification_sock = accept(listen_sock, (struct sockaddr*)NULL, NULL);
		printf("Client connection established.\n");
	}

	return NULL;
}



void *run_client_side_server(void* ptr) {
	pthread_t tmp_thread;
	int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in serv_addr;
	memset(&serv_addr, '0', sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(SERVER_TRANSACT_PORT);
	bind(listen_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	listen(listen_sock, SOMAXCONN);
	while (1) {
		int transaction_sock = accept(listen_sock, (struct sockaddr*)NULL, NULL);
		pthread_create(&tmp_thread, NULL, map_client_side_socket_thread, (void*)&transaction_sock);
		pthread_detach(tmp_thread);
	}

	return NULL;

}

void* map_client_side_socket_thread(void* ptr) {
	char buf[32], *recognize_code;
	int transaction_sock = *((int*)ptr);
	int buf_len = recv(transaction_sock, buf, 32, 0);
	usleep(5*1000);
	if (buf_len<=0) {
		printf("Client connection lost.\n");
		close(transaction_sock);
		return NULL;
	}
	recognize_code = replace_str(buf, "\n" ,"");
	int i=0;
	printf("recognize_code from client side. buf=%s\n", recognize_code);
	for (i=0;i<128;i++) {
		if (map_queue[i].recognize_code == NULL) {
			continue;
		}
		if (strcmp(map_queue[i].recognize_code, recognize_code) == 0) {
			map_queue[i].client_side_sock = transaction_sock;
			return NULL;
		}
	}
	printf("Recognize_code not found. Closing socket.\n");
	close(transaction_sock);

	return NULL;
}

int push_and_wait_for_client(int* client_trans_sock, int user_sock, struct server_config config) {
	char recognize_code[32];
	char buf[48];
	sprintf(recognize_code, "%d%d", (int)time(NULL), rand_interval(100000,999999));
	sprintf(buf, "%s|%d|%s", recognize_code, config.client_side_lan_port, config.client_side_lan_ip);
	if (send(notification_sock, buf, strlen(buf)+1, MSG_NOSIGNAL)<0){
		printf("Client connection not found. Exiting...\n");
		return -1;
	}
	printf("recognize_code sent. buf=%s\n", buf);
	int i=0;
	int flag = 0;
	usleep(5*1000);
	
	for (i=0;i<128;i++) {
		if (map_queue[i].user_sock == 0 && map_queue[i].client_side_sock == 0 && map_queue[i].recognize_code == NULL) {
			map_queue[i].user_sock = user_sock;
			map_queue[i].client_side_sock = 0;
			map_queue[i].recognize_code = recognize_code;
			flag = 1;
			break;
		}
	}
	if (!flag) {
		printf("Queue full!\n");
		return -1;
	}
	
	int wait_count = 0;
	while (map_queue[i].client_side_sock == 0) {
		usleep(1000);
		wait_count++;
		if (wait_count>=10*1000) {
			printf("Client timeout.\n");
			map_queue[i].user_sock = 0;
			map_queue[i].client_side_sock = 0;
			map_queue[i].recognize_code = NULL;
			return -1;
		}
	}
	printf("Client side connected. recognize_code=%s\n", recognize_code);
	*client_trans_sock = map_queue[i].client_side_sock;
	map_queue[i].user_sock = 0;
	map_queue[i].client_side_sock = 0;
	map_queue[i].recognize_code = NULL;
	return 1;
}

void* run_user_side_server(void* ptr){
	struct server_config config = *((struct server_config*)ptr);
	pthread_t tmp_thread;
	struct transact_bundle tmp_bundle;
	int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in serv_addr;
	memset(&serv_addr, '0', sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(config.user_port);
	bind(listen_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	listen(listen_sock, SOMAXCONN);
	while (1) {
		int trans_sock = accept(listen_sock, (struct sockaddr*)NULL, NULL);
		tmp_bundle.user_sock = trans_sock;
		tmp_bundle.config = config;
		pthread_create(&tmp_thread, NULL, transaction_thread, (void*)&tmp_bundle);
		pthread_detach(tmp_thread);
		usleep(50*1000);
	}

	return NULL;
}

void *transaction_thread(void* thread_bundle) {
	int user_sock = ((struct transact_bundle*)thread_bundle)->user_sock;
	struct server_config config = ((struct transact_bundle*)thread_bundle)->config;
	int client_sock = 0;
	if (push_and_wait_for_client(&client_sock, user_sock, config) == -1){
		close(user_sock);
		close(client_sock);
		return NULL;
	}
	printf("Transaction tunnel start.\n");

	char trans_buf[1024];
	int bytes_read = 0;

	fd_set user_set, client_set;

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 300*1000;



	while (1) {
		FD_ZERO(&user_set);
		FD_ZERO(&client_set);
		FD_SET(user_sock, &user_set);
		FD_SET(client_sock, &client_set);
		int user_select_result = select(user_sock+1, &user_set, NULL, NULL, &timeout);
		if (user_select_result<0) {
			printf("user_sock select error.\n");
			close(user_sock);
			close(client_sock);
			break;
		}
		int client_select_result = select(client_sock+1, &client_set, NULL, NULL, &timeout);
		if (client_select_result<0) {
			printf("client_sock select error.\n");
			close(user_sock);
			close(client_sock);
			break;
		}
		if (user_select_result>0) {
			bytes_read = read(user_sock, trans_buf, 1024);
			if (bytes_read<=0) {
				printf("User connection closed.\n");
				close(user_sock);
				close(client_sock);
				break;
			}
			if(send(client_sock, trans_buf, bytes_read, MSG_NOSIGNAL)<0) {
				printf("Client connection closed.\n");
				close(user_sock);
				close(client_sock);
				break;
			}
		}
		if (client_select_result>0) {
			bytes_read = read(client_sock, trans_buf, 1024);
			if (bytes_read<=0) {
				printf("Client connection closed.\n");
				close(user_sock);
				close(client_sock);
				break;
			}
			if (send(user_sock, trans_buf, bytes_read, MSG_NOSIGNAL)<0) {
				printf("User connection closed.\n");
				close(user_sock);
				close(client_sock);
				break;
			}
		}
	}

	return NULL;

}


int main(int argc, char* argv[]) {
	srand((int)time(NULL));
	int i=0;
	for (i=0;i<128;i++) {
		map_queue[i].user_sock = 0;
		map_queue[i].client_side_sock = 0;
		map_queue[i].recognize_code = NULL;
	}


	struct server_config config[100];
	printf("Loading server config file...\n");
	int count = load_server_config(config, argv[1]);

	printf("Init notification server...\n");
	pthread_t notification_listen_thread;
	pthread_create(&notification_listen_thread, NULL, notification_server, NULL);
	pthread_detach(notification_listen_thread);

	printf("Init client side server...\n");
	pthread_t client_listen_thread;
	pthread_create(&client_listen_thread, NULL, run_client_side_server, NULL);
	pthread_detach(client_listen_thread);


	for (i=0;i<count;i++) {
		printf("ip=%s, user_port=%i, lan_port=%i", config[i].client_side_lan_ip, config[i].user_port, config[i].client_side_lan_port);
		printf("\n");
		printf("Init user side server...\n");
		pthread_t user_listen_thread;
		pthread_create(&user_listen_thread, NULL, run_user_side_server, (void*)(config+i));
		pthread_detach(user_listen_thread);

	}

	while(1)
		sleep(1);
	return 0;
}


char *replace_str(char *str, char *orig, char *rep)
{
	static char buffer[4096];
	char *p;

	if(!(p = strstr(str, orig)))  // Is 'orig' even in 'str'?
	return str;

	strncpy(buffer, str, p-str); // Copy characters from 'str' start to 'orig' st$
	buffer[p-str] = '\0';

	sprintf(buffer+(p-str), "%s%s", rep, p+strlen(orig));

	return buffer;
}

unsigned int rand_interval(unsigned int min, unsigned int max)
{
    int r;
    const unsigned int range = 1 + max - min;
    const unsigned int buckets = RAND_MAX / range;
    const unsigned int limit = buckets * range;

    /* Create equal size buckets all in a row, then fire randomly towards
     * the buckets until you land in one of them. All buckets are equally
     * likely. If you land off the end of the line of buckets, try again. */
    do
    {
        r = rand();
    } while (r >= limit);

    return min + (r / buckets);
}

int load_server_config(struct server_config* config, char* config_file_path){
	struct server_config* tmp_result = (struct server_config*)malloc(sizeof(struct server_config)*100);
	char *line = NULL;
	int line_count = 0;
	size_t len = 0;
	ssize_t read;
	printf("Reading config file at %s\n", config_file_path);
	FILE *stream = fopen(config_file_path, "r");
	if (stream == NULL) {
		exit(EXIT_FAILURE);
	}
	int i = 0;
	struct server_config tmp_config;
	while((read = getline(&line, &len, stream)) != -1) {
		if (line_count>=100) {
			break;
		}
		char* formated_str = replace_str(line,"\n","");
		if (strcmp("", formated_str) == 0) {
			continue;
		}
		sscanf(formated_str, "%i|%i|%s", &(tmp_config.user_port), &(tmp_config.client_side_lan_port) ,tmp_config.client_side_lan_ip);
		tmp_result[line_count] = tmp_config;
		line_count++;
	}
	struct server_config* result = (struct server_config*)malloc(sizeof(struct server_config)*line_count);
	memcpy((void*)config, (void*)tmp_result, sizeof(struct server_config)*line_count);
	free(tmp_result);
	return line_count;
}

