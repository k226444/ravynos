/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static const char copyright[] =
"@(#) Copyright (c) 1983, 1993\n\
	The Regents of the University of California.  All rights reserved.\n";
#if 0
static char sccsid[] = "@(#)ifconfig.c	8.2 (Berkeley) 2/16/94";
#endif
static const char rcsid[] =
  "$FreeBSD$";
#endif /* not lint */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/module.h>
#include <sys/linker.h>
#include <sys/nv.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>

/* IP */
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <fnmatch.h>
#include <ifaddrs.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#ifdef JAIL
#include <jail.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libifconfig.h>

#include "ifconfig.h"

ifconfig_handle_t *lifh;

/*
 * Since "struct ifreq" is composed of various union members, callers
 * should pay special attention to interpret the value.
 * (.e.g. little/big endian difference in the structure.)
 */
struct	ifreq ifr;

char	name[IFNAMSIZ];
char	*descr = NULL;
size_t	descrlen = 64;
int	setaddr;
int	setmask;
int	doalias;
int	clearaddr;
int	newaddr = 1;
int	verbose;
int	printifname = 0;

struct ifconfig_args args;

int	printkeys = 0;		/* Print keying material for interfaces. */
int	exit_code = 0;

/* Formatter Strings */
char	*f_inet, *f_inet6, *f_ether, *f_addr;

static void list_interfaces_ioctl(struct ifconfig_args *args);
static	void status(struct ifconfig_args *args, const struct sockaddr_dl *sdl,
		struct ifaddrs *ifa);
static _Noreturn void usage(void);

static int getifflags(const char *ifname, int us, bool err_ok);

static struct afswtch *af_getbyname(const char *name);

void printifnamemaybe(void);

static struct option *opts = NULL;

struct ifa_order_elt {
	int if_order;
	int af_orders[255];
	struct ifaddrs *ifa;
	TAILQ_ENTRY(ifa_order_elt) link;
};

TAILQ_HEAD(ifa_queue, ifa_order_elt);

static struct module_map_entry {
	const char *ifname;
	const char *kldname;
} module_map[] = {
	{
		.ifname = "tun",
		.kldname = "if_tuntap",
	},
	{
		.ifname = "tap",
		.kldname = "if_tuntap",
	},
	{
		.ifname = "vmnet",
		.kldname = "if_tuntap",
	},
	{
		.ifname = "ipsec",
		.kldname = "ipsec",
	},
	{
		/*
		 * This mapping exists because there is a conflicting enc module
		 * in CAM.  ifconfig's guessing behavior will attempt to match
		 * the ifname to a module as well as if_${ifname} and clash with
		 * CAM enc.  This is an assertion of the correct module to load.
		 */
		.ifname = "enc",
		.kldname = "if_enc",
	},
};


void
opt_register(struct option *p)
{
	p->next = opts;
	opts = p;
}

static void
usage(void)
{
	char options[1024];
	struct option *p;

	/* XXX not right but close enough for now */
	options[0] = '\0';
	for (p = opts; p != NULL; p = p->next) {
		strlcat(options, p->opt_usage, sizeof(options));
		strlcat(options, " ", sizeof(options));
	}

	fprintf(stderr,
	"usage: ifconfig [-f type:format] %sinterface address_family\n"
	"                [address [dest_address]] [parameters]\n"
	"       ifconfig interface create\n"
	"       ifconfig -a %s[-d] [-m] [-u] [-v] [address_family]\n"
	"       ifconfig -l [-d] [-u] [address_family]\n"
	"       ifconfig %s[-d] [-m] [-u] [-v]\n",
		options, options, options);
	exit(1);
}

void
ioctl_ifcreate(int s, struct ifreq *ifr)
{
	if (ioctl(s, SIOCIFCREATE2, ifr) < 0) {
		switch (errno) {
		case EEXIST:
			errx(1, "interface %s already exists", ifr->ifr_name);
		default:
			err(1, "SIOCIFCREATE2 (%s)", ifr->ifr_name);
		}
	}
}

static int
calcorders(struct ifaddrs *ifa, struct ifa_queue *q)
{
	struct ifaddrs *prev;
	struct ifa_order_elt *cur;
	unsigned int ord, af, ifa_ord;

	prev = NULL;
	cur = NULL;
	ord = 0;
	ifa_ord = 0;

	while (ifa != NULL) {
		if (prev == NULL ||
		    strcmp(ifa->ifa_name, prev->ifa_name) != 0) {
			cur = calloc(1, sizeof(*cur));

			if (cur == NULL)
				return (-1);

			TAILQ_INSERT_TAIL(q, cur, link);
			cur->if_order = ifa_ord ++;
			cur->ifa = ifa;
			ord = 0;
		}

		if (ifa->ifa_addr) {
			af = ifa->ifa_addr->sa_family;

			if (af < nitems(cur->af_orders) &&
			    cur->af_orders[af] == 0)
				cur->af_orders[af] = ++ord;
		}
		prev = ifa;
		ifa = ifa->ifa_next;
	}

	return (0);
}

static int
cmpifaddrs(struct ifaddrs *a, struct ifaddrs *b, struct ifa_queue *q)
{
	struct ifa_order_elt *cur, *e1, *e2;
	unsigned int af1, af2;
	int ret;

	e1 = e2 = NULL;

	ret = strcmp(a->ifa_name, b->ifa_name);
	if (ret != 0) {
		TAILQ_FOREACH(cur, q, link) {
			if (e1 && e2)
				break;

			if (strcmp(cur->ifa->ifa_name, a->ifa_name) == 0)
				e1 = cur;
			else if (strcmp(cur->ifa->ifa_name, b->ifa_name) == 0)
				e2 = cur;
		}

		if (!e1 || !e2)
			return (0);
		else
			return (e1->if_order - e2->if_order);

	} else if (a->ifa_addr != NULL && b->ifa_addr != NULL) {
		TAILQ_FOREACH(cur, q, link) {
			if (strcmp(cur->ifa->ifa_name, a->ifa_name) == 0) {
				e1 = cur;
				break;
			}
		}

		if (!e1)
			return (0);

		af1 = a->ifa_addr->sa_family;
		af2 = b->ifa_addr->sa_family;

		if (af1 < nitems(e1->af_orders) && af2 < nitems(e1->af_orders))
			return (e1->af_orders[af1] - e1->af_orders[af2]);
	}

	return (0);
}

static void freeformat(void)
{

	if (f_inet != NULL)
		free(f_inet);
	if (f_inet6 != NULL)
		free(f_inet6);
	if (f_ether != NULL)
		free(f_ether);
	if (f_addr != NULL)
		free(f_addr);
}

static void setformat(char *input)
{
	char	*formatstr, *category, *modifier; 

	formatstr = strdup(input);
	while ((category = strsep(&formatstr, ",")) != NULL) {
		modifier = strchr(category, ':');
		if (modifier == NULL || modifier[1] == '\0') {
			warnx("Skipping invalid format specification: %s\n",
			    category);
			continue;
		}

		/* Split the string on the separator, then seek past it */
		modifier[0] = '\0';
		modifier++;

		if (strcmp(category, "addr") == 0)
			f_addr = strdup(modifier);
		else if (strcmp(category, "ether") == 0)
			f_ether = strdup(modifier);
		else if (strcmp(category, "inet") == 0)
			f_inet = strdup(modifier);
		else if (strcmp(category, "inet6") == 0)
			f_inet6 = strdup(modifier);
	}
	free(formatstr);
}

static struct ifaddrs *
sortifaddrs(struct ifaddrs *list,
    int (*compare)(struct ifaddrs *, struct ifaddrs *, struct ifa_queue *),
    struct ifa_queue *q)
{
	struct ifaddrs *right, *temp, *last, *result, *next, *tail;
	
	right = list;
	temp = list;
	last = list;
	result = NULL;
	next = NULL;
	tail = NULL;

	if (!list || !list->ifa_next)
		return (list);

	while (temp && temp->ifa_next) {
		last = right;
		right = right->ifa_next;
		temp = temp->ifa_next->ifa_next;
	}

	last->ifa_next = NULL;

	list = sortifaddrs(list, compare, q);
	right = sortifaddrs(right, compare, q);

	while (list || right) {

		if (!right) {
			next = list;
			list = list->ifa_next;
		} else if (!list) {
			next = right;
			right = right->ifa_next;
		} else if (compare(list, right, q) <= 0) {
			next = list;
			list = list->ifa_next;
		} else {
			next = right;
			right = right->ifa_next;
		}

		if (!result)
			result = next;
		else
			tail->ifa_next = next;

		tail = next;
	}

	return (result);
}

void printifnamemaybe()
{
	if (printifname)
		printf("%s\n", name);
}

static void
list_interfaces(struct ifconfig_args *args)
{
#ifdef WITHOUT_NETLINK
	list_interfaces_ioctl(args);
#else
	list_interfaces_nl(args);
#endif
}

int
main(int argc, char *argv[])
{
	int c;
	const struct afswtch *afp = NULL;
	int ifindex;
	char options[1024], *envformat;
	struct option *p;
	size_t iflen;
	int flags;

	f_inet = f_inet6 = f_ether = f_addr = NULL;

	lifh = ifconfig_open();
	if (lifh == NULL)
		err(EXIT_FAILURE, "ifconfig_open");

	envformat = getenv("IFCONFIG_FORMAT");
	if (envformat != NULL)
		setformat(envformat);

	/*
	 * Ensure we print interface name when expected to,
	 * even if we terminate early due to error.
	 */
	atexit(printifnamemaybe);

	/* Parse leading line options */
	strlcpy(options, "G:adf:klmnuv", sizeof(options));
	for (p = opts; p != NULL; p = p->next)
		strlcat(options, p->opt, sizeof(options));
	while ((c = getopt(argc, argv, options)) != -1) {
		switch (c) {
		case 'a':	/* scan all interfaces */
			args.all = true;
			break;
		case 'd':	/* restrict scan to "down" interfaces */
			args.downonly = true;
			break;
		case 'f':
			if (optarg == NULL)
				usage();
			setformat(optarg);
			break;
		case 'G':
			if (optarg == NULL || args.all == 0)
				usage();
			args.nogroup = optarg;
			break;
		case 'k':
			args.printkeys = true;
			break;
		case 'l':	/* scan interface names only */
			args.namesonly++;
			break;
		case 'm':	/* show media choices in status */
			args.supmedia = true;
			break;
		case 'n':	/* suppress module loading */
			args.noload = true;
			break;
		case 'u':	/* restrict scan to "up" interfaces */
			args.uponly = true;
			break;
		case 'v':
			args.verbose++;
			break;
		case 'g':
			if (args.all) {
				if (optarg == NULL)
					usage();
				args.matchgroup = optarg;
				break;
			}
			/* FALLTHROUGH */
		default:
			for (p = opts; p != NULL; p = p->next)
				if (p->opt[0] == c) {
					p->cb(optarg);
					break;
				}
			if (p == NULL)
				usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	/* Sync global variables */
	printkeys = args.printkeys;
	verbose = args.verbose;

	/* -l cannot be used with -a or -m */
	if (args.namesonly && (args.all || args.supmedia))
		usage();

	/* nonsense.. */
	if (args.uponly && args.downonly)
		usage();

	/* no arguments is equivalent to '-a' */
	if (!args.namesonly && argc < 1)
		args.all = 1;

	/* -a and -l allow an address family arg to limit the output */
	if (args.all || args.namesonly) {
		if (argc > 1)
			usage();

		ifindex = 0;
		if (argc == 1) {
			afp = af_getbyname(*argv);
			if (afp == NULL) {
				warnx("Address family '%s' unknown.", *argv);
				usage();
			}
			if (afp->af_name != NULL)
				argc--, argv++;
			/* leave with afp non-zero */
		}
	} else {
		/* not listing, need an argument */
		if (argc < 1)
			usage();

		args.ifname = *argv;
		argc--, argv++;

		/* check and maybe load support for this interface */
		ifmaybeload(&args, args.ifname);

		ifindex = if_nametoindex(args.ifname);
		if (ifindex == 0) {
			/*
			 * NOTE:  We must special-case the `create' command
			 * right here as we would otherwise fail when trying
			 * to find the interface.
			 */
			if (argc > 0 && (strcmp(argv[0], "create") == 0 ||
			    strcmp(argv[0], "plumb") == 0)) {
				iflen = strlcpy(name, args.ifname, sizeof(name));
				if (iflen >= sizeof(name))
					errx(1, "%s: cloning name too long",
					    args.ifname);
				ifconfig(argc, argv, 1, NULL);
				exit(exit_code);
			}
#ifdef JAIL
			/*
			 * NOTE:  We have to special-case the `-vnet' command
			 * right here as we would otherwise fail when trying
			 * to find the interface as it lives in another vnet.
			 */
			if (argc > 0 && (strcmp(argv[0], "-vnet") == 0)) {
				iflen = strlcpy(name, args.ifname, sizeof(name));
				if (iflen >= sizeof(name))
					errx(1, "%s: interface name too long",
					    args.ifname);
				ifconfig(argc, argv, 0, NULL);
				exit(exit_code);
			}
#endif
			errx(1, "interface %s does not exist", args.ifname);
		} else {
			/*
			 * Do not allow use `create` command as hostname if
			 * address family is not specified.
			 */
			if (argc > 0 && (strcmp(argv[0], "create") == 0 ||
			    strcmp(argv[0], "plumb") == 0)) {
				if (argc == 1)
					errx(1, "interface %s already exists",
					    args.ifname);
				argc--, argv++;
			}
		}
	}

	/* Check for address family */
	if (argc > 0) {
		afp = af_getbyname(*argv);
		if (afp != NULL)
			argc--, argv++;
	}

	/*
	 * Check for a requested configuration action on a single interface,
	 * which doesn't require building, sorting, and searching the entire
	 * system address list
	 */
	if ((argc > 0) && (args.ifname != NULL)) {
		iflen = strlcpy(name, args.ifname, sizeof(name));
		if (iflen >= sizeof(name)) {
			warnx("%s: interface name too long, skipping", args.ifname);
		} else {
			flags = getifflags(name, -1, false);
			if (!(((flags & IFF_CANTCONFIG) != 0) ||
				(args.downonly && (flags & IFF_UP) != 0) ||
				(args.uponly && (flags & IFF_UP) == 0)))
				ifconfig(argc, argv, 0, afp);
		}
		goto done;
	}

	args.afp = afp;
	args.allfamilies = afp == NULL;
	args.argc = argc;
	args.argv = argv;

	list_interfaces(&args);

done:
	freeformat();
	ifconfig_close(lifh);
	exit(exit_code);
}

bool
match_ether(const struct sockaddr_dl *sdl)
{
	switch (sdl->sdl_type) {
		case IFT_ETHER:
		case IFT_L2VLAN:
		case IFT_BRIDGE:
			if (sdl->sdl_alen == ETHER_ADDR_LEN)
				return (true);
		default:
			return (false);
	}
}

static bool
match_afp(const struct afswtch *afp, int sa_family, const struct sockaddr_dl *sdl)
{
	if (afp == NULL)
		return (true);
	/* special case for "ether" address family */
	if (!strcmp(afp->af_name, "ether")) {
		if (sdl == NULL && !match_ether(sdl))
			return (false);
		return (true);
	}
	return (afp->af_af == sa_family);
}

bool
match_if_flags(struct ifconfig_args *args, int if_flags)
{
	if ((if_flags & IFF_CANTCONFIG) != 0)
		return (false);
	if (args->downonly && (if_flags & IFF_UP) != 0)
		return (false);
	if (args->uponly && (if_flags & IFF_UP) == 0)
		return (false);
	return (true);
}

#ifdef WITHOUT_NETLINK
static void
list_interfaces_ioctl(struct ifconfig_args *args)
{
	struct ifa_queue q = TAILQ_HEAD_INITIALIZER(q);
	struct ifaddrs *ifap, *sifap, *ifa;
	struct ifa_order_elt *cur, *tmp;
	char *namecp = NULL;
	int ifindex;
	size_t iflen;

	if (getifaddrs(&ifap) != 0)
		err(EXIT_FAILURE, "getifaddrs");

	char *cp = NULL;
	
	if (calcorders(ifap, &q) != 0)
		err(EXIT_FAILURE, "calcorders");
		
	sifap = sortifaddrs(ifap, cmpifaddrs, &q);

	TAILQ_FOREACH_SAFE(cur, &q, link, tmp)
		free(cur);

	ifindex = 0;
	for (ifa = sifap; ifa; ifa = ifa->ifa_next) {
		struct ifreq paifr = {};
		const struct sockaddr_dl *sdl;

		strlcpy(paifr.ifr_name, ifa->ifa_name, sizeof(paifr.ifr_name));
		if (sizeof(paifr.ifr_addr) >= ifa->ifa_addr->sa_len) {
			memcpy(&paifr.ifr_addr, ifa->ifa_addr,
			    ifa->ifa_addr->sa_len);
		}

		if (args->ifname != NULL && strcmp(args->ifname, ifa->ifa_name) != 0)
			continue;
		if (ifa->ifa_addr->sa_family == AF_LINK)
			sdl = (const struct sockaddr_dl *) ifa->ifa_addr;
		else
			sdl = NULL;
		if (cp != NULL && strcmp(cp, ifa->ifa_name) == 0 && !args->namesonly)
			continue;
		iflen = strlcpy(name, ifa->ifa_name, sizeof(name));
		if (iflen >= sizeof(name)) {
			warnx("%s: interface name too long, skipping",
			    ifa->ifa_name);
			continue;
		}
		cp = ifa->ifa_name;

		if (!match_if_flags(args, ifa->ifa_flags))
			continue;
		if (!group_member(ifa->ifa_name, args->matchgroup, args->nogroup))
			continue;
		/*
		 * Are we just listing the interfaces?
		 */
		if (args->namesonly) {
			if (namecp == cp)
				continue;
			if (!match_afp(args->afp, ifa->ifa_addr->sa_family, sdl))
				continue;
			namecp = cp;
			ifindex++;
			if (ifindex > 1)
				printf(" ");
			fputs(name, stdout);
			continue;
		}
		ifindex++;

		if (args->argc > 0)
			ifconfig(args->argc, args->argv, 0, args->afp);
		else
			status(args, sdl, ifa);
	}
	if (args->namesonly)
		printf("\n");
	freeifaddrs(ifap);
}
#endif

/*
 * Returns true if an interface should be listed because any its groups
 * matches shell pattern "match" and none of groups matches pattern "nomatch".
 * If any pattern is NULL, corresponding condition is skipped.
 */
bool
group_member(const char *ifname, const char *match, const char *nomatch)
{
	static int		 sock = -1;

	struct ifgroupreq	 ifgr;
	struct ifg_req		*ifg;
	int			 len;
	bool			 matched, nomatched;

	/* Sanity checks. */
	if (match == NULL && nomatch == NULL)
		return (true);
	if (ifname == NULL)
		return (false);

	memset(&ifgr, 0, sizeof(ifgr));
	strlcpy(ifgr.ifgr_name, ifname, IFNAMSIZ);

	/* The socket is opened once. Let _exit() close it. */
	if (sock == -1) {
		sock = socket(AF_LOCAL, SOCK_DGRAM, 0);
    		if (sock == -1)
            	    errx(1, "%s: socket(AF_LOCAL,SOCK_DGRAM)", __func__);
	}

	/* Determine amount of memory for the list of groups. */
	if (ioctl(sock, SIOCGIFGROUP, (caddr_t)&ifgr) == -1) {
		if (errno == EINVAL || errno == ENOTTY)
			return (false);
		else
			errx(1, "%s: SIOCGIFGROUP", __func__);
	}

	/* Obtain the list of groups. */
	len = ifgr.ifgr_len;
	ifgr.ifgr_groups =
	    (struct ifg_req *)calloc(len / sizeof(*ifg), sizeof(*ifg));

	if (ifgr.ifgr_groups == NULL)
		errx(1, "%s: no memory", __func__);
	if (ioctl(sock, SIOCGIFGROUP, (caddr_t)&ifgr) == -1)
		errx(1, "%s: SIOCGIFGROUP", __func__);

	/* Perform matching. */
	matched = false;
	nomatched = true;
	for (ifg = ifgr.ifgr_groups; ifg && len >= sizeof(*ifg); ifg++) {
		len -= sizeof(struct ifg_req);
		if (match)
			matched |= !fnmatch(match, ifg->ifgrq_group, 0);
		if (nomatch)
			nomatched &= fnmatch(nomatch, ifg->ifgrq_group, 0);
	}
	free(ifgr.ifgr_groups);

	if (match && !nomatch)
		return (matched);
	if (!match && nomatch)
		return (nomatched);
	return (matched && nomatched);
}

static struct afswtch *afs = NULL;

void
af_register(struct afswtch *p)
{
	p->af_next = afs;
	afs = p;
}

static struct afswtch *
af_getbyname(const char *name)
{
	struct afswtch *afp;

	for (afp = afs; afp !=  NULL; afp = afp->af_next)
		if (strcmp(afp->af_name, name) == 0)
			return afp;
	return NULL;
}

struct afswtch *
af_getbyfamily(int af)
{
	struct afswtch *afp;

	for (afp = afs; afp != NULL; afp = afp->af_next)
		if (afp->af_af == af)
			return afp;
	return NULL;
}

void
af_other_status(int s)
{
	struct afswtch *afp;
	uint8_t afmask[howmany(AF_MAX, NBBY)];

	memset(afmask, 0, sizeof(afmask));
	for (afp = afs; afp != NULL; afp = afp->af_next) {
		if (afp->af_other_status == NULL)
			continue;
		if (afp->af_af != AF_UNSPEC && isset(afmask, afp->af_af))
			continue;
		afp->af_other_status(s);
		setbit(afmask, afp->af_af);
	}
}

static void
af_all_tunnel_status(int s)
{
	struct afswtch *afp;
	uint8_t afmask[howmany(AF_MAX, NBBY)];

	memset(afmask, 0, sizeof(afmask));
	for (afp = afs; afp != NULL; afp = afp->af_next) {
		if (afp->af_status_tunnel == NULL)
			continue;
		if (afp->af_af != AF_UNSPEC && isset(afmask, afp->af_af))
			continue;
		afp->af_status_tunnel(s);
		setbit(afmask, afp->af_af);
	}
}

static struct cmd *cmds = NULL;

void
cmd_register(struct cmd *p)
{
	p->c_next = cmds;
	cmds = p;
}

static const struct cmd *
cmd_lookup(const char *name, int iscreate)
{
	const struct cmd *p;

	for (p = cmds; p != NULL; p = p->c_next)
		if (strcmp(name, p->c_name) == 0) {
			if (iscreate) {
				if (p->c_iscloneop)
					return p;
			} else {
				if (!p->c_iscloneop)
					return p;
			}
		}
	return NULL;
}

struct callback {
	callback_func *cb_func;
	void	*cb_arg;
	struct callback *cb_next;
};
static struct callback *callbacks = NULL;

void
callback_register(callback_func *func, void *arg)
{
	struct callback *cb;

	cb = malloc(sizeof(struct callback));
	if (cb == NULL)
		errx(1, "unable to allocate memory for callback");
	cb->cb_func = func;
	cb->cb_arg = arg;
	cb->cb_next = callbacks;
	callbacks = cb;
}

/* specially-handled commands */
static void setifaddr(const char *, int, int, const struct afswtch *);
static const struct cmd setifaddr_cmd = DEF_CMD("ifaddr", 0, setifaddr);

static void setifdstaddr(const char *, int, int, const struct afswtch *);
static const struct cmd setifdstaddr_cmd =
	DEF_CMD("ifdstaddr", 0, setifdstaddr);

int
ifconfig(int argc, char *const *argv, int iscreate, const struct afswtch *uafp)
{
	const struct afswtch *afp, *nafp;
	const struct cmd *p;
	struct callback *cb;
	int s;

	strlcpy(ifr.ifr_name, name, sizeof ifr.ifr_name);
	afp = NULL;
	if (uafp != NULL)
		afp = uafp;
	/*
	 * This is the historical "accident" allowing users to configure IPv4
	 * addresses without the "inet" keyword which while a nice feature has
	 * proven to complicate other things.  We cannot remove this but only
	 * make sure we will never have a similar implicit default for IPv6 or
	 * any other address familiy.  We need a fallback though for
	 * ifconfig IF up/down etc. to work without INET support as people
	 * never used ifconfig IF link up/down, etc. either.
	 */
#ifndef RESCUE
#ifdef INET
	if (afp == NULL && feature_present("inet"))
		afp = af_getbyname("inet");
#endif
#endif
	if (afp == NULL)
		afp = af_getbyname("link");
	if (afp == NULL) {
		warnx("Please specify an address_family.");
		usage();
	}
top:
	ifr.ifr_addr.sa_family =
		afp->af_af == AF_LINK || afp->af_af == AF_UNSPEC ?
		AF_LOCAL : afp->af_af;

	if ((s = socket(ifr.ifr_addr.sa_family, SOCK_DGRAM, 0)) < 0 &&
	    (uafp != NULL || errno != EAFNOSUPPORT ||
	     (s = socket(AF_LOCAL, SOCK_DGRAM, 0)) < 0))
		err(1, "socket(family %u,SOCK_DGRAM)", ifr.ifr_addr.sa_family);

	while (argc > 0) {
		p = cmd_lookup(*argv, iscreate);
		if (iscreate && p == NULL) {
			/*
			 * Push the clone create callback so the new
			 * device is created and can be used for any
			 * remaining arguments.
			 */
			cb = callbacks;
			if (cb == NULL)
				errx(1, "internal error, no callback");
			callbacks = cb->cb_next;
			cb->cb_func(s, cb->cb_arg);
			iscreate = 0;
			/*
			 * Handle any address family spec that
			 * immediately follows and potentially
			 * recreate the socket.
			 */
			nafp = af_getbyname(*argv);
			if (nafp != NULL) {
				argc--, argv++;
				if (nafp != afp) {
					close(s);
					afp = nafp;
					goto top;
				}
			}
			/*
			 * Look for a normal parameter.
			 */
			continue;
		}
		if (p == NULL) {
			/*
			 * Not a recognized command, choose between setting
			 * the interface address and the dst address.
			 */
			p = (setaddr ? &setifdstaddr_cmd : &setifaddr_cmd);
		}
		if (p->c_parameter == NEXTARG && p->c_u.c_func) {
			if (argv[1] == NULL)
				errx(1, "'%s' requires argument",
				    p->c_name);
			p->c_u.c_func(argv[1], 0, s, afp);
			argc--, argv++;
		} else if (p->c_parameter == OPTARG && p->c_u.c_func) {
			p->c_u.c_func(argv[1], 0, s, afp);
			if (argv[1] != NULL)
				argc--, argv++;
		} else if (p->c_parameter == NEXTARG2 && p->c_u.c_func2) {
			if (argc < 3)
				errx(1, "'%s' requires 2 arguments",
				    p->c_name);
			p->c_u.c_func2(argv[1], argv[2], s, afp);
			argc -= 2, argv += 2;
		} else if (p->c_parameter == SPARAM && p->c_u.c_func3) {
			p->c_u.c_func3(*argv, p->c_sparameter, s, afp);
		} else if (p->c_u.c_func)
			p->c_u.c_func(*argv, p->c_parameter, s, afp);
		argc--, argv++;
	}

	/*
	 * Do any post argument processing required by the address family.
	 */
	if (afp->af_postproc != NULL)
		afp->af_postproc(s, afp, newaddr, getifflags(name, s, true));
	/*
	 * Do deferred callbacks registered while processing
	 * command-line arguments.
	 */
	for (cb = callbacks; cb != NULL; cb = cb->cb_next)
		cb->cb_func(s, cb->cb_arg);
	/*
	 * Do deferred operations.
	 */
	if (clearaddr) {
		if (afp->af_ridreq == NULL || afp->af_difaddr == 0) {
			warnx("interface %s cannot change %s addresses!",
			      name, afp->af_name);
			clearaddr = 0;
		}
	}
	if (clearaddr) {
		int ret;
		strlcpy(((struct ifreq *)afp->af_ridreq)->ifr_name, name,
			sizeof ifr.ifr_name);
		ret = ioctl(s, afp->af_difaddr, afp->af_ridreq);
		if (ret < 0) {
			if (errno == EADDRNOTAVAIL && (doalias >= 0)) {
				/* means no previous address for interface */
			} else
				Perror("ioctl (SIOCDIFADDR)");
		}
	}
	if (newaddr) {
		if (afp->af_addreq == NULL || afp->af_aifaddr == 0) {
			warnx("interface %s cannot change %s addresses!",
			      name, afp->af_name);
			newaddr = 0;
		}
	}
	if (newaddr && (setaddr || setmask)) {
		strlcpy(((struct ifreq *)afp->af_addreq)->ifr_name, name,
			sizeof ifr.ifr_name);
		if (ioctl(s, afp->af_aifaddr, afp->af_addreq) < 0)
			Perror("ioctl (SIOCAIFADDR)");
	}

	close(s);
	return(0);
}

/*ARGSUSED*/
static void
setifaddr(const char *addr, int param, int s, const struct afswtch *afp)
{
	if (afp->af_getaddr == NULL)
		return;
	/*
	 * Delay the ioctl to set the interface addr until flags are all set.
	 * The address interpretation may depend on the flags,
	 * and the flags may change when the address is set.
	 */
	setaddr++;
	if (doalias == 0 && afp->af_af != AF_LINK)
		clearaddr = 1;
	afp->af_getaddr(addr, (doalias >= 0 ? ADDR : RIDADDR));
}

static void
settunnel(const char *src, const char *dst, int s, const struct afswtch *afp)
{
	struct addrinfo *srcres, *dstres;
	int ecode;

	if (afp->af_settunnel == NULL) {
		warn("address family %s does not support tunnel setup",
			afp->af_name);
		return;
	}

	if ((ecode = getaddrinfo(src, NULL, NULL, &srcres)) != 0)
		errx(1, "error in parsing address string: %s",
		    gai_strerror(ecode));

	if ((ecode = getaddrinfo(dst, NULL, NULL, &dstres)) != 0)
		errx(1, "error in parsing address string: %s",
		    gai_strerror(ecode));

	if (srcres->ai_addr->sa_family != dstres->ai_addr->sa_family)
		errx(1,
		    "source and destination address families do not match");

	afp->af_settunnel(s, srcres, dstres);

	freeaddrinfo(srcres);
	freeaddrinfo(dstres);
}

/* ARGSUSED */
static void
deletetunnel(const char *vname, int param, int s, const struct afswtch *afp)
{

	if (ioctl(s, SIOCDIFPHYADDR, &ifr) < 0)
		err(1, "SIOCDIFPHYADDR");
}

#ifdef JAIL
static void
setifvnet(const char *jname, int dummy __unused, int s,
    const struct afswtch *afp)
{
	struct ifreq my_ifr;

	memcpy(&my_ifr, &ifr, sizeof(my_ifr));
	my_ifr.ifr_jid = jail_getid(jname);
	if (my_ifr.ifr_jid < 0)
		errx(1, "%s", jail_errmsg);
	if (ioctl(s, SIOCSIFVNET, &my_ifr) < 0)
		err(1, "SIOCSIFVNET");
}

static void
setifrvnet(const char *jname, int dummy __unused, int s,
    const struct afswtch *afp)
{
	struct ifreq my_ifr;

	memcpy(&my_ifr, &ifr, sizeof(my_ifr));
	my_ifr.ifr_jid = jail_getid(jname);
	if (my_ifr.ifr_jid < 0)
		errx(1, "%s", jail_errmsg);
	if (ioctl(s, SIOCSIFRVNET, &my_ifr) < 0)
		err(1, "SIOCSIFRVNET(%d, %s)", my_ifr.ifr_jid, my_ifr.ifr_name);
}
#endif

static void
setifnetmask(const char *addr, int dummy __unused, int s,
    const struct afswtch *afp)
{
	if (afp->af_getaddr != NULL) {
		setmask++;
		afp->af_getaddr(addr, MASK);
	}
}

static void
setifbroadaddr(const char *addr, int dummy __unused, int s,
    const struct afswtch *afp)
{
	if (afp->af_getaddr != NULL)
		afp->af_getaddr(addr, DSTADDR);
}

static void
notealias(const char *addr, int param, int s, const struct afswtch *afp)
{
#define rqtosa(x) (&(((struct ifreq *)(afp->x))->ifr_addr))
	if (setaddr && doalias == 0 && param < 0)
		if (afp->af_addreq != NULL && afp->af_ridreq != NULL)
			bcopy((caddr_t)rqtosa(af_addreq),
			      (caddr_t)rqtosa(af_ridreq),
			      rqtosa(af_addreq)->sa_len);
	doalias = param;
	if (param < 0) {
		clearaddr = 1;
		newaddr = 0;
	} else
		clearaddr = 0;
#undef rqtosa
}

/*ARGSUSED*/
static void
setifdstaddr(const char *addr, int param __unused, int s, 
    const struct afswtch *afp)
{
	if (afp->af_getaddr != NULL)
		afp->af_getaddr(addr, DSTADDR);
}

static int
getifflags(const char *ifname, int us, bool err_ok)
{
	struct ifreq my_ifr;
	int s;
	
	memset(&my_ifr, 0, sizeof(my_ifr));
	(void) strlcpy(my_ifr.ifr_name, ifname, sizeof(my_ifr.ifr_name));
	if (us < 0) {
		if ((s = socket(AF_LOCAL, SOCK_DGRAM, 0)) < 0)
			err(1, "socket(family AF_LOCAL,SOCK_DGRAM");
	} else
		s = us;
 	if (ioctl(s, SIOCGIFFLAGS, (caddr_t)&my_ifr) < 0) {
		if (!err_ok) {
			Perror("ioctl (SIOCGIFFLAGS)");
			exit(1);
		}
 	}
	if (us < 0)
		close(s);
	return ((my_ifr.ifr_flags & 0xffff) | (my_ifr.ifr_flagshigh << 16));
}

/*
 * Note: doing an SIOCIGIFFLAGS scribbles on the union portion
 * of the ifreq structure, which may confuse other parts of ifconfig.
 * Make a private copy so we can avoid that.
 */
static void
setifflags(const char *vname, int value, int s, const struct afswtch *afp)
{
	struct ifreq		my_ifr;
	int flags;

	flags = getifflags(name, s, false);
	if (value < 0) {
		value = -value;
		flags &= ~value;
	} else
		flags |= value;
	memset(&my_ifr, 0, sizeof(my_ifr));
	(void) strlcpy(my_ifr.ifr_name, name, sizeof(my_ifr.ifr_name));
	my_ifr.ifr_flags = flags & 0xffff;
	my_ifr.ifr_flagshigh = flags >> 16;
	if (ioctl(s, SIOCSIFFLAGS, (caddr_t)&my_ifr) < 0)
		Perror(vname);
}

void
setifcap(const char *vname, int value, int s, const struct afswtch *afp)
{
	int flags;

 	if (ioctl(s, SIOCGIFCAP, (caddr_t)&ifr) < 0) {
 		Perror("ioctl (SIOCGIFCAP)");
 		exit(1);
 	}
	flags = ifr.ifr_curcap;
	if (value < 0) {
		value = -value;
		flags &= ~value;
	} else
		flags |= value;
	flags &= ifr.ifr_reqcap;
	/* Check for no change in capabilities. */
	if (ifr.ifr_curcap == flags)
		return;
	ifr.ifr_reqcap = flags;
	if (ioctl(s, SIOCSIFCAP, (caddr_t)&ifr) < 0)
		Perror(vname);
}

void
setifcapnv(const char *vname, const char *arg, int s, const struct afswtch *afp)
{
	nvlist_t *nvcap;
	void *buf;
	char *marg, *mopt;
	size_t nvbuflen;
	bool neg;

	if (ioctl(s, SIOCGIFCAP, (caddr_t)&ifr) < 0)
		Perror("ioctl (SIOCGIFCAP)");
	if ((ifr.ifr_curcap & IFCAP_NV) == 0) {
		warnx("IFCAP_NV not supported");
		return; /* Not exit() */
	}

	marg = strdup(arg);
	if (marg == NULL)
		Perror("strdup");
	nvcap = nvlist_create(0);
	if (nvcap == NULL)
		Perror("nvlist_create");
	while ((mopt = strsep(&marg, ",")) != NULL) {
		neg = *mopt == '-';
		if (neg)
			mopt++;
		if (strcmp(mopt, "rxtls") == 0) {
			nvlist_add_bool(nvcap, "rxtls4", !neg);
			nvlist_add_bool(nvcap, "rxtls6", !neg);
		} else {
			nvlist_add_bool(nvcap, mopt, !neg);
		}
	}
	buf = nvlist_pack(nvcap, &nvbuflen);
	if (buf == NULL) {
		errx(1, "nvlist_pack error");
		exit(1);
	}
	ifr.ifr_cap_nv.buf_length = ifr.ifr_cap_nv.length = nvbuflen;
	ifr.ifr_cap_nv.buffer = buf;
	if (ioctl(s, SIOCSIFCAPNV, (caddr_t)&ifr) < 0)
		Perror(vname);
	free(buf);
	nvlist_destroy(nvcap);
	free(marg);
}

static void
setifmetric(const char *val, int dummy __unused, int s, 
    const struct afswtch *afp)
{
	strlcpy(ifr.ifr_name, name, sizeof (ifr.ifr_name));
	ifr.ifr_metric = atoi(val);
	if (ioctl(s, SIOCSIFMETRIC, (caddr_t)&ifr) < 0)
		err(1, "ioctl SIOCSIFMETRIC (set metric)");
}

static void
setifmtu(const char *val, int dummy __unused, int s, 
    const struct afswtch *afp)
{
	strlcpy(ifr.ifr_name, name, sizeof (ifr.ifr_name));
	ifr.ifr_mtu = atoi(val);
	if (ioctl(s, SIOCSIFMTU, (caddr_t)&ifr) < 0)
		err(1, "ioctl SIOCSIFMTU (set mtu)");
}

static void
setifpcp(const char *val, int arg __unused, int s, const struct afswtch *afp)
{
	u_long ul;
	char *endp;

	ul = strtoul(val, &endp, 0);
	if (*endp != '\0')
		errx(1, "invalid value for pcp");
	if (ul > 7)
		errx(1, "value for pcp out of range");
	ifr.ifr_lan_pcp = ul;
	if (ioctl(s, SIOCSLANPCP, (caddr_t)&ifr) == -1)
		err(1, "SIOCSLANPCP");
}

static void
disableifpcp(const char *val, int arg __unused, int s,
    const struct afswtch *afp)
{

	ifr.ifr_lan_pcp = IFNET_PCP_NONE;
	if (ioctl(s, SIOCSLANPCP, (caddr_t)&ifr) == -1)
		err(1, "SIOCSLANPCP");
}

static void
setifname(const char *val, int dummy __unused, int s, 
    const struct afswtch *afp)
{
	char *newname;
	
	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

	newname = strdup(val);
	if (newname == NULL)
		err(1, "no memory to set ifname");
	ifr.ifr_data = newname;
	if (ioctl(s, SIOCSIFNAME, (caddr_t)&ifr) < 0) {
		free(newname);
		err(1, "ioctl SIOCSIFNAME (set name)");
	}
	printifname = 1;
	strlcpy(name, newname, sizeof(name));
	free(newname);
}

/* ARGSUSED */
static void
setifdescr(const char *val, int dummy __unused, int s, 
    const struct afswtch *afp)
{
	char *newdescr;

	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
	
	ifr.ifr_buffer.length = strlen(val) + 1;
	if (ifr.ifr_buffer.length == 1) {
		ifr.ifr_buffer.buffer = newdescr = NULL;
		ifr.ifr_buffer.length = 0;
	} else {
		newdescr = strdup(val);
		ifr.ifr_buffer.buffer = newdescr;
		if (newdescr == NULL) {
			warn("no memory to set ifdescr");
			return;
		}
	}

	if (ioctl(s, SIOCSIFDESCR, (caddr_t)&ifr) < 0)
		err(1, "ioctl SIOCSIFDESCR (set descr)");

	free(newdescr);
}

/* ARGSUSED */
static void
unsetifdescr(const char *val, int value, int s, const struct afswtch *afp)
{

	setifdescr("", 0, s, 0);
}

#define	IFFBITS \
"\020\1UP\2BROADCAST\3DEBUG\4LOOPBACK\5POINTOPOINT\7RUNNING" \
"\10NOARP\11PROMISC\12ALLMULTI\13OACTIVE\14SIMPLEX\15LINK0\16LINK1\17LINK2" \
"\20MULTICAST\22PPROMISC\23MONITOR\24STATICARP\25STICKYARP"

#define	IFCAPBITS \
"\020\1RXCSUM\2TXCSUM\3NETCONS\4VLAN_MTU\5VLAN_HWTAGGING\6JUMBO_MTU\7POLLING" \
"\10VLAN_HWCSUM\11TSO4\12TSO6\13LRO\14WOL_UCAST\15WOL_MCAST\16WOL_MAGIC" \
"\17TOE4\20TOE6\21VLAN_HWFILTER\23VLAN_HWTSO\24LINKSTATE\25NETMAP" \
"\26RXCSUM_IPV6\27TXCSUM_IPV6\31TXRTLMT\32HWRXTSTMP\33NOMAP\34TXTLS4\35TXTLS6" \
"\36VXLAN_HWCSUM\37VXLAN_HWTSO\40TXTLS_RTLMT"

static void
print_ifcap_nv(struct ifconfig_args *args, int s)
{
	nvlist_t *nvcap;
	const char *nvname;
	void *buf, *cookie;
	bool first, val;
	int type;

	buf = malloc(IFR_CAP_NV_MAXBUFSIZE);
	if (buf == NULL)
		Perror("malloc");
	ifr.ifr_cap_nv.buffer = buf;
	ifr.ifr_cap_nv.buf_length = IFR_CAP_NV_MAXBUFSIZE;
	if (ioctl(s, SIOCGIFCAPNV, (caddr_t)&ifr) != 0)
		Perror("ioctl (SIOCGIFCAPNV)");
	nvcap = nvlist_unpack(ifr.ifr_cap_nv.buffer,
	    ifr.ifr_cap_nv.length, 0);
	if (nvcap == NULL)
		Perror("nvlist_unpack");
	printf("\toptions");
	cookie = NULL;
	for (first = true;; first = false) {
		nvname = nvlist_next(nvcap, &type, &cookie);
		if (nvname == NULL) {
			printf("\n");
			break;
		}
		if (type == NV_TYPE_BOOL) {
			val = nvlist_get_bool(nvcap, nvname);
			if (val) {
				printf("%c%s",
				    first ? ' ' : ',', nvname);
			}
		}
	}
	if (args->supmedia) {
		printf("\tcapabilities");
		cookie = NULL;
		for (first = true;; first = false) {
			nvname = nvlist_next(nvcap, &type,
			    &cookie);
			if (nvname == NULL) {
				printf("\n");
				break;
			}
			if (type == NV_TYPE_BOOL)
				printf("%c%s", first ? ' ' :
				    ',', nvname);
		}
	}
	nvlist_destroy(nvcap);
	free(buf);

	if (ioctl(s, SIOCGIFCAP, (caddr_t)&ifr) != 0)
		Perror("ioctl (SIOCGIFCAP)");
}

void
print_ifcap(struct ifconfig_args *args, int s)
{
	if (ioctl(s, SIOCGIFCAP, (caddr_t)&ifr) != 0)
		return;

	if ((ifr.ifr_curcap & IFCAP_NV) != 0)
		print_ifcap_nv(args, s);
	else {
		printb("\toptions", ifr.ifr_curcap, IFCAPBITS);
		putchar('\n');
		if (args->supmedia && ifr.ifr_reqcap != 0) {
			printb("\tcapabilities", ifr.ifr_reqcap,
			    IFCAPBITS);
			putchar('\n');
		}
	}
}

void
print_ifstatus(int s)
{
	struct ifstat ifs;

	strlcpy(ifs.ifs_name, name, sizeof ifs.ifs_name);
	if (ioctl(s, SIOCGIFSTATUS, &ifs) == 0)
		printf("%s", ifs.ascii);
}

void
print_metric(int s)
{
	if (ioctl(s, SIOCGIFMETRIC, &ifr) != -1)
		printf(" metric %d", ifr.ifr_metric);
}

#ifdef WITHOUT_NETLINK
static void
print_mtu(int s)
{
	if (ioctl(s, SIOCGIFMTU, &ifr) != -1)
		printf(" mtu %d", ifr.ifr_mtu);
}

static void
print_description(int s)
{
	for (;;) {
		if ((descr = reallocf(descr, descrlen)) != NULL) {
			ifr.ifr_buffer.buffer = descr;
			ifr.ifr_buffer.length = descrlen;
			if (ioctl(s, SIOCGIFDESCR, &ifr) == 0) {
				if (ifr.ifr_buffer.buffer == descr) {
					if (strlen(descr) > 0)
						printf("\tdescription: %s\n",
						    descr);
				} else if (ifr.ifr_buffer.length > descrlen) {
					descrlen = ifr.ifr_buffer.length;
					continue;
				}
			}
		} else
			warn("unable to allocate memory for interface"
			    "description");
		break;
	}
}

/*
 * Print the status of the interface.  If an address family was
 * specified, show only it; otherwise, show them all.
 */
static void
status(struct ifconfig_args *args, const struct sockaddr_dl *sdl,
	struct ifaddrs *ifa)
{
	struct ifaddrs *ift;
	int s;
	bool allfamilies = args->afp == NULL;

	if (args->afp == NULL)
		ifr.ifr_addr.sa_family = AF_LOCAL;
	else
		ifr.ifr_addr.sa_family =
		   args->afp->af_af == AF_LINK ? AF_LOCAL : args->afp->af_af;
	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

	s = socket(ifr.ifr_addr.sa_family, SOCK_DGRAM, 0);
	if (s < 0)
		err(1, "socket(family %u,SOCK_DGRAM)", ifr.ifr_addr.sa_family);

	printf("%s: ", name);
	printb("flags", ifa->ifa_flags, IFFBITS);
	print_metric(s);
	print_mtu(s);
	putchar('\n');

	print_description(s);

	print_ifcap(args, s);

	tunnel_status(s);

	for (ift = ifa; ift != NULL; ift = ift->ifa_next) {
		if (ift->ifa_addr == NULL)
			continue;
		if (strcmp(ifa->ifa_name, ift->ifa_name) != 0)
			continue;
		if (allfamilies) {
			const struct afswtch *p;
			p = af_getbyfamily(ift->ifa_addr->sa_family);
			if (p != NULL && p->af_status != NULL)
				p->af_status(s, ift);
		} else if (args->afp->af_af == ift->ifa_addr->sa_family)
			args->afp->af_status(s, ift);
	}
#if 0
	if (allfamilies || afp->af_af == AF_LINK) {
		const struct afswtch *lafp;

		/*
		 * Hack; the link level address is received separately
		 * from the routing information so any address is not
		 * handled above.  Cobble together an entry and invoke
		 * the status method specially.
		 */
		lafp = af_getbyname("lladdr");
		if (lafp != NULL) {
			info.rti_info[RTAX_IFA] = (struct sockaddr *)sdl;
			lafp->af_status(s, &info);
		}
	}
#endif
	if (allfamilies)
		af_other_status(s);
	else if (args->afp->af_other_status != NULL)
		args->afp->af_other_status(s);

	print_ifstatus(s);
	if (args->verbose > 0)
		sfp_status(s, &ifr, args->verbose);

	close(s);
	return;
}
#endif

void
tunnel_status(int s)
{
	af_all_tunnel_status(s);
}

void
Perror(const char *cmd)
{
	switch (errno) {

	case ENXIO:
		errx(1, "%s: no such interface", cmd);
		break;

	case EPERM:
		errx(1, "%s: permission denied", cmd);
		break;

	default:
		err(1, "%s", cmd);
	}
}

/*
 * Print a value a la the %b format of the kernel's printf
 */
void
printb(const char *s, unsigned v, const char *bits)
{
	int i, any = 0;
	char c;

	if (bits && *bits == 8)
		printf("%s=%o", s, v);
	else
		printf("%s=%x", s, v);
	if (bits) {
		bits++;
		putchar('<');
		while ((i = *bits++) != '\0') {
			if (v & (1u << (i-1))) {
				if (any)
					putchar(',');
				any = 1;
				for (; (c = *bits) > 32; bits++)
					putchar(c);
			} else
				for (; *bits > 32; bits++)
					;
		}
		putchar('>');
	}
}

void
print_vhid(const struct ifaddrs *ifa, const char *s)
{
	struct if_data *ifd;

	if (ifa->ifa_data == NULL)
		return;

	ifd = ifa->ifa_data;
	if (ifd->ifi_vhid == 0)
		return;
	
	printf(" vhid %d", ifd->ifi_vhid);
}

void
ifmaybeload(struct ifconfig_args *args, const char *name)
{
#define MOD_PREFIX_LEN		3	/* "if_" */
	struct module_stat mstat;
	int i, fileid, modid;
	char ifkind[IFNAMSIZ + MOD_PREFIX_LEN], ifname[IFNAMSIZ], *dp;
	const char *cp;
	struct module_map_entry *mme;
	bool found;

	/* loading suppressed by the user */
	if (args->noload)
		return;

	/* trim the interface number off the end */
	strlcpy(ifname, name, sizeof(ifname));
	for (dp = ifname; *dp != 0; dp++)
		if (isdigit(*dp)) {
			*dp = 0;
			break;
		}

	/* Either derive it from the map or guess otherwise */
	*ifkind = '\0';
	found = false;
	for (i = 0; i < nitems(module_map); ++i) {
		mme = &module_map[i];
		if (strcmp(mme->ifname, ifname) == 0) {
			strlcpy(ifkind, mme->kldname, sizeof(ifkind));
			found = true;
			break;
		}
	}

	/* We didn't have an alias for it... we'll guess. */
	if (!found) {
	    /* turn interface and unit into module name */
	    strlcpy(ifkind, "if_", sizeof(ifkind));
	    strlcat(ifkind, ifname, sizeof(ifkind));
	}

	/* scan files in kernel */
	mstat.version = sizeof(struct module_stat);
	for (fileid = kldnext(0); fileid > 0; fileid = kldnext(fileid)) {
		/* scan modules in file */
		for (modid = kldfirstmod(fileid); modid > 0;
		     modid = modfnext(modid)) {
			if (modstat(modid, &mstat) < 0)
				continue;
			/* strip bus name if present */
			if ((cp = strchr(mstat.name, '/')) != NULL) {
				cp++;
			} else {
				cp = mstat.name;
			}
			/*
			 * Is it already loaded?  Don't compare with ifname if
			 * we were specifically told which kld to use.  Doing
			 * so could lead to conflicts not trivially solved.
			 */
			if ((!found && strcmp(ifname, cp) == 0) ||
			    strcmp(ifkind, cp) == 0)
				return;
		}
	}

	/*
	 * Try to load the module.  But ignore failures, because ifconfig can't
	 * infer the names of all drivers (eg mlx4en(4)).
	 */
	(void) kldload(ifkind);
}

static struct cmd basic_cmds[] = {
	DEF_CMD("up",		IFF_UP,		setifflags),
	DEF_CMD("down",		-IFF_UP,	setifflags),
	DEF_CMD("arp",		-IFF_NOARP,	setifflags),
	DEF_CMD("-arp",		IFF_NOARP,	setifflags),
	DEF_CMD("debug",	IFF_DEBUG,	setifflags),
	DEF_CMD("-debug",	-IFF_DEBUG,	setifflags),
	DEF_CMD_ARG("description",		setifdescr),
	DEF_CMD_ARG("descr",			setifdescr),
	DEF_CMD("-description",	0,		unsetifdescr),
	DEF_CMD("-descr",	0,		unsetifdescr),
	DEF_CMD("promisc",	IFF_PPROMISC,	setifflags),
	DEF_CMD("-promisc",	-IFF_PPROMISC,	setifflags),
	DEF_CMD("add",		IFF_UP,		notealias),
	DEF_CMD("alias",	IFF_UP,		notealias),
	DEF_CMD("-alias",	-IFF_UP,	notealias),
	DEF_CMD("delete",	-IFF_UP,	notealias),
	DEF_CMD("remove",	-IFF_UP,	notealias),
#ifdef notdef
#define	EN_SWABIPS	0x1000
	DEF_CMD("swabips",	EN_SWABIPS,	setifflags),
	DEF_CMD("-swabips",	-EN_SWABIPS,	setifflags),
#endif
	DEF_CMD_ARG("netmask",			setifnetmask),
	DEF_CMD_ARG("metric",			setifmetric),
	DEF_CMD_ARG("broadcast",		setifbroadaddr),
	DEF_CMD_ARG2("tunnel",			settunnel),
	DEF_CMD("-tunnel", 0,			deletetunnel),
	DEF_CMD("deletetunnel", 0,		deletetunnel),
#ifdef JAIL
	DEF_CMD_ARG("vnet",			setifvnet),
	DEF_CMD_ARG("-vnet",			setifrvnet),
#endif
	DEF_CMD("link0",	IFF_LINK0,	setifflags),
	DEF_CMD("-link0",	-IFF_LINK0,	setifflags),
	DEF_CMD("link1",	IFF_LINK1,	setifflags),
	DEF_CMD("-link1",	-IFF_LINK1,	setifflags),
	DEF_CMD("link2",	IFF_LINK2,	setifflags),
	DEF_CMD("-link2",	-IFF_LINK2,	setifflags),
	DEF_CMD("monitor",	IFF_MONITOR,	setifflags),
	DEF_CMD("-monitor",	-IFF_MONITOR,	setifflags),
	DEF_CMD("mextpg",	IFCAP_MEXTPG,	setifcap),
	DEF_CMD("-mextpg",	-IFCAP_MEXTPG,	setifcap),
	DEF_CMD("staticarp",	IFF_STATICARP,	setifflags),
	DEF_CMD("-staticarp",	-IFF_STATICARP,	setifflags),
	DEF_CMD("stickyarp",	IFF_STICKYARP,	setifflags),
	DEF_CMD("-stickyarp",	-IFF_STICKYARP,	setifflags),
	DEF_CMD("rxcsum6",	IFCAP_RXCSUM_IPV6,	setifcap),
	DEF_CMD("-rxcsum6",	-IFCAP_RXCSUM_IPV6,	setifcap),
	DEF_CMD("txcsum6",	IFCAP_TXCSUM_IPV6,	setifcap),
	DEF_CMD("-txcsum6",	-IFCAP_TXCSUM_IPV6,	setifcap),
	DEF_CMD("rxcsum",	IFCAP_RXCSUM,	setifcap),
	DEF_CMD("-rxcsum",	-IFCAP_RXCSUM,	setifcap),
	DEF_CMD("txcsum",	IFCAP_TXCSUM,	setifcap),
	DEF_CMD("-txcsum",	-IFCAP_TXCSUM,	setifcap),
	DEF_CMD("netcons",	IFCAP_NETCONS,	setifcap),
	DEF_CMD("-netcons",	-IFCAP_NETCONS,	setifcap),
	DEF_CMD_ARG("pcp",			setifpcp),
	DEF_CMD("-pcp", 0,			disableifpcp),
	DEF_CMD("polling",	IFCAP_POLLING,	setifcap),
	DEF_CMD("-polling",	-IFCAP_POLLING,	setifcap),
	DEF_CMD("tso6",		IFCAP_TSO6,	setifcap),
	DEF_CMD("-tso6",	-IFCAP_TSO6,	setifcap),
	DEF_CMD("tso4",		IFCAP_TSO4,	setifcap),
	DEF_CMD("-tso4",	-IFCAP_TSO4,	setifcap),
	DEF_CMD("tso",		IFCAP_TSO,	setifcap),
	DEF_CMD("-tso",		-IFCAP_TSO,	setifcap),
	DEF_CMD("toe",		IFCAP_TOE,	setifcap),
	DEF_CMD("-toe",		-IFCAP_TOE,	setifcap),
	DEF_CMD("lro",		IFCAP_LRO,	setifcap),
	DEF_CMD("-lro",		-IFCAP_LRO,	setifcap),
	DEF_CMD("txtls",	IFCAP_TXTLS,	setifcap),
	DEF_CMD("-txtls",	-IFCAP_TXTLS,	setifcap),
	DEF_CMD_SARG("rxtls",	IFCAP2_RXTLS4_NAME "," IFCAP2_RXTLS6_NAME,
	    setifcapnv),
	DEF_CMD_SARG("-rxtls",	"-"IFCAP2_RXTLS4_NAME ",-" IFCAP2_RXTLS6_NAME,
	    setifcapnv),
	DEF_CMD("wol",		IFCAP_WOL,	setifcap),
	DEF_CMD("-wol",		-IFCAP_WOL,	setifcap),
	DEF_CMD("wol_ucast",	IFCAP_WOL_UCAST,	setifcap),
	DEF_CMD("-wol_ucast",	-IFCAP_WOL_UCAST,	setifcap),
	DEF_CMD("wol_mcast",	IFCAP_WOL_MCAST,	setifcap),
	DEF_CMD("-wol_mcast",	-IFCAP_WOL_MCAST,	setifcap),
	DEF_CMD("wol_magic",	IFCAP_WOL_MAGIC,	setifcap),
	DEF_CMD("-wol_magic",	-IFCAP_WOL_MAGIC,	setifcap),
	DEF_CMD("txrtlmt",	IFCAP_TXRTLMT,	setifcap),
	DEF_CMD("-txrtlmt",	-IFCAP_TXRTLMT,	setifcap),
	DEF_CMD("txtlsrtlmt",	IFCAP_TXTLS_RTLMT,	setifcap),
	DEF_CMD("-txtlsrtlmt",	-IFCAP_TXTLS_RTLMT,	setifcap),
	DEF_CMD("hwrxtstmp",	IFCAP_HWRXTSTMP,	setifcap),
	DEF_CMD("-hwrxtstmp",	-IFCAP_HWRXTSTMP,	setifcap),
	DEF_CMD("normal",	-IFF_LINK0,	setifflags),
	DEF_CMD("compress",	IFF_LINK0,	setifflags),
	DEF_CMD("noicmp",	IFF_LINK1,	setifflags),
	DEF_CMD_ARG("mtu",			setifmtu),
	DEF_CMD_ARG("name",			setifname),
};

static __constructor void
ifconfig_ctor(void)
{
	size_t i;

	for (i = 0; i < nitems(basic_cmds);  i++)
		cmd_register(&basic_cmds[i]);
}
