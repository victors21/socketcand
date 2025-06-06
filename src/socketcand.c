/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * Authors:
 * Andre Naujoks (the socket server stuff)
 * Oliver Hartkopp (the rest)
 * Jan-Niklas Meier (extensions for use with kayak)
 *
 * Copyright (c) 2002-2009 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Send feedback to <linux-can@vger.kernel.org>
 *
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <syslog.h>
#ifdef HAVE_LIBCONFIG
#include <libconfig.h>
#endif

#include "socketcand.h"
#include "statistics.h"
#include "beacon.h"

void print_usage(void);
void sigint(int signum);
void childdied(int signum);
void determine_adress();
int receive_command(int socket, char *buf);

int sl, client_socket;
pthread_t beacon_thread, statistics_thread;
char **interface_names;
int interface_count = 0;
int port;
int verbose_flag = 0;
int daemon_flag = 0;
int disable_beacon = 0;
int tcp_quickack_flag = 0;
int state = STATE_NO_BUS;
int previous_state = -1;
char bus_name[MAX_BUSNAME];
char cmd_buffer[MAXLEN];
int cmd_index = 0;
char *description;
char *afuxname;
int more_elements = 0;
int can_fd_mode_flag = 0;
can_err_mask_t error_mask = 0;
struct sockaddr_in saddr, broadcast_addr;
struct sockaddr_un unaddr;
socklen_t unaddrlen;
struct sockaddr_un remote_unaddr;
socklen_t remote_unaddrlen;
char *interface_string;

void tcp_quickack(int s)
{
	int i = 1;

	if (tcp_quickack_flag)
		setsockopt(s, IPPROTO_TCP, TCP_QUICKACK, &i, sizeof(int));
}

int state_changed(char *buf, int current_state)
{
	if (!strcmp("< rawmode >", buf))
		state = STATE_RAW;
	else if (!strcmp("< bcmmode >", buf))
		state = STATE_BCM;
	else if (!strcmp("< isotpmode >", buf))
		state = STATE_ISOTP;
	else if (!strcmp("< controlmode >", buf))
		state = STATE_CONTROL;

	if (current_state != state)
		PRINT_INFO("state changed to %d\n", state);

	return (current_state != state);
}

char *element_start(char *buf, int element)
{
	int len = strlen(buf);
	int elem, i;

	/*
	 * < elem1 elem2 elem3 >
	 *
	 * get the position of the requested element as char pointer
	 */

	for (i = 0, elem = 0; i < len; i++) {
		if (buf[i] == ' ') {
			elem++;

			/* step to next non-space */
			while (buf[i] == ' ')
				i++;

			if (i >= len)
				return NULL;
		}

		if (elem == element)
			return &buf[i];
	}
	return NULL;
}

int element_length(char *buf, int element)
{
	int len;
	int j = 0;
	char *elembuf;

	/*
	 * < elem1 elem2 elem3 >
	 *
	 * get the length of the requested element in bytes
	 */

	elembuf = element_start(buf, element);
	if (elembuf == NULL)
		return 0;

	len = strlen(elembuf);

	while (j < len && elembuf[j] != ' ')
		j++;

	return j;
}

int asc2nibble(char c)
{
	if ((c >= '0') && (c <= '9'))
		return c - '0';

	if ((c >= 'A') && (c <= 'F'))
		return c - 'A' + 10;

	if ((c >= 'a') && (c <= 'f'))
		return c - 'a' + 10;

	return 16; /* error */
}

int main(int argc, char **argv)
{
	int i;
	struct sockaddr_in clientaddr;
	socklen_t sin_size = sizeof(clientaddr);
	struct sigaction signalaction, sigint_action;
	sigset_t sigset;
	int c;
	char *busses_string;
#ifdef HAVE_LIBCONFIG
	config_t config;
#endif

	/* set default config settings */
	port = PORT;
	description = malloc(sizeof(BEACON_DESCRIPTION));
	strcpy(description, BEACON_DESCRIPTION);
	interface_string = malloc(strlen(DEFAULT_INTERFACE) + 1);
	strcpy(interface_string, DEFAULT_INTERFACE);
	busses_string = malloc(strlen(DEFAULT_BUSNAME) + 1);
	strcpy(busses_string, DEFAULT_BUSNAME);
	afuxname = NULL;

#ifdef HAVE_LIBCONFIG
	/* Read config file before parsing commandline arguments */
	config_init(&config);
	if (CONFIG_TRUE == config_read_file(&config, "/etc/socketcand.conf")) {
		config_lookup_int(&config, "port", (int *)&port);
		config_lookup_string(&config, "description", (const char **)&description);
		config_lookup_string(&config, "afuxname", (const char **)&afuxname);
		config_lookup_string(&config, "busses", (const char **)&busses_string);
		config_lookup_string(&config, "listen", (const char **)&interface_string);
	}
#endif

	/* Parse commandline arguments */
	while (1) {
		/* getopt_long stores the option index here. */
		int option_index = 0;
		static struct option long_options[] = {
			{ "verbose", no_argument, 0, 'v' },
			{ "interfaces", required_argument, 0, 'i' },
			{ "port", required_argument, 0, 'p' },
			{ "quick-ack", no_argument, 0, 'q' },
			{ "afuxname", required_argument, 0, 'u' },
			{ "listen", required_argument, 0, 'l' },
			{ "daemon", no_argument, 0, 'd' },
			{ "version", no_argument, 0, 'z' },
			{ "no-beacon", no_argument, 0, 'n' },
			{ "error-mask", required_argument, 0, 'e' },
			{ "can-fd", no_argument, 0, 'f' },
			{ "help", no_argument, 0, 'h' },
			{ 0, 0, 0, 0 }
		};

		c = getopt_long(argc, argv, "vi:p:qu:l:dzne:h", long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
		case 0:
			/* If this option set a flag, do nothing else now. */
			if (long_options[option_index].flag != 0)
				break;
			break;

		case 'v':
			puts("Verbose output activated\n");
			verbose_flag = 1;
			break;

		case 'i':
			busses_string = realloc(busses_string, strlen(optarg) + 1);
			strcpy(busses_string, optarg);
			break;

		case 'p':
			port = atoi(optarg);
			break;

		case 'q':
			PRINT_VERBOSE("TCP_QUICKACK socket option activated\n");
			tcp_quickack_flag = 1;
			break;

		case 'u':
			afuxname = realloc(afuxname, strlen(optarg) + 1);
			strcpy(afuxname, optarg);
			break;

		case 'l':
			interface_string = realloc(interface_string, strlen(optarg) + 1);
			strcpy(interface_string, optarg);
			break;

		case 'd':
			daemon_flag = 1;
			break;

		case 'z':
			printf("socketcand version '%s'\n", PACKAGE_VERSION);
			return 0;

		case 'n':
			disable_beacon = 1;
			break;

		case 'e':
			error_mask = strtoul(optarg, NULL, 16);
			break;

		case 'h':
			print_usage();
			return 0;
		
		case 'f':
			can_fd_mode_flag = 1;
			break;
		
		case '?':
			print_usage();
			return 0;
		default:
			print_usage();
			return -1;
		}
	}

	/* parse buses */
	for (i = 0;; i++) {
		if (busses_string[i] == '\0')
			break;
		if (busses_string[i] == ',')
			interface_count++;
	}
	interface_count++;

	interface_names = malloc(sizeof(char *) * interface_count);

	interface_names[0] = strtok(busses_string, ",");

	for (i = 1; i < interface_count; i++) {
		interface_names[i] = strtok(NULL, ",");
	}

	/* if daemon mode was activated the syslog must be opened */
	if (daemon_flag) {
		openlog("socketcand", 0, LOG_DAEMON);
	}

	sigemptyset(&sigset);
	signalaction.sa_handler = &childdied;
	signalaction.sa_mask = sigset;
	signalaction.sa_flags = 0;
	sigaction(SIGCHLD, &signalaction, NULL); /* signal for dying child */

	sigint_action.sa_handler = &sigint;
	sigint_action.sa_mask = sigset;
	sigint_action.sa_flags = 0;
	sigaction(SIGINT, &sigint_action, NULL);

	determine_adress();

	if (!disable_beacon) {
		PRINT_VERBOSE("creating broadcast thread...\n");
		i = pthread_create(&beacon_thread, NULL, &beacon_loop, NULL);
		if (i)
			PRINT_ERROR("could not create broadcast thread.\n");
	} else {
		PRINT_VERBOSE("Discovery beacon disabled\n");
	}

	if (afuxname) {
		/* create PF_UNIX socket */
		if ((sl = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
			perror("unixsocket");
			exit(1);
		}

		unaddr.sun_family = AF_UNIX;
		if (strlen(afuxname) > sizeof(unaddr.sun_path) - 3) {
			printf("afuxname is too long.\n");
			exit(1);
		}

		/* when the given afuxname starts with a '/' we assume the path name scheme, e.g.
		 * /var/run/socketcand or /tmp/socketcand-afunix-socket
		 * Without the leading '/' we use the string as abstract socket address.
		 */

		if (afuxname[0] == '/') {
			strcpy(&unaddr.sun_path[0], afuxname);
			/* due to the trailing \0 in path name definition we can write the entire struct */
			unaddrlen = sizeof(unaddr);
		} else {
			strcpy(&unaddr.sun_path[1], afuxname);
			unaddr.sun_path[0] = 0;
			/* abstract name length definition without trailing \0 but with leading \0 */
			unaddrlen = strlen(afuxname) + sizeof(unaddr.sun_family) + 1;
		}
		PRINT_VERBOSE("binding unix socket to '%s' with unaddrlen %d\n", afuxname, unaddrlen);
		if (bind(sl, (struct sockaddr *)&unaddr, unaddrlen) < 0) {
			perror("unixbind");
			exit(-1);
		}

		if (listen(sl, 3) != 0) {
			perror("unixlisten");
			exit(1);
		}

		while (1) {
			remote_unaddrlen = sizeof(struct sockaddr_un);
			client_socket = accept(sl, (struct sockaddr *)&remote_unaddr, &remote_unaddrlen);
			if (client_socket > 0) {
				if (fork())
					close(client_socket);
				else
					break;
			} else {
				if (errno != EINTR) {
					/*
					 * If the cause for the error was NOT the
					 * signal from a dying child => give an error
					 */
					perror("accept");
					exit(1);
				}
			}
		}

		PRINT_VERBOSE("client connected\n");

	} else {
		/* create PF_INET socket */

		if ((sl = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
			perror("inetsocket");
			exit(1);
		}

#ifdef DEBUG
		if (verbose_flag)
			printf("setting SO_REUSEADDR\n");
		i = 1;
		if (setsockopt(sl, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i)) < 0) {
			perror("setting SO_REUSEADDR failed");
		}
#endif

		PRINT_VERBOSE("binding socket to %s:%d\n", inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port));
		if (bind(sl, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
			perror("bind");
			exit(-1);
		}

		if (listen(sl, 3) != 0) {
			perror("listen");
			exit(1);
		}

		while (1) {
			client_socket = accept(sl, (struct sockaddr *)&clientaddr, &sin_size);
			if (client_socket > 0) {
				int flag;
				flag = 1;
				setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));
				if (fork())
					close(client_socket);
				else
					break;
			} else {
				if (errno != EINTR) {
					/*
					 * If the cause for the error was NOT the
					 * signal from a dying child => give an error
					 */
					perror("accept");
					exit(1);
				}
			}
		}

		PRINT_VERBOSE("client connected\n");

#ifdef DEBUG
		PRINT_VERBOSE("setting SO_REUSEADDR\n");
		i = 1;
		if (setsockopt(client_socket, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i)) < 0) {
			perror("setting SO_REUSEADDR failed");
		}
#endif
	}
	/* main loop with state machine */
	while (1) {
		switch (state) {
		case STATE_NO_BUS:
			state_nobus();
			break;
		case STATE_BCM:
			state_bcm();
			break;
		case STATE_RAW:
			state_raw();
			break;
		case STATE_ISOTP:
			state_isotp();
			break;
		case STATE_CONTROL:
			state_control();
			break;

		case STATE_SHUTDOWN:
			PRINT_VERBOSE("Closing client connection.\n");
			close(client_socket);
			return 0;
		}
	}
	return 0;
}

/* reads all available data from the socket into the command buffer.
 * returns '-1' if no command could be received.
 */
int receive_command(int socket, char *buffer)
{
	int i, start, stop;

	/* if there are no more elements in the buffer read more data from the
	 * socket.
	 */
	if (!more_elements) {
		cmd_index += read(socket, cmd_buffer + cmd_index, MAXLEN - cmd_index);
		tcp_quickack(client_socket);
#ifdef DEBUG_RECEPTION
		PRINT_VERBOSE("\tRead from socket\n");
#endif
	}

#ifdef DEBUG_RECEPTION
	PRINT_VERBOSE("\tcmd_index now %d\n", cmd_index);
#endif

	more_elements = 0;

	/* find first '<' in string */
	start = -1;
	for (i = 0; i < cmd_index; i++) {
		if (cmd_buffer[i] == '<') {
			start = i;
			break;
		}
	}

	/*
	 * if there is no '<' in string it makes no sense to keep data because
	 * we will never be able to construct a command of it
	 */
	if (start == -1) {
		cmd_index = 0;
#ifdef DEBUG_RECEPTION
		PRINT_VERBOSE("\tBad data. No element found\n");
#endif
		return -1;
	}

	/* check whether the command is completely in the buffer */
	stop = -1;
	for (i = 1; i < cmd_index; i++) {
		if (cmd_buffer[i] == '>') {
			stop = i;
			break;
		}
	}

	/* if no '>' is in the string we have to wait for more data */
	if (stop == -1) {
#ifdef DEBUG_RECEPTION
		PRINT_VERBOSE("\tNo full element in the buffer\n");
#endif
		return -1;
	}

#ifdef DEBUG_RECEPTION
	PRINT_VERBOSE("\tElement between %d and %d\n", start, stop);
#endif

	/* copy string to new destination and correct cmd_buffer */
	for (i = start; i <= stop; i++) {
		buffer[i - start] = cmd_buffer[i];
	}
	buffer[i - start] = '\0';

#ifdef DEBUG_RECEPTION
	PRINT_VERBOSE("\tElement is '%s'\n", buffer);
#endif

	/* if only this message was in the buffer we're done */
	if (stop == cmd_index - 1) {
		cmd_index = 0;
	} else {
		/* check if there is a '<' after the stop */
		start = -1;
		for (i = stop; i < cmd_index; i++) {
			if (cmd_buffer[i] == '<') {
				start = i;
				break;
			}
		}

		/* if there is none it is only garbage we can remove */
		if (start == -1) {
			cmd_index = 0;
#ifdef DEBUG_RECEPTION
			PRINT_VERBOSE("\tGarbage after the first element in the buffer\n");
#endif
			return 0;
			/* otherwise we copy the valid data to the beginning of the buffer */
		} else {
			for (i = start; i < cmd_index; i++) {
				cmd_buffer[i - start] = cmd_buffer[i];
			}
			cmd_index -= start;

			/* check if there is at least one full element in the buffer */
			stop = -1;
			for (i = 1; i < cmd_index; i++) {
				if (cmd_buffer[i] == '>') {
					stop = i;
					break;
				}
			}

			if (stop != -1) {
				more_elements = 1;
#ifdef DEBUG_RECEPTION
				PRINT_VERBOSE("\tMore than one full element in the buffer.\n");
#endif
			}
		}
	}
	return 0;
}

void determine_adress()
{
	struct ifreq ifr, ifr_mask;

	int probe_socket = socket(AF_INET, SOCK_DGRAM, 0);

	if (probe_socket < 0) {
		PRINT_ERROR("Could not create socket!\n");
		exit(-1);
	}

	PRINT_VERBOSE("Using network interface '%s'\n", interface_string);

	ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name, interface_string, IFNAMSIZ - 1);
	ioctl(probe_socket, SIOCGIFADDR, &ifr);

	ifr_mask.ifr_addr.sa_family = AF_INET;
	strncpy(ifr_mask.ifr_name, interface_string, IFNAMSIZ - 1);
	ioctl(probe_socket, SIOCGIFNETMASK, &ifr_mask);

	close(probe_socket);

	PRINT_VERBOSE("Listen address is %s\n", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	PRINT_VERBOSE("Netmask is %s\n", inet_ntoa(((struct sockaddr_in *)&ifr_mask.ifr_netmask)->sin_addr));

	/* set listen address */
	saddr.sin_family = AF_INET;
	saddr.sin_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
	saddr.sin_port = htons(port);

	/* calculate and set broadcast address */
	broadcast_addr.sin_family = AF_INET;
	broadcast_addr.sin_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
	broadcast_addr.sin_addr.s_addr |= ~((struct sockaddr_in *)&ifr_mask.ifr_netmask)->sin_addr.s_addr;
	broadcast_addr.sin_port = htons(BROADCAST_PORT);
	PRINT_VERBOSE("Broadcast address is %s\n", inet_ntoa(broadcast_addr.sin_addr));
}

void print_usage(void)
{
	printf("%s Version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
	printf("Report bugs to %s\n\n", PACKAGE_BUGREPORT);
	printf("Usage: socketcand [-v | --verbose] [-i interfaces | --interfaces interfaces]\n\t\t[-p port | --port port] [-q | --quick-ack]\n\t\t[-l interface | --listen interface] [-u name | --afuxname name]\n\t\t[-e error_mask | --error-mask error_mask]\n\t\t[-n | --no-beacon] [-f | --can-fd] [-d | --daemon] [-h | --help]\n\n");
	printf("Options:\n");
	printf("\t-v (activates verbose output to STDOUT)\n");
	printf("\t-i <interfaces> (comma separated list of CAN interfaces the daemon\n\t\tshall provide access to e.g. '-i can0,vcan1' - default: %s)\n", DEFAULT_BUSNAME);
	printf("\t-p <port> (changes the default port '%d' the daemon is listening at)\n", PORT);
	printf("\t-q (enable TCP_QUICKACK socket option)\n");
	printf("\t-l <interface> (changes the default network interface the daemon will\n\t\tbind to - default: %s)\n", DEFAULT_INTERFACE);
	printf("\t-u <name> (the AF_UNIX socket path - an abstract name is used when\n\t\tthe leading '/' is missing. N.B. the AF_UNIX binding will\n\t\tsupersede the port/interface settings)\n");
	printf("\t-n (deactivates the discovery beacon)\n");
	printf("\t-f (allow CAN-FD frames in socket raw mode. Use only if your harware support it)\n");
	printf("\t-e <error_mask> (enable CAN error frames in raw mode providing an\n\t\thexadecimal error mask, e.g: 0x1FFFFFFF)\n");
	printf("\t-d (set this flag if you want log to syslog instead of STDOUT)\n");
	printf("\t-h (prints this message)\n");
}

void childdied(int signum)
{
	wait(NULL);
}

void sigint(int signum)
{
	if (verbose_flag)
		PRINT_ERROR("received SIGINT\n");

	if (sl != -1) {
		if (verbose_flag)
			PRINT_INFO("closing listening socket\n");
		if (!close(sl))
			sl = -1;
	}

	if (client_socket != -1) {
		if (verbose_flag)
			PRINT_INFO("closing client socket\n");
		if (!close(client_socket))
			client_socket = -1;
	}

	closelog();

	exit(0);
}
