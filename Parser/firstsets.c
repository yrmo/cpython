/* Computation of FIRST stets */

#include <stdio.h>

#include "PROTO.h"
#include "malloc.h"
#include "grammar.h"
#include "token.h"

extern int debugging;

static void
calcfirstset(g, d)
	grammar *g;
	dfa *d;
{
	int i, j;
	state *s;
	arc *a;
	int nsyms;
	int *sym;
	int nbits;
	static bitset dummy;
	bitset result;
	int type;
	dfa *d1;
	label *l0;
	
	if (debugging)
		printf("Calculate FIRST set for '%s'\n", d->d_name);
	
	if (dummy == NULL)
		dummy = newbitset(1);
	if (d->d_first == dummy) {
		fprintf(stderr, "Left-recursion for '%s'\n", d->d_name);
		return;
	}
	if (d->d_first != NULL) {
		fprintf(stderr, "Re-calculating FIRST set for '%s' ???\n",
			d->d_name);
	}
	d->d_first = dummy;
	
	l0 = g->g_ll.ll_label;
	nbits = g->g_ll.ll_nlabels;
	result = newbitset(nbits);
	
	sym = NEW(int, 1);
	if (sym == NULL)
		fatal("no mem for new sym in calcfirstset");
	nsyms = 1;
	sym[0] = findlabel(&g->g_ll, d->d_type, (char *)NULL);
	
	s = &d->d_state[d->d_initial];
	for (i = 0; i < s->s_narcs; i++) {
		a = &s->s_arc[i];
		for (j = 0; j < nsyms; j++) {
			if (sym[j] == a->a_lbl)
				break;
		}
		if (j >= nsyms) { /* New label */
			RESIZE(sym, int, nsyms + 1);
			if (sym == NULL)
				fatal("no mem to resize sym in calcfirstset");
			sym[nsyms++] = a->a_lbl;
			type = l0[a->a_lbl].lb_type;
			if (ISNONTERMINAL(type)) {
				d1 = finddfa(g, type);
				if (d1->d_first == dummy) {
					fprintf(stderr,
						"Left-recursion below '%s'\n",
						d->d_name);
				}
				else {
					if (d1->d_first == NULL)
						calcfirstset(g, d1);
					mergebitset(result, d1->d_first, nbits);
				}
			}
			else if (ISTERMINAL(type)) {
				addbit(result, a->a_lbl);
			}
		}
	}
	d->d_first = result;
	if (debugging) {
		printf("FIRST set for '%s': {", d->d_name);
		for (i = 0; i < nbits; i++) {
			if (testbit(result, i))
				printf(" %s", labelrepr(&l0[i]));
		}
		printf(" }\n");
	}
}

void
addfirstsets(g)
	grammar *g;
{
	int i;
	dfa *d;
	
	printf("Adding FIRST sets ...\n");
	for (i = 0; i < g->g_ndfas; i++) {
		d = &g->g_dfa[i];
		if (d->d_first == NULL)
			calcfirstset(g, d);
	}
}
