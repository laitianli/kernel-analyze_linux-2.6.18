/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  linux/include/linux/rbtree.h

  To use rbtrees you'll have to implement your own insert and search cores.
  This will avoid us to use callbacks and to drop drammatically performances.
  I know it's not the cleaner way,  but in C (not in C++) to get
  performances and genericity...

  Some example of insert and search follows here. The search is a plain
  normal search over an ordered tree. The insert instead must be implemented
  int two steps: as first thing the code must insert the element in
  order as a red leaf in the tree, then the support library function
  rb_insert_color() must be called. Such function will do the
  not trivial work to rebalance the rbtree if necessary.

-----------------------------------------------------------------------
static inline struct page * rb_search_page_cache(struct inode * inode,
						 unsigned long offset)
{
	struct rb_node * n = inode->i_rb_page_cache.rb_node;
	struct page * page;

	while (n)
	{
		page = rb_entry(n, struct page, rb_page_cache);

		if (offset < page->offset)
			n = n->rb_left;
		else if (offset > page->offset)
			n = n->rb_right;
		else
			return page;
	}
	return NULL;
}

static inline struct page * __rb_insert_page_cache(struct inode * inode,
						   unsigned long offset,
						   struct rb_node * node)
{
	struct rb_node ** p = &inode->i_rb_page_cache.rb_node;
	struct rb_node * parent = NULL;
	struct page * page;

	while (*p)
	{
		parent = *p;
		page = rb_entry(parent, struct page, rb_page_cache);

		if (offset < page->offset)
			p = &(*p)->rb_left;
		else if (offset > page->offset)
			p = &(*p)->rb_right;
		else
			return page;
	}

	rb_link_node(node, parent, p);

	return NULL;
}

static inline struct page * rb_insert_page_cache(struct inode * inode,
						 unsigned long offset,
						 struct rb_node * node)
{
	struct page * ret;
	if ((ret = __rb_insert_page_cache(inode, offset, node)))
		goto out;
	rb_insert_color(node, &inode->i_rb_page_cache);
 out:
	return ret;
}
-----------------------------------------------------------------------
*/
/*
红黑树的定义:
1.所有的节点非黑即白。
2.根节点为黑色。
3.所有的叶子节点都是黑色（NULL节点也为黑色）
4.每个红色节点的两个叶子节点都是黑色。
5.每个节点到其叶子节点的所有路径包含的黑色节点数都一样。
6.红黑树是一棵AVL树(平衡二叉树)

AVL树的定义:
每一个平衡因子只能是: 1、0 或 -1,如果是-2与2或者大小小于这个数，则不是AVL树
注:节点的平衡因子是它的左子树的高度减去它的右子树的高度
*/
#ifndef	_LINUX_RBTREE_H
#define	_LINUX_RBTREE_H

#include <linux/kernel.h>
#include <linux/stddef.h>

/**ltl
注:1. __attribute__((aligned(sizeof(long))))这个保证在申请rb_node值时，是按sizeof(long)对齐，
因此就保证了每个节点的首地址的第0位和第1位都是0。这样用第0位的值来标示红黑树的红黑属性而不会影响到父亲节点的地址。
因此rb_parent_color域才能用来记录父亲节点的地址和红黑标志.
*/
struct rb_node
{
	unsigned long  rb_parent_color;	//bit[0]=0:表示此节点"红"，
									//bit[0]=1:表示此节点"黑"
									//bit[31-2]:表示父亲节点的地址。
#define	RB_RED		0
#define	RB_BLACK	1
	struct rb_node *rb_right;	//左子树节点
	struct rb_node *rb_left;	//右子树节点
} __attribute__((aligned(sizeof(long))));
    /* The alignment might seem pointless, but allegedly CRIS needs it */

struct rb_root
{
	struct rb_node *rb_node;
};

//求父节点
#define rb_parent(r)   ((struct rb_node *)((r)->rb_parent_color & ~3))
//求颜色位
#define rb_color(r)   ((r)->rb_parent_color & 1)
//是否红节点
#define rb_is_red(r)   (!rb_color(r))
//是否黑节点
#define rb_is_black(r) rb_color(r)
//设置节点为"红"
#define rb_set_red(r)  do { (r)->rb_parent_color &= ~1; } while (0)
//设置节点为"黑"
#define rb_set_black(r)  do { (r)->rb_parent_color |= 1; } while (0)
//设置rb的父节点p
static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p)
{
	rb->rb_parent_color = (rb->rb_parent_color & 3) | (unsigned long)p;
}
//设置rb的颜色
static inline void rb_set_color(struct rb_node *rb, int color)
{
	rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

#define RB_ROOT	(struct rb_root) { NULL, }
#define	rb_entry(ptr, type, member) container_of(ptr, type, member)
/* 是否是空树 */
#define RB_EMPTY_ROOT(root)	((root)->rb_node == NULL)
/* 是否是孤立节点 */
#define RB_EMPTY_NODE(node)	(rb_parent(node) != node)
/* 设置node为孤立节点 */
#define RB_CLEAR_NODE(node)	(rb_set_parent(node, node))

extern void rb_insert_color(struct rb_node *, struct rb_root *);
extern void rb_erase(struct rb_node *, struct rb_root *);

/* Find logical next and previous nodes in a tree */
extern struct rb_node *rb_next(struct rb_node *);
extern struct rb_node *rb_prev(struct rb_node *);
extern struct rb_node *rb_first(struct rb_root *);
extern struct rb_node *rb_last(struct rb_root *);

/* Fast replacement of a single node without remove/rebalance/add/rebalance */
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new, 
			    struct rb_root *root);

/**ltl
功能:把node插入到rb_link树中，注:rb_link之所以用"**"是为了方便插入后，直接返回树根。可参见:__deadline_add_drq_rb
*/
static inline void rb_link_node(struct rb_node * node, struct rb_node * parent,
				struct rb_node ** rb_link)
{
	node->rb_parent_color = (unsigned long )parent;
	node->rb_left = node->rb_right = NULL;

	*rb_link = node;
}

#endif	/* _LINUX_RBTREE_H */
