#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "debug.h"
#include "engine.h"
#include "move.h"
#include "playout.h"
#include "tactics.h"
#include "uct/internal.h"
#include "uct/prior.h"
#include "uct/tree.h"


static struct tree_node *
tree_init_node(struct tree *t, coord_t coord, int depth)
{
	struct tree_node *n = calloc(1, sizeof(*n));
	if (!n) {
		fprintf(stderr, "tree_init_node(): OUT OF MEMORY\n");
		exit(1);
	}
	n->coord = coord;
	n->depth = depth;
	static long c = 1000000;
	n->hash = c++;
	if (depth > t->max_depth)
		t->max_depth = depth;
	return n;
}

struct tree *
tree_init(struct board *board, enum stone color)
{
	struct tree *t = calloc(1, sizeof(*t));
	t->board = board;
	/* The root PASS move is only virtual, we never play it. */
	t->root = tree_init_node(t, pass, 0);
	t->root_symmetry = board->symmetry;
	t->root_color = stone_other(color); // to research black moves, root will be white
	return t;
}


static void
tree_done_node(struct tree *t, struct tree_node *n)
{
	struct tree_node *ni = n->children;
	while (ni) {
		struct tree_node *nj = ni->sibling;
		tree_done_node(t, ni);
		ni = nj;
	}
	free(n);
}

void
tree_done(struct tree *t)
{
	tree_done_node(t, t->root);
	free(t);
}


static void
tree_node_dump(struct tree *tree, struct tree_node *node, int l, int thres)
{
	for (int i = 0; i < l; i++) fputc(' ', stderr);
	int children = 0;
	for (struct tree_node *ni = node->children; ni; ni = ni->sibling)
		children++;
	/* We use 1 as parity, since for all nodes we want to know the
	 * win probability of _us_, not the node color. */
	fprintf(stderr, "[%s] %f (%d/%d playouts [prior %d/%d amaf %d/%d]; hints %x; %d children) <%lld>\n",
		coord2sstr(node->coord, tree->board),
		tree_node_get_value(tree, node, u, 1),
		tree_node_get_wins(tree, node, u, 1), node->u.playouts,
		tree_node_get_wins(tree, node, prior, 1), node->prior.playouts,
		tree_node_get_wins(tree, node, amaf, 1), node->amaf.playouts,
		node->hints, children, node->hash);

	/* Print nodes sorted by #playouts. */

	struct tree_node *nbox[1000]; int nboxl = 0;
	for (struct tree_node *ni = node->children; ni; ni = ni->sibling)
		if (ni->u.playouts > thres)
			nbox[nboxl++] = ni;

	while (true) {
		int best = -1;
		for (int i = 0; i < nboxl; i++)
			if (nbox[i] && (best < 0 || nbox[i]->u.playouts > nbox[best]->u.playouts))
				best = i;
		if (best < 0)
			break;
		tree_node_dump(tree, nbox[best], l + 1, /* node->u.value < 0.1 ? 0 : */ thres);
		nbox[best] = NULL;
	}
}

void
tree_dump(struct tree *tree, int thres)
{
	if (thres && tree->root->u.playouts / thres > 100) {
		/* Be a bit sensible about this; the opening book can create
		 * huge dumps at first. */
		thres = tree->root->u.playouts / 100 * (thres < 1000 ? 1 : thres / 1000);
	}
	tree_node_dump(tree, tree->root, 0, thres);
}


static char *
tree_book_name(struct board *b)
{
	static char buf[256];
	if (b->handicap > 0) {
		sprintf(buf, "uctbook-%d-%02.01f-h%d.pachitree", b->size - 2, b->komi, b->handicap);
	} else {
		sprintf(buf, "uctbook-%d-%02.01f.pachitree", b->size - 2, b->komi);
	}
	return buf;
}

static void
tree_node_save(FILE *f, struct tree_node *node, int thres)
{
	fputc(1, f);
	fwrite(((void *) node) + offsetof(struct tree_node, depth),
	       sizeof(struct tree_node) - offsetof(struct tree_node, depth),
	       1, f);

	if (node->u.playouts >= thres)
		for (struct tree_node *ni = node->children; ni; ni = ni->sibling)
			tree_node_save(f, ni, thres);

	fputc(0, f);
}

void
tree_save(struct tree *tree, struct board *b, int thres)
{
	char *filename = tree_book_name(b);
	FILE *f = fopen(filename, "wb");
	if (!f) {
		perror("fopen");
		return;
	}
	tree_node_save(f, tree->root, thres);
	fputc(0, f);
	fclose(f);
}


void
tree_node_load(FILE *f, struct tree_node *node, int *num)
{
	(*num)++;

	fread(((void *) node) + offsetof(struct tree_node, depth),
	       sizeof(struct tree_node) - offsetof(struct tree_node, depth),
	       1, f);

	/* Keep values in sane scale, otherwise we start overflowing.
	 * We may go slow here but we must be careful about not getting
	 * too huge integers.*/
#define MAX_PLAYOUTS	10000000
	if (node->u.playouts > MAX_PLAYOUTS) {
		int over = node->u.playouts - MAX_PLAYOUTS;
		node->u.wins -= ((double) node->u.wins / node->u.playouts) * over;
		node->u.playouts = MAX_PLAYOUTS;
	}
	if (node->amaf.playouts > MAX_PLAYOUTS) {
		int over = node->amaf.playouts - MAX_PLAYOUTS;
		node->amaf.wins -= ((double) node->amaf.wins / node->amaf.playouts) * over;
		node->amaf.playouts = MAX_PLAYOUTS;
	}

	memcpy(&node->pamaf, &node->amaf, sizeof(node->amaf));
	memcpy(&node->pu, &node->u, sizeof(node->u));

	struct tree_node *ni = NULL, *ni_prev = NULL;
	while (fgetc(f)) {
		ni_prev = ni; ni = calloc(1, sizeof(*ni));
		if (!node->children)
			node->children = ni;
		else
			ni_prev->sibling = ni;
		ni->parent = node;
		tree_node_load(f, ni, num);
	}
}

void
tree_load(struct tree *tree, struct board *b)
{
	char *filename = tree_book_name(b);
	FILE *f = fopen(filename, "rb");
	if (!f)
		return;

	fprintf(stderr, "Loading opening book %s...\n", filename);

	int num = 0;
	if (fgetc(f))
		tree_node_load(f, tree->root, &num);
	fprintf(stderr, "Loaded %d nodes.\n", num);

	fclose(f);
}


static struct tree_node *
tree_node_copy(struct tree_node *node)
{
	struct tree_node *n2 = malloc(sizeof(*n2));
	*n2 = *node;
	if (!node->children)
		return n2;
	struct tree_node *ni = node->children;
	struct tree_node *ni2 = tree_node_copy(ni);
	n2->children = ni2; ni2->parent = n2;
	while ((ni = ni->sibling)) {
		ni2->sibling = tree_node_copy(ni);
		ni2 = ni2->sibling; ni2->parent = n2;
	}
	return n2;
}

struct tree *
tree_copy(struct tree *tree)
{
	struct tree *t2 = malloc(sizeof(*t2));
	*t2 = *tree;
	t2->root = tree_node_copy(tree->root);
	return t2;
}


static void
tree_node_merge(struct tree_node *dest, struct tree_node *src, bool amaf_prior)
{
	/* Do not merge nodes that weren't touched at all. */
	assert(dest->pamaf.playouts == src->pamaf.playouts);
	assert(dest->pu.playouts == src->pu.playouts);
	if (src->amaf.playouts - src->pamaf.playouts == 0
	    && src->u.playouts - src->pu.playouts == 0) {
		return;
	}

	dest->hints |= src->hints;

	/* Merge the children, both are coord-sorted lists. */
	struct tree_node *di = dest->children, **dref = &dest->children;
	struct tree_node *si = src->children, **sref = &src->children;
	while (di && si) {
		if (di->coord != si->coord) {
			/* src has some extra items or misses di */
			struct tree_node *si2 = si->sibling;
			while (si2 && di->coord != si2->coord) {
				si2 = si2->sibling;
			}
			if (!si2)
				goto next_di; /* src misses di, move on */
			/* chain the extra [si,si2) items before di */
			(*dref) = si;
			while (si->sibling != si2) {
				si->parent = dest;
				si = si->sibling;
			}
			si->parent = dest;
			si->sibling = di;
			si = si2;
			(*sref) = si;
		}
		/* Matching nodes - recurse... */
		tree_node_merge(di, si, amaf_prior);
		/* ...and move on. */
		sref = &si->sibling; si = si->sibling;
next_di:
		dref = &di->sibling; di = di->sibling;
	}
	if (si) {
		/* Some outstanding nodes are left on src side, rechain
		 * them to dst. */
		(*dref) = si;
		while (si) {
			si->parent = dest;
			si = si->sibling;
		}
		(*sref) = NULL;
	}

	/* Priors should be constant. */
	assert(dest->prior.playouts == src->prior.playouts && dest->prior.wins == src->prior.wins);

	dest->amaf.playouts += src->amaf.playouts;
	dest->amaf.wins += src->amaf.wins;
	if (dest->amaf.playouts)
		tree_update_node_rvalue(dest, amaf_prior);

	dest->u.playouts += src->u.playouts;
	dest->u.wins += src->u.wins;
	if (dest->u.playouts)
		tree_update_node_value(dest, amaf_prior);
}

/* Merge two trees built upon the same board. Note that the operation is
 * destructive on src. */
void
tree_merge(struct tree *dest, struct tree *src, bool amaf_prior)
{
	if (src->max_depth > dest->max_depth)
		dest->max_depth = src->max_depth;
	tree_node_merge(dest->root, src->root, amaf_prior);
}


static void
tree_node_normalize(struct tree_node *node, int factor)
{
	for (struct tree_node *ni = node->children; ni; ni = ni->sibling)
		tree_node_normalize(ni, factor);

#define normalize(s1, s2, t) node->s2.t = node->s1.t + (node->s2.t - node->s1.t) / factor;
	normalize(pamaf, amaf, playouts);
	normalize(pamaf, amaf, wins);
	memcpy(&node->pamaf, &node->amaf, sizeof(node->amaf));

	normalize(pu, u, playouts);
	normalize(pu, u, wins);
	memcpy(&node->pu, &node->u, sizeof(node->u));
#undef normalize
}

/* Normalize a tree, dividing the amaf and u values by given
 * factor; otherwise, simulations run in independent threads
 * two trees built upon the same board. To correctly handle
 * results taken from previous simulation run, they are backed
 * up in tree. */
void
tree_normalize(struct tree *tree, int factor)
{
	tree_node_normalize(tree->root, factor);
}


/* Tree symmetry: When possible, we will localize the tree to a single part
 * of the board in tree_expand_node() and possibly flip along symmetry axes
 * to another part of the board in tree_promote_at(). We follow b->symmetry
 * guidelines here. */


void
tree_expand_node(struct tree *t, struct tree_node *node, struct board *b, enum stone color, int radar, struct uct *u, int parity)
{
	/* First, get a map of prior values to initialize the new
	 * nodes with. */
	struct prior_map map = {
		.b = b,
		.to_play = color,
		.parity = tree_parity(t, parity),
	};
	// Include pass in the prior map.
	struct move_stats map_prior[board_size2(b) + 1]; map.prior = &map_prior[1];
	bool map_consider[board_size2(b) + 1]; map.consider = &map_consider[1];
	memset(map_prior, 0, sizeof(map_prior));
	memset(map_consider, 0, sizeof(map_consider));
	struct move pm = { .color = color };
	map.consider[pass] = true;
	foreach_point(b) {
		if (board_at(b, c) != S_NONE)
			continue;
		/* This looks very useful on large boards - weeds out huge amount of crufty moves. */
		if (b->hash /* not empty board */ && radar && !board_stone_radar(b, c, radar))
			continue;
		pm.coord = c;
		if (!board_is_valid_move(b, &pm))
			continue;
		map.consider[c] = true;
	} foreach_point_end;
	uct_prior(u, node, &map);

	/* Now, create the nodes. */
	struct tree_node *ni = tree_init_node(t, pass, node->depth + 1);
	ni->parent = node; node->children = ni;
	ni->prior = map.prior[pass];
	if (ni->prior.playouts) {
		if (u->amaf_prior)
			tree_update_node_rvalue(ni, u->amaf_prior);
		else
			tree_update_node_value(ni, u->amaf_prior);
	}

	/* The loop considers only the symmetry playground. */
	if (UDEBUGL(6)) {
		fprintf(stderr, "expanding %s within [%d,%d],[%d,%d] %d-%d\n",
				coord2sstr(node->coord, b),
				b->symmetry.x1, b->symmetry.y1,
				b->symmetry.x2, b->symmetry.y2,
				b->symmetry.type, b->symmetry.d);
	}
	for (int i = b->symmetry.x1; i <= b->symmetry.x2; i++) {
		for (int j = b->symmetry.y1; j <= b->symmetry.y2; j++) {
			if (b->symmetry.d) {
				int x = b->symmetry.type == SYM_DIAG_DOWN ? board_size(b) - 1 - i : i;
				if (x > j) {
					if (UDEBUGL(7))
						fprintf(stderr, "drop %d,%d\n", i, j);
					continue;
				}
			}

			coord_t c = coord_xy_otf(i, j, t->board);
			if (!map.consider[c]) // Filter out invalid moves
				continue;
			assert(c != node->coord); // I have spotted "C3 C3" in some sequence...

			struct tree_node *nj = tree_init_node(t, c, node->depth + 1);
			nj->parent = node; ni->sibling = nj; ni = nj;

			ni->prior = map.prior[c];
			if (ni->prior.playouts) {
				if (u->amaf_prior)
					tree_update_node_rvalue(ni, u->amaf_prior);
				else
					tree_update_node_value(ni, u->amaf_prior);
			}
		}
	}
}


static coord_t
flip_coord(struct board *b, coord_t c,
           bool flip_horiz, bool flip_vert, int flip_diag)
{
	int x = coord_x(c, b), y = coord_y(c, b);
	if (flip_diag) {
		int z = x; x = y; y = z;
	}
	if (flip_horiz) {
		x = board_size(b) - 1 - x;
	}
	if (flip_vert) {
		y = board_size(b) - 1 - y;
	}
	return coord_xy_otf(x, y, b);
}

static void
tree_fix_node_symmetry(struct board *b, struct tree_node *node,
                       bool flip_horiz, bool flip_vert, int flip_diag)
{
	if (!is_pass(node->coord))
		node->coord = flip_coord(b, node->coord, flip_horiz, flip_vert, flip_diag);

	for (struct tree_node *ni = node->children; ni; ni = ni->sibling)
		tree_fix_node_symmetry(b, ni, flip_horiz, flip_vert, flip_diag);
}

static void
tree_fix_symmetry(struct tree *tree, struct board *b, coord_t c)
{
	if (is_pass(c))
		return;

	struct board_symmetry *s = &tree->root_symmetry;
	int cx = coord_x(c, b), cy = coord_y(c, b);

	/* playground	X->h->v->d normalization
	 * :::..	.d...
	 * .::..	v....
	 * ..:..	.....
	 * .....	h...X
	 * .....	.....  */
	bool flip_horiz = cx < s->x1 || cx > s->x2;
	bool flip_vert = cy < s->y1 || cy > s->y2;

	bool flip_diag = 0;
	if (s->d) {
		bool dir = (s->type == SYM_DIAG_DOWN);
		int x = dir ^ flip_horiz ^ flip_vert ? board_size(b) - 1 - cx : cx;
		if (flip_vert ? x < cy : x > cy) {
			flip_diag = 1;
		}
	}

	if (UDEBUGL(4)) {
		fprintf(stderr, "%s will flip %d %d %d -> %s, sym %d (%d) -> %d (%d)\n",
			coord2sstr(c, b), flip_horiz, flip_vert, flip_diag,
			coord2sstr(flip_coord(b, c, flip_horiz, flip_vert, flip_diag), b),
			s->type, s->d, b->symmetry.type, b->symmetry.d);
	}
	if (flip_horiz || flip_vert || flip_diag)
		tree_fix_node_symmetry(b, tree->root, flip_horiz, flip_vert, flip_diag);
}


static void
tree_unlink_node(struct tree_node *node)
{
	struct tree_node *ni = node->parent;
	if (ni->children == node) {
		ni->children = node->sibling;
	} else {
		ni = ni->children;
		while (ni->sibling != node)
			ni = ni->sibling;
		ni->sibling = node->sibling;
	}
	node->sibling = NULL;
	node->parent = NULL;
}

void
tree_delete_node(struct tree *tree, struct tree_node *node)
{
	tree_unlink_node(node);
	tree_done_node(tree, node);
}

void
tree_promote_node(struct tree *tree, struct tree_node *node)
{
	assert(node->parent == tree->root);
	tree_unlink_node(node);
	tree_done_node(tree, tree->root);
	tree->root = node;
	tree->root_color = stone_other(tree->root_color);
	board_symmetry_update(tree->board, &tree->root_symmetry, node->coord);
}

bool
tree_promote_at(struct tree *tree, struct board *b, coord_t c)
{
	tree_fix_symmetry(tree, b, c);

	for (struct tree_node *ni = tree->root->children; ni; ni = ni->sibling) {
		if (ni->coord == c) {
			tree_promote_node(tree, ni);
			return true;
		}
	}
	return false;
}
