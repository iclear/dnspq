#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "dnspq.h"

/* http://www.freesoft.org/CIE/RFC/1035/40.htm */

#define ID(buf)      (ntohs(*(uint16_t*)(buf)))
#define QR(buf)      ((buf[2] >> 7) & 0x01)
#define OPCODE(buf)  ((buf[2] >> 3) & 0x0F)
#define AA(buf)      ((buf[2] >> 2) & 0x01)
#define TC(buf)      ((buf[2] >> 1) & 0x01)
#define RD(buf)      ((buf[2]     ) & 0x01)
#define RA(buf)      ((buf[3] >> 7) & 0x01)
#define Z(buf)       ((buf[3] >> 4) & 0x07)
#define RCODE(buf)   ((buf[3]     ) & 0x0f)
#define QDCOUNT(buf) (ntohs(*(uint16_t*)(buf+4)))
#define ANCOUNT(buf) (ntohs(*(uint16_t*)(buf+6)))
#define NSCOUNT(buf) (ntohs(*(uint16_t*)(buf+8)))
#define ARCOUNT(buf) (ntohs(*(uint16_t*)(buf+10)))

#define SET_ID(buf, val)      (*(uint16_t*)(buf) = htons(val))
#define SET_QR(buf, val)      (buf[2] |= ((val & 0x01) << 7))
#define SET_OPCODE(buf, val)  (buf[2] |= ((val & 0x0f) << 3))
#define SET_AA(buf, val)      (buf[2] |= ((val & 0x01) << 2))
#define SET_TC(buf, val)      (buf[2] |= ((val & 0x01) << 1))
#define SET_RD(buf, val)      (buf[2] |= ((val & 0x01)     ))
#define SET_RA(buf, val)      (buf[3] |= ((val & 0x01) << 7))
#define SET_Z(buf, val)       (buf[3] |= ((val & 0x07) << 4))
#define SET_RCODE(buf, val)   (buf[3] |= ((val & 0x0f)     ))
#define SET_QDCOUNT(buf, val) (*(uint16_t*)(buf+4) = htons(val))
#define SET_ANCOUNT(buf, val) (*(uint16_t*)(buf+6) = htons(val))
#define SET_NSCOUNT(buf, val) (*(uint16_t*)(buf+8) = htons(val))
#define SET_ARCOUNT(buf, val) (*(uint16_t*)(buf+10) = htons(val))


#ifndef MAXSERVERS
# define MAXSERVERS  8
#endif
#ifndef RESOLV_CONF
# define RESOLV_CONF "/etc/resolv-dnspq.conf"
#endif
#ifndef MAX_RETRIES
# define MAX_RETRIES  1
#endif

static uint16_t cntr = 0;
static struct sockaddr_in *dnsservers[MAXSERVERS] = { 0, 0, 0, 0, 0, 0, 0, 0 };

int init(void) {
	FILE *resolvconf = NULL;
	char buf[512];
	char *p;
	int i;

	/* TODO: check some resolv.conf */
	if (dnsservers[0] != 0)
		return 0;

	if ((resolvconf = fopen(RESOLV_CONF, "r")) == NULL)
		return 1;
	for (i = 0; i < 24 && fgets(buf, sizeof(buf), resolvconf) != NULL; i++)
		if (
				buf[0] == 'n' &&
				buf[1] == 'a' &&
				buf[2] == 'm' &&
				buf[3] == 'e' &&
				buf[4] == 's' &&
				buf[5] == 'e' &&
				buf[6] == 'r' &&
				buf[7] == 'v' &&
				buf[8] == 'e' &&
				buf[9] == 'r')
		{
			if ((p = strchr(buf + 11, '\n')) != NULL)
				*p = '\0';
			adddnsserver(buf + 11);
		}
	fclose(resolvconf);
	return 0;
}

int adddnsserver(const char *server) {
	int i;
	struct sockaddr_in *dnsserver;

	/* find free slot */
	for (i = 0; i < MAXSERVERS && dnsservers[i] != NULL; i++)
		;
	if (i == MAXSERVERS)
		return 1;

	dnsserver = dnsservers[i] = malloc(sizeof(*dnsserver));
	if (inet_pton(AF_INET, server, &(dnsserver->sin_addr)) <= 0)
		return 2;
	dnsserver->sin_family = AF_INET;
	dnsserver->sin_port = htons(53);

	return 0;
}

#define timediff(X, Y) \
		(Y.tv_sec - X.tv_sec) * 1000 * 1000 + (Y.tv_usec - X.tv_usec)

int dnsq(const char *a, struct in_addr *ret, unsigned int *ttl, char *serverid) {
	unsigned char dnspkg[512];
	unsigned char *p = dnspkg;
	char *ap;
	unsigned char len;
	int saddr_buf_len;
	fd_set fds;
	int fd;
	struct timeval tv;
	struct timeval begin, end;
	int i;
	int nums = 0;
	uint16_t qid;
	char retries = MAX_RETRIES;
	suseconds_t maxtime = 500 * 1000;  /* 500ms, the max time we want to wait */
	char err = 0;

	if (++cntr == 0)  /* next sequence number, start at 1 (detect errs)  */
		cntr++;
	if (USHRT_MAX - MAXSERVERS < cntr)  /* avoid having to deal with overflow */
		cntr = 1;

	/* header */
	memset(p, 0, 4);  /* need zeros; macros below do or-ing due to bits */
	/* SET_ID is done per server */
	SET_QR(p, 0 /* query */);
	SET_OPCODE(p, 0 /* standard query */);
	SET_AA(p, 0);
	SET_TC(p, 0);
	SET_RD(p, 0);
	SET_RA(p, 0);
	SET_Z(p, 0);
	SET_RCODE(p, 0);
	SET_QDCOUNT(p, 1 /* one question */);
	SET_ANCOUNT(p, 0);
	SET_NSCOUNT(p, 0);
	SET_ARCOUNT(p, 0);
	p += 12;

	/* question section */
	while ((ap = strchr(a, '.')) != NULL) {
		len = (unsigned char)(ap - a);
		*p++ = len;
		memcpy(p, a, len);
		p += len;
		a = ap + 1;
	}
	len = (unsigned char)strlen(a);  /* truncation, hmm ... */
	*p++ = len;
	memcpy(p, a, len + 1);
	p += len + 1;  /* including the trailing null label */
	SET_ID(p, 1 /* QTYPE == A */);
	p += 2;
	SET_ID(p, 1 /* QCLASS == IN */);
	p += 2;

	/* answer sections not necessary */

	if ((fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
		return 1;
	len = (unsigned char)(p - dnspkg);  /* should always fit */
	p = dnspkg;

	tv.tv_sec = 0;
	tv.tv_usec = 0;  
	gettimeofday(&begin, NULL);
	do {
		for (i = 0; i < MAXSERVERS && dnsservers[i] != NULL; i++) {
			SET_ID(p, cntr + i);
			if (sendto(fd, dnspkg, len, 0,
						(struct sockaddr *)dnsservers[i],
						sizeof(*dnsservers[i])) != len)
				return 2;  /* TODO: fail only when all fail? */
		}

		/* wait for max 300ms */
		tv.tv_usec = maxtime > 300 * 1000 ? 300 * 1000 : maxtime;

		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
			if (retries-- > 0) {
				err = 1;
				continue;
			} else {
				break;
			}
		}

		/* got a response, see if it's sane */
		nums = i - 1;
		i = 0;
		do {
			err = 0;
			saddr_buf_len = recvfrom(fd, dnspkg, sizeof(dnspkg), 0, NULL, 0);
			if (saddr_buf_len == -1) {
				err = 4;
				continue;
			}

			qid = ID(p);
			if (qid < cntr || qid > cntr + nums) {
				err = 7; /* message not matching our request id */
				continue;
			}
			*serverid = qid - cntr;
			if (QR(p) != 1) {
				err = 8; /* not a response */
				continue;
			}
			if (OPCODE(p) != 0) {
				err = 9; /* not a standard query */
				continue;
			}
			switch (RCODE(p)) {
				case 0: /* no error */
					break;
				case 1:
				case 2:
				case 3:
				case 4:
				case 5:
					/* we likely did something wrong */
					err = 10;
					continue;
				default:
					err = 11;
					continue;
			}
			if (ANCOUNT(p) < 1) {
				err = 12; /* we only support non-empty answers */
				continue;
			}

			/* skip header + request */
			p += len;

			if ((*p | 3 << 6) == 3 << 6) {
				/* compression pointer, skip two octets */
				p += 2;
			} else {
				/* read labels */
				while (*p != 0)
					p += 1 + *p;
				p++;
			}
			if (ID(p) != 1 /* QTYPE == A */) {
				err = 13;
				continue;
			}
			p += 2;
			if (ID(p) != 1 /* QCLASS == IN */) {
				err = 14;
				continue;
			}
			p += 2;
			*ttl = ntohs(*(uint32_t*)p);
			p += 4;
			len = ID(p);
			p += 2;
			if (len != 4) {
				err = 15;
				continue;
			}
			memcpy(ret, p, 4);

			break;
		} while(gettimeofday(&end, NULL) == 0 &&
				(tv.tv_usec -= timediff(begin, end)) > 0 &&
				select(fd + 1, &fds, NULL, NULL, &tv) <= 0);
	} while(err != 0 && i++ <= nums &&
			gettimeofday(&end, NULL) == 0 &&
			(maxtime -= timediff(begin, end)) > 0);
	if (err != 0)
		return err;

	/* close, we don't need anything following after this point */
	close(fd);

	return 0;
}

int main(int argc, char *argv[]) {
	struct in_addr ip;
	unsigned int ttl;
	char serverid;
	int ret;
	if (init() != 0)
		return 1;
	if ((ret = dnsq(argv[1], &ip, &ttl, &serverid)) == 0) {
		printf("%s (%us/%d)\n", inet_ntoa(ip), ttl, serverid);
		return 0;
	}
	return ret;
}
