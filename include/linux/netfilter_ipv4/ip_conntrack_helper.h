/* IP connection tracking helpers. */
#ifndef _IP_CONNTRACK_HELPER_H
#define _IP_CONNTRACK_HELPER_H
#include <linux/netfilter_ipv4/ip_conntrack.h>

struct module;
/* 连接跟踪的辅助数据结构
 * 此数据结构的主要运用场景是: 当一个数据包即将离开netfilter框架之前，可以对数据包再做一次处理。
 * helper模块被注册到低优先级的LOCAL_OUT,POST_ROUTING两个hook点上。 
 */
struct ip_conntrack_helper
{	
	struct list_head list; 		/* Internal use. */

	const char *name;		/* name of the module */
	struct module *me;		/* pointer to self */
	unsigned int max_expected;	/* Maximum number of concurrent 
					 * expected connections */
	unsigned int timeout;		/* timeout for expecteds */

	/* Mask of things we will help (compared against server response) */
	struct ip_conntrack_tuple tuple;
	struct ip_conntrack_tuple mask;
	
	/* Function to call when data passes; return verdict, or -1 to
           invalidate. */
	int (*help)(struct sk_buff **pskb,
		    struct ip_conntrack *ct,
		    enum ip_conntrack_info conntrackinfo);

	int (*to_nfattr)(struct sk_buff *skb, const struct ip_conntrack *ct);
};

extern int ip_conntrack_helper_register(struct ip_conntrack_helper *);
extern void ip_conntrack_helper_unregister(struct ip_conntrack_helper *);

/* Allocate space for an expectation: this is mandatory before calling 
   ip_conntrack_expect_related.  You will have to call put afterwards. */
extern struct ip_conntrack_expect *
ip_conntrack_expect_alloc(struct ip_conntrack *master);
extern void ip_conntrack_expect_put(struct ip_conntrack_expect *exp);

/* Add an expected connection: can have more than one per connection */
extern int ip_conntrack_expect_related(struct ip_conntrack_expect *exp);
extern void ip_conntrack_unexpect_related(struct ip_conntrack_expect *exp);

#endif /*_IP_CONNTRACK_HELPER_H*/
