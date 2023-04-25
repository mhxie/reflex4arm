/*
 * Copyright (c) 2015-2017, Stanford University
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright 2013-16 Board of Trustees of Stanford University
 * Copyright 2013-16 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * list.h - The CCAN linked list library
 *
 * see http://ccodearchive.net/info/list.html
 * Licensed under BSD-MIT
 *
 * Two other types of lists were also added to better support hash tables
 * - "slist" is a singly linked list
 * - "hlist" is a doubly linked list with a single head pointer to save space.
 *
 * hlist's can only be traversed in the forward direction. However, unlike
 * slist's, they support constant time removals.
 */

#pragma once

#include <ix/stddef.h>

/**
 * struct list_node - an entry in a doubly-linked list
 * @next: next entry (self if empty)
 * @prev: previous entry (self if empty)
 *
 * This is used as an entry in a linked list.
 * Example:
 *	struct child {
 *		const char *name;
 *		// Linked list of all us children.
 *		struct list_node list;
 *	};
 */
struct list_node {
    struct list_node *next, *prev;
};

/**
 * struct list_head - the head of a doubly-linked list
 * @h: the list_head (containing next and prev pointers)
 *
 * This is used as the head of a linked list.
 * Example:
 *	struct parent {
 *		const char *name;
 *		struct list_head children;
 *		unsigned int num_children;
 *	};
 */
struct list_head {
    struct list_node n;
};

/**
 * list_check - check head of a list for consistency
 * @h: the list_head
 * @abortstr: the location to print on aborting, or NULL.
 *
 * Because list_nodes have redundant information, consistency checking between
 * the back and forward links can be done.  This is useful as a debugging check.
 * If @abortstr is non-NULL, that will be printed in a diagnostic if the list
 * is inconsistent, and the function will abort.
 *
 * Returns the list head if the list is consistent, NULL if not (it
 * can never return NULL if @abortstr is set).
 *
 * See also: list_check_node()
 *
 * Example:
 *	static void dump_parent(struct parent *p)
 *	{
 *		struct child *c;
 *
 *		printf("%s (%u children):\n", p->name, p->num_children);
 *		list_check(&p->children, "bad child list");
 *		list_for_each(&p->children, c, list)
 *			printf(" -> %s\n", c->name);
 *	}
 */
struct list_head *list_check(const struct list_head *h, const char *abortstr);

/**
 * list_check_node - check node of a list for consistency
 * @n: the list_node
 * @abortstr: the location to print on aborting, or NULL.
 *
 * Check consistency of the list node is in (it must be in one).
 *
 * See also: list_check()
 *
 * Example:
 *	static void dump_child(const struct child *c)
 *	{
 *		list_check_node(&c->list, "bad child list");
 *		printf("%s\n", c->name);
 *	}
 */
struct list_node *list_check_node(const struct list_node *n,
                                  const char *abortstr);

#ifdef CCAN_LIST_DEBUG
#define list_debug(h) list_check((h), __func__)
#define list_debug_node(n) list_check_node((n), __func__)
#else
#define list_debug(h) (h)
#define list_debug_node(n) (n)
#endif

/**
 * LIST_HEAD_INIT - initializer for an empty list_head
 * @name: the name of the list.
 *
 * Explicit initializer for an empty list.
 *
 * See also:
 *	LIST_HEAD, list_head_init()
 *
 * Example:
 *	static struct list_head my_list = LIST_HEAD_INIT(my_list);
 */
#define LIST_HEAD_INIT(name) \
    {                        \
        { &name.n, &name.n } \
    }

/**
 * LIST_HEAD - define and initialize an empty list_head
 * @name: the name of the list.
 *
 * The LIST_HEAD macro defines a list_head and initializes it to an empty
 * list.  It can be prepended by "static" to define a static list_head.
 *
 * See also:
 *	LIST_HEAD_INIT, list_head_init()
 *
 * Example:
 *	static LIST_HEAD(my_global_list);
 */
#define LIST_HEAD_(name) struct list_head name = LIST_HEAD_INIT(name)

/**
 * list_head_init - initialize a list_head
 * @h: the list_head to set to the empty list
 *
 * Example:
 *	...
 *	struct parent *parent = malloc(sizeof(*parent));
 *
 *	list_head_init(&parent->children);
 *	parent->num_children = 0;
 */
static inline void list_head_init(struct list_head *h) {
    h->n.next = h->n.prev = &h->n;
}

/**
 * list_head_reset - reset the list_head to a node
 * @h: the list_head to set to the empty list
 * @n: the list_node to reset as the new first entry.
 *
 */
static inline void list_head_reset(struct list_head *h, struct list_node *n) {
    h->n.prev->next = h->n.next;
    h->n.next->prev = h->n.prev;
    h->n.next = n;
    h->n.prev = n->prev;
    n->prev->next = &h->n;
    n->prev = &h->n;
    (void)list_debug(h);
}

/**
 * list_add - add an entry at the start of a linked list.
 * @h: the list_head to add the node to
 * @n: the list_node to add to the list.
 *
 * The list_node does not need to be initialized; it will be overwritten.
 * Example:
 *	struct child *child = malloc(sizeof(*child));
 *
 *	child->name = "marvin";
 *	list_add(&parent->children, &child->list);
 *	parent->num_children++;
 */
static inline void list_add(struct list_head *h, struct list_node *n) {
    n->next = h->n.next;
    n->prev = &h->n;
    h->n.next->prev = n;
    h->n.next = n;
    (void)list_debug(h);
}

/**
 * list_add_tail - add an entry at the end of a linked list.
 * @h: the list_head to add the node to
 * @n: the list_node to add to the list.
 *
 * The list_node does not need to be initialized; it will be overwritten.
 * Example:
 *	list_add_tail(&parent->children, &child->list);
 *	parent->num_children++;
 */
static inline void list_add_tail(struct list_head *h, struct list_node *n) {
    n->next = &h->n;
    n->prev = h->n.prev;
    h->n.prev->next = n;
    h->n.prev = n;
    (void)list_debug(h);
}

/**
 * list_empty - is a list empty?
 * @h: the list_head
 *
 * If the list is empty, returns true.
 *
 * Example:
 *	assert(list_empty(&parent->children) == (parent->num_children == 0));
 */
static inline bool list_empty(const struct list_head *h) {
    (void)list_debug(h);
    return h->n.next == &h->n;
}

/**
 * list_del - delete an entry from an (unknown) linked list.
 * @n: the list_node to delete from the list.
 *
 * Note that this leaves @n in an undefined state; it can be added to
 * another list, but not deleted again.
 *
 * See also:
 *	list_del_from()
 *
 * Example:
 *	list_del(&child->list);
 *	parent->num_children--;
 */
static inline void list_del(struct list_node *n) {
    (void)list_debug_node(n);
    n->next->prev = n->prev;
    n->prev->next = n->next;
#ifdef CCAN_LIST_DEBUG
    /* Catch use-after-del. */
    n->next = n->prev = NULL;
#endif
}

/**
 * list_del_from - delete an entry from a known linked list.
 * @h: the list_head the node is in.
 * @n: the list_node to delete from the list.
 *
 * This explicitly indicates which list a node is expected to be in,
 * which is better documentation and can catch more bugs.
 *
 * See also: list_del()
 *
 * Example:
 *	list_del_from(&parent->children, &child->list);
 *	parent->num_children--;
 */
static inline void list_del_from(struct list_head *h, struct list_node *n) {
#ifdef CCAN_LIST_DEBUG
    {
        /* Thorough check: make sure it was in list! */
        struct list_node *i;
        for (i = h->n.next; i != n; i = i->next) assert(i != &h->n);
    }
    assert(!list_empty(h));
#endif /* CCAN_LIST_DEBUG */

    /* Quick test that catches a surprising number of bugs. */
    list_del(n);
}

/**
 * list_entry - convert a list_node back into the structure containing it.
 * @n: the list_node
 * @type: the type of the entry
 * @member: the list_node member of the type
 *
 * Example:
 *	// First list entry is children.next; convert back to child.
 *	child = list_entry(parent->children.n.next, struct child, list);
 *
 * See Also:
 *	list_top(), list_for_each()
 */
#define list_entry(n, type, member) container_of(n, type, member)

/**
 * list_top - get the first entry in a list
 * @h: the list_head
 * @type: the type of the entry
 * @member: the list_node member of the type
 *
 * If the list is empty, returns NULL.
 *
 * Example:
 *	struct child *first;
 *	first = list_top(&parent->children, struct child, list);
 *	if (!first)
 *		printf("Empty list!\n");
 */
#define list_top(h, type, member) \
    ((type *)list_top_((h), list_off_(type, member)))

static inline const void *list_top_(const struct list_head *h, size_t off) {
    if (list_empty(h)) return NULL;
    return (const char *)h->n.next - off;
}

/**
 * list_pop - remove the first entry in a list
 * @h: the list_head
 * @type: the type of the entry
 * @member: the list_node member of the type
 *
 * If the list is empty, returns NULL.
 *
 * Example:
 *	struct child *one;
 *	one = list_pop(&parent->children, struct child, list);
 *	if (!one)
 *		printf("Empty list!\n");
 */
#define list_pop(h, type, member) \
    ((type *)list_pop_((h), list_off_(type, member)))

static inline const void *list_pop_(const struct list_head *h, size_t off) {
    struct list_node *n;

    if (list_empty(h)) return NULL;
    n = h->n.next;
    list_del(n);
    return (const char *)n - off;
}

/**
 * list_tail - get the last entry in a list
 * @h: the list_head
 * @type: the type of the entry
 * @member: the list_node member of the type
 *
 * If the list is empty, returns NULL.
 *
 * Example:
 *	struct child *last;
 *	last = list_tail(&parent->children, struct child, list);
 *	if (!last)
 *		printf("Empty list!\n");
 */
#define list_tail(h, type, member) \
    ((type *)list_tail_((h), list_off_(type, member)))

static inline const void *list_tail_(const struct list_head *h, size_t off) {
    if (list_empty(h)) return NULL;
    return (const char *)h->n.prev - off;
}

/**
 * list_for_each - iterate through a list.
 * @h: the list_head (warning: evaluated multiple times!)
 * @i: the structure containing the list_node
 * @member: the list_node member of the structure
 *
 * This is a convenient wrapper to iterate @i over the entire list.  It's
 * a for loop, so you can break and continue as normal.
 *
 * Example:
 *	list_for_each(&parent->children, child, list)
 *		printf("Name: %s\n", child->name);
 */
#define list_for_each(h, i, member) \
    list_for_each_off(h, i, list_off_var_(i, member))

/**
 * list_for_each_rev - iterate through a list backwards.
 * @h: the list_head
 * @i: the structure containing the list_node
 * @member: the list_node member of the structure
 *
 * This is a convenient wrapper to iterate @i over the entire list.  It's
 * a for loop, so you can break and continue as normal.
 *
 * Example:
 *	list_for_each_rev(&parent->children, child, list)
 *		printf("Name: %s\n", child->name);
 */
#define list_for_each_rev(h, i, member)                          \
    for (i = container_of_var(list_debug(h)->n.prev, i, member); \
         &i->member != &(h)->n;                                  \
         i = container_of_var(i->member.prev, i, member))

/**
 * list_for_each_safe - iterate through a list, maybe during deletion
 * @h: the list_head
 * @i: the structure containing the list_node
 * @nxt: the structure containing the list_node
 * @member: the list_node member of the structure
 *
 * This is a convenient wrapper to iterate @i over the entire list.  It's
 * a for loop, so you can break and continue as normal.  The extra variable
 * @nxt is used to hold the next element, so you can delete @i from the list.
 *
 * Example:
 *	struct child *next;
 *	list_for_each_safe(&parent->children, child, next, list) {
 *		list_del(&child->list);
 *		parent->num_children--;
 *	}
 */
#define list_for_each_safe(h, i, nxt, member) \
    list_for_each_safe_off(h, i, nxt, list_off_var_(i, member))

/**
 * list_next - get the next entry in a list
 * @h: the list_head
 * @i: a pointer to an entry in the list.
 * @member: the list_node member of the structure
 *
 * If @i was the last entry in the list, returns NULL.
 *
 * Example:
 *	struct child *second;
 *	second = list_next(&parent->children, first, list);
 *	if (!second)
 *		printf("No second child!\n");
 */
#define list_next(h, i, member)                                          \
    ((list_typeof(i))list_entry_or_null(list_debug(h), (i)->member.next, \
                                        list_off_var_((i), member)))

/**
 * list_prev - get the previous entry in a list
 * @h: the list_head
 * @i: a pointer to an entry in the list.
 * @member: the list_node member of the structure
 *
 * If @i was the first entry in the list, returns NULL.
 *
 * Example:
 *	first = list_prev(&parent->children, second, list);
 *	if (!first)
 *		printf("Can't go back to first child?!\n");
 */
#define list_prev(h, i, member)                                          \
    ((list_typeof(i))list_entry_or_null(list_debug(h), (i)->member.prev, \
                                        list_off_var_((i), member)))

/**
 * list_append_list - empty one list onto the end of another.
 * @to: the list to append into
 * @from: the list to empty.
 *
 * This takes the entire contents of @from and moves it to the end of
 * @to.  After this @from will be empty.
 *
 * Example:
 *	struct list_head adopter;
 *
 *	list_append_list(&adopter, &parent->children);
 *	assert(list_empty(&parent->children));
 *	parent->num_children = 0;
 */
static inline void list_append_list(struct list_head *to,
                                    struct list_head *from) {
    struct list_node *from_tail = list_debug(from)->n.prev;
    struct list_node *to_tail = list_debug(to)->n.prev;

    /* Sew in head and entire list. */
    to->n.prev = from_tail;
    from_tail->next = &to->n;
    to_tail->next = &from->n;
    from->n.prev = to_tail;

    /* Now remove head. */
    list_del(&from->n);
    list_head_init(from);
}

/**
 * list_prepend_list - empty one list into the start of another.
 * @to: the list to prepend into
 * @from: the list to empty.
 *
 * This takes the entire contents of @from and moves it to the start
 * of @to.  After this @from will be empty.
 *
 * Example:
 *	list_prepend_list(&adopter, &parent->children);
 *	assert(list_empty(&parent->children));
 *	parent->num_children = 0;
 */
static inline void list_prepend_list(struct list_head *to,
                                     struct list_head *from) {
    struct list_node *from_tail = list_debug(from)->n.prev;
    struct list_node *to_head = list_debug(to)->n.next;

    /* Sew in head and entire list. */
    to->n.next = &from->n;
    from->n.prev = &to->n;
    to_head->prev = from_tail;
    from_tail->next = to_head;

    /* Now remove head. */
    list_del(&from->n);
    list_head_init(from);
}

/**
 * list_for_each_off - iterate through a list of memory regions.
 * @h: the list_head
 * @i: the pointer to a memory region wich contains list node data.
 * @off: offset(relative to @i) at which list node data resides.
 *
 * This is a low-level wrapper to iterate @i over the entire list, used to
 * implement all oher, more high-level, for-each constructs. It's a for loop,
 * so you can break and continue as normal.
 *
 * WARNING! Being the low-level macro that it is, this wrapper doesn't know
 * nor care about the type of @i. The only assumtion made is that @i points
 * to a chunk of memory that at some @offset, relative to @i, contains a
 * properly filled `struct node_list' which in turn contains pointers to
 * memory chunks and it's turtles all the way down. Whith all that in mind
 * remember that given the wrong pointer/offset couple this macro will
 * happilly churn all you memory untill SEGFAULT stops it, in other words
 * caveat emptor.
 *
 * It is worth mentioning that one of legitimate use-cases for that wrapper
 * is operation on opaque types with known offset for `struct list_node'
 * member(preferably 0), because it allows you not to disclose the type of
 * @i.
 *
 * Example:
 *	list_for_each_off(&parent->children, child,
 *				offsetof(struct child, list))
 *		printf("Name: %s\n", child->name);
 */
#define list_for_each_off(h, i, off)                                        \
    for (i = list_node_to_off_(list_debug(h)->n.next, (off));               \
         list_node_from_off_((void *)i, (off)) != &(h)->n;                  \
         i = list_node_to_off_(list_node_from_off_((void *)i, (off))->next, \
                               (off)))

/**
 * list_for_each_safe_off - iterate through a list of memory regions, maybe
 * during deletion
 * @h: the list_head
 * @i: the pointer to a memory region wich contains list node data.
 * @nxt: the structure containing the list_node
 * @off: offset(relative to @i) at which list node data resides.
 *
 * For details see `list_for_each_off' and `list_for_each_safe'
 * descriptions.
 *
 * Example:
 *	list_for_each_safe_off(&parent->children, child,
 *		next, offsetof(struct child, list))
 *		printf("Name: %s\n", child->name);
 */
#define list_for_each_safe_off(h, i, nxt, off)                               \
    for (i = list_node_to_off_(list_debug(h)->n.next, (off)),                \
        nxt = list_node_to_off_(list_node_from_off_(i, (off))->next, (off)); \
         list_node_from_off_(i, (off)) != &(h)->n; i = nxt,                  \
        nxt = list_node_to_off_(list_node_from_off_(i, (off))->next, (off)))

/* Other -off variants. */
#define list_entry_off(n, type, off) ((type *)list_node_from_off_((n), (off)))

#define list_head_off(h, type, off) ((type *)list_head_off((h), (off)))

#define list_tail_off(h, type, off) ((type *)list_tail_((h), (off)))

#define list_add_off(h, n, off) list_add((h), list_node_from_off_((n), (off)))

#define list_del_off(n, off) list_del(list_node_from_off_((n), (off)))

#define list_del_from_off(h, n, off) \
    list_del_from(h, list_node_from_off_((n), (off)))

/* Offset helper functions so we only single-evaluate. */
static inline void *list_node_to_off_(struct list_node *node, size_t off) {
    return (void *)((char *)node - off);
}
static inline struct list_node *list_node_from_off_(void *ptr, size_t off) {
    return (struct list_node *)((char *)ptr + off);
}

/* Get the offset of the member, but make sure it's a list_node. */
#define list_off_(type, member) \
    (offsetof(type, member) + check_type(((type *)0)->member, struct list_node))

#define list_off_var_(var, member) \
    (offsetof(typeof(*var), member) + check_type(var->member, struct list_node))

#define list_typeof(var) typeof(var)

/* Returns member, or NULL if at end of list. */
static inline void *list_entry_or_null(const struct list_head *h,
                                       const struct list_node *n, size_t off) {
    if (n == &h->n) return NULL;
    return (char *)n - off;
}

struct slist_node {
    struct slist_node *next;
};

struct slist_head {
    struct slist_node head;
};

static inline void slist_init_head(struct slist_head *h) {
    h->head.next = NULL;
}

static inline void slist_add_head(struct slist_head *h, struct slist_node *n) {
    n->next = h->head.next;
    h->head.next = n;
}

static inline void slist_del_head(struct slist_head *h) {
    h->head.next = h->head.next->next;
}

static inline void slist_del(struct slist_node *prev, struct slist_node *n) {
    prev->next = n->next;
}

static inline bool slist_empty(struct slist_head *h) {
    return h->head.next == NULL;
}

#define slist_entry(n, type, member) container_of(n, type, member)

#define slist_for_each(h, pos) \
    for ((pos) = (h)->head.next; (pos); (pos) = (pos)->next)

#define slist_for_each_prev(h, pos, ppos)                       \
    for ((ppos) = &(h)->head; ((pos) = ((ppos)->next)) != NULL; \
         (ppos) = (ppos)->next)

struct hlist_node {
    /* WARNING: @next has to line up with @head in struct hlist_head */
    struct hlist_node *next, *prev;
};

struct hlist_head {
    struct hlist_node *head;
};

static inline void hlist_init_head(struct hlist_head *h) { h->head = NULL; }

static inline void hlist_add_head(struct hlist_head *h, struct hlist_node *n) {
    n->next = h->head;
    n->prev = (struct hlist_node *)h;
    if (n->next) n->next->prev = n;
    h->head = n;
}

static inline void hlist_del_head(struct hlist_head *h) {
    h->head = h->head->next;
    if (h->head) h->head->prev = (struct hlist_node *)h;
}

static inline void hlist_del(struct hlist_node *n) {
    n->prev->next = n->next;
    if (n->next) n->next->prev = n->prev;
}

static inline bool hlist_empty(struct hlist_head *h) { return h->head == NULL; }

#define hlist_entry(n, type, member) container_of(n, type, member)

#define hlist_for_each(h, pos) \
    for ((pos) = (h)->head; (pos); (pos) = (pos)->next)

#define hlist_for_each_safe(h, pos, tmp) \
    for ((pos) = (h)->head; (pos) && ((tmp) = (pos)->next, 1); (pos) = (tmp))
