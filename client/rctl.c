/*
 * ============================================================================
 *
 *       Filename:  rctl.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  2014年09月28日 11时02分55秒
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  jianxi sun (jianxi), ycsunjane@gmail.com
 *   Organization:  
 *
 * ============================================================================
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/file.h>
/* netdevice */
#include <sys/ioctl.h>
#include <net/if.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <netinet/tcp.h>
#include <netdb.h>

#include "common.h"
#include "rctl.h"
#include "ssltcp.h"
#include "log.h"
#include "config.h"
#include "exchange.h"

static char cmd[CMDLEN];
static char buf[BUFLEN];
static struct sockaddr_in seraddr;

static in_addr_t r_server(int num)
{
	struct hostent *host;
	host = gethostbyname(serverip[num]);
	if(host != NULL && host->h_length > 0) {
		sys_debug("Try connect: %s\n", serverip[num]);
		return *(in_addr_t *)host->h_addr_list[0];
	}
	fprintf(stderr, "Can not get: %s\n", serverip[num]);
	return 0;
}

static int tcp_alive(int sock)
{
	int optval = 1;
	int optlen = sizeof(optval);
	if(setsockopt(sock, SOL_SOCKET, 
			SO_KEEPALIVE, &optval, optlen) < -1) {
		sys_err("Set tcp keepalive failed: %s\n",
			strerror(errno));
		return 0;
	}

	optval = 5;
	if(setsockopt(sock, SOL_TCP,
			TCP_KEEPCNT, &optval, optlen) < -1) {
		sys_err("Set tcp_keepalive_probes failed: %s\n",
			strerror(errno));
		return 0;
	}

	optval = 30;
	if(setsockopt(sock, SOL_TCP, 
			TCP_KEEPIDLE, &optval, optlen) < -1) {
		sys_err("Set tcp_keepalive_time failed: %s\n",
			strerror(errno));
		return 0;
	}

	optval = 30;
	if(setsockopt(sock, SOL_TCP, 
			TCP_KEEPINTVL, &optval, optlen) < -1) {
		sys_err("Set tcp_keepalive_intvl failed: %s\n",
			strerror(errno));
		return 0;
	}

	return 1;
}

static int r_connect()
{
	static int count = 0;
	sys_debug("Trying %d ......\n", count++);
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0) {
		sys_err("Create socket failed: %s(%d)\n", 
			strerror(errno), errno);
		return -1;
	}

	int i, j;
	in_addr_t server;
	for(i = 0; i < TOTSER; i++) {
		socklen_t addr_len = sizeof(seraddr);
		seraddr.sin_family = AF_INET;
		server = r_server(i);
		if(!server) continue;
		seraddr.sin_addr.s_addr = server;

		for(j = 0; j < TOTPRT; j++) {
			seraddr.sin_port = htons(port[j]);
			sys_debug("Try %s port: %d\n", 
				inet_ntoa(seraddr.sin_addr), port[j]);

			if(!connect(fd, (void *)&seraddr, addr_len) && 
				tcp_alive(fd)) {
				sys_debug("connect success\n");
				return fd;
			}
			sys_debug("connect failed\n");
		}
	}
	close(fd);
	return -1;
}

static char *pty_init(int *pptm)
{
	int ptm = posix_openpt(O_RDWR);
	if(ptm < 0) {
		sys_err("posix_openpt failed: %s(%d\n)", 
			strerror(errno), errno);
		exit(-1);
	}

	if(grantpt(ptm) < 0) {
		sys_err("grantpt failed: %s(%d\n)", 
			strerror(errno), errno);
		exit(-1);
	}

	if(unlockpt(ptm) < 0){
		sys_err("unlockpt failed: %s(%d\n)", 
			strerror(errno), errno);
		exit(-1);
	}

	char *name = ptsname(ptm);
	if(!name) {
		sys_err("ptsname failed: %s(%d\n)", 
			strerror(errno), errno);
		exit(-1);
	}

	*pptm = ptm;

	return name;
}

int bashfd()
{
	int fd = Socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0)
		return -1;

	socklen_t addr_len = sizeof(seraddr);
	seraddr.sin_port = htons(BASHPORT);
	if(!connect(fd, (void *)&seraddr, addr_len) && 
		tcp_alive(fd)) {
		sys_debug("connect success\n");
		return fd;
	} else {
		sys_debug("connect failed\n");
		return -1;
	}
}

static int bashpid;
static void term(int signum)
{
	kill(bashpid, SIGKILL);
	exit(0);
}

static void init_signal()
{
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = term;
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL); 
	sigaction(SIGKILL, &act, NULL); 
	sigaction(SIGHUP, &act, NULL); 
}

static void killme()
{
	prctl(PR_SET_PDEATHSIG, SIGHUP);
	init_signal();
}

static void bashfrom()
{
	pid_t pid;
	if( (pid = fork()) < 0) {
		sys_err("Fork failed: %s(%d)\n", strerror(errno), errno);
		exit(-1);
	} else if(pid) {
		return;
	}

	/* child */
	int ptm;
	char *ptsname = pty_init(&ptm);
	int fd = bashfd();
	if(fd < 0) return;

	SSL *ssl;
	if( !(ssl = ssltcp_ssl(fd))) {
		sys_err("Create ssl failed\n");
		close(fd);
		return;
	}

	if( ssltcp_connect(ssl) < 0) {
		sys_err("connect ssl failed\n");
		close(fd);
		return;
	}

	if( (bashpid = fork()) < 0) {
		sys_err("Fork failed: %s(%d)\n", 
			strerror(errno), errno);
		exit(-1);
	} else if(bashpid == 0) {
		if(setsid() < 0) {
			sys_err("set new session id failed: %s(%d)\n",
				strerror(errno), errno);
			exit(-1);
		}
		/* child */
		int pts = open(ptsname, O_RDWR);
		if(pts < 0) {
			sys_err("open %s failed: %s(%d)\n", 
				ptsname, strerror(errno), errno);
			exit(-1);
		}

		dup2(pts, 0);
		dup2(pts, 1);
		dup2(pts, 2);
		if(execlp("sh", "sh", "-i", NULL) < 0) {
			sys_err("Execlp failed: %s(%d)\n", 
				strerror(errno), errno);
			exit(-1);
		}
	} else {
		killme();
		exchange(ptm, fd, ssl);
		exit(0);
	}
}

static void ssl_free(SSL *ssl)
{
	ssltcp_shutdown(ssl);
	ssltcp_free(ssl);
}

static void getmac(unsigned char *dmac, char *nic)
{
	int sock;
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock < 0) {
		sys_err("Create dllayer socket failed: %s\n", 
			strerror(errno));
		exit(-1);
	}

	struct ifreq req;
	strncpy(req.ifr_name, nic, IFNAMSIZ-1);
	if(ioctl(sock, SIOCGIFHWADDR, &req) < 0) {
		sys_err("get %s mac failed: %s\n", 
			nic, strerror(errno));
		exit(-1);
	}
	memcpy(dmac, req.ifr_hwaddr.sa_data, ETH_ALEN);
}

static void one_instance()
{
	int pid_file = open("/var/run/rctlcli.pid", 
		O_CREAT | O_RDWR, 0666);
	int rc = flock(pid_file, LOCK_EX | LOCK_NB);
	if(rc) {
		if(EWOULDBLOCK == errno) {
			sys_err("another rctlcli is running\n");
			exit(0);
		}
	}
}

static struct reg_t reg;
void rctl(char *devid, char *nic)
{
	one_instance();

	pid_t pid;
	if( (pid = fork()) < 0) {
		sys_err("Fork failed: %s(%d)\n", strerror(errno), errno);
		exit(-1);
	} else if(pid) {
		return;
	}

	/* child */
	ssltcp_init(0);

	int fd = 0;
reconnect:
	close(fd);
	sleep(1);
	fd = r_connect();
	if(fd == -1) goto reconnect;

	SSL *ssl = ssltcp_ssl(fd);
	if(!ssl) goto reconnect;

	if(ssltcp_connect(ssl) < 0) {
		ssltcp_free(ssl);
		goto reconnect;
	}

	int ret;
	strncpy(reg.class, devid, DEVID_LEN);
	getmac(reg.mac, nic);
	ret = ssltcp_write(ssl, (char *)&reg, sizeof(reg));
	if(ret <= 0) {
		ssl_free(ssl);
		goto reconnect;
	}

	while(1) {
		/* max command len should less than
		 * CMDLEN, so ret always complete */
		ret = ssltcp_read(ssl, cmd, CMDLEN);
		if(ret <= 0) {
			ssl_free(ssl);
			goto reconnect;
		}
		cmd[ret] = 0;

		if(!strcmp(cmd, RCTLBASH)) {
			bashfrom();
			continue;
		}

		strncat(cmd, " 2>&1", CMDLEN - strlen(cmd));
		FILE *fp; int size;
		fp = popen(cmd, "r"); 
		if(!fp) {
			sprintf(buf, "exec fail: %s\n", cmd);
			ret = ssltcp_write(ssl, buf, strlen(buf));
			if(ret <= 0) {
				ssl_free(ssl);
				goto reconnect;
			}
		} else {
			int isfirst = 1;
			do {
				size = fread(buf, 1, CMDLEN, fp);
				/* some command have no output */
				if(isfirst && size == 0) {
					sprintf(buf, "exec success: %s\n", cmd);
					ret = ssltcp_write(ssl, buf, strlen(buf));
					if(ret <= 0) {
						ssl_free(ssl);
						pclose(fp);
						goto reconnect;
					}
				}

				isfirst = 0;
				if(size > 0) {
					ret = ssltcp_write(ssl, buf, size);
					if(ret <= 0) {
						ssl_free(ssl);
						pclose(fp);
						goto reconnect;
					}
				}
			} while(size == BUFLEN);
			pclose(fp);
		}
	}
}
