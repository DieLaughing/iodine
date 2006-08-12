/*
 * Copyright (c) 2006 Bjorn Andersson <flex@kryo.se>, Erik Ekman <yarrick@kryo.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#ifdef DARWIN
#include <arpa/nameser8_compat.h>
#endif
#include <netdb.h>
#include <time.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "read.h"
#include "structs.h"
#include "dns.h"
#include "encoding.h"


static int host2dns(const char *, char *, int);
static int dns_write(int, int, char *, int, char);

struct sockaddr_in peer;
char topdomain[256];

// Current IP packet
char activepacket[4096];
int lastlen;
int packetpos;
int packetlen;
uint16_t chunkid;

uint16_t pingid;


int 
open_dns(const char *domain, int localport, in_addr_t listen_ip) 
{
	int fd;
	int flag;
	struct sockaddr_in addr;

	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(localport);
	addr.sin_addr.s_addr = listen_ip; // This is already network byte order, inet_addr() or constant INADDR_ANY (==0)

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(fd < 0) {
		warn("socket");
		return -1;
	}

	flag = 1;
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
#endif
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

	if(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		warn("bind");
		return -1;
	}

	// Save top domain used
	strncpy(topdomain, domain, sizeof(topdomain) - 2);
	topdomain[sizeof(topdomain) - 1] = 0;

	printf("Opened UDP socket\n");
	return fd;
}

int 
dns_settarget(const char *host) 
{
	struct hostent *h;

	// Init dns target struct
	h = gethostbyname(host);
	if (!h) {
		printf("Could not resolve name %s, exiting\n", host);
		return -1;
	}

	bzero(&peer, sizeof(peer));
	peer.sin_family = AF_INET;
	peer.sin_port = htons(53);
	peer.sin_addr = *((struct in_addr *) h->h_addr);

	// Init chunk id
	chunkid = 0;
	pingid = 0;

	return 0;
}

void
close_dns(int fd)
{
	close(fd);
}

int
dns_sending()
{
	return (packetlen != 0);
}

static void
dns_send_chunk(int fd)
{
	int avail;
	char *p;

	p = activepacket;
	p += packetpos;
	avail = packetlen - packetpos;
	lastlen = dns_write(fd, ++chunkid, p, avail, 0);
}

void
dns_handle_tun(int fd, char *data, int len)
{
	memcpy(activepacket, data, MIN(len, sizeof(activepacket)));
	lastlen = 0;
	packetpos = 0;
	packetlen = len;

	dns_send_chunk(fd);
}

void
dns_ping(int dns_fd)
{
	char data[2];
	if (dns_sending()) {
		lastlen = 0;
		packetpos = 0;
		packetlen = 0;
	}
	data[0] = (pingid & 0xFF00) >> 8;
	data[1] = (pingid & 0xFF);
	dns_write(dns_fd, ++pingid, data, 2, 'P');
}

void 
dns_handshake(int dns_fd)
{
	char data[2];
	data[0] = (pingid & 0xFF00) >> 8;
	data[1] = (pingid & 0xFF);
	dns_write(dns_fd, ++pingid, data, 2, 'H');
}

void 
dns_query(int fd, int id, char *host, int type)
{
	char *p;
	int len;
	int peerlen;
	char buf[1024];
	HEADER *header;
	
	len = 0;
	memset(buf, 0, sizeof(buf));

	header = (HEADER*)buf;	
	
	header->id = htons(id);
	header->qr = 0;
	header->opcode = 0;
	header->aa = 0;
	header->tc = 0;
	header->rd = 1;
	header->ra = 0;

	header->qdcount = htons(1);
	header->arcount = htons(1);

	p = buf + sizeof(HEADER);
	p += host2dns(host, p, strlen(host));	

	PUTSHORT(type, p);
	PUTSHORT(C_IN, p);

	// EDNS0
	*p++ = 0x00; //Root
	PUTSHORT(0x0029, p); // OPT
	PUTSHORT(0x1000, p); // Payload size: 4096
	PUTSHORT(0x0000, p); // Higher bits/ edns version
	PUTSHORT(0x8000, p); // Z
	PUTSHORT(0x0000, p); // Data length

	peerlen = sizeof(peer);

	len = p - buf;
	sendto(fd, buf, len, 0, (struct sockaddr*)&peer, peerlen);
}

static int
dns_write(int fd, int id, char *buf, int len, char flag)
{
	int avail;
	int written;
	int encoded;
	char data[257];
	char *d;

	avail = 0xFF - strlen(topdomain) - 2;
	bzero(data, sizeof(data));
	d = data;
	written = encode_data(buf, len, avail, d, flag);
	encoded = strlen(data);
	d += encoded;
	if (*d != '.') {
		*d++ = '.';
	}
	strncpy(d, topdomain, strlen(topdomain)+1);
	
	dns_query(fd, id, data, T_NULL);
	return written;
}

int
dns_read(int fd, char *buf, int buflen)
{
	int r;
	long ttl;
	short rlen;
	short type;
	short class;
	short qdcount;
	short ancount;
	char *data;
	char name[255];
	char rdata[4*1024];
	HEADER *header;
	socklen_t addrlen;
	char packet[64*1024];
	struct sockaddr_in from;

	addrlen = sizeof(struct sockaddr);
	r = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr*)&from, &addrlen);

	if(r == -1) {
		perror("recvfrom");
	} else {
		header = (HEADER*)packet;
		
		data = packet + sizeof(HEADER);

		if(header->qr) { /* qr=1 => response */
			qdcount = ntohs(header->qdcount);
			ancount = ntohs(header->ancount);

			rlen = 0;

			if(qdcount == 1) {
				READNAME(packet, name, data);
				READSHORT(type, data);
				READSHORT(class, data);
			}
			if(ancount == 1) {
				READNAME(packet, name, data);
				READSHORT(type, data);
				READSHORT(class, data);
				READLONG(ttl, data);
				READSHORT(rlen, data);
				READDATA(rdata, data, rlen);
			}
			if (dns_sending() && chunkid == ntohs(header->id)) {
				// Got ACK on sent packet
				packetpos += lastlen;
				if (packetpos == packetlen) {
					// Packet completed
					packetpos = 0;
					packetlen = 0;
					lastlen = 0;
				} else {
					// More to send
					dns_send_chunk(fd);
				}
			}

			if(type == T_NULL && rlen > 2) {
				memcpy(buf, rdata, rlen);
				return rlen;
			} else {
				return 0;
			}
		}
	}

	return 0;
}

static int
host2dns(const char *host, char *buffer, int size)
{
	char *h;
	char *p;
	char *word;

	h = strdup(host);
	memset(buffer, 0, size);
	p = buffer;
	
	word = strtok(h, ".");
	while(word) {
		*p++ = (char)strlen(word);
		memcpy(p, word, strlen(word));
		p += strlen(word);

		word = strtok(NULL, ".");
	}

	*p++ = 0;

	free(h);

	return p - buffer;
}

void
dnsd_send(int fd, struct query *q, char *data, int datalen)
{
	int len;
	char *p;
	char buf[64*1024];
	short name;
	HEADER *header;

	memset(buf, 0, sizeof(buf));

	len = 0;
	header = (HEADER*)buf;	

	header->id = htons(q->id);
	header->qr = 1;
	header->opcode = 0;
	header->aa = 1;
	header->tc = 0;
	header->rd = 0;
	header->ra = 0;

	header->qdcount = htons(1);
	header->ancount = htons(1);

	p = buf + sizeof(HEADER);
	
	name = 0xc000 | ((p - buf) & 0x3fff);
	p += host2dns(q->name, p, strlen(q->name));
	PUTSHORT(q->type, p);
	PUTSHORT(C_IN, p);
	
	PUTSHORT(name, p);
	PUTSHORT(q->type, p);
	PUTSHORT(C_IN, p);
	PUTLONG(0, p);

	q->id = 0;

	PUTSHORT(datalen, p);
	memcpy(p, data, datalen);
	p += datalen;

	len = p - buf;
	sendto(fd, buf, len, 0, (struct sockaddr*)&q->from, q->fromlen);
}

static int
decodepacket(const char *name, char *buf, int buflen)
{
	int len;
	char *domain;

	domain = strstr(name, topdomain);

	len = decode_data(buf, buflen, name, domain);
	if (len == buflen)
		return -1; 
	return len;
}

int
dnsd_read(int fd, struct query *q, char *buf, int buflen)
{
	int r;
	short id;
	short type;
	short class;
	short qdcount;
	char *data;
	char name[255];
	HEADER *header;
	socklen_t addrlen;
	char packet[64*1024];
	struct sockaddr_in from;

	addrlen = sizeof(struct sockaddr);
	r = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr*)&from, &addrlen);

	if (r >= sizeof(HEADER)) {
		header = (HEADER*)packet;

		id = ntohs(header->id);

		data = packet + sizeof(HEADER);

		if(!header->qr) {
			qdcount = ntohs(header->qdcount);

			if(qdcount == 1) {
				bzero(name, sizeof(name));
				READNAME(packet, name, data);
				READSHORT(type, data);
				READSHORT(class, data);

				strncpy(q->name, name, 256);
				q->type = type;
				q->id = id;
				q->fromlen = addrlen;
				memcpy((struct sockaddr*)&q->from, (struct sockaddr*)&from, addrlen);

				return decodepacket(name, buf, buflen);
			}
		}
	} else {
		perror("recvfrom");
	}

	return 0;
}
