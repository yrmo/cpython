#define DEBUG
/* Compile an expression node to intermediate code */

#include <stdio.h>
#include <ctype.h>
#include "string.h"

#include "PROTO.h"
#include "object.h"
#include "objimpl.h"
#include "intobject.h"
#include "floatobject.h"
#include "stringobject.h"
#include "listobject.h"
#include "node.h"
#include "token.h"
#include "graminit.h"
#include "errors.h"
#include "compile.h"
#include "opcode.h"

static void
code_dealloc(c)
	codeobject *c;
{
	XDECREF(c->co_code);
	XDECREF(c->co_consts);
	XDECREF(c->co_names);
	DEL(c);
}

typeobject Codetype = {
	OB_HEAD_INIT(&Typetype)
	0,
	"code",
	sizeof(codeobject),
	0,
	code_dealloc,	/*tp_dealloc*/
	0,		/*tp_print*/
	0,		/*tp_getattr*/
	0,		/*tp_setattr*/
	0,		/*tp_compare*/
	0,		/*tp_repr*/
	0,		/*tp_as_number*/
	0,		/*tp_as_sequence*/
	0,		/*tp_as_mapping*/
};

static codeobject *newcodeobject PROTO((object *, object *, object *));

static codeobject *
newcodeobject(code, consts, names)
	object *code;
	object *consts;
	object *names;
{
	codeobject *co;
	int i;
	/* Check argument types */
	if (code == NULL || !is_stringobject(code) ||
		consts == NULL || !is_listobject(consts) ||
		names == NULL || !is_listobject(names)) {
		err_badcall();
		return NULL;
	}
	/* Make sure the list of names contains only strings */
	for (i = getlistsize(names); --i >= 0; ) {
		object *v = getlistitem(names, i);
		if (v == NULL || !is_stringobject(v)) {
			err_badcall();
			return NULL;
		}
	}
	co = NEWOBJ(codeobject, &Codetype);
	if (co != NULL) {
		INCREF(code);
		co->co_code = (stringobject *)code;
		INCREF(consts);
		co->co_consts = consts;
		INCREF(names);
		co->co_names = names;
	}
	return co;
}


/* Data structure used internally */
struct compiling {
	object *c_code;		/* string */
	object *c_consts;	/* list of objects */
	object *c_names;	/* list of strings (names) */
	int c_nexti;		/* index into c_code */
	int c_errors;		/* counts errors occurred */
};

/* Prototypes */
static int com_init PROTO((struct compiling *));
static void com_free PROTO((struct compiling *));
static void com_done PROTO((struct compiling *));
static void com_node PROTO((struct compiling *, struct _node *));
static void com_addbyte PROTO((struct compiling *, int));
static void com_addint PROTO((struct compiling *, int));
static void com_addoparg PROTO((struct compiling *, int, int));
static void com_addfwref PROTO((struct compiling *, int, int *));
static void com_backpatch PROTO((struct compiling *, int));
static int com_add PROTO((struct compiling *, object *, object *));
static int com_addconst PROTO((struct compiling *, object *));
static int com_addname PROTO((struct compiling *, object *));
static void com_addopname PROTO((struct compiling *, int, node *));

static int
com_init(c)
	struct compiling *c;
{
	if ((c->c_code = newsizedstringobject((char *)NULL, 0)) == NULL)
		goto fail_3;
	if ((c->c_consts = newlistobject(0)) == NULL)
		goto fail_2;
	if ((c->c_names = newlistobject(0)) == NULL)
		goto fail_1;
	c->c_nexti = 0;
	c->c_errors = 0;
	return 1;
	
  fail_1:
	DECREF(c->c_consts);
  fail_2:
	DECREF(c->c_code);
  fail_3:
 	return 0;
}

static void
com_free(c)
	struct compiling *c;
{
	XDECREF(c->c_code);
	XDECREF(c->c_consts);
	XDECREF(c->c_names);
}

static void
com_done(c)
	struct compiling *c;
{
	if (c->c_code != NULL)
		resizestring(&c->c_code, c->c_nexti);
}

static void
com_addbyte(c, byte)
	struct compiling *c;
	int byte;
{
	int len;
	if (byte < 0 || byte > 255) {
		err_setstr(SystemError, "com_addbyte: byte out of range");
		c->c_errors++;
	}
	if (c->c_code == NULL)
		return;
	len = getstringsize(c->c_code);
	if (c->c_nexti >= len) {
		if (resizestring(&c->c_code, len+1000) != 0) {
			c->c_errors++;
			return;
		}
	}
	getstringvalue(c->c_code)[c->c_nexti++] = byte;
}

static void
com_addint(c, x)
	struct compiling *c;
	int x;
{
	com_addbyte(c, x);
}

static void
com_addoparg(c, op, arg)
	struct compiling *c;
	int op;
	int arg;
{
	com_addbyte(c, op);
	com_addint(c, arg);
}

static void
com_addfwref(c, op, p_anchor)
	struct compiling *c;
	int op;
	int *p_anchor;
{
	/* Compile a forward reference for backpatching */
	int here;
	int anchor;
	com_addbyte(c, op);
	here = c->c_nexti;
	anchor = *p_anchor;
	*p_anchor = here;
	com_addint(c, anchor == 0 ? 0 : here - anchor);
}

static void
com_backpatch(c, anchor)
	struct compiling *c;
	int anchor; /* Must be nonzero */
{
	unsigned char *code = (unsigned char *) getstringvalue(c->c_code);
	int target = c->c_nexti;
	int lastanchor = 0;
	int dist;
	int prev;
	for (;;) {
		/* Make the JUMP instruction at anchor point to target */
		prev = code[anchor];
		dist = target - (anchor+1);
		if (dist > 255 && lastanchor &&
				code[lastanchor-1] == code[anchor-1]) {
			/* Attempt to jump to a similar jump closer by */
/* XXX Show that it works */
fprintf(stderr, "com_backpatch: huge jump rescued (?)\n");
			target = lastanchor-1;
			dist = target - (anchor+1);
			lastanchor = 0;
		}
		if (dist > 255) {
			err_setstr(SystemError, "relative jump > 255 bytes");
			c->c_errors++;
		}
		code[anchor] = dist;
		if (!prev)
			break;
		lastanchor = anchor;
		anchor -= prev;
	}
}

/* Handle constants and names uniformly */

static int
com_add(c, list, v)
	struct compiling *c;
	object *list;
	object *v;
{
	/* XXX Should look through list for object with same value */
	int i = getlistsize(list);
	if (addlistitem(list, v) != 0)
		c->c_errors++;
	return i;
}

static int
com_addconst(c, v)
	struct compiling *c;
	object *v;
{
	return com_add(c, c->c_consts, v);
}

static int
com_addname(c, v)
	struct compiling *c;
	object *v;
{
	return com_add(c, c->c_names, v);
}

static void
com_addopname(c, op, n)
	struct compiling *c;
	int op;
	node *n;
{
	object *v;
	int i;
	char *name;
	if (TYPE(n) == STAR)
		name = "*";
	else {
		REQ(n, NAME);
		name = STR(n);
	}
	if ((v = newstringobject(name)) == NULL) {
		c->c_errors++;
		i = 255;
	}
	else {
		i = com_addname(c, v);
		DECREF(v);
	}
	com_addoparg(c, op, i);
}

static object *
parsenumber(s)
	char *s;
{
	extern long strtol();
	extern double atof();
	char *end = s;
	long x;
	x = strtol(s, &end, 0);
	if (*end == '\0')
		return newintobject(x);
	if (*end == '.' || *end == 'e' || *end == 'E')
		return newfloatobject(atof(s));
	err_setstr(RuntimeError, "bad number syntax");
	return NULL;
}

static object *
parsestr(s)
	char *s;
{
	object *v;
	int len;
	char *buf;
	char *p;
	int c;
	if (*s != '\'') {
		err_badcall();
		return NULL;
	}
	s++;
	len = strlen(s);
	if (s[--len] != '\'') {
		err_badcall();
		return NULL;
	}
	if (strchr(s, '\\') == NULL)
		return newsizedstringobject(s, len);
	v = newsizedstringobject((char *)NULL, len);
	p = buf = getstringvalue(v);
	while (*s != '\0' && *s != '\'') {
		if (*s != '\\') {
			*p++ = *s++;
			continue;
		}
		s++;
		switch (*s++) {
		/* XXX This assumes ASCII! */
		case '\\': *p++ = '\\'; break;
		case '\'': *p++ = '\''; break;
		case 'b': *p++ = '\b'; break;
		case 'f': *p++ = '\014'; break; /* FF */
		case 't': *p++ = '\t'; break;
		case 'n': *p++ = '\n'; break;
		case 'r': *p++ = '\r'; break;
		case 'v': *p++ = '\013'; break; /* VT */
		case 'E': *p++ = '\033'; break; /* ESC, not C */
		case 'a': *p++ = '\007'; break; /* BEL, not classic C */
		case '0': case '1': case '2': case '3':
		case '4': case '5': case '6': case '7':
			c = s[-1] - '0';
			if ('0' <= *s && *s <= '7') {
				c = (c<<3) + *s++ - '0';
				if ('0' <= *s && *s <= '7')
					c = (c<<3) + *s++ - '0';
			}
			*p++ = c;
			break;
		case 'x':
			if (isxdigit(*s)) {
				sscanf(s, "%x", &c);
				*p++ = c;
				do {
					s++;
				} while (isxdigit(*s));
				break;
			}
		/* FALLTHROUGH */
		default: *p++ = '\\'; *p++ = s[-1]; break;
		}
	}
	resizestring(&v, (int)(p - buf));
	return v;
}

static void
com_list_constructor(c, n)
	struct compiling *c;
	node *n;
{
	int len;
	int i;
	object *v, *w;
	if (TYPE(n) != testlist)
		REQ(n, exprlist);
	/* exprlist: expr (',' expr)* [',']; likewise for testlist */
	len = (NCH(n) + 1) / 2;
	for (i = 0; i < NCH(n); i += 2)
		com_node(c, CHILD(n, i));
	com_addoparg(c, BUILD_LIST, len);
}

static void
com_atom(c, n)
	struct compiling *c;
	node *n;
{
	node *ch;
	object *v;
	int i;
	REQ(n, atom);
	ch = CHILD(n, 0);
	switch (TYPE(ch)) {
	case LPAR:
		if (TYPE(CHILD(n, 1)) == RPAR)
			com_addoparg(c, BUILD_TUPLE, 0);
		else
			com_node(c, CHILD(n, 1));
		break;
	case LSQB:
		if (TYPE(CHILD(n, 1)) == RSQB)
			com_addoparg(c, BUILD_LIST, 0);
		else
			com_list_constructor(c, CHILD(n, 1));
		break;
	case LBRACE:
		com_addoparg(c, BUILD_MAP, 0);
		break;
	case BACKQUOTE:
		com_node(c, CHILD(n, 1));
		com_addbyte(c, UNARY_CONVERT);
		break;
	case NUMBER:
		if ((v = parsenumber(STR(ch))) == NULL) {
			c->c_errors++;
			i = 255;
		}
		else {
			i = com_addconst(c, v);
			DECREF(v);
		}
		com_addoparg(c, LOAD_CONST, i);
		break;
	case STRING:
		if ((v = parsestr(STR(ch))) == NULL) {
			c->c_errors++;
			i = 255;
		}
		else {
			i = com_addconst(c, v);
			DECREF(v);
		}
		com_addoparg(c, LOAD_CONST, i);
		break;
	case NAME:
		com_addopname(c, LOAD_NAME, ch);
		break;
	default:
		fprintf(stderr, "node type %d\n", TYPE(ch));
		err_setstr(SystemError, "com_atom: unexpected node type");
		c->c_errors++;
	}
}

static void
com_slice(c, n, op)
	struct compiling *c;
	node *n;
	int op;
{
	if (NCH(n) == 1) {
		com_addbyte(c, op);
	}
	else if (NCH(n) == 2) {
		if (TYPE(CHILD(n, 0)) != COLON) {
			com_node(c, CHILD(n, 0));
			com_addbyte(c, op+1);
		}
		else {
			com_node(c, CHILD(n, 1));
			com_addbyte(c, op+2);
		}
	}
	else {
		com_node(c, CHILD(n, 0));
		com_node(c, CHILD(n, 2));
		com_addbyte(c, op+3);
	}
}

static void
com_apply_subscript(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, subscript);
	if (NCH(n) == 1 && TYPE(CHILD(n, 0)) != COLON) {
		/* It's a single subscript */
		com_node(c, CHILD(n, 0));
		com_addbyte(c, BINARY_SUBSCR);
	}
	else {
		/* It's a slice: [expr] ':' [expr] */
		com_slice(c, n, SLICE);
	}
}

static void
com_call_function(c, n)
	struct compiling *c;
	node *n; /* EITHER testlist OR ')' */
{
	if (TYPE(n) == RPAR) {
		com_addbyte(c, UNARY_CALL);
	}
	else {
		com_node(c, n);
		com_addbyte(c, BINARY_CALL);
	}
}

static void
com_select_member(c, n)
	struct compiling *c;
	node *n;
{
	com_addopname(c, LOAD_ATTR, n);
}

static void
com_apply_trailer(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, trailer);
	switch (TYPE(CHILD(n, 0))) {
	case LPAR:
		com_call_function(c, CHILD(n, 1));
		break;
	case DOT:
		com_select_member(c, CHILD(n, 1));
		break;
	case LSQB:
		com_apply_subscript(c, CHILD(n, 1));
		break;
	default:
		err_setstr(SystemError,
			"com_apply_trailer: unknown trailer type");
		c->c_errors++;
	}
}

static void
com_factor(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	REQ(n, factor);
	if (TYPE(CHILD(n, 0)) == PLUS) {
		com_factor(c, CHILD(n, 1));
		com_addbyte(c, UNARY_POSITIVE);
	}
	else if (TYPE(CHILD(n, 0)) == MINUS) {
		com_factor(c, CHILD(n, 1));
		com_addbyte(c, UNARY_NEGATIVE);
	}
	else {
		com_atom(c, CHILD(n, 0));
		for (i = 1; i < NCH(n); i++)
			com_apply_trailer(c, CHILD(n, i));
	}
}

static void
com_term(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	int op;
	REQ(n, term);
	com_factor(c, CHILD(n, 0));
	for (i = 2; i < NCH(n); i += 2) {
		com_factor(c, CHILD(n, i));
		switch (TYPE(CHILD(n, i-1))) {
		case STAR:
			op = BINARY_MULTIPLY;
			break;
		case SLASH:
			op = BINARY_DIVIDE;
			break;
		case PERCENT:
			op = BINARY_MODULO;
			break;
		default:
			err_setstr(SystemError,
				"com_term: term operator not *, / or %");
			c->c_errors++;
			op = 255;
		}
		com_addbyte(c, op);
	}
}

static void
com_expr(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	int op;
	REQ(n, expr);
	com_term(c, CHILD(n, 0));
	for (i = 2; i < NCH(n); i += 2) {
		com_term(c, CHILD(n, i));
		switch (TYPE(CHILD(n, i-1))) {
		case PLUS:
			op = BINARY_ADD;
			break;
		case MINUS:
			op = BINARY_SUBTRACT;
			break;
		default:
			err_setstr(SystemError,
				"com_expr: expr operator not + or -");
			c->c_errors++;
			op = 255;
		}
		com_addbyte(c, op);
	}
}

static enum cmp_op
cmp_type(n)
	node *n;
{
	REQ(n, comp_op);
	/* comp_op: '<' | '>' | '=' | '>' '=' | '<' '=' | '<' '>'
	          | 'in' | 'not' 'in' | 'is' | 'is' not' */
	if (NCH(n) == 1) {
		n = CHILD(n, 0);
		switch (TYPE(n)) {
		case LESS:	return LT;
		case GREATER:	return GT;
		case EQUAL:	return EQ;
		case NAME:	if (strcmp(STR(n), "in") == 0) return IN;
				if (strcmp(STR(n), "is") == 0) return IS;
		}
	}
	else if (NCH(n) == 2) {
		int t2 = TYPE(CHILD(n, 1));
		switch (TYPE(CHILD(n, 0))) {
		case LESS:	if (t2 == EQUAL)	return LE;
				if (t2 == GREATER)	return NE;
				break;
		case GREATER:	if (t2 == EQUAL)	return GE;
				break;
		case NAME:	if (strcmp(STR(CHILD(n, 1)), "in") == 0)
					return NOT_IN;
				if (strcmp(STR(CHILD(n, 0)), "is") == 0)
					return IS_NOT;
		}
	}
	return BAD;
}

static void
com_comparison(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	enum cmp_op op;
	int anchor;
	REQ(n, comparison); /* comparison: expr (comp_op expr)* */
	com_expr(c, CHILD(n, 0));
	if (NCH(n) == 1)
		return;
	
	/****************************************************************
	   The following code is generated for all but the last
	   comparison in a chain:
	   
	   label:	on stack:	opcode:		jump to:
	   
			a		<code to load b>
			a, b		DUP_TOP
			a, b, b		ROT_THREE
			b, a, b		COMPARE_OP
			b, 0-or-1	JUMP_IF_FALSE	L1
			b, 1		POP_TOP
			b		
	
	   We are now ready to repeat this sequence for the next
	   comparison in the chain.
	   
	   For the last we generate:
	   
	   		b		<code to load c>
	   		b, c		COMPARE_OP
	   		0-or-1		
	   
	   If there were any jumps to L1 (i.e., there was more than one
	   comparison), we generate:
	   
	   		0-or-1		JUMP_FORWARD	L2
	   L1:		b, 0		ROT_TWO
	   		0, b		POP_TOP
	   		0
	   L2:
	****************************************************************/
	
	anchor = 0;
	
	for (i = 2; i < NCH(n); i += 2) {
		com_expr(c, CHILD(n, i));
		if (i+2 < NCH(n)) {
			com_addbyte(c, DUP_TOP);
			com_addbyte(c, ROT_THREE);
		}
		op = cmp_type(CHILD(n, i-1));
		if (op == BAD) {
			err_setstr(SystemError,
				"com_comparison: unknown comparison op");
			c->c_errors++;
		}
		com_addoparg(c, COMPARE_OP, op);
		if (i+2 < NCH(n)) {
			com_addfwref(c, JUMP_IF_FALSE, &anchor);
			com_addbyte(c, POP_TOP);
		}
	}
	
	if (anchor) {
		int anchor2 = 0;
		com_addfwref(c, JUMP_FORWARD, &anchor2);
		com_backpatch(c, anchor);
		com_addbyte(c, ROT_TWO);
		com_addbyte(c, POP_TOP);
		com_backpatch(c, anchor2);
	}
}

static void
com_not_test(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, not_test); /* 'not' not_test | comparison */
	if (NCH(n) == 1) {
		com_comparison(c, CHILD(n, 0));
	}
	else {
		com_not_test(c, CHILD(n, 1));
		com_addbyte(c, UNARY_NOT);
	}
}

static void
com_and_test(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	int anchor;
	REQ(n, and_test); /* not_test ('and' not_test)* */
	anchor = 0;
	i = 0;
	for (;;) {
		com_not_test(c, CHILD(n, i));
		if ((i += 2) >= NCH(n))
			break;
		com_addfwref(c, JUMP_IF_FALSE, &anchor);
		com_addbyte(c, POP_TOP);
	}
	if (anchor)
		com_backpatch(c, anchor);
}

static void
com_test(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	int anchor;
	REQ(n, test); /* and_test ('and' and_test)* */
	anchor = 0;
	i = 0;
	for (;;) {
		com_and_test(c, CHILD(n, i));
		if ((i += 2) >= NCH(n))
			break;
		com_addfwref(c, JUMP_IF_TRUE, &anchor);
		com_addbyte(c, POP_TOP);
	}
	if (anchor)
		com_backpatch(c, anchor);
}

static void
com_list(c, n)
	struct compiling *c;
	node *n;
{
	/* exprlist: expr (',' expr)* [',']; likewise for testlist */
	if (NCH(n) == 1) {
		com_node(c, CHILD(n, 0));
	}
	else {
		int i;
		int len;
		len = (NCH(n) + 1) / 2;
		for (i = 0; i < NCH(n); i += 2)
			com_node(c, CHILD(n, i));
		com_addoparg(c, BUILD_TUPLE, len);
	}
}


/* Begin of assignment compilation */

static void com_assign_name PROTO((struct compiling *, node *, int));
static void com_assign PROTO((struct compiling *, node *, int));

static void
com_assign_attr(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	com_addopname(c, assigning ? STORE_ATTR : DELETE_ATTR, n);
}

static void
com_assign_slice(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	com_slice(c, n, assigning ? STORE_SLICE : DELETE_SLICE);
}

static void
com_assign_subscript(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	com_node(c, n);
	com_addbyte(c, assigning ? STORE_SUBSCR : DELETE_SUBSCR);
}

static void
com_assign_trailer(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	char *name;
	REQ(n, trailer);
	switch (TYPE(CHILD(n, 0))) {
	case LPAR: /* '(' [exprlist] ')' */
		err_setstr(TypeError, "can't assign to function call");
		c->c_errors++;
		break;
	case DOT: /* '.' NAME */
		com_assign_attr(c, CHILD(n, 1), assigning);
		break;
	case LSQB: /* '[' subscript ']' */
		n = CHILD(n, 1);
		REQ(n, subscript); /* subscript: expr | [expr] ':' [expr] */
		if (NCH(n) > 1 || TYPE(CHILD(n, 0)) == COLON)
			com_assign_slice(c, n, assigning);
		else
			com_assign_subscript(c, CHILD(n, 0), assigning);
		break;
	default:
		err_setstr(TypeError, "unknown trailer type");
		c->c_errors++;
	}
}

static void
com_assign_tuple(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	int i;
	if (TYPE(n) != testlist)
		REQ(n, exprlist);
	if (assigning)
		com_addoparg(c, UNPACK_TUPLE, (NCH(n)+1)/2);
	for (i = 0; i < NCH(n); i += 2)
		com_assign(c, CHILD(n, i), assigning);
}

static void
com_assign_list(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	int i;
	if (assigning)
		com_addoparg(c, UNPACK_LIST, (NCH(n)+1)/2);
	for (i = 0; i < NCH(n); i += 2)
		com_assign(c, CHILD(n, i), assigning);
}

static void
com_assign_name(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	REQ(n, NAME);
	com_addopname(c, assigning ? STORE_NAME : DELETE_NAME, n);
}

static void
com_assign(c, n, assigning)
	struct compiling *c;
	node *n;
	int assigning;
{
	/* Loop to avoid trivial recursion */
	for (;;) {
		switch (TYPE(n)) {
		case exprlist:
		case testlist:
			if (NCH(n) > 1) {
				com_assign_tuple(c, n, assigning);
				return;
			}
			n = CHILD(n, 0);
			break;
		case test:
		case and_test:
		case not_test:
			if (NCH(n) > 1) {
				err_setstr(TypeError,
					"can't assign to operator");
				c->c_errors++;
				return;
			}
			n = CHILD(n, 0);
			break;
		case comparison:
			if (NCH(n) > 1) {
				err_setstr(TypeError,
					"can't assign to operator");
				c->c_errors++;
				return;
			}
			n = CHILD(n, 0);
			break;
		case expr:
			if (NCH(n) > 1) {
				err_setstr(TypeError,
					"can't assign to operator");
				c->c_errors++;
				return;
			}
			n = CHILD(n, 0);
			break;
		case term:
			if (NCH(n) > 1) {
				err_setstr(TypeError,
					"can't assign to operator");
				c->c_errors++;
				return;
			}
			n = CHILD(n, 0);
			break;
		case factor: /* ('+'|'-') factor | atom trailer* */
			if (TYPE(CHILD(n, 0)) != atom) { /* '+' | '-' */
				err_setstr(TypeError,
					"can't assign to operator");
				c->c_errors++;
				return;
			}
			if (NCH(n) > 1) { /* trailer present */
				int i;
				com_node(c, CHILD(n, 0));
				for (i = 1; i+1 < NCH(n); i++) {
					com_apply_trailer(c, CHILD(n, i));
				} /* NB i is still alive */
				com_assign_trailer(c,
						CHILD(n, i), assigning);
				return;
			}
			n = CHILD(n, 0);
			break;
		case atom:
			switch (TYPE(CHILD(n, 0))) {
			case LPAR:
				n = CHILD(n, 1);
				if (TYPE(n) == RPAR) {
					/* XXX Should allow () = () ??? */
					err_setstr(TypeError,
						"can't assign to ()");
					c->c_errors++;
					return;
				}
				break;
			case LSQB:
				n = CHILD(n, 1);
				if (TYPE(n) == RSQB) {
					err_setstr(TypeError,
						"can't assign to []");
					c->c_errors++;
					return;
				}
				com_assign_list(c, n, assigning);
				return;
			case NAME:
				com_assign_name(c, CHILD(n, 0), assigning);
				return;
			default:
				err_setstr(TypeError,
						"can't assign to constant");
				c->c_errors++;
				return;
			}
			break;
		default:
			fprintf(stderr, "node type %d\n", TYPE(n));
			err_setstr(SystemError, "com_assign: bad node");
			c->c_errors++;
			return;
		}
	}
}

static void
com_expr_stmt(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, expr_stmt); /* exprlist ('=' exprlist)* NEWLINE */
	com_node(c, CHILD(n, NCH(n)-2));
	if (NCH(n) == 2) {
		com_addbyte(c, PRINT_EXPR);
	}
	else {
		int i;
		for (i = 0; i < NCH(n)-3; i+=2) {
			if (i+2 < NCH(n)-3)
				com_addbyte(c, DUP_TOP);
			com_assign(c, CHILD(n, i), 1/*assign*/);
		}
	}
}

static void
com_print_stmt(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	REQ(n, print_stmt); /* 'print' (test ',')* [test] NEWLINE */
	for (i = 1; i+1 < NCH(n); i += 2) {
		com_node(c, CHILD(n, i));
		com_addbyte(c, PRINT_ITEM);
	}
	if (TYPE(CHILD(n, NCH(n)-2)) != COMMA)
		com_addbyte(c, PRINT_NEWLINE);
}

static void
com_return_stmt(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, return_stmt); /* 'return' [testlist] NEWLINE */
	if (NCH(n) == 2)
		com_addoparg(c, LOAD_CONST, com_addconst(c, None));
	else
		com_node(c, CHILD(n, 1));
	com_addbyte(c, RETURN_VALUE);
}

static void
com_raise_stmt(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, raise_stmt); /* 'raise' expr [',' expr] NEWLINE */
	com_node(c, CHILD(n, 1));
	if (NCH(n) > 3)
		com_node(c, CHILD(n, 3));
	else
		com_addoparg(c, LOAD_CONST, com_addconst(c, None));
	com_addbyte(c, RAISE_EXCEPTION);
}

static void
com_import_stmt(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	REQ(n, import_stmt);
	/* 'import' NAME (',' NAME)* NEWLINE |
	   'from' NAME 'import' ('*' | NAME (',' NAME)*) NEWLINE */
	if (STR(CHILD(n, 0))[0] == 'f') {
		/* 'from' NAME 'import' ... */
		REQ(CHILD(n, 1), NAME);
		com_addopname(c, IMPORT_NAME, CHILD(n, 1));
		for (i = 3; i < NCH(n); i += 2)
			com_addopname(c, IMPORT_FROM, CHILD(n, i));
		com_addbyte(c, POP_TOP);
	}
	else {
		for (i = 1; i < NCH(n); i += 2) {
			com_addopname(c, IMPORT_NAME, CHILD(n, i));
			com_addopname(c, STORE_NAME, CHILD(n, i));
		}
	}
}

static void
com_if_stmt(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	int anchor = 0;
	REQ(n, if_stmt);
	/*'if' test ':' suite ('elif' test ':' suite)* ['else' ':' suite] */
	for (i = 0; i+3 < NCH(n); i+=4) {
		int a = 0;
		com_node(c, CHILD(n, i+1));
		com_addfwref(c, JUMP_IF_FALSE, &a);
		com_addbyte(c, POP_TOP);
		com_node(c, CHILD(n, i+3));
		com_addfwref(c, JUMP_FORWARD, &anchor);
		com_backpatch(c, a);
		com_addbyte(c, POP_TOP);
	}
	if (i+2 < NCH(n))
		com_node(c, CHILD(n, i+2));
	com_backpatch(c, anchor);
}

static void
com_while_stmt(c, n)
	struct compiling *c;
	node *n;
{
	int break_anchor = 0;
	int anchor = 0;
	int begin;
	REQ(n, while_stmt); /* 'while' test ':' suite ['else' ':' suite] */
	com_addfwref(c, SETUP_LOOP, &break_anchor);
	begin = c->c_nexti;
	com_node(c, CHILD(n, 1));
	com_addfwref(c, JUMP_IF_FALSE, &anchor);
	com_addbyte(c, POP_TOP);
	com_node(c, CHILD(n, 3));
	com_addoparg(c, JUMP_ABSOLUTE, begin);
	com_backpatch(c, anchor);
	com_addbyte(c, POP_TOP);
	com_addbyte(c, POP_BLOCK);
	if (NCH(n) > 4)
		com_node(c, CHILD(n, 6));
	com_backpatch(c, break_anchor);
}

static void
com_for_stmt(c, n)
	struct compiling *c;
	node *n;
{
	object *v;
	int break_anchor = 0;
	int anchor = 0;
	int begin;
	REQ(n, for_stmt);
	/* 'for' exprlist 'in' exprlist ':' suite ['else' ':' suite] */
	com_addfwref(c, SETUP_LOOP, &break_anchor);
	com_node(c, CHILD(n, 3));
	v = newintobject(0L);
	if (v == NULL)
		c->c_errors++;
	com_addoparg(c, LOAD_CONST, com_addconst(c, v));
	XDECREF(v);
	begin = c->c_nexti;
	com_addfwref(c, FOR_LOOP, &anchor);
	com_assign(c, CHILD(n, 1), 1/*assigning*/);
	com_node(c, CHILD(n, 5));
	com_addoparg(c, JUMP_ABSOLUTE, begin);
	com_backpatch(c, anchor);
	com_addbyte(c, POP_BLOCK);
	if (NCH(n) > 8)
		com_node(c, CHILD(n, 8));
	com_backpatch(c, break_anchor);
}

static void
com_try_stmt(c, n)
	struct compiling *c;
	node *n;
{
	int finally_anchor = 0;
	int except_anchor = 0;
	REQ(n, try_stmt);
	/* 'try' ':' suite (except_clause ':' suite)* ['finally' ':' suite] */
	if (NCH(n) > 3 && TYPE(CHILD(n, NCH(n)-3)) != except_clause) {
		/* Have a 'finally' clause */
		com_addfwref(c, SETUP_FINALLY, &finally_anchor);
	}
	if (NCH(n) > 3 && TYPE(CHILD(n, 3)) == except_clause) {
		/* Have an 'except' clause */
		com_addfwref(c, SETUP_EXCEPT, &except_anchor);
	}
	com_node(c, CHILD(n, 2));
	if (except_anchor) {
		int end_anchor = 0;
		int i;
		node *ch;
		com_addbyte(c, POP_BLOCK);
		com_addfwref(c, JUMP_FORWARD, &end_anchor);
		com_backpatch(c, except_anchor);
		for (i = 3;
			i < NCH(n) && TYPE(ch = CHILD(n, i)) == except_clause;
								i += 3) {
			/* except_clause: 'except' [expr [',' expr]] */
			int next_anchor = 0;
			if (NCH(ch) > 1) {
				com_addbyte(c, DUP_TOP);
				com_node(c, CHILD(ch, 1));
				com_addoparg(c, COMPARE_OP, EXC_MATCH);
				com_addfwref(c, JUMP_IF_FALSE, &next_anchor);
				com_addbyte(c, POP_TOP);
			}
			com_addbyte(c, POP_TOP);
			if (NCH(ch) > 3)
				com_assign(c, CHILD(ch, 3), 1/*assigning*/);
			else
				com_addbyte(c, POP_TOP);
			com_node(c, CHILD(n, i+2));
			com_addfwref(c, JUMP_FORWARD, &end_anchor);
			if (next_anchor)
				com_backpatch(c, next_anchor);
		}
		com_addbyte(c, END_FINALLY);
		com_backpatch(c, end_anchor);
	}
	if (finally_anchor) {
		com_addbyte(c, POP_BLOCK);
		com_addoparg(c, LOAD_CONST, com_addconst(c, None));
		com_addoparg(c, LOAD_CONST, com_addconst(c, None));
		com_backpatch(c, finally_anchor);
		com_node(c, CHILD(n, NCH(n)-1));
		com_addbyte(c, END_FINALLY);
	}
}

static void
com_suite(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, suite);
	/* simple_stmt | NEWLINE INDENT NEWLINE* (stmt NEWLINE*)+ DEDENT */
	if (NCH(n) == 1) {
		com_node(c, CHILD(n, 0));
	}
	else {
		int i;
		for (i = 0; i < NCH(n); i++) {
			node *ch = CHILD(n, i);
			if (TYPE(ch) == stmt)
				com_node(c, ch);
		}
	}
}

static void
com_funcdef(c, n)
	struct compiling *c;
	node *n;
{
	object *v;
	REQ(n, funcdef); /* funcdef: 'def' NAME parameters ':' suite */
	v = (object *)compile(n);
	if (v == NULL)
		c->c_errors++;
	else {
		int i = com_addconst(c, v);
		com_addoparg(c, LOAD_CONST, i);
		com_addbyte(c, BUILD_FUNCTION);
		com_addopname(c, STORE_NAME, CHILD(n, 1));
		DECREF(v);
	}
}

static void
com_classdef(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, classdef);
	/*
	classdef: 'class' NAME parameters ['=' baselist] ':' suite
	baselist: atom arguments (',' atom arguments)*
	arguments: '(' [testlist] ')'
	*/
	err_setstr(SystemError, "can't compile 'class' yet");
	c->c_errors++;
}

static void
com_node(c, n)
	struct compiling *c;
	node *n;
{
	switch (TYPE(n)) {
	
	/* Definition nodes */
	
	case funcdef:
		com_funcdef(c, n);
		break;
	case classdef:
		com_classdef(c, n);
		break;
	
	/* Trivial parse tree nodes */
	
	case stmt:
	case simple_stmt:
	case flow_stmt:
	case compound_stmt:
		com_node(c, CHILD(n, 0));
		break;

	/* Statement nodes */
	
	case expr_stmt:
		com_expr_stmt(c, n);
		break;
	case print_stmt:
		com_print_stmt(c, n);
		break;
	case del_stmt: /* 'del' exprlist NEWLINE */
		com_assign(c, CHILD(n, 1), 0/*delete*/);
		break;
	case pass_stmt:
		break;
	case break_stmt:
		com_addbyte(c, BREAK_LOOP);
		break;
	case return_stmt:
		com_return_stmt(c, n);
		break;
	case raise_stmt:
		com_raise_stmt(c, n);
		break;
	case import_stmt:
		com_import_stmt(c, n);
		break;
	case if_stmt:
		com_if_stmt(c, n);
		break;
	case while_stmt:
		com_while_stmt(c, n);
		break;
	case for_stmt:
		com_for_stmt(c, n);
		break;
	case try_stmt:
		com_try_stmt(c, n);
		break;
	case suite:
		com_suite(c, n);
		break;
	
	/* Expression nodes */
	
	case testlist:
		com_list(c, n);
		break;
	case test:
		com_test(c, n);
		break;
	case and_test:
		com_and_test(c, n);
		break;
	case not_test:
		com_not_test(c, n);
		break;
	case comparison:
		com_comparison(c, n);
		break;
	case exprlist:
		com_list(c, n);
		break;
	case expr:
		com_expr(c, n);
		break;
	case term:
		com_term(c, n);
		break;
	case factor:
		com_factor(c, n);
		break;
	case atom:
		com_atom(c, n);
		break;
	
	default:
		fprintf(stderr, "node type %d\n", TYPE(n));
		err_setstr(SystemError, "com_node: unexpected node type");
		c->c_errors++;
	}
}

static void com_fplist PROTO((struct compiling *, node *));

static void
com_fpdef(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, fpdef); /* fpdef: NAME | '(' fplist ')' */
	if (TYPE(CHILD(n, 0)) == LPAR)
		com_fplist(c, CHILD(n, 1));
	else
		com_addopname(c, STORE_NAME, CHILD(n, 0));
}

static void
com_fplist(c, n)
	struct compiling *c;
	node *n;
{
	REQ(n, fplist); /* fplist: fpdef (',' fpdef)* */
	if (NCH(n) == 1) {
		com_fpdef(c, CHILD(n, 0));
	}
	else {
		int i;
		com_addoparg(c, UNPACK_TUPLE, (NCH(n)+1)/2);
		for (i = 0; i < NCH(n); i += 2)
			com_fpdef(c, CHILD(n, i));
	}
}

static void
com_file_input(c, n)
	struct compiling *c;
	node *n;
{
	int i;
	REQ(n, file_input); /* (NEWLINE | stmt)* ENDMARKER */
	for (i = 0; i < NCH(n); i++) {
		node *ch = CHILD(n, i);
		if (TYPE(ch) != ENDMARKER && TYPE(ch) != NEWLINE)
			com_node(c, ch);
	}
}

/* Top-level compile-node interface */

static void
compile_funcdef(c, n)
	struct compiling *c;
	node *n;
{
	node *ch;
	REQ(n, funcdef); /* funcdef: 'def' NAME parameters ':' suite */
	ch = CHILD(n, 2); /* parameters: '(' [fplist] ')' */
	ch = CHILD(ch, 1); /* ')' | fplist */
	if (TYPE(ch) == RPAR)
		com_addbyte(c, REFUSE_ARGS);
	else {
		com_addbyte(c, REQUIRE_ARGS);
		com_fplist(c, ch);
	}
	com_node(c, CHILD(n, 4));
	com_addoparg(c, LOAD_CONST, com_addconst(c, None));
	com_addbyte(c, RETURN_VALUE);
}

static void
compile_node(c, n)
	struct compiling *c;
	node *n;
{
	switch (TYPE(n)) {
	
	case single_input:
		/* NEWLINE | simple_stmt | compound_stmt NEWLINE */
		n = CHILD(n, 0);
		if (TYPE(n) != NEWLINE)
			com_node(c, n);
		break;
	
	case file_input:
		com_file_input(c, n);
		break;
	
	case expr_input:
	case eval_input:
		com_node(c, CHILD(n, 0));
		break;
	
	case funcdef:
		compile_funcdef(c, n);
		break;
	
	default:
		fprintf(stderr, "node type %d\n", TYPE(n));
		err_setstr(SystemError, "compile_node: unexpected node type");
		c->c_errors++;
	}
}

codeobject *
compile(n)
	node *n;
{
	struct compiling sc;
	codeobject *co;
	if (!com_init(&sc))
		return NULL;
	compile_node(&sc, n);
	com_done(&sc);
	if (sc.c_errors == 0)
		co = newcodeobject(sc.c_code, sc.c_consts, sc.c_names);
	else
		co = NULL;
	com_free(&sc);
	return co;
}
