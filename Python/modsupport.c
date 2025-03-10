/* Module support implementation */

#include <stdio.h>

#include "PROTO.h"
#include "object.h"
#include "intobject.h"
#include "stringobject.h"
#include "tupleobject.h"
#include "listobject.h"
#include "methodobject.h"
#include "moduleobject.h"
#include "modsupport.h"
#include "import.h"
#include "errors.h"


/* Find a method in a module's method table.
   Usually called from a module's getattr method. */

object *
findmethod(ml, op, name)
	struct methodlist *ml;
	object *op;
	char *name;
{
	for (; ml->ml_name != NULL; ml++) {
		if (strcmp(name, ml->ml_name) == 0)
			return newmethodobject(ml->ml_name, ml->ml_meth, op);
	}
	err_setstr(NameError, name);
	return NULL;
}


object *
initmodule(name, methods)
	char *name;
	struct methodlist *methods;
{
	object *m, *d, *v;
	struct methodlist *ml;
	if ((m = new_module(name)) == NULL) {
		fprintf(stderr, "initializing module: %s\n", name);
		fatal("can't create a module");
	}
	d = getmoduledict(m);
	for (ml = methods; ml->ml_name != NULL; ml++) {
		v = newmethodobject(ml->ml_name, ml->ml_meth, (object *)NULL);
		if (v == NULL || dictinsert(d, ml->ml_name, v) != 0) {
			fprintf(stderr, "initializing module: %s\n", name);
			fatal("can't initialize module");
		}
		DECREF(v);
	}
	DECREF(m);
	return m; /* Yes, it still exists, in sys.modules... */
}


/* Argument list handling tools.
   All return 1 for success, or call err_set*() and return 0 for failure */

int
getnoarg(v)
	object *v;
{
	if (v != NULL) {
		return err_badarg();
	}
	return 1;
}

int
getintarg(v, a)
	object *v;
	int *a;
{
	if (v == NULL || !is_intobject(v)) {
		return err_badarg();
	}
	*a = getintvalue(v);
	return 1;
}

int
getintintarg(v, a, b)
	object *v;
	int *a;
	int *b;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getintarg(gettupleitem(v, 0), a) &&
		getintarg(gettupleitem(v, 1), b);
}

int
getlongarg(v, a)
	object *v;
	long *a;
{
	if (v == NULL || !is_intobject(v)) {
		return err_badarg();
	}
	*a = getintvalue(v);
	return 1;
}

int
getlonglongargs(v, a, b)
	object *v;
	long *a, *b;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getlongarg(gettupleitem(v, 0), a) &&
		getlongarg(gettupleitem(v, 1), b);
}

int
getlonglongobjectargs(v, a, b, c)
	object *v;
	long *a, *b;
	object **c;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 3) {
		return err_badarg();
	}
	if (getlongarg(gettupleitem(v, 0), a) &&
		getlongarg(gettupleitem(v, 1), b)) {
		*c = gettupleitem(v, 2);
		return 1;
	}
	else {
		return err_badarg();
	}
}

int
getstrarg(v, a)
	object *v;
	object **a;
{
	if (v == NULL || !is_stringobject(v)) {
		return err_badarg();
	}
	*a = v;
	return 1;
}

int
getstrstrarg(v, a, b)
	object *v;
	object **a;
	object **b;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getstrarg(gettupleitem(v, 0), a) &&
		getstrarg(gettupleitem(v, 1), b);
}

int
getstrstrintarg(v, a, b, c)
	object *v;
	object **a;
	object **b;
	int *c;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 3) {
		return err_badarg();
	}
	return getstrarg(gettupleitem(v, 0), a) &&
		getstrarg(gettupleitem(v, 1), b) &&
		getintarg(gettupleitem(v, 2), c);
}

int
getstrintarg(v, a, b)
	object *v;
	object **a;
	int *b;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getstrarg(gettupleitem(v, 0), a) &&
		getintarg(gettupleitem(v, 1), b);
}

int
getintstrarg(v, a, b)
	object *v;
	int *a;
	object **b;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getintarg(gettupleitem(v, 0), a) &&
		getstrarg(gettupleitem(v, 1), b);
}

int
getpointarg(v, a)
	object *v;
	int *a; /* [2] */
{
	return getintintarg(v, a, a+1);
}

int
get3pointarg(v, a)
	object *v;
	int *a; /* [6] */
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 3) {
		return err_badarg();
	}
	return getpointarg(gettupleitem(v, 0), a) &&
		getpointarg(gettupleitem(v, 1), a+2) &&
		getpointarg(gettupleitem(v, 2), a+4);
}

int
getrectarg(v, a)
	object *v;
	int *a; /* [2+2] */
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getpointarg(gettupleitem(v, 0), a) &&
		getpointarg(gettupleitem(v, 1), a+2);
}

int
getrectintarg(v, a)
	object *v;
	int *a; /* [4+1] */
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getrectarg(gettupleitem(v, 0), a) &&
		getintarg(gettupleitem(v, 1), a+4);
}

int
getpointintarg(v, a)
	object *v;
	int *a; /* [2+1] */
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getpointarg(gettupleitem(v, 0), a) &&
		getintarg(gettupleitem(v, 1), a+2);
}

int
getpointstrarg(v, a, b)
	object *v;
	int *a; /* [2] */
	object **b;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getpointarg(gettupleitem(v, 0), a) &&
		getstrarg(gettupleitem(v, 1), b);
}

int
getstrintintarg(v, a, b, c)
	object *v;
	object *a;
	int *b, *c;
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 3) {
		return err_badarg();
	}
	return getstrarg(gettupleitem(v, 0), a) &&
		getintarg(gettupleitem(v, 1), b) &&
		getintarg(gettupleitem(v, 2), c);
}

int
getrectpointarg(v, a)
	object *v;
	int *a; /* [4+2] */
{
	if (v == NULL || !is_tupleobject(v) || gettuplesize(v) != 2) {
		return err_badarg();
	}
	return getrectarg(gettupleitem(v, 0), a) &&
		getpointarg(gettupleitem(v, 1), a+4);
}

int
getlongtuplearg(args, a, n)
	object *args;
	long *a; /* [n] */
	int n;
{
	int i;
	if (!is_tupleobject(args) || gettuplesize(args) != n) {
		return err_badarg();
	}
	for (i = 0; i < n; i++) {
		object *v = gettupleitem(args, i);
		if (!is_intobject(v)) {
			return err_badarg();
		}
		a[i] = getintvalue(v);
	}
	return 1;
}

int
getshorttuplearg(args, a, n)
	object *args;
	short *a; /* [n] */
	int n;
{
	int i;
	if (!is_tupleobject(args) || gettuplesize(args) != n) {
		return err_badarg();
	}
	for (i = 0; i < n; i++) {
		object *v = gettupleitem(args, i);
		if (!is_intobject(v)) {
			return err_badarg();
		}
		a[i] = getintvalue(v);
	}
	return 1;
}

int
getlonglistarg(args, a, n)
	object *args;
	long *a; /* [n] */
	int n;
{
	int i;
	if (!is_listobject(args) || getlistsize(args) != n) {
		return err_badarg();
	}
	for (i = 0; i < n; i++) {
		object *v = getlistitem(args, i);
		if (!is_intobject(v)) {
			return err_badarg();
		}
		a[i] = getintvalue(v);
	}
	return 1;
}

int
getshortlistarg(args, a, n)
	object *args;
	short *a; /* [n] */
	int n;
{
	int i;
	if (!is_listobject(args) || getlistsize(args) != n) {
		return err_badarg();
	}
	for (i = 0; i < n; i++) {
		object *v = getlistitem(args, i);
		if (!is_intobject(v)) {
			return err_badarg();
		}
		a[i] = getintvalue(v);
	}
	return 1;
}
