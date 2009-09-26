/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2009 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* $Core */

/*
dns.cpp - Nonblocking DNS functions.
Very very loosely based on the firedns library,
Copyright (C) 2002 Ian Gulliver. This file is no
longer anything like firedns, there are many major
differences between this code and the original.
Please do not assume that firedns works like this,
looks like this, walks like this or tastes like this.
*/

#ifndef WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#include "inspircd_win32wrapper.h"
#include "inspircd_se_config.h"
#endif

#include "inspircd.h"
#include "socketengine.h"
#include "configreader.h"
#include "socket.h"

/** Masks to mask off the responses we get from the DNSRequest methods
 */
enum QueryInfo
{
	ERROR_MASK	= 0x10000	/* Result is an error */
};

/** Flags which can be ORed into a request or reply for different meanings
 */
enum QueryFlags
{
	FLAGS_MASK_RD		= 0x01,	/* Recursive */
	FLAGS_MASK_TC		= 0x02,
	FLAGS_MASK_AA		= 0x04,	/* Authoritative */
	FLAGS_MASK_OPCODE	= 0x78,
	FLAGS_MASK_QR		= 0x80,
	FLAGS_MASK_RCODE	= 0x0F,	/* Request */
	FLAGS_MASK_Z		= 0x70,
	FLAGS_MASK_RA 		= 0x80
};


/** Represents a dns resource record (rr)
 */
struct ResourceRecord
{
	QueryType	type;		/* Record type */
	unsigned int	rr_class;	/* Record class */
	unsigned long	ttl;		/* Time to live */
	unsigned int	rdlength;	/* Record length */
};

/** Represents a dns request/reply header, and its payload as opaque data.
 */
class DNSHeader
{
 public:
	unsigned char	id[2];		/* Request id */
	unsigned int	flags1;		/* Flags */
	unsigned int	flags2;		/* Flags */
	unsigned int	qdcount;
	unsigned int	ancount;	/* Answer count */
	unsigned int	nscount;	/* Nameserver count */
	unsigned int	arcount;
	unsigned char	payload[512];	/* Packet payload */
};

class DNSRequest
{
 public:
	unsigned char   id[2];		/* Request id */
	unsigned char*  res;		/* Result processing buffer */
	unsigned int    rr_class;       /* Request class */
	QueryType       type;		/* Request type */
	DNS*            dnsobj;		/* DNS caller (where we get our FD from) */
	unsigned long	ttl;		/* Time to live */
	std::string     orig;		/* Original requested name/ip */
	InspIRCd*	ServerInstance;

	DNSRequest(InspIRCd* Instance, DNS* dns, int id, const std::string &original);
	~DNSRequest();
	DNSInfo ResultIsReady(DNSHeader &h, int length);
	int SendRequests(const DNSHeader *header, const int length, QueryType qt);
};

class CacheTimer : public Timer
{
 private:
	InspIRCd* ServerInstance;
	DNS* dns;
 public:
	CacheTimer(InspIRCd* Instance, DNS* thisdns)
		: Timer(3600, Instance->Time(), true), ServerInstance(Instance), dns(thisdns) { }

	virtual void Tick(time_t)
	{
		dns->PruneCache();
	}
};

class RequestTimeout : public Timer
{
	InspIRCd* ServerInstance;
	DNSRequest* watch;
	int watchid;
 public:
	RequestTimeout(unsigned long n, InspIRCd* SI, DNSRequest* watching, int id) : Timer(n, SI->Time()), ServerInstance(SI), watch(watching), watchid(id)
	{
	}
	~RequestTimeout()
	{
		if (ServerInstance->Res)
			Tick(0);
	}

	void Tick(time_t)
	{
		if (ServerInstance->Res->requests[watchid] == watch)
		{
			/* Still exists, whack it */
			if (ServerInstance->Res->Classes[watchid])
			{
				ServerInstance->Res->Classes[watchid]->OnError(RESOLVER_TIMEOUT, "Request timed out");
				delete ServerInstance->Res->Classes[watchid];
				ServerInstance->Res->Classes[watchid] = NULL;
			}
			ServerInstance->Res->requests[watchid] = NULL;
			delete watch;
		}
	}
};

/* Allocate the processing buffer */
DNSRequest::DNSRequest(InspIRCd* Instance, DNS* dns, int rid, const std::string &original) : dnsobj(dns), ServerInstance(Instance)
{
	res = new unsigned char[512];
	*res = 0;
	orig = original;
	RequestTimeout* RT = new RequestTimeout(Instance->Config->dns_timeout ? Instance->Config->dns_timeout : 5, Instance, this, rid);
	Instance->Timers->AddTimer(RT); /* The timer manager frees this */
}

/* Deallocate the processing buffer */
DNSRequest::~DNSRequest()
{
	delete[] res;
}

/** Fill a ResourceRecord class based on raw data input */
inline void DNS::FillResourceRecord(ResourceRecord* rr, const unsigned char *input)
{
	rr->type = (QueryType)((input[0] << 8) + input[1]);
	rr->rr_class = (input[2] << 8) + input[3];
	rr->ttl = (input[4] << 24) + (input[5] << 16) + (input[6] << 8) + input[7];
	rr->rdlength = (input[8] << 8) + input[9];
}

/** Fill a DNSHeader class based on raw data input of a given length */
inline void DNS::FillHeader(DNSHeader *header, const unsigned char *input, const int length)
{
	header->id[0] = input[0];
	header->id[1] = input[1];
	header->flags1 = input[2];
	header->flags2 = input[3];
	header->qdcount = (input[4] << 8) + input[5];
	header->ancount = (input[6] << 8) + input[7];
	header->nscount = (input[8] << 8) + input[9];
	header->arcount = (input[10] << 8) + input[11];
	memcpy(header->payload,&input[12],length);
}

/** Empty a DNSHeader class out into raw data, ready for transmission */
inline void DNS::EmptyHeader(unsigned char *output, const DNSHeader *header, const int length)
{
	output[0] = header->id[0];
	output[1] = header->id[1];
	output[2] = header->flags1;
	output[3] = header->flags2;
	output[4] = header->qdcount >> 8;
	output[5] = header->qdcount & 0xFF;
	output[6] = header->ancount >> 8;
	output[7] = header->ancount & 0xFF;
	output[8] = header->nscount >> 8;
	output[9] = header->nscount & 0xFF;
	output[10] = header->arcount >> 8;
	output[11] = header->arcount & 0xFF;
	memcpy(&output[12],header->payload,length);
}

/** Send requests we have previously built down the UDP socket */
int DNSRequest::SendRequests(const DNSHeader *header, const int length, QueryType qt)
{
	ServerInstance->Logs->Log("RESOLVER", DEBUG,"DNSRequest::SendRequests");

	unsigned char payload[sizeof(DNSHeader)];

	this->rr_class = 1;
	this->type = qt;

	DNS::EmptyHeader(payload,header,length);

	if (this->dnsobj->socketfamily == AF_INET6)
	{
		sockaddr_in6 addr;
		memset(&addr,0,sizeof(addr));
		memcpy(&addr.sin6_addr,&dnsobj->myserver6,sizeof(addr.sin6_addr));
		addr.sin6_family = AF_INET6;
		addr.sin6_port = htons(DNS::QUERY_PORT);
		if (ServerInstance->SE->SendTo(dnsobj, payload, length + 12, 0, (sockaddr *) &addr, sizeof(addr)) != length+12)
			return -1;
	}
	else
	{
		sockaddr_in addr;
		memset(&addr,0,sizeof(addr));
		memcpy(&addr.sin_addr.s_addr,&dnsobj->myserver4,sizeof(addr.sin_addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(DNS::QUERY_PORT);
		if (ServerInstance->SE->SendTo(dnsobj, (const char*)payload, length + 12, 0, (sockaddr *) &addr, sizeof(addr)) != length+12)
			return -1;
	}

	ServerInstance->Logs->Log("RESOLVER",DEBUG,"Sent OK");
	return 0;
}

/** Add a query with a predefined header, and allocate an ID for it. */
DNSRequest* DNS::AddQuery(DNSHeader *header, int &id, const char* original)
{
	/* Is the DNS connection down? */
	if (this->GetFd() == -1)
		return NULL;

	/* Create an id */
	id = this->PRNG() & DNS::MAX_REQUEST_ID;

	/* If this id is already 'in flight', pick another. */
	while (requests[id])
		id = this->PRNG() & DNS::MAX_REQUEST_ID;

	DNSRequest* req = new DNSRequest(ServerInstance, this, id, original);

	header->id[0] = req->id[0] = id >> 8;
	header->id[1] = req->id[1] = id & 0xFF;
	header->flags1 = FLAGS_MASK_RD;
	header->flags2 = 0;
	header->qdcount = 1;
	header->ancount = 0;
	header->nscount = 0;
	header->arcount = 0;

	/* At this point we already know the id doesnt exist,
	 * so there needs to be no second check for the ::end()
	 */
	requests[id] = req;

	/* According to the C++ spec, new never returns NULL. */
	return req;
}

int DNS::ClearCache()
{
	/* This ensures the buckets are reset to sane levels */
	int rv = this->cache->size();
	delete this->cache;
	this->cache = new dnscache();
	return rv;
}

int DNS::PruneCache()
{
	int n = 0;
	dnscache* newcache = new dnscache();
	for (dnscache::iterator i = this->cache->begin(); i != this->cache->end(); i++)
		/* Dont include expired items (theres no point) */
		if (i->second.CalcTTLRemaining())
			newcache->insert(*i);
		else
			n++;

	delete this->cache;
	this->cache = newcache;
	return n;
}

void DNS::Rehash()
{
	ip6munge = false;
	int portpass = 0;

	if (this->GetFd() > -1)
	{
		if (ServerInstance && ServerInstance->SE)
			ServerInstance->SE->DelFd(this);
		ServerInstance->SE->Shutdown(this, 2);
		ServerInstance->SE->Close(this);
		this->SetFd(-1);

		/* Rehash the cache */
		this->PruneCache();
	}
	else
	{
		/* Create initial dns cache */
		this->cache = new dnscache();
	}

	if ((strstr(ServerInstance->Config->DNSServer,"::ffff:") == (char*)&ServerInstance->Config->DNSServer) ||  (strstr(ServerInstance->Config->DNSServer,"::FFFF:") == (char*)&ServerInstance->Config->DNSServer))
	{
		ServerInstance->Logs->Log("RESOLVER",DEFAULT,"WARNING: Using IPv4 addresses over IPv6 forces some DNS checks to be disabled.");
		ServerInstance->Logs->Log("RESOLVER",DEFAULT,"         This should not cause a problem, however it is recommended you migrate");
		ServerInstance->Logs->Log("RESOLVER",DEFAULT,"         to a true IPv6 environment.");
		this->ip6munge = true;
	}

	this->socketfamily = AF_INET;
	if (strchr(ServerInstance->Config->DNSServer,':'))
	{
		this->socketfamily = AF_INET6;
		inet_pton(AF_INET6, ServerInstance->Config->DNSServer, &this->myserver6);
	}
	else
	{
		inet_aton(ServerInstance->Config->DNSServer, &this->myserver4);
		portpass = -1;
	}

	/* Initialize mastersocket */
	int s = irc::sockets::OpenTCPSocket(ServerInstance->Config->DNSServer, SOCK_DGRAM);
	this->SetFd(s);
	ServerInstance->SE->NonBlocking(this->GetFd());

	/* Have we got a socket and is it nonblocking? */
	if (this->GetFd() != -1)
	{
		/* Bind the port - port 0 INADDR_ANY */
		if (!ServerInstance->BindSocket(this->GetFd(), portpass, "", false))
		{
			/* Failed to bind */
			ServerInstance->Logs->Log("RESOLVER",DEBUG,"Error binding dns socket");
			ServerInstance->SE->Shutdown(this, 2);
			ServerInstance->SE->Close(this);
			this->SetFd(-1);
		}

		if (this->GetFd() >= 0)
		{
			/* Hook the descriptor into the socket engine */
			if (ServerInstance && ServerInstance->SE)
			{
				if (!ServerInstance->SE->AddFd(this, FD_WANT_POLL_READ | FD_WANT_NO_WRITE))
				{
					ServerInstance->Logs->Log("RESOLVER",DEFAULT,"Internal error starting DNS - hostnames will NOT resolve.");
					ServerInstance->SE->Shutdown(this, 2);
					ServerInstance->SE->Close(this);
					this->SetFd(-1);
				}
			}
		}
	}
	else
	{
		ServerInstance->Logs->Log("RESOLVER",DEBUG,"Error creating dns socket");
	}
}

/** Initialise the DNS UDP socket so that we can send requests */
DNS::DNS(InspIRCd* Instance) : ServerInstance(Instance)
{
	ServerInstance->Logs->Log("RESOLVER",DEBUG,"DNS::DNS");
	/* Clear the Resolver class table */
	memset(Classes,0,sizeof(Classes));

	/* Clear the requests class table */
	memset(requests,0,sizeof(requests));

	/* Set the id of the next request to 0
	 */
	currid = 0;

	/* DNS::Rehash() sets this to a valid ptr
	 */
	this->cache = NULL;

	/* Again, DNS::Rehash() sets this to a
	 * valid value
	 */
	this->SetFd(-1);

	/* Actually read the settings
	 */
	this->Rehash();

	this->PruneTimer = new CacheTimer(ServerInstance, this);

	ServerInstance->Timers->AddTimer(this->PruneTimer);
}

/** Build a payload to be placed after the header, based upon input data, a resource type, a class and a pointer to a buffer */
int DNS::MakePayload(const char * const name, const QueryType rr, const unsigned short rr_class, unsigned char * const payload)
{
	short payloadpos = 0;
	const char* tempchr, *tempchr2 = name;
	unsigned short length;

	/* split name up into labels, create query */
	while ((tempchr = strchr(tempchr2,'.')) != NULL)
	{
		length = tempchr - tempchr2;
		if (payloadpos + length + 1 > 507)
			return -1;
		payload[payloadpos++] = length;
		memcpy(&payload[payloadpos],tempchr2,length);
		payloadpos += length;
		tempchr2 = &tempchr[1];
	}
	length = strlen(tempchr2);
	if (length)
	{
		if (payloadpos + length + 2 > 507)
			return -1;
		payload[payloadpos++] = length;
		memcpy(&payload[payloadpos],tempchr2,length);
		payloadpos += length;
		payload[payloadpos++] = 0;
	}
	if (payloadpos > 508)
		return -1;
	length = htons(rr);
	memcpy(&payload[payloadpos],&length,2);
	length = htons(rr_class);
	memcpy(&payload[payloadpos + 2],&length,2);
	return payloadpos + 4;
}

/** Start lookup of an hostname to an IP address */
int DNS::GetIP(const char *name)
{
	DNSHeader h;
	int id;
	int length;

	if ((length = this->MakePayload(name, DNS_QUERY_A, 1, (unsigned char*)&h.payload)) == -1)
		return -1;

	DNSRequest* req = this->AddQuery(&h, id, name);

	if ((!req) || (req->SendRequests(&h, length, DNS_QUERY_A) == -1))
		return -1;

	return id;
}

/** Start lookup of an hostname to an IPv6 address */
int DNS::GetIP6(const char *name)
{
	DNSHeader h;
	int id;
	int length;

	if ((length = this->MakePayload(name, DNS_QUERY_AAAA, 1, (unsigned char*)&h.payload)) == -1)
		return -1;

	DNSRequest* req = this->AddQuery(&h, id, name);

	if ((!req) || (req->SendRequests(&h, length, DNS_QUERY_AAAA) == -1))
		return -1;

	return id;
}

/** Start lookup of a cname to another name */
int DNS::GetCName(const char *alias)
{
	DNSHeader h;
	int id;
	int length;

	if ((length = this->MakePayload(alias, DNS_QUERY_CNAME, 1, (unsigned char*)&h.payload)) == -1)
		return -1;

	DNSRequest* req = this->AddQuery(&h, id, alias);

	if ((!req) || (req->SendRequests(&h, length, DNS_QUERY_CNAME) == -1))
		return -1;

	return id;
}

/** Start lookup of an IP address to a hostname */
int DNS::GetNameForce(const char *ip, ForceProtocol fp)
{
	char query[128];
	DNSHeader h;
	int id;
	int length;

	if (fp == PROTOCOL_IPV6)
	{
		in6_addr i;
		if (inet_pton(AF_INET6, ip, &i) > 0)
		{
			DNS::MakeIP6Int(query, &i);
		}
		else
			/* Invalid IP address */
			return -1;
	}
	else
	{
		in_addr i;
		if (inet_aton(ip, &i))
		{
			unsigned char* c = (unsigned char*)&i.s_addr;
			sprintf(query,"%d.%d.%d.%d.in-addr.arpa",c[3],c[2],c[1],c[0]);
		}
		else
			/* Invalid IP address */
			return -1;
	}

	if ((length = this->MakePayload(query, DNS_QUERY_PTR, 1, (unsigned char*)&h.payload)) == -1)
		return -1;

	DNSRequest* req = this->AddQuery(&h, id, ip);

	if ((!req) || (req->SendRequests(&h, length, DNS_QUERY_PTR) == -1))
		return -1;

	return id;
}

/** Build an ipv6 reverse domain from an in6_addr
 */
void DNS::MakeIP6Int(char* query, const in6_addr *ip)
{
	const char* hex = "0123456789abcdef";
	for (int index = 31; index >= 0; index--) /* for() loop steps twice per byte */
	{
		if (index % 2)
			/* low nibble */
			*query++ = hex[ip->s6_addr[index / 2] & 0x0F];
		else
			/* high nibble */
			*query++ = hex[(ip->s6_addr[index / 2] & 0xF0) >> 4];
		*query++ = '.'; /* Seperator */
	}
	strcpy(query,"ip6.arpa"); /* Suffix the string */
}

/** Return the next id which is ready, and the result attached to it */
DNSResult DNS::GetResult()
{
	/* Fetch dns query response and decide where it belongs */
	DNSHeader header;
	DNSRequest *req;
	unsigned char buffer[sizeof(DNSHeader)];
	irc::sockets::sockaddrs from;
	socklen_t x = this->socketfamily == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
	const char* ipaddr_from;
	unsigned short int port_from = 0;

	int length = ServerInstance->SE->RecvFrom(this, (char*)buffer, sizeof(DNSHeader), 0, &from.sa, &x);

	/* Did we get the whole header? */
	if (length < 12)
	{
		/* Nope - something screwed up. */
		return DNSResult(-1,"",0,"");
	}

	/* Check wether the reply came from a different DNS
	 * server to the one we sent it to, or the source-port
	 * is not 53.
	 * A user could in theory still spoof dns packets anyway
	 * but this is less trivial than just sending garbage
	 * to the client, which is possible without this check.
	 *
	 * -- Thanks jilles for pointing this one out.
	 */
	char nbuf[MAXBUF];
	if (this->socketfamily == AF_INET6)
	{
		ipaddr_from = inet_ntop(AF_INET6, &from.in6.sin6_addr, nbuf, sizeof(nbuf));
		port_from = ntohs(from.in6.sin6_port);
	}
	else
	{
		ipaddr_from = inet_ntoa(from.in4.sin_addr);
		port_from = ntohs(from.in4.sin_port);
	}

	/* We cant perform this security check if you're using 4in6.
	 * Tough luck to you, choose one or't other!
	 */
	if (!ip6munge)
	{
		if ((port_from != DNS::QUERY_PORT) || (strcasecmp(ipaddr_from, ServerInstance->Config->DNSServer)))
		{
			return DNSResult(-1,"",0,"");
		}
	}

	/* Put the read header info into a header class */
	DNS::FillHeader(&header,buffer,length - 12);

	/* Get the id of this request.
	 * Its a 16 bit value stored in two char's,
	 * so we use logic shifts to create the value.
	 */
	unsigned long this_id = header.id[1] + (header.id[0] << 8);

	/* Do we have a pending request matching this id? */
	if (!requests[this_id])
	{
		/* Somehow we got a DNS response for a request we never made... */
		return DNSResult(-1,"",0,"");
	}
	else
	{
		/* Remove the query from the list of pending queries */
		req = requests[this_id];
		requests[this_id] = NULL;
	}

	/* Inform the DNSRequest class that it has a result to be read.
	 * When its finished it will return a DNSInfo which is a pair of
	 * unsigned char* resource record data, and an error message.
	 */
	DNSInfo data = req->ResultIsReady(header, length);
	std::string resultstr;

	/* Check if we got a result, if we didnt, its an error */
	if (data.first == NULL)
	{
		/* An error.
		 * Mask the ID with the value of ERROR_MASK, so that
		 * the dns_deal_with_classes() function knows that its
		 * an error response and needs to be treated uniquely.
		 * Put the error message in the second field.
		 */
		std::string ro = req->orig;
		delete req;
		return DNSResult(this_id | ERROR_MASK, data.second, 0, ro);
	}
	else
	{
		unsigned long ttl = req->ttl;
		char formatted[128];

		/* Forward lookups come back as binary data. We must format them into ascii */
		switch (req->type)
		{
			case DNS_QUERY_A:
				snprintf(formatted,16,"%u.%u.%u.%u",data.first[0],data.first[1],data.first[2],data.first[3]);
				resultstr = formatted;
			break;

			case DNS_QUERY_AAAA:
			{
				inet_ntop(AF_INET6, data.first, formatted, sizeof(formatted));
				char* c = strstr(formatted,":0:");
				if (c != NULL)
				{
					memmove(c+1,c+2,strlen(c+2) + 1);
					c += 2;
					while (memcmp(c,"0:",2) == 0)
						memmove(c,c+2,strlen(c+2) + 1);
					if (memcmp(c,"0",2) == 0)
						*c = 0;
					if (memcmp(formatted,"0::",3) == 0)
						memmove(formatted,formatted + 1, strlen(formatted + 1) + 1);
				}
				resultstr = formatted;

				/* Special case. Sending ::1 around between servers
				 * and to clients is dangerous, because the : on the
				 * start makes the client or server interpret the IP
				 * as the last parameter on the line with a value ":1".
				 */
				if (*formatted == ':')
					resultstr.insert(0, "0");
			}
			break;

			case DNS_QUERY_CNAME:
				/* Identical handling to PTR */

			case DNS_QUERY_PTR:
				/* Reverse lookups just come back as char* */
				resultstr = std::string((const char*)data.first);
			break;

			default:
			break;
		}

		/* Build the reply with the id and hostname/ip in it */
		std::string ro = req->orig;
		delete req;
		return DNSResult(this_id,resultstr,ttl,ro);
	}
}

/** A result is ready, process it */
DNSInfo DNSRequest::ResultIsReady(DNSHeader &header, int length)
{
	int i = 0;
	int q = 0;
	int curanswer, o;
	ResourceRecord rr;
 	unsigned short ptr;

	/* This is just to keep _FORTIFY_SOURCE happy */
	rr.type = DNS_QUERY_NONE;
	rr.rdlength = 0;
	rr.ttl = 1;	/* GCC is a whiney bastard -- see the XXX below. */
	rr.rr_class = 0; /* Same for VC++ */

	if (!(header.flags1 & FLAGS_MASK_QR))
		return std::make_pair((unsigned char*)NULL,"Not a query result");

	if (header.flags1 & FLAGS_MASK_OPCODE)
		return std::make_pair((unsigned char*)NULL,"Unexpected value in DNS reply packet");

	if (header.flags2 & FLAGS_MASK_RCODE)
		return std::make_pair((unsigned char*)NULL,"Domain name not found");

	if (header.ancount < 1)
		return std::make_pair((unsigned char*)NULL,"No resource records returned");

	/* Subtract the length of the header from the length of the packet */
	length -= 12;

	while ((unsigned int)q < header.qdcount && i < length)
	{
		if (header.payload[i] > 63)
		{
			i += 6;
			q++;
		}
		else
		{
			if (header.payload[i] == 0)
			{
				q++;
				i += 5;
			}
			else i += header.payload[i] + 1;
		}
	}
	curanswer = 0;
	while ((unsigned)curanswer < header.ancount)
	{
		q = 0;
		while (q == 0 && i < length)
		{
			if (header.payload[i] > 63)
			{
				i += 2;
				q = 1;
			}
			else
			{
				if (header.payload[i] == 0)
				{
					i++;
					q = 1;
				}
				else i += header.payload[i] + 1; /* skip length and label */
			}
		}
		if (length - i < 10)
			return std::make_pair((unsigned char*)NULL,"Incorrectly sized DNS reply");

		/* XXX: We actually initialise 'rr' here including its ttl field */
		DNS::FillResourceRecord(&rr,&header.payload[i]);

		i += 10;
		ServerInstance->Logs->Log("RESOLVER",DEBUG,"Resolver: rr.type is %d and this.type is %d rr.class %d this.class %d", rr.type, this->type, rr.rr_class, this->rr_class);
		if (rr.type != this->type)
		{
			curanswer++;
			i += rr.rdlength;
			continue;
		}
		if (rr.rr_class != this->rr_class)
		{
			curanswer++;
			i += rr.rdlength;
			continue;
		}
		break;
	}
	if ((unsigned int)curanswer == header.ancount)
		return std::make_pair((unsigned char*)NULL,"No A, AAAA or PTR type answers (" + ConvToStr(header.ancount) + " answers)");

	if (i + rr.rdlength > (unsigned int)length)
		return std::make_pair((unsigned char*)NULL,"Resource record larger than stated");

	if (rr.rdlength > 1023)
		return std::make_pair((unsigned char*)NULL,"Resource record too large");

	this->ttl = rr.ttl;

	switch (rr.type)
	{
		case DNS_QUERY_CNAME:
			/* CNAME and PTR have the same processing code */
		case DNS_QUERY_PTR:
			o = 0;
			q = 0;
			while (q == 0 && i < length && o + 256 < 1023)
			{
				if (header.payload[i] > 63)
				{
					memcpy(&ptr,&header.payload[i],2);
					i = ntohs(ptr) - 0xC000 - 12;
				}
				else
				{
					if (header.payload[i] == 0)
					{
						q = 1;
					}
					else
					{
						res[o] = 0;
						if (o != 0)
							res[o++] = '.';
						memcpy(&res[o],&header.payload[i + 1],header.payload[i]);
						o += header.payload[i];
						i += header.payload[i] + 1;
					}
				}
			}
			res[o] = 0;
		break;
		case DNS_QUERY_AAAA:
			memcpy(res,&header.payload[i],rr.rdlength);
			res[rr.rdlength] = 0;
		break;
		case DNS_QUERY_A:
			memcpy(res,&header.payload[i],rr.rdlength);
			res[rr.rdlength] = 0;
		break;
		default:
			memcpy(res,&header.payload[i],rr.rdlength);
			res[rr.rdlength] = 0;
		break;
	}
	return std::make_pair(res,"No error");
}

/** Close the master socket */
DNS::~DNS()
{
	ServerInstance->SE->Shutdown(this, 2);
	ServerInstance->SE->Close(this);
	ServerInstance->Timers->DelTimer(this->PruneTimer);
	if (cache)
		delete cache;
}

CachedQuery* DNS::GetCache(const std::string &source)
{
	dnscache::iterator x = cache->find(source.c_str());
	if (x != cache->end())
		return &(x->second);
	else
		return NULL;
}

void DNS::DelCache(const std::string &source)
{
	cache->erase(source.c_str());
}

void Resolver::TriggerCachedResult()
{
	if (CQ)
		OnLookupComplete(CQ->data, time_left, true);
}

/** High level abstraction of dns used by application at large */
Resolver::Resolver(InspIRCd* Instance, const std::string &source, QueryType qt, bool &cached, Module* creator) : ServerInstance(Instance), Creator(creator), input(source), querytype(qt)
{
	ServerInstance->Logs->Log("RESOLVER",DEBUG,"Resolver::Resolver");
	cached = false;

	CQ = ServerInstance->Res->GetCache(source);
	if (CQ)
	{
		time_left = CQ->CalcTTLRemaining();
		if (!time_left)
		{
			ServerInstance->Res->DelCache(source);
		}
		else
		{
			cached = true;
			return;
		}
	}

	switch (querytype)
	{
		case DNS_QUERY_A:
			this->myid = ServerInstance->Res->GetIP(source.c_str());
		break;

		case DNS_QUERY_PTR4:
			querytype = DNS_QUERY_PTR;
			this->myid = ServerInstance->Res->GetNameForce(source.c_str(), PROTOCOL_IPV4);
		break;

		case DNS_QUERY_PTR6:
			querytype = DNS_QUERY_PTR;
			this->myid = ServerInstance->Res->GetNameForce(source.c_str(), PROTOCOL_IPV6);
		break;

		case DNS_QUERY_AAAA:
			this->myid = ServerInstance->Res->GetIP6(source.c_str());
		break;

		case DNS_QUERY_CNAME:
			this->myid = ServerInstance->Res->GetCName(source.c_str());
		break;

		default:
			this->myid = -1;
		break;
	}
	if (this->myid == -1)
	{
		this->OnError(RESOLVER_NSDOWN, "Nameserver is down");
		throw ModuleException("Resolver: Couldnt get an id to make a request");
		/* We shouldnt get here really */
		return;
	}
	else
	{
		ServerInstance->Logs->Log("RESOLVER",DEBUG,"DNS request id %d", this->myid);
	}
}

/** Called when an error occurs */
void Resolver::OnError(ResolverError, const std::string&)
{
	/* Nothing in here */
}

/** Destroy a resolver */
Resolver::~Resolver()
{
	/* Nothing here (yet) either */
}

/** Get the request id associated with this class */
int Resolver::GetId()
{
	return this->myid;
}

Module* Resolver::GetCreator()
{
	return this->Creator;
}

/** Process a socket read event */
void DNS::HandleEvent(EventType, int)
{
	/* Fetch the id and result of the next available packet */
	DNSResult res(0,"",0,"");
	res.id = 0;
	ServerInstance->Logs->Log("RESOLVER",DEBUG,"Handle DNS event");

	res = this->GetResult();

	ServerInstance->Logs->Log("RESOLVER",DEBUG,"Result id %d", res.id);

	/* Is there a usable request id? */
	if (res.id != -1)
	{
		/* Its an error reply */
		if (res.id & ERROR_MASK)
		{
			/* Mask off the error bit */
			res.id -= ERROR_MASK;
			/* Marshall the error to the correct class */
			if (Classes[res.id])
			{
				if (ServerInstance && ServerInstance->stats)
					ServerInstance->stats->statsDnsBad++;
				Classes[res.id]->OnError(RESOLVER_NXDOMAIN, res.result);
				delete Classes[res.id];
				Classes[res.id] = NULL;
			}
			return;
		}
		else
		{
			/* It is a non-error result, marshall the result to the correct class */
			if (Classes[res.id])
			{
				if (ServerInstance && ServerInstance->stats)
					ServerInstance->stats->statsDnsGood++;

				if (!this->GetCache(res.original.c_str()))
					this->cache->insert(std::make_pair(res.original.c_str(), CachedQuery(res.result, res.ttl)));

				Classes[res.id]->OnLookupComplete(res.result, res.ttl, false);
				delete Classes[res.id];
				Classes[res.id] = NULL;
			}
		}

		if (ServerInstance && ServerInstance->stats)
			ServerInstance->stats->statsDns++;
	}
}

/** Add a derived Resolver to the working set */
bool DNS::AddResolverClass(Resolver* r)
{
	ServerInstance->Logs->Log("RESOLVER",DEBUG,"AddResolverClass 0x%08lx", (unsigned long)r);
	/* Check the pointers validity and the id's validity */
	if ((r) && (r->GetId() > -1))
	{
		/* Check the slot isnt already occupied -
		 * This should NEVER happen unless we have
		 * a severely broken DNS server somewhere
		 */
		if (!Classes[r->GetId()])
		{
			/* Set up the pointer to the class */
			Classes[r->GetId()] = r;
			return true;
		}
		else
			/* Duplicate id */
			return false;
	}
	else
	{
		/* Pointer or id not valid.
		 * Free the item and return
		 */
		if (r)
			delete r;

		return false;
	}
}

void DNS::CleanResolvers(Module* module)
{
	for (int i = 0; i < MAX_REQUEST_ID; i++)
	{
		if (Classes[i])
		{
			if (Classes[i]->GetCreator() == module)
			{
				Classes[i]->OnError(RESOLVER_FORCEUNLOAD, "Parent module is unloading");
				delete Classes[i];
				Classes[i] = NULL;
			}
		}
	}
}

/** Generate pseudo-random number */
unsigned long DNS::PRNG()
{
	unsigned long val = 0;
	timeval n;
	serverstats* s = ServerInstance->stats;
	gettimeofday(&n,NULL);
	val = (n.tv_usec ^ (getpid() ^ geteuid()) ^ ((this->currid++)) ^ s->statsAccept) + n.tv_sec;
	val = val + (s->statsCollisions ^ s->statsDnsGood) - s->statsDnsBad;
	val += (s->statsConnects ^ (unsigned long)s->statsSent ^ (unsigned long)s->statsRecv) - ServerInstance->ports.size();
	return val;
}
