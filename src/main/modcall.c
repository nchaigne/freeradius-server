/*
 * @name modcall.c
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 */

RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modpriv.h>
#include <freeradius-devel/interpreter.h>
#include <freeradius-devel/parser.h>


/* Here's where we recognize all of our keywords: first the rcodes, then the
 * actions */
const FR_NAME_NUMBER mod_rcode_table[] = {
	{ "reject",     RLM_MODULE_REJECT       },
	{ "fail",       RLM_MODULE_FAIL	 },
	{ "ok",	 	RLM_MODULE_OK	   },
	{ "handled",    RLM_MODULE_HANDLED      },
	{ "invalid",    RLM_MODULE_INVALID      },
	{ "userlock",   RLM_MODULE_USERLOCK     },
	{ "notfound",   RLM_MODULE_NOTFOUND     },
	{ "noop",       RLM_MODULE_NOOP	 },
	{ "updated",    RLM_MODULE_UPDATED      },
	{ NULL, 0 }
};


/* Some short names for debugging output */
char const * const comp2str[] = {
	"authenticate",
	"authorize",
	"preacct",
	"accounting",
	"session",
	"pre-proxy",
	"post-proxy",
	"post-auth"
#ifdef WITH_COA
	,
	"recv-coa",
	"send-coa"
#endif
};


static char const modcall_spaces[] = "                                                                                                                                                                                                                                                                ";


#if 0
static char const *action2str(int action)
{
	static char buf[32];
	if(action==MOD_ACTION_RETURN)
		return "return";
	if(action==MOD_ACTION_REJECT)
		return "reject";
	snprintf(buf, sizeof buf, "%d", action);
	return buf;
}

/* If you suspect a bug in the parser, you'll want to use these dump
 * functions. dump_tree should reproduce a whole tree exactly as it was found
 * in radiusd.conf, but in long form (all actions explicitly defined) */
static void dump_mc(modcallable *c, int indent)
{
	int i;

	if(c->type==MOD_SINGLE) {
		modsingle *single = mod_callabletosingle(c);
		DEBUG("%.*s%s {", indent, "\t\t\t\t\t\t\t\t\t\t\t",
			single->modinst->name);
	} else if ((c->type > MOD_SINGLE) && (c->type <= MOD_POLICY)) {
		modgroup *g = mod_callabletogroup(c);
		modcallable *p;
		DEBUG("%.*s%s {", indent, "\t\t\t\t\t\t\t\t\t\t\t",
		      unlang_keyword[c->type]);
		for(p = g->children;p;p = p->next)
			dump_mc(p, indent+1);
	} /* else ignore it for now */

	for(i = 0; i<RLM_MODULE_NUMCODES; ++i) {
		DEBUG("%.*s%s = %s", indent+1, "\t\t\t\t\t\t\t\t\t\t\t",
		      fr_int2str(mod_rcode_table, i, "<invalid>"),
		      action2str(c->actions[i]));
	}

	DEBUG("%.*s}", indent, "\t\t\t\t\t\t\t\t\t\t\t");
}

static void dump_tree(rlm_components_t comp, modcallable *c)
{
	DEBUG("[%s]", comp2str[comp]);
	dump_mc(c, 0);
}
#else
#define dump_tree(a, b)
#endif

/* These are the default actions. For each component, the group{} block
 * behaves like the code from the old module_*() function. redundant{}
 * are based on my guesses of what they will be used for. --Pac. */
static const int
defaultactions[MOD_COUNT][GROUPTYPE_COUNT][RLM_MODULE_NUMCODES] =
{
	/* authenticate */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			1,			/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			1,			/* noop     */
			1			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	},
	/* authorize */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			3,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			1,			/* notfound */
			2,			/* noop     */
			4			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	},
	/* preacct */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			2,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			1,			/* noop     */
			3			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	},
	/* accounting */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			2,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			1,			/* noop     */
			3			/* updated  */
		},
		/* redundant */
		{
			1,			/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			1,			/* invalid  */
			1,			/* userlock */
			1,			/* notfound */
			2,			/* noop     */
			4			/* updated  */
		}
	},
	/* checksimul */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	},
	/* pre-proxy */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			3,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			1,			/* notfound */
			2,			/* noop     */
			4			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	},
	/* post-proxy */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			3,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			1,			/* notfound */
			2,			/* noop     */
			4			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	},
	/* post-auth */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			3,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			1,			/* notfound */
			2,			/* noop     */
			4			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	}
#ifdef WITH_COA
	,
	/* recv-coa */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			3,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			1,			/* notfound */
			2,			/* noop     */
			4			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	},
	/* send-coa */
	{
		/* group */
		{
			MOD_ACTION_RETURN,	/* reject   */
			MOD_ACTION_RETURN,	/* fail     */
			3,			/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			1,			/* notfound */
			2,			/* noop     */
			4			/* updated  */
		},
		/* redundant */
		{
			MOD_ACTION_RETURN,	/* reject   */
			1,			/* fail     */
			MOD_ACTION_RETURN,	/* ok       */
			MOD_ACTION_RETURN,	/* handled  */
			MOD_ACTION_RETURN,	/* invalid  */
			MOD_ACTION_RETURN,	/* userlock */
			MOD_ACTION_RETURN,	/* notfound */
			MOD_ACTION_RETURN,	/* noop     */
			MOD_ACTION_RETURN	/* updated  */
		}
	}
#endif
};

static const int authtype_actions[GROUPTYPE_COUNT][RLM_MODULE_NUMCODES] =
{
	/* group */
	{
		MOD_ACTION_RETURN,	/* reject   */
		MOD_ACTION_RETURN,	/* fail     */
		2,			/* ok       */
		MOD_ACTION_RETURN,	/* handled  */
		MOD_ACTION_RETURN,	/* invalid  */
		MOD_ACTION_RETURN,	/* userlock */
		1,			/* notfound */
		3,			/* noop     */
		4			/* updated  */
	},
	/* redundant */
	{
		MOD_ACTION_RETURN,	/* reject   */
		1,			/* fail     */
		MOD_ACTION_RETURN,	/* ok       */
		MOD_ACTION_RETURN,	/* handled  */
		MOD_ACTION_RETURN,	/* invalid  */
		MOD_ACTION_RETURN,	/* userlock */
		MOD_ACTION_RETURN,	/* notfound */
		MOD_ACTION_RETURN,	/* noop     */
		MOD_ACTION_RETURN	/* updated  */
	}
};


#ifdef WITH_UNLANG
static bool pass2_fixup_xlat(CONF_ITEM const *ci, vp_tmpl_t **pvpt, bool convert,
			       fr_dict_attr_t const *da)
{
	ssize_t slen;
	char *fmt;
	char const *error;
	xlat_exp_t *head;
	vp_tmpl_t *vpt;

	vpt = *pvpt;

	rad_assert(vpt->type == TMPL_TYPE_XLAT);

	fmt = talloc_typed_strdup(vpt, vpt->name);
	slen = xlat_tokenize(vpt, fmt, &head, &error);

	if (slen < 0) {
		char *spaces, *text;

		fr_canonicalize_error(vpt, &spaces, &text, slen, vpt->name);

		cf_log_err(ci, "Failed parsing expanded string:");
		cf_log_err(ci, "%s", text);
		cf_log_err(ci, "%s^ %s", spaces, error);

		talloc_free(spaces);
		talloc_free(text);
		return false;
	}

	/*
	 *	Convert %{Attribute-Name} to &Attribute-Name
	 */
	if (convert) {
		vp_tmpl_t *attr;

		attr = xlat_to_tmpl_attr(talloc_parent(vpt), head);
		if (attr) {
			/*
			 *	If it's a virtual attribute, leave it
			 *	alone.
			 */
			if (attr->tmpl_da->flags.virtual) {
				talloc_free(attr);
				return true;
			}

			/*
			 *	If the attribute is of incompatible
			 *	type, leave it alone.
			 */
			if (da && (da->type != attr->tmpl_da->type)) {
				talloc_free(attr);
				return true;
			}

			if (cf_item_is_pair(ci)) {
				CONF_PAIR *cp = cf_item_to_pair(ci);

				WARN("%s[%d]: Please change \"%%{%s}\" to &%s",
				       cf_pair_filename(cp), cf_pair_lineno(cp),
				       attr->name, attr->name);
			} else {
				CONF_SECTION *cs = cf_item_to_section(ci);

				WARN("%s[%d]: Please change \"%%{%s}\" to &%s",
				       cf_section_filename(cs), cf_section_lineno(cs),
				       attr->name, attr->name);
			}
			TALLOC_FREE(*pvpt);
			*pvpt = attr;
			return true;
		}
	}

	/*
	 *	Re-write it to be a pre-parsed XLAT structure.
	 */
	vpt->type = TMPL_TYPE_XLAT_STRUCT;
	vpt->tmpl_xlat = head;

	return true;
}


#ifdef HAVE_REGEX
static bool pass2_fixup_regex(CONF_ITEM const *ci, vp_tmpl_t *vpt)
{
	ssize_t slen;
	regex_t *preg;

	rad_assert(vpt->type == TMPL_TYPE_REGEX);

	/*
	 *	It's a dynamic expansion.  We can't expand the string,
	 *	but we can pre-parse it as an xlat struct.  In that
	 *	case, we convert it to a pre-compiled XLAT.
	 *
	 *	This is a little more complicated than it needs to be
	 *	because radius_evaluate_map() keys off of the src
	 *	template type, instead of the operators.  And, the
	 *	pass2_fixup_xlat() function expects to get passed an
	 *	XLAT instead of a REGEX.
	 */
	if (strchr(vpt->name, '%')) {
		vpt->type = TMPL_TYPE_XLAT;
		return pass2_fixup_xlat(ci, &vpt, false, NULL);
	}

	slen = regex_compile(vpt, &preg, vpt->name, vpt->len,
			     vpt->tmpl_iflag, vpt->tmpl_mflag, true, false);
	if (slen <= 0) {
		char *spaces, *text;

		fr_canonicalize_error(vpt, &spaces, &text, slen, vpt->name);

		cf_log_err(ci, "Invalid regular expression:");
		cf_log_err(ci, "%s", text);
		cf_log_err(ci, "%s^ %s", spaces, fr_strerror());

		talloc_free(spaces);
		talloc_free(text);

		return false;
	}

	vpt->type = TMPL_TYPE_REGEX_STRUCT;
	vpt->tmpl_preg = preg;

	return true;
}
#endif

static bool pass2_fixup_undefined(CONF_ITEM const *ci, vp_tmpl_t *vpt)
{
	fr_dict_attr_t const *da;

	rad_assert(vpt->type == TMPL_TYPE_ATTR_UNDEFINED);

	da = fr_dict_attr_by_name(NULL, vpt->tmpl_unknown_name);
	if (!da) {
		cf_log_err(ci, "Unknown attribute '%s'", vpt->tmpl_unknown_name);
		return false;
	}

	vpt->tmpl_da = da;
	vpt->type = TMPL_TYPE_ATTR;
	return true;
}


static bool pass2_fixup_tmpl(CONF_ITEM const *ci, vp_tmpl_t **pvpt, bool convert)
{
	vp_tmpl_t *vpt = *pvpt;

	if (vpt->type == TMPL_TYPE_XLAT) {
		return pass2_fixup_xlat(ci, pvpt, convert, NULL);
	}

	/*
	 *	The existence check might have been &Foo-Bar,
	 *	where Foo-Bar is defined by a module.
	 */
	if (vpt->type == TMPL_TYPE_ATTR_UNDEFINED) {
		return pass2_fixup_undefined(ci, vpt);
	}

	/*
	 *	Convert virtual &Attr-Foo to "%{Attr-Foo}"
	 */
	if ((vpt->type == TMPL_TYPE_ATTR) && vpt->tmpl_da->flags.virtual) {
		vpt->tmpl_xlat = xlat_from_tmpl_attr(vpt, vpt);
		vpt->type = TMPL_TYPE_XLAT_STRUCT;
	}

	return true;
}

static bool pass2_cond_callback(void *ctx, fr_cond_t *c)
{
	vp_map_t *map;
	vp_tmpl_t *vpt;

	/*
	 *	These don't get optimized.
	 */
	if ((c->type == COND_TYPE_TRUE) ||
	    (c->type == COND_TYPE_FALSE)) {
		return true;
	}

	/*
	 *	Call children.
	 */
	if (c->type == COND_TYPE_CHILD) {
		return pass2_cond_callback(ctx, c->data.child);
	}

	/*
	 *	Fix up the template.
	 */
	if (c->type == COND_TYPE_EXISTS) {
		rad_assert(c->data.vpt->type != TMPL_TYPE_REGEX);
		return pass2_fixup_tmpl(c->ci, &c->data.vpt, true);
	}

	/*
	 *	And tons of complicated checks.
	 */
	rad_assert(c->type == COND_TYPE_MAP);

	map = c->data.map;	/* shorter */

	/*
	 *	Auth-Type := foo
	 *
	 *	Where "foo" is dynamically defined.
	 */
	if (c->pass2_fixup == PASS2_FIXUP_TYPE) {
		if (!fr_dict_enum_by_name(NULL, map->lhs->tmpl_da, map->rhs->name)) {
			cf_log_err(map->ci, "Invalid reference to non-existent %s %s { ... }",
				   map->lhs->tmpl_da->name,
				   map->rhs->name);
			return false;
		}

		/*
		 *	These guys can't have a paircompare fixup applied.
		 */
		c->pass2_fixup = PASS2_FIXUP_NONE;
		return true;
	}

	if (c->pass2_fixup == PASS2_FIXUP_ATTR) {
		if (map->lhs->type == TMPL_TYPE_ATTR_UNDEFINED) {
			if (!pass2_fixup_undefined(map->ci, map->lhs)) return false;
		}

		if (map->rhs->type == TMPL_TYPE_ATTR_UNDEFINED) {
			if (!pass2_fixup_undefined(map->ci, map->rhs)) return false;
		}

		c->pass2_fixup = PASS2_FIXUP_NONE;
	}

	/*
	 *	Just in case someone adds a new fixup later.
	 */
	rad_assert((c->pass2_fixup == PASS2_FIXUP_NONE) ||
		   (c->pass2_fixup == PASS2_PAIRCOMPARE));

	/*
	 *	Precompile xlat's
	 */
	if (map->lhs->type == TMPL_TYPE_XLAT) {
		/*
		 *	Compile the LHS to an attribute reference only
		 *	if the RHS is a literal.
		 *
		 *	@todo v3.1: allow anything anywhere.
		 */
		if (map->rhs->type != TMPL_TYPE_UNPARSED) {
			if (!pass2_fixup_xlat(map->ci, &map->lhs, false, NULL)) {
				return false;
			}
		} else {
			if (!pass2_fixup_xlat(map->ci, &map->lhs, true, NULL)) {
				return false;
			}

			/*
			 *	Attribute compared to a literal gets
			 *	the literal cast to the data type of
			 *	the attribute.
			 *
			 *	The code in parser.c did this for
			 *
			 *		&Attr == data
			 *
			 *	But now we've just converted "%{Attr}"
			 *	to &Attr, so we've got to do it again.
			 */
			if ((map->lhs->type == TMPL_TYPE_ATTR) &&
			    (map->rhs->type == TMPL_TYPE_UNPARSED)) {
				/*
				 *	RHS is hex, try to parse it as
				 *	type-specific data.
				 */
				if (map->lhs->auto_converted &&
				    (map->rhs->name[0] == '0') && (map->rhs->name[1] == 'x') &&
				    (map->rhs->len > 2) && ((map->rhs->len & 0x01) == 0)) {
					vpt = map->rhs;
					map->rhs = NULL;

					if (!map_cast_from_hex(map, T_BARE_WORD, vpt->name)) {
						map->rhs = vpt;
						cf_log_err(map->ci, "%s", fr_strerror());
						return -1;
					}
					talloc_free(vpt);

				} else if ((map->rhs->len > 0) ||
					   (map->op != T_OP_CMP_EQ) ||
					   (map->lhs->tmpl_da->type == PW_TYPE_STRING) ||
					   (map->lhs->tmpl_da->type == PW_TYPE_OCTETS)) {

					if (tmpl_cast_in_place(map->rhs, map->lhs->tmpl_da->type, map->lhs->tmpl_da) < 0) {
						cf_log_err(map->ci, "Failed to parse data type %s from string: %s",
							   fr_int2str(dict_attr_types, map->lhs->tmpl_da->type, "<UNKNOWN>"),
							   map->rhs->name);
						return false;
					} /* else the cast was successful */

				} else {	/* RHS is empty, it's just a check for empty / non-empty string */
					vpt = talloc_steal(c, map->lhs);
					map->lhs = NULL;
					talloc_free(c->data.map);

					/*
					 *	"%{Foo}" == '' ---> !Foo
					 *	"%{Foo}" != '' ---> Foo
					 */
					c->type = COND_TYPE_EXISTS;
					c->data.vpt = vpt;
					c->negate = !c->negate;

					WARN("%s[%d]: Please change (\"%%{%s}\" %s '') to %c&%s",
					     cf_section_filename(cf_item_to_section(c->ci)),
					     cf_section_lineno(cf_item_to_section(c->ci)),
					     vpt->name, c->negate ? "==" : "!=",
					     c->negate ? '!' : ' ', vpt->name);

					/*
					 *	No more RHS, so we can't do more optimizations
					 */
					return true;
				}
			}
		}
	}

	if (map->rhs->type == TMPL_TYPE_XLAT) {
		/*
		 *	Convert the RHS to an attribute reference only
		 *	if the LHS is an attribute reference, AND is
		 *	of the same type as the RHS.
		 *
		 *	We can fix this when the code in evaluate.c
		 *	can handle strings on the LHS, and attributes
		 *	on the RHS.  For now, the code in parser.c
		 *	forbids this.
		 */
		if (map->lhs->type == TMPL_TYPE_ATTR) {
			fr_dict_attr_t const *da = c->cast;

			if (!c->cast) da = map->lhs->tmpl_da;

			if (!pass2_fixup_xlat(map->ci, &map->rhs, true, da)) {
				return false;
			}

		} else {
			if (!pass2_fixup_xlat(map->ci, &map->rhs, false, NULL)) {
				return false;
			}
		}
	}

	/*
	 *	Convert bare refs to %{Foreach-Variable-N}
	 */
	if ((map->lhs->type == TMPL_TYPE_UNPARSED) &&
	    (strncmp(map->lhs->name, "Foreach-Variable-", 17) == 0)) {
		char *fmt;
		ssize_t slen;

		fmt = talloc_asprintf(map->lhs, "%%{%s}", map->lhs->name);
		slen = tmpl_afrom_str(map, &vpt, fmt, talloc_array_length(fmt) - 1,
				      T_DOUBLE_QUOTED_STRING, REQUEST_CURRENT, PAIR_LIST_REQUEST, true);
		if (slen < 0) {
			char *spaces, *text;

			fr_canonicalize_error(map->ci, &spaces, &text, slen, fr_strerror());

			cf_log_err(map->ci, "Failed converting %s to xlat", map->lhs->name);
			cf_log_err(map->ci, "%s", fmt);
			cf_log_err(map->ci, "%s^ %s", spaces, text);

			talloc_free(spaces);
			talloc_free(text);
			talloc_free(fmt);

			return false;
		}
		talloc_free(map->lhs);
		map->lhs = vpt;
	}

#ifdef HAVE_REGEX
	if (map->rhs->type == TMPL_TYPE_REGEX) {
		if (!pass2_fixup_regex(map->ci, map->rhs)) {
			return false;
		}
	}
	rad_assert(map->lhs->type != TMPL_TYPE_REGEX);
#endif

	/*
	 *	Convert &Packet-Type to "%{Packet-Type}", because
	 *	these attributes don't really exist.  The code to
	 *	find an attribute reference doesn't work, but the
	 *	xlat code does.
	 */
	vpt = c->data.map->lhs;
	if ((vpt->type == TMPL_TYPE_ATTR) && vpt->tmpl_da->flags.virtual) {
		if (!c->cast) c->cast = vpt->tmpl_da;
		vpt->tmpl_xlat = xlat_from_tmpl_attr(vpt, vpt);
		vpt->type = TMPL_TYPE_XLAT_STRUCT;
	}

	/*
	 *	@todo v3.1: do the same thing for the RHS...
	 */

	/*
	 *	Only attributes can have a paircompare registered, and
	 *	they can only be with the current REQUEST, and only
	 *	with the request pairs.
	 */
	if ((map->lhs->type != TMPL_TYPE_ATTR) ||
	    (map->lhs->tmpl_request != REQUEST_CURRENT) ||
	    (map->lhs->tmpl_list != PAIR_LIST_REQUEST)) {
		return true;
	}

	if (!radius_find_compare(map->lhs->tmpl_da)) return true;

	if (map->rhs->type == TMPL_TYPE_ATTR) {
		cf_log_err(map->ci, "Cannot compare virtual attribute %s to another attribute",
			   map->lhs->name);
		return false;
	}

	if (map->rhs->type == TMPL_TYPE_REGEX) {
		cf_log_err(map->ci, "Cannot compare virtual attribute %s via a regex",
			   map->lhs->name);
		return false;
	}

	if (c->cast) {
		cf_log_err(map->ci, "Cannot cast virtual attribute %s",
			   map->lhs->name);
		return false;
	}

	if (map->op != T_OP_CMP_EQ) {
		cf_log_err(map->ci, "Must use '==' for comparisons with virtual attribute %s",
			   map->lhs->name);
		return false;
	}

	/*
	 *	Mark it as requiring a paircompare() call, instead of
	 *	fr_pair_cmp().
	 */
	c->pass2_fixup = PASS2_PAIRCOMPARE;

	return true;
}


/*
 *	Compile the RHS of update sections to xlat_exp_t
 */
static bool pass2_fixup_update(modgroup *g)
{
	vp_map_t *map;

	for (map = g->map; map != NULL; map = map->next) {
		if (map->rhs->type == TMPL_TYPE_XLAT) {
			rad_assert(map->rhs->tmpl_xlat == NULL);

			/*
			 *	FIXME: compile to attribute && handle
			 *	the conversion in map_to_vp().
			 */
			if (!pass2_fixup_xlat(map->ci, &map->rhs, false, NULL)) {
				return false;
			}
		}

		rad_assert(map->rhs->type != TMPL_TYPE_REGEX);

		/*
		 *	Deal with undefined attributes now.
		 */
		if (map->lhs->type == TMPL_TYPE_ATTR_UNDEFINED) {
			if (!pass2_fixup_undefined(map->ci, map->lhs)) return false;
		}

		if (map->rhs->type == TMPL_TYPE_ATTR_UNDEFINED) {
			if (!pass2_fixup_undefined(map->ci, map->rhs)) return false;
		}
	}

	return true;
}

/*
 *	Compile the RHS of map sections to xlat_exp_t
 */
static bool pass2_fixup_map(modgroup *g)
{
	/*
	 *	Compile the map
	 */
	if (!pass2_fixup_update(g)) return false;

	return pass2_fixup_tmpl(g->map->ci, &g->vpt, false);
}
#endif

void modcall_debug(modcallable *mc, int depth)
{
	modcallable *this;
	modgroup *g;
	vp_map_t *map;
	char buffer[1024];

	for (this = mc; this != NULL; this = this->next) {
		switch (this->type) {
		default:
			break;

		case MOD_SINGLE: {
			modsingle *single = mod_callabletosingle(this);

			DEBUG("%.*s%s", depth, modcall_spaces,
				single->modinst->name);
			}
			break;

#ifdef WITH_UNLANG
		case MOD_MAP:
			g = mod_callabletogroup(this); /* FIXMAP: print option 3, too */
			DEBUG("%.*s%s %s {", depth, modcall_spaces,
			      unlang_keyword[this->type],
			      cf_section_name2(g->cs));
			goto print_map;

		case MOD_UPDATE:
			g = mod_callabletogroup(this);
			DEBUG("%.*s%s {", depth, modcall_spaces,
				unlang_keyword[this->type]);

		print_map:
			for (map = g->map; map != NULL; map = map->next) {
				map_snprint(buffer, sizeof(buffer), map);
				DEBUG("%.*s%s", depth + 1, modcall_spaces, buffer);
			}

			DEBUG("%.*s}", depth, modcall_spaces);
			break;

		case MOD_ELSE:
			g = mod_callabletogroup(this);
			DEBUG("%.*s%s {", depth, modcall_spaces,
				unlang_keyword[this->type]);
			modcall_debug(g->children, depth + 1);
			DEBUG("%.*s}", depth, modcall_spaces);
			break;

		case MOD_IF:
		case MOD_ELSIF:
			g = mod_callabletogroup(this);
			fr_cond_snprint(buffer, sizeof(buffer), g->cond);
			DEBUG("%.*s%s (%s) {", depth, modcall_spaces,
				unlang_keyword[this->type], buffer);
			modcall_debug(g->children, depth + 1);
			DEBUG("%.*s}", depth, modcall_spaces);
			break;

		case MOD_SWITCH:
		case MOD_CASE:
			g = mod_callabletogroup(this);
			tmpl_snprint(buffer, sizeof(buffer), g->vpt, NULL);
			DEBUG("%.*s%s %s {", depth, modcall_spaces,
				unlang_keyword[this->type], buffer);
			modcall_debug(g->children, depth + 1);
			DEBUG("%.*s}", depth, modcall_spaces);
			break;

		case MOD_POLICY:
		case MOD_FOREACH:
			g = mod_callabletogroup(this);
			DEBUG("%.*s%s %s {", depth, modcall_spaces,
				unlang_keyword[this->type], this->name);
			modcall_debug(g->children, depth + 1);
			DEBUG("%.*s}", depth, modcall_spaces);
			break;

		case MOD_BREAK:
			DEBUG("%.*sbreak", depth, modcall_spaces);
			break;

#endif
		case MOD_GROUP:
			g = mod_callabletogroup(this);
			DEBUG("%.*s%s {", depth, modcall_spaces,
			      unlang_keyword[this->type]);
			modcall_debug(g->children, depth + 1);
			DEBUG("%.*s}", depth, modcall_spaces);
			break;


		case MOD_LOAD_BALANCE:
		case MOD_REDUNDANT_LOAD_BALANCE:
			g = mod_callabletogroup(this);
			DEBUG("%.*s%s {", depth, modcall_spaces,
				unlang_keyword[this->type]);
			modcall_debug(g->children, depth + 1);
			DEBUG("%.*s}", depth, modcall_spaces);
			break;
		}
	}
}

#ifdef WITH_UNLANG
/** Validate and fixup a map that's part of an map section.
 *
 * @param map to validate.
 * @param ctx data to pass to fixup function (currently unused).
 * @return 0 if valid else -1.
 */
static int modcall_fixup_map(vp_map_t *map, UNUSED void *ctx)
{
	CONF_PAIR *cp = cf_item_to_pair(map->ci);

	/*
	 *	Anal-retentive checks.
	 */
	if (DEBUG_ENABLED3) {
		if ((map->lhs->type == TMPL_TYPE_ATTR) && (map->lhs->name[0] != '&')) {
			WARN("%s[%d]: Please change attribute reference to '&%s %s ...'",
			     cf_pair_filename(cp), cf_pair_lineno(cp),
			     map->lhs->name, fr_int2str(fr_tokens_table, map->op, "<INVALID>"));
		}

		if ((map->rhs->type == TMPL_TYPE_ATTR) && (map->rhs->name[0] != '&')) {
			WARN("%s[%d]: Please change attribute reference to '... %s &%s'",
			     cf_pair_filename(cp), cf_pair_lineno(cp),
			     fr_int2str(fr_tokens_table, map->op, "<INVALID>"), map->rhs->name);
		}
	}

	switch (map->lhs->type) {
	case TMPL_TYPE_ATTR:
	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_XLAT_STRUCT:
		break;

	default:
		cf_log_err(map->ci, "Left side of map must be an attribute "
		           "or an xlat (that expands to an attribute), not a %s",
		           fr_int2str(tmpl_names, map->lhs->type, "<INVALID>"));
		return -1;
	}

	switch (map->rhs->type) {
	case TMPL_TYPE_UNPARSED:
	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_XLAT_STRUCT:
	case TMPL_TYPE_ATTR:
	case TMPL_TYPE_EXEC:
		break;

	default:
		cf_log_err(map->ci, "Right side of map must be an attribute, literal, xlat or exec");
		return -1;
	}

	if (!fr_assignment_op[map->op] && !fr_equality_op[map->op]) {
		cf_log_err(map->ci, "Invalid operator \"%s\" in map section.  "
			   "Only assignment or filter operators are allowed",
			   fr_int2str(fr_tokens_table, map->op, "<INVALID>"));
		return -1;
	}

	return 0;
}


/** Validate and fixup a map that's part of an update section.
 *
 * @param map to validate.
 * @param ctx data to pass to fixup function (currently unused).
 * @return
 *	- 0 if valid.
 *	- -1 not valid.
 */
int modcall_fixup_update(vp_map_t *map, UNUSED void *ctx)
{
	CONF_PAIR *cp = cf_item_to_pair(map->ci);

	/*
	 *	Anal-retentive checks.
	 */
	if (DEBUG_ENABLED3) {
		if ((map->lhs->type == TMPL_TYPE_ATTR) && (map->lhs->name[0] != '&')) {
			WARN("%s[%d]: Please change attribute reference to '&%s %s ...'",
			     cf_pair_filename(cp), cf_pair_lineno(cp),
			     map->lhs->name, fr_int2str(fr_tokens_table, map->op, "<INVALID>"));
		}

		if ((map->rhs->type == TMPL_TYPE_ATTR) && (map->rhs->name[0] != '&')) {
			WARN("%s[%d]: Please change attribute reference to '... %s &%s'",
			     cf_pair_filename(cp), cf_pair_lineno(cp),
			     fr_int2str(fr_tokens_table, map->op, "<INVALID>"), map->rhs->name);
		}
	}

	/*
	 *	Values used by unary operators should be literal ANY
	 *
	 *	We then free the template and alloc a NULL one instead.
	 */
	if (map->op == T_OP_CMP_FALSE) {
		if ((map->rhs->type != TMPL_TYPE_UNPARSED) || (strcmp(map->rhs->name, "ANY") != 0)) {
			WARN("%s[%d] Wildcard deletion MUST use '!* ANY'",
			     cf_pair_filename(cp), cf_pair_lineno(cp));
		}

		TALLOC_FREE(map->rhs);

		map->rhs = tmpl_alloc(map, TMPL_TYPE_NULL, NULL, 0, T_INVALID);
	}

	/*
	 *	Lots of sanity checks for insane people...
	 */

	/*
	 *	What exactly where you expecting to happen here?
	 */
	if ((map->lhs->type == TMPL_TYPE_ATTR) &&
	    (map->rhs->type == TMPL_TYPE_LIST)) {
		cf_log_err(map->ci, "Can't copy list into an attribute");
		return -1;
	}

	/*
	 *	Depending on the attribute type, some operators are disallowed.
	 */
	if ((map->lhs->type == TMPL_TYPE_ATTR) && (!fr_assignment_op[map->op] && !fr_equality_op[map->op])) {
		cf_log_err(map->ci, "Invalid operator \"%s\" in update section.  "
			   "Only assignment or filter operators are allowed",
			   fr_int2str(fr_tokens_table, map->op, "<INVALID>"));
		return -1;
	}

	if (map->lhs->type == TMPL_TYPE_LIST) {
		/*
		 *	Can't copy an xlat expansion or literal into a list,
		 *	we don't know what type of attribute we'd need
		 *	to create.
		 *
		 *	The only exception is where were using a unary
		 *	operator like !*.
		 */
		if (map->op != T_OP_CMP_FALSE) switch (map->rhs->type) {
		case TMPL_TYPE_XLAT:
		case TMPL_TYPE_UNPARSED:
			cf_log_err(map->ci, "Can't copy value into list (we don't know which attribute to create)");
			return -1;

		default:
			break;
		}

		/*
		 *	Only += and :=, and !* operators are supported
		 *	for lists.
		 */
		switch (map->op) {
		case T_OP_CMP_FALSE:
			break;

		case T_OP_ADD:
			if ((map->rhs->type != TMPL_TYPE_LIST) &&
			    (map->rhs->type != TMPL_TYPE_EXEC)) {
				cf_log_err(map->ci, "Invalid source for list assignment '%s += ...'", map->lhs->name);
				return -1;
			}
			break;

		case T_OP_SET:
			if (map->rhs->type == TMPL_TYPE_EXEC) {
				WARN("%s[%d]: Please change ':=' to '=' for list assignment",
				     cf_pair_filename(cp), cf_pair_lineno(cp));
			}

			if (map->rhs->type != TMPL_TYPE_LIST) {
				cf_log_err(map->ci, "Invalid source for list assignment '%s := ...'", map->lhs->name);
				return -1;
			}
			break;

		case T_OP_EQ:
			if (map->rhs->type != TMPL_TYPE_EXEC) {
				cf_log_err(map->ci, "Invalid source for list assignment '%s = ...'", map->lhs->name);
				return -1;
			}
			break;

		default:
			cf_log_err(map->ci, "Operator \"%s\" not allowed for list assignment",
				   fr_int2str(fr_tokens_table, map->op, "<INVALID>"));
			return -1;
		}
	}

	/*
	 *	If the map has a unary operator there's no further
	 *	processing we need to, as RHS is unused.
	 */
	if (map->op == T_OP_CMP_FALSE) return 0;

	/*
	 *	If LHS is an attribute, and RHS is a literal, we can
	 *	preparse the information into a TMPL_TYPE_DATA.
	 *
	 *	Unless it's a unary operator in which case we
	 *	ignore map->rhs.
	 */
	if ((map->lhs->type == TMPL_TYPE_ATTR) && (map->rhs->type == TMPL_TYPE_UNPARSED)) {
		/*
		 *	Convert it to the correct type.
		 */
		if (map->lhs->auto_converted &&
		    (map->rhs->name[0] == '0') && (map->rhs->name[1] == 'x') &&
		    (map->rhs->len > 2) && ((map->rhs->len & 0x01) == 0)) {
			vp_tmpl_t *vpt = map->rhs;
			map->rhs = NULL;

			if (!map_cast_from_hex(map, T_BARE_WORD, vpt->name)) {
				map->rhs = vpt;
				cf_log_err(map->ci, "%s", fr_strerror());
				return -1;
			}
			talloc_free(vpt);

		/*
		 *	It's a literal string, just copy it.
		 *	Don't escape anything.
		 */
		} else if (tmpl_cast_in_place(map->rhs, map->lhs->tmpl_da->type, map->lhs->tmpl_da) < 0) {
			cf_log_err(map->ci, "%s", fr_strerror());
			return -1;
		}

		/*
		 *	Fixup LHS da if it doesn't match the type
		 *	of the RHS.
		 */
		if (map->lhs->tmpl_da->type != map->rhs->tmpl_data_type) {
			fr_dict_attr_t const *da;

			da = fr_dict_attr_by_type(NULL, map->lhs->tmpl_da->vendor, map->lhs->tmpl_da->attr,
						  map->rhs->tmpl_data_type);
			if (!da) {
				fr_strerror_printf("Cannot find %s variant of attribute \"%s\"",
						   fr_int2str(dict_attr_types, map->rhs->tmpl_data_type,
						   "<INVALID>"), map->lhs->tmpl_da->name);
				return -1;
			}
			map->lhs->tmpl_da = da;
		}
	} /* else we can't precompile the data */

	return 0;
}


static modcallable *compile_map(modcallable *parent, rlm_components_t component,
				CONF_SECTION *cs, UNUSED grouptype_t grouptype, grouptype_t parentgrouptype, UNUSED mod_type_t mod_type)
{
	int rcode;
	modgroup *g;
	modcallable *c;
	CONF_SECTION *modules;
	ssize_t slen;
	char const *tmpl_str;
	FR_TOKEN type;
	char quote;
	size_t tmpl_len, quoted_len;
	char *quoted_str;

	vp_map_t *head;
	vp_tmpl_t *vpt;

	map_proc_t *proc;
	map_proc_inst_t *proc_inst;

	char const *name2 = cf_section_name2(cs);

	modules = cf_section_sub_find(main_config.config, "modules");
	if (!modules) {
		cf_log_err_cs(cs, "'map' sections require a 'modules' section");
		return NULL;
	}

	proc = map_proc_find(name2);
	if (!proc) {
		cf_log_err_cs(cs, "Failed to find map processor '%s'", name2);
		return NULL;
	}

	tmpl_str = cf_section_argv(cs, 0); /* AFTER name1, name2 */
	if (!tmpl_str) {
		cf_log_err_cs(cs, "No template found in map");
		return NULL;
	}

	tmpl_len = strlen(tmpl_str);
	type = cf_section_argv_type(cs, 0);

	/*
	 *	Try to parse the template.
	 */
	slen = tmpl_afrom_str(cs, &vpt, tmpl_str, tmpl_len, type, REQUEST_CURRENT, PAIR_LIST_REQUEST, true);
	if (slen < 0) {
		cf_log_err_cs(cs, "Failed parsing map: %s", fr_strerror());
		return NULL;
	}

	/*
	 *	Limit the allowed template types.
	 */
	switch (vpt->type) {
	case TMPL_TYPE_UNPARSED:
	case TMPL_TYPE_ATTR:
	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_ATTR_UNDEFINED:
	case TMPL_TYPE_EXEC:
		break;

	default:
		talloc_free(vpt);
		cf_log_err_cs(cs, "Invalid third argument for map");
		return NULL;
	}

	/*
	 *	This looks at cs->name2 to determine which list to update
	 */
	rcode = map_afrom_cs(&head, cs, PAIR_LIST_REQUEST, PAIR_LIST_REQUEST, modcall_fixup_map, NULL, 256);
	if (rcode < 0) return NULL; /* message already printed */
	if (!head) {
		cf_log_err_cs(cs, "'map' sections cannot be empty");
		return NULL;
	}

	g = talloc_zero(parent, modgroup);
	proc_inst = map_proc_instantiate(g, proc, vpt, head);
	if (!proc_inst) {
		talloc_free(g);
		cf_log_err_cs(cs, "Failed instantiating map function '%s'", name2);
		return NULL;
	}

	c = mod_grouptocallable(g);

	c->parent = parent;
	c->next = NULL;

	switch (type) {
	case T_DOUBLE_QUOTED_STRING:
		quote = '"';
		break;

	case T_SINGLE_QUOTED_STRING:
		quote = '\'';
		break;

	case T_BACK_QUOTED_STRING:
		quote = '`';
		break;

	default:
		quote = '\0';
		break;
	}

	quoted_len = fr_snprint_len(tmpl_str, tmpl_len, quote);
	quoted_str = talloc_array(c, char, quoted_len);
	fr_snprint(quoted_str, quoted_len, tmpl_str, tmpl_len, quote);

	c->name = talloc_asprintf(c, "map %s %s", name2, quoted_str);
	c->debug_name = c->name;
	c->type = MOD_MAP;
	c->method = component;

	talloc_free(quoted_str);

	memcpy(c->actions, defaultactions[component][parentgrouptype],
	       sizeof(c->actions));

	g->grouptype = GROUPTYPE_SIMPLE;
	g->children = NULL;
	g->cs = cs;
	g->map = talloc_steal(g, head);
	g->vpt = talloc_steal(g, vpt);
	g->proc_inst = proc_inst;

	/*
	 *	Cache the module in the modgroup struct.
	 *
	 *	Ensure that the module has a "map" entry in its module
	 *	header?  Or ensure that the map is registered in the
	 *	"boostrap" phase, so that it's always available here.
	 */
	if (!pass2_fixup_map(g)) {
		talloc_free(g);
		return NULL;
	}
	g->done_pass2 = true;

	return c;

}

static modcallable *compile_update(modcallable *parent, rlm_components_t component,
				   CONF_SECTION *cs, grouptype_t grouptype, UNUSED grouptype_t parentgrouptype, UNUSED mod_type_t mod_type)
{
	int rcode;
	modgroup *g;
	modcallable *c;
	char const *name2 = cf_section_name2(cs);

	vp_map_t *head;

	/*
	 *	This looks at cs->name2 to determine which list to update
	 */
	rcode = map_afrom_cs(&head, cs, PAIR_LIST_REQUEST, PAIR_LIST_REQUEST, modcall_fixup_update, NULL, 128);
	if (rcode < 0) return NULL; /* message already printed */
	if (!head) {
		cf_log_err_cs(cs, "'update' sections cannot be empty");
		return NULL;
	}

	g = talloc_zero(parent, modgroup);
	c = mod_grouptocallable(g);

	c->type = MOD_UPDATE;
	c->parent = parent;
	c->next = NULL;

	if (name2) {
		c->name = name2;
		c->debug_name = talloc_asprintf(c, "update %s", name2);
	} else {
		c->name = unlang_keyword[c->type];
		c->debug_name = unlang_keyword[c->type];
	}
	c->method = component;

	memcpy(c->actions, defaultactions[component][GROUPTYPE_SIMPLE],
	       sizeof(c->actions));

	g->grouptype = grouptype;
	g->children = NULL;
	g->cs = cs;
	g->map = talloc_steal(g, head);

#ifdef WITH_CONF_WRITE
//	cf_data_add(cs, "update", g->map, NULL); /* for output normalization */
#endif

	if (!pass2_fixup_update(g)) {
		talloc_free(g);
		return NULL;
	}

	g->done_pass2 = true;

	return c;
}

/*
 *	Compile action && rcode for later use.
 */
static int compile_action_pair(modcallable *c, CONF_PAIR *cp)
{
	int action;
	char const *attr, *value;

	attr = cf_pair_attr(cp);
	value = cf_pair_value(cp);
	if (!value) return 0;

	if (!strcasecmp(value, "return"))
		action = MOD_ACTION_RETURN;

	else if (!strcasecmp(value, "break"))
		action = MOD_ACTION_RETURN;

	else if (!strcasecmp(value, "reject"))
		action = MOD_ACTION_REJECT;

	else if (strspn(value, "0123456789")==strlen(value)) {
		action = atoi(value);

		/*
		 *	Don't allow priority zero, for future use.
		 */
		if (action == 0) return 0;
	} else {
		cf_log_err_cp(cp, "Unknown action '%s'.\n",
			   value);
		return 0;
	}

	if (strcasecmp(attr, "default") != 0) {
		int rcode;

		rcode = fr_str2int(mod_rcode_table, attr, -1);
		if (rcode < 0) {
			cf_log_err_cp(cp,
				   "Unknown module rcode '%s'.\n",
				   attr);
			return 0;
		}
		c->actions[rcode] = action;

	} else {		/* set all unset values to the default */
		int i;

		for (i = 0; i < RLM_MODULE_NUMCODES; i++) {
			if (!c->actions[i]) c->actions[i] = action;
		}
	}

	return 1;
}

static bool compile_action_section(modcallable *c, CONF_ITEM *ci)
{
	CONF_ITEM *csi;
	CONF_SECTION *cs;

	if (!cf_item_is_section(ci)) return c;

	/*
	 *	Over-ride the default return codes of the module.
	 */
	cs = cf_item_to_section(ci);
	for (csi=cf_item_find_next(cs, NULL);
	     csi != NULL;
	     csi=cf_item_find_next(cs, csi)) {

		if (cf_item_is_section(csi)) {
			cf_log_err(csi, "Invalid subsection.  Expected 'action = value'");
			return false;
		}

		if (!cf_item_is_pair(csi)) continue;

		if (!compile_action_pair(c, cf_item_to_pair(csi))) {
			return false;
		}
	}

	return true;
}

static modcallable *compile_defaultactions(modcallable *c, modcallable *parent, rlm_components_t component, grouptype_t parentgrouptype)
{
	int i;

	/*
	 *	Set the default actions, if they haven't already been
	 *	set.
	 */
	for (i = 0; i < RLM_MODULE_NUMCODES; i++) {
		if (!c->actions[i]) {
			if (!parent || (component != MOD_AUTHENTICATE)) {
				c->actions[i] = defaultactions[component][parentgrouptype][i];
			} else { /* inside Auth-Type has different rules */
				c->actions[i] = authtype_actions[parentgrouptype][i];
			}
		}
	}

	/*
	 *	FIXME: If there are no children, return NULL?
	 */
	return c;
}


static modgroup *group_allocate(modcallable *parent, CONF_SECTION *cs,
				grouptype_t grouptype, mod_type_t mod_type, rlm_components_t component)
{
	modgroup *g;
	modcallable *c;
	TALLOC_CTX *ctx;

	ctx = parent;
	if (!ctx) ctx = cs;

	g = talloc_zero(ctx, modgroup);
	if (!g) return NULL;

	g->grouptype = grouptype;
	g->children = NULL;
	g->cs = cs;

	c = mod_grouptocallable(g);
	c->method = component;
	c->parent = parent;
	c->type = mod_type;
	c->next = NULL;
	memset(c->actions, 0, sizeof(c->actions));

	return g;
}


static modcallable *compile_empty(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
				  grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type,
				  fr_cond_type_t cond_type)
{
	modgroup *g;
	modcallable *c;

	g = group_allocate(parent, cs, grouptype, mod_type, component);
	if (!g) return NULL;

	c = mod_grouptocallable(g);
	if (!cs) {
		c->name = unlang_keyword[c->type];
		c->debug_name = c->name;

	} else {
		char const *name2;

		name2 = cf_section_name2(cs);
		if (!name2) {
			c->name = cf_section_name1(cs);
			c->debug_name = c->name;
		} else {
			c->name = name2;
			c->debug_name = talloc_asprintf(c, "%s %s", unlang_keyword[c->type], name2);
		}
	}

	if (cond_type != COND_TYPE_INVALID) {
		g->cond = talloc_zero(g, fr_cond_t);
		g->cond->type = cond_type;
	}

	return compile_defaultactions(c, parent, component, parentgrouptype);
}


static modcallable *compile_item(modcallable *parent, rlm_components_t component, CONF_ITEM *ci,
				 grouptype_t parent_grouptype, char const **modname);


/* modgroups are grown by adding a modcallable to the end */
static void add_child(modgroup *g, modcallable *c)
{
	if (!c) return;

	(void) talloc_steal(g, c);

	if (!g->children) {
		g->children = g->tail = c;
	} else {
		rad_assert(g->tail->next == NULL);
		g->tail->next = c;
		g->tail = c;
	}

	c->parent = mod_grouptocallable(g);
}

/*
 *	compile 'actions { ... }' inside of another group.
 */
static bool compile_action_subsection(modcallable *c, CONF_SECTION *cs, CONF_SECTION *subcs)
{
	CONF_ITEM *ci, *next;

	ci = cf_section_to_item(subcs);

	next = cf_item_find_next(cs, ci);
	if (next && (cf_item_is_pair(next) || cf_item_is_section(next))) {
		cf_log_err(ci, "'actions' MUST be the last block in a section");
		return false;
	}

	if (cf_section_name2(subcs) != NULL) {
		cf_log_err(ci, "Invalid name for 'actions' section");
		return false;
	}

	/*
	 *	Over-riding actions makes no sense in some situations.
	 *	They just don't make sense for many group types.
	 */
	if (!((c->type == MOD_CASE) || (c->type == MOD_IF) || (c->type == MOD_ELSIF) ||
	      (c->type == MOD_ELSE))) {
		cf_log_err(ci, "'actions' MUST NOT be in a '%s' block", unlang_keyword[c->type]);
		return false;
	}

	return compile_action_section(c, ci);
}


static modcallable *compile_children(modgroup *g, modcallable *parent, rlm_components_t component,
				     grouptype_t grouptype, grouptype_t parentgrouptype)
{
	CONF_ITEM *ci;
	modcallable *c;

	c = mod_grouptocallable(g);

	/*
	 *	Loop over the children of this group.
	 */
	for (ci=cf_item_find_next(g->cs, NULL);
	     ci != NULL;
	     ci=cf_item_find_next(g->cs, ci)) {

		/*
		 *	Sections are references to other groups, or
		 *	to modules with updated return codes.
		 */
		if (cf_item_is_section(ci)) {
			char const *name1 = NULL;
			modcallable *single;
			CONF_SECTION *subcs = cf_item_to_section(ci);

			/*
			 *	Skip precompiled blocks.
			 */
			if (cf_data_find(subcs, "unlang")) continue;

			/*
			 *	"actions" apply to the current group.
			 *	It's not a subgroup.
			 */
			name1 = cf_section_name1(subcs);
			if (strcmp(name1, "actions") == 0) {
				if (!compile_action_subsection(c, g->cs, subcs)) {
					talloc_free(c);
					return NULL;
				}

				continue;
			}

			/*
			 *	Otherwise it's a real keyword.
			 */
			single = compile_item(c, component, ci, grouptype, &name1);
			if (!single) {
				cf_log_err(ci, "Failed to parse \"%s\" subsection.",
				       cf_section_name1(subcs));
				talloc_free(c);
				return NULL;
			}
			add_child(g, single);

		} else if (!cf_item_is_pair(ci)) { /* CONF_DATA */
			continue;

		} else {
			char const *attr, *value;
			CONF_PAIR *cp = cf_item_to_pair(ci);

			attr = cf_pair_attr(cp);
			value = cf_pair_value(cp);

			/*
			 *	A CONF_PAIR is either a module
			 *	instance with no actions
			 *	specified ...
			 */
			if (!value) {
				modcallable *single;
				char const *name = NULL;

				single = compile_item(c, component, ci, grouptype, &name);
				if (!single) {
					/*
					 *	Skip optional modules, which start with '-'
					 */
					name = cf_pair_attr(cp);
					if (name[0] == '-') {
						WARN("%s[%d]: Ignoring \"%s\" (see raddb/mods-available/README.rst)",
						     cf_pair_filename(cp), cf_pair_lineno(cp), name + 1);
						continue;
					}

					cf_log_err(ci,
						   "Failed to parse \"%s\" entry.",
						   attr);
					talloc_free(c);
					return NULL;
				}
				add_child(g, single);

				/*
				 *	Or a module instance with action.
				 */
			} else if (!compile_action_pair(c, cp)) {
				talloc_free(c);
				return NULL;
			} /* else it worked */
		}
	}

	return compile_defaultactions(c, parent, component, parentgrouptype);
}


/*
 *	Generic "compile a section with more unlang inside of it".
 */
static modcallable *compile_group(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
				  grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	modgroup *g;
	modcallable *c;

	g = group_allocate(parent, cs, grouptype, mod_type, component);
	if (!g) return NULL;

	c = mod_grouptocallable(g);

	/*
	 *	Remember the name for printing, etc.
	 *
	 *	FIXME: We may also want to put the names into a
	 *	rbtree, so that groups can reference each other...
	 */
	c->name = unlang_keyword[c->type];
	c->debug_name = c->name;

	return compile_children(g, parent, component, grouptype, parentgrouptype);
}

static modcallable *compile_switch(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
				   grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	CONF_ITEM *ci;
	FR_TOKEN type;
	char const *name2;
	bool had_seen_default = false;
	modcallable *c;
	modgroup *g;
	ssize_t slen;

	name2 = cf_section_name2(cs);
	if (!name2) {
		cf_log_err_cs(cs, "You must specify a variable to switch over for 'switch'");
		return NULL;
	}

	g = group_allocate(parent, cs, grouptype, mod_type, component);
	if (!g) return NULL;

	/*
	 *	Create the template.  All attributes and xlats are
	 *	defined by now.
	 */
	type = cf_section_name2_type(cs);
	slen = tmpl_afrom_str(g, &g->vpt, name2, strlen(name2), type, REQUEST_CURRENT, PAIR_LIST_REQUEST, true);
	if (slen < 0) {
		char *spaces, *text;

		fr_canonicalize_error(cs, &spaces, &text, slen, fr_strerror());

		cf_log_err_cs(cs, "Syntax error");
		cf_log_err_cs(cs, "%s", name2);
		cf_log_err_cs(cs, "%s^ %s", spaces, text);

		talloc_free(g);
		talloc_free(spaces);
		talloc_free(text);

		return NULL;
	}

	/*
	 *	Walk through the children of the switch section,
	 *	ensuring that they're all 'case' statements
	 */
	for (ci = cf_item_find_next(cs, NULL);
	     ci != NULL;
	     ci = cf_item_find_next(cs, ci)) {
		CONF_SECTION *subcs;
		char const *name1;

		if (!cf_item_is_section(ci)) {
			if (!cf_item_is_pair(ci)) continue;

			cf_log_err(ci, "\"switch\" sections can only have \"case\" subsections");
			talloc_free(g);
			return NULL;
		}

		subcs = cf_item_to_section(ci);	/* can't return NULL */
		name1 = cf_section_name1(subcs);

		if (strcmp(name1, "case") != 0) {
			cf_log_err(ci, "\"switch\" sections can only have \"case\" subsections");
			talloc_free(g);
			return NULL;
		}

		name2 = cf_section_name2(subcs);
		if (!name2) {
			if (!had_seen_default) {
				had_seen_default = true;
				continue;
			}

			cf_log_err(ci, "Cannot have two 'default' case statements");
			talloc_free(g);
			return NULL;
		}
	}

	c = mod_grouptocallable(g);
	c->name = unlang_keyword[c->type];
	c->debug_name = talloc_asprintf(c, "%s %s", unlang_keyword[c->type], cf_section_name2(cs));

	/*
	 *	Fixup the template before compiling the children.
	 *	This is so that compile_case() can do attribute type
	 *	checks / casts against us.
	 */
	if (!pass2_fixup_tmpl(cf_section_to_item(g->cs), &g->vpt, true)) {
		talloc_free(g);
		return NULL;
	}

	return compile_children(g, parent, component, grouptype, parentgrouptype);
}

static modcallable *compile_case(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
				 grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	int i;
	char const *name2;
	modcallable *c;
	modgroup *g;
	vp_tmpl_t *vpt = NULL;

	if (!parent || (parent->type != MOD_SWITCH)) {
		cf_log_err_cs(cs, "\"case\" statements may only appear within a \"switch\" section");
		return NULL;
	}

	/*
	 *	case THING means "match THING"
	 *	case       means "match anything"
	 */
	name2 = cf_section_name2(cs);
	if (name2) {
		ssize_t slen;
		FR_TOKEN type;
		modgroup *f;

		type = cf_section_name2_type(cs);

		slen = tmpl_afrom_str(cs, &vpt, name2, strlen(name2), type, REQUEST_CURRENT, PAIR_LIST_REQUEST, true);
		if (slen < 0) {
			char *spaces, *text;

			fr_canonicalize_error(cs, &spaces, &text, slen, fr_strerror());

			cf_log_err_cs(cs, "Syntax error");
			cf_log_err_cs(cs, "%s", name2);
			cf_log_err_cs(cs, "%s^ %s", spaces, text);

			talloc_free(spaces);
			talloc_free(text);

			return NULL;
		}

		if (vpt->type == TMPL_TYPE_ATTR_UNDEFINED) {
			if (!pass2_fixup_undefined(cf_section_to_item(cs), vpt)) {
				talloc_free(vpt);
				return NULL;
			}
		}

		f = mod_callabletogroup(parent);
		rad_assert(f->vpt != NULL);

		/*
		 *	Do type-specific checks on the case statement
		 */

		/*
		 *	We're switching over an
		 *	attribute.  Check that the
		 *	values match.
		 */
		if ((vpt->type == TMPL_TYPE_UNPARSED) &&
		    (f->vpt->type == TMPL_TYPE_ATTR)) {
			rad_assert(f->vpt->tmpl_da != NULL);

			if (tmpl_cast_in_place(vpt, f->vpt->tmpl_da->type, f->vpt->tmpl_da) < 0) {
				cf_log_err_cs(cs, "Invalid argument for case statement: %s",
					      fr_strerror());
				talloc_free(vpt);
				return NULL;
			}
		}

		/*
		 *	Compile and sanity check xlat
		 *	expansions.
		 */
		if (vpt->type == TMPL_TYPE_XLAT) {
			fr_dict_attr_t const *da = NULL;

			if (f->vpt->type == TMPL_TYPE_ATTR) da = f->vpt->tmpl_da;

			/*
			 *	Don't expand xlat's into an
			 *	attribute of a different type.
			 */
			if (!pass2_fixup_xlat(cf_section_to_item(cs), &vpt, true, da)) {
				talloc_free(vpt);
				return NULL;
			}
		}
	} /* else it's a default 'case' statement */

	c = compile_group(parent, component, cs, grouptype, parentgrouptype, mod_type);
	if (!c) {
		talloc_free(vpt);
		return NULL;
	}

	/*
	 *	The interpretor expects this to be NULL for the
	 *	default case.  compile_group sets it to name2,
	 *	unless name2 is NULL, in which case it sets it to name1.
	 */
	c->name = name2;
	if (!name2) {
		c->debug_name = unlang_keyword[c->type];
	} else {
		c->debug_name = talloc_asprintf(c, "%s %s", unlang_keyword[c->type], name2);
	}

	g = mod_callabletogroup(c);
	g->vpt = talloc_steal(g, vpt);

	/*
	 *	Set all of it's codes to return, so that
	 *	when we pick a 'case' statement, we don't
	 *	fall through to processing the next one.
	 */
	for (i = 0; i < RLM_MODULE_NUMCODES; i++) {
		c->actions[i] = MOD_ACTION_RETURN;
	}

	return c;
}

static modcallable *compile_foreach(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
				    grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	FR_TOKEN type;
	char const *name2;
	modcallable *c;
	modgroup *g;
	ssize_t slen;
	vp_tmpl_t *vpt;

	name2 = cf_section_name2(cs);
	if (!name2) {
		cf_log_err_cs(cs,
			   "You must specify an attribute to loop over in 'foreach'");
		return NULL;
	}

	/*
	 *	Create the template.  If we fail, AND it's a bare word
	 *	with &Foo-Bar, it MAY be an attribute defined by a
	 *	module.  Allow it for now.  The pass2 checks below
	 *	will fix it up.
	 */
	type = cf_section_name2_type(cs);
	slen = tmpl_afrom_str(cs, &vpt, name2, strlen(name2), type, REQUEST_CURRENT, PAIR_LIST_REQUEST, true);
	if ((slen < 0) && ((type != T_BARE_WORD) || (name2[0] != '&'))) {
		char *spaces, *text;

		fr_canonicalize_error(cs, &spaces, &text, slen, fr_strerror());

		cf_log_err_cs(cs, "Syntax error");
		cf_log_err_cs(cs, "%s", name2);
		cf_log_err_cs(cs, "%s^ %s", spaces, text);

		talloc_free(spaces);
		talloc_free(text);

		return NULL;
	}

	/*
	 *	If we don't have a negative return code, we must have a vpt
	 *	(mostly to quiet coverity).
	 */
	rad_assert(vpt);

	if ((vpt->type != TMPL_TYPE_ATTR) && (vpt->type != TMPL_TYPE_LIST)) {
		cf_log_err_cs(cs, "MUST use attribute or list reference in 'foreach'");
		talloc_free(vpt);
		return NULL;
	}

	if ((vpt->tmpl_num != NUM_ALL) && (vpt->tmpl_num != NUM_ANY)) {
		cf_log_err_cs(cs, "MUST NOT use instance selectors in 'foreach'");
		talloc_free(vpt);
		return NULL;
	}

	/*
	 *	Fix up the template to iterate over all instances of
	 *	the attribute. In a perfect consistent world, users would do
	 *	foreach &attr[*], but that's taking the consistency thing a bit far.
	 */
	vpt->tmpl_num = NUM_ALL;

	c = compile_group(parent, component, cs, grouptype, parentgrouptype, mod_type);
	if (!c) {
		talloc_free(vpt);
		return NULL;
	}

	c->name = unlang_keyword[c->type];
	c->debug_name = talloc_asprintf(c, "%s %s", unlang_keyword[c->type], name2);

	g = mod_callabletogroup(c);
	g->vpt = vpt;

	return c;
}



static modcallable *compile_break(modcallable *parent, rlm_components_t component, CONF_ITEM const *ci)
{
	modcallable *foreach;

	for (foreach = parent; foreach != NULL; foreach = foreach->parent) {
		if (foreach->type == MOD_FOREACH) break;
	}

	if (!foreach) {
		cf_log_err(ci, "'break' can only be used in a 'foreach' section");
		return NULL;
	}

	return compile_empty(parent, component, NULL, GROUPTYPE_SIMPLE, GROUPTYPE_SIMPLE,
			     MOD_BREAK, COND_TYPE_INVALID);
}
#endif

static modcallable *compile_xlat(modcallable *parent,
				 rlm_components_t component, char const *fmt)
{
	modcallable *c;
	modxlat *mx;

	mx = talloc_zero(parent, modxlat);

	c = mod_xlattocallable(mx);
	c->parent = parent;
	c->next = NULL;
	c->name = "expand";
	c->debug_name = c->name;
	c->type = MOD_XLAT;
	c->method = component;

	memcpy(c->actions, defaultactions[component][GROUPTYPE_SIMPLE],
	       sizeof(c->actions));

	mx->xlat_name = talloc_typed_strdup(mx, fmt);
	if (fmt[0] != '%') {
		char *p;
		mx->exec = true;

		strcpy(mx->xlat_name, fmt + 1);
		p = strrchr(mx->xlat_name, '`');
		if (p) *p = '\0';
	}

	return c;
}

static modcallable *compile_if(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
			       grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	modcallable *c;
	modgroup *g;
	fr_cond_t *cond;

	if (!cf_section_name2(cs)) {
		cf_log_err_cs(cs, "'%s' without condition", unlang_keyword[mod_type]);
		return NULL;
	}

	cond = cf_data_find(cs, "if");
	rad_assert(cond != NULL);

	if (cond->type == COND_TYPE_FALSE) {
		INFO(" # Skipping contents of '%s' as it is always 'false' -- %s:%d",
		     unlang_keyword[mod_type],
		     cf_section_filename(cs), cf_section_lineno(cs));
		return compile_empty(parent, component, cs, grouptype, parentgrouptype, mod_type, COND_TYPE_FALSE);
	}

	/*
	 *	The condition may refer to attributes, xlats, or
	 *	Auth-Types which didn't exist when it was first
	 *	parsed.  Now that they are all defined, we need to fix
	 *	them up.
	 */
	if (!fr_condition_walk(cond, pass2_cond_callback, NULL)) {
		return NULL;
	}

	c = compile_group(parent, component, cs, grouptype, parentgrouptype, mod_type);
	if (!c) return NULL;

	c->name = unlang_keyword[c->type];
	c->debug_name = talloc_asprintf(c, "%s %s", unlang_keyword[c->type], cf_section_name2(cs));

	g = mod_callabletogroup(c);
	g->cond = cond;

	return c;
}

static int previous_if(CONF_SECTION *cs, modcallable *parent, mod_type_t mod_type)
{
	modgroup *p, *f;

	p = mod_callabletogroup(parent);
	if (!p->tail) goto else_fail;

	f = mod_callabletogroup(p->tail);
	if ((f->mc.type != MOD_IF) && (f->mc.type != MOD_ELSIF)) {
	else_fail:
		cf_log_err_cs(cs, "Invalid location for '%s'.  There is no preceding 'if' or 'elsif' statement",
			      unlang_keyword[mod_type]);
		return -1;
	}

	if (f->cond->type == COND_TYPE_TRUE) {
		INFO(" # Skipping contents of '%s' as previous '%s' is always 'true' -- %s:%d",
		     unlang_keyword[mod_type],
		     unlang_keyword[f->mc.type],
		     cf_section_filename(cs), cf_section_lineno(cs));
		return 0;
	}

	return 1;
}

static modcallable *compile_elsif(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
				  grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	int rcode;

	/*
	 *	This is always a syntax error.
	 */
	if (!cf_section_name2(cs)) {
		cf_log_err_cs(cs, "'%s' without condition", unlang_keyword[mod_type]);
		return NULL;
	}

	rcode = previous_if(cs, parent, mod_type);
	if (rcode < 0) return NULL;

	if (rcode == 0) return compile_empty(parent, component, cs, grouptype, parentgrouptype, mod_type, COND_TYPE_TRUE);

	return compile_if(parent, component, cs, grouptype, parentgrouptype, mod_type);
}

static modcallable *compile_else(modcallable *parent,
			       rlm_components_t component, CONF_SECTION *cs,
			       grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	int rcode;
	modcallable *c;

	if (cf_section_name2(cs)) {
		cf_log_err_cs(cs, "'%s' cannot have a condition", unlang_keyword[mod_type]);
		return NULL;
	}

	rcode = previous_if(cs, parent, mod_type);
	if (rcode < 0) return NULL;

	if (rcode == 0) {
		c = compile_empty(parent, component, cs, grouptype, parentgrouptype, mod_type, COND_TYPE_TRUE);
	} else {
		c = compile_group(parent, component, cs, grouptype, parentgrouptype, mod_type);
	}

	if (!c) return c;

	c->name = unlang_keyword[c->type];
	c->debug_name = c->name;

	return c;
}

/*
 *	redundant, etc. can refer to modules or groups, but not much else.
 */
static int all_children_are_modules(CONF_SECTION *cs, char const *name)
{
	CONF_ITEM *ci;

	for (ci=cf_item_find_next(cs, NULL);
	     ci != NULL;
	     ci=cf_item_find_next(cs, ci)) {
		/*
		 *	If we're a redundant, etc. group, then the
		 *	intention is to call modules, rather than
		 *	processing logic.  These checks aren't
		 *	*strictly* necessary, but they keep the users
		 *	from doing crazy things.
		 */
		if (cf_item_is_section(ci)) {
			CONF_SECTION *subcs = cf_item_to_section(ci);
			char const *name1 = cf_section_name1(subcs);

			if ((strcmp(name1, "if") == 0) ||
			    (strcmp(name1, "else") == 0) ||
			    (strcmp(name1, "elsif") == 0) ||
			    (strcmp(name1, "update") == 0) ||
			    (strcmp(name1, "switch") == 0) ||
			    (strcmp(name1, "case") == 0)) {
				cf_log_err(ci, "%s sections cannot contain a \"%s\" statement",
				       name, name1);
				return 0;
			}
			continue;
		}

		if (cf_item_is_pair(ci)) {
			CONF_PAIR *cp = cf_item_to_pair(ci);
			if (cf_pair_value(cp) != NULL) {
				cf_log_err(ci,
					   "Entry with no value is invalid");
				return 0;
			}
		}
	}

	return 1;
}


static modcallable *compile_redundant(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
				      grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type)
{
	modcallable *c;

	/*
	 *	No children?  Die!
	 */
	if (!cf_item_find_next(cs, NULL)) {
		cf_log_err_cs(cs, "%s sections cannot be empty", unlang_keyword[mod_type]);
		return NULL;
	}

	if (!all_children_are_modules(cs, cf_section_name1(cs))) {
		return NULL;
	}

	c = compile_group(parent, component, cs, grouptype, parentgrouptype, mod_type);
	if (!c) return NULL;

	c->name = unlang_keyword[c->type];
	c->debug_name = c->name;

	return c;
}


/** Load a named module from "instantiate" or "policy".
 *
 * If it's "foo.method", look for "foo", and return "method" as the method
 * we wish to use, instead of the input component.
 *
 * @param[out] pcomponent Where to write the method we found, if any.  If no method is specified
 *	will be set to MOD_COUNT.
 * @param[in] real_name Complete name string e.g. foo.authorize.
 * @param[in] virtual_name Virtual module name e.g. foo.
 * @param[in] method_name Method override (may be NULL) or the method name e.g. authorize.
 * @return the CONF_SECTION specifying the virtual module.
 */
static CONF_SECTION *virtual_module_find_cs(rlm_components_t *pcomponent,
					    char const *real_name, char const *virtual_name, char const *method_name)
{
	CONF_SECTION *cs, *subcs;
	rlm_components_t method = *pcomponent;
	char buffer[256];

	/*
	 *	Turn the method name into a method enum.
	 */
	if (method_name) {
		rlm_components_t i;

		for (i = MOD_AUTHENTICATE; i < MOD_COUNT; i++) {
			if (strcmp(comp2str[i], method_name) == 0) break;
		}

		if (i != MOD_COUNT) {
			method = i;
		} else {
			method_name = NULL;
			virtual_name = real_name;
		}
	}

	/*
	 *	Look for "foo" in the "instantiate" section.  If we
	 *	find it, AND there's no method name, we've found the
	 *	right thing.
	 *
	 *	Return it to the caller, with the updated method.
	 */
	cs = cf_section_sub_find(main_config.config, "instantiate");
	if (cs) {
		/*
		 *	Found "foo".  Load it as "foo", or "foo.method".
		 */
		subcs = cf_section_sub_find_name2(cs, NULL, virtual_name);
		if (subcs) {
			*pcomponent = method;
			return subcs;
		}
	}

	/*
	 *	Look for it in "policy".
	 *
	 *	If there's no policy section, we can't do anything else.
	 */
	cs = cf_section_sub_find(main_config.config, "policy");
	if (!cs) return NULL;

	/*
	 *	"foo.authorize" means "load policy "foo" as method "authorize".
	 *
	 *	And bail out if there's no policy "foo".
	 */
	if (method_name) {
		subcs = cf_section_sub_find_name2(cs, NULL, virtual_name);
		if (subcs) *pcomponent = method;

		return subcs;
	}

	/*
	 *	"foo" means "look for foo.component" first, to allow
	 *	method overrides.  If that's not found, just look for
	 *	a policy "foo".
	 *
	 */
	snprintf(buffer, sizeof(buffer), "%s.%s",
		 virtual_name, comp2str[method]);
	subcs = cf_section_sub_find_name2(cs, NULL, buffer);
	if (subcs) return subcs;

	return cf_section_sub_find_name2(cs, NULL, virtual_name);
}


static modcallable *compile_csingle(modcallable *parent, rlm_components_t component, CONF_ITEM *ci, module_instance_t *this, grouptype_t grouptype, char const *realname)
{
	modcallable *c;
	modsingle *single;

	/*
	 *	Check if the module in question has the necessary
	 *	component.
	 */
	if (!this->module->methods[component]) {
		cf_log_err(ci, "\"%s\" modules aren't allowed in '%s' sections -- they have no such method.", this->module->name,
			   comp2str[component]);
		return NULL;
	}

	single = talloc_zero(parent, modsingle);
	single->modinst = this;

	c = mod_singletocallable(single);
	c->parent = parent;
	c->next = NULL;
	if (!parent || (component != MOD_AUTHENTICATE)) {
		memcpy(c->actions, defaultactions[component][grouptype],
		       sizeof c->actions);
	} else { /* inside Auth-Type has different rules */
		memcpy(c->actions, authtype_actions[grouptype],
		       sizeof c->actions);
	}

	c->name = realname;
	c->debug_name = realname;
	c->type = MOD_SINGLE;
	c->method = component;

	if (!compile_action_section(c, ci)) {
		talloc_free(c);
		return NULL;
	}

	return c;
}

typedef modcallable *(*modcall_compile_function_t)(modcallable *parent, rlm_components_t component, CONF_SECTION *cs,
					 grouptype_t grouptype, grouptype_t parentgrouptype, mod_type_t mod_type);
typedef struct modcall_compile_t {
	char const			*name;
	modcall_compile_function_t	compile;
	grouptype_t			grouptype;
	mod_type_t			mod_type;
} modcall_compile_t;

static modcall_compile_t compile_table[] = {
	{ "group",		compile_group, GROUPTYPE_SIMPLE, MOD_GROUP },
	{ "redundant",		compile_redundant, GROUPTYPE_REDUNDANT, MOD_GROUP },
	{ "load-balance",	compile_redundant, GROUPTYPE_SIMPLE, MOD_LOAD_BALANCE },
	{ "redundant-load-balance", compile_redundant, GROUPTYPE_REDUNDANT, MOD_REDUNDANT_LOAD_BALANCE },

	{ "case",		compile_case, GROUPTYPE_SIMPLE, MOD_CASE },
	{ "foreach",		compile_foreach, GROUPTYPE_SIMPLE, MOD_FOREACH },
	{ "if",			compile_if, GROUPTYPE_SIMPLE, MOD_IF },
	{ "elsif",		compile_elsif, GROUPTYPE_SIMPLE, MOD_ELSIF },
	{ "else",		compile_else, GROUPTYPE_SIMPLE, MOD_ELSE },
	{ "update",		compile_update, GROUPTYPE_SIMPLE, MOD_UPDATE },
	{ "map",		compile_map, GROUPTYPE_SIMPLE, MOD_MAP },
	{ "switch",		compile_switch, GROUPTYPE_SIMPLE, MOD_SWITCH },

	{ NULL, NULL, 0, MOD_NULL }
};


/*
 *	Compile one entry of a module call.
 */
static modcallable *compile_item(modcallable *parent, rlm_components_t component, CONF_ITEM *ci,
				 grouptype_t parent_grouptype, char const **modname)
{
	char const *modrefname, *p;
	modcallable *c;
	module_instance_t *this;
	CONF_SECTION *cs, *subcs, *modules;
	CONF_SECTION *loop;
	char const *realname;
	rlm_components_t method = component;

	if (cf_item_is_section(ci)) {
		int i;
		char const *name2;

		cs = cf_item_to_section(ci);
		modrefname = cf_section_name1(cs);
		name2 = cf_section_name2(cs);
		if (!name2) name2 = "";

		for (i = 0; compile_table[i].name != NULL; i++) {
			if (strcmp(modrefname, compile_table[i].name) == 0) {
				*modname = name2;

				/*
				 *	Some blocks can be empty.  The rest need
				 *	to have contents.
				 */
				if (!cf_item_find_next(cs, NULL) &&
				    !((compile_table[i].mod_type == MOD_CASE) ||
				      (compile_table[i].mod_type == MOD_IF) ||
				      (compile_table[i].mod_type == MOD_ELSIF))) {
					cf_log_err(ci, "'%s' sections cannot be empty", modrefname);
					return NULL;
				}

				return compile_table[i].compile(parent, component, cs,
								compile_table[i].grouptype, parent_grouptype,
								compile_table[i].mod_type);
			}
		}

#ifdef WITH_UNLANG
		if (strcmp(modrefname, "break") == 0) {
			cf_log_err(ci, "Invalid use of 'break'");
			return NULL;

		} else if (strcmp(modrefname, "return") == 0) {
			cf_log_err(ci, "Invalid use of 'return'");
			return NULL;

		} /* else it's something like sql { fail = 1 ...} */
#endif

	} else if (!cf_item_is_pair(ci)) { /* CONF_DATA or some such */
		return NULL;

		/*
		 *	Else it's a module reference, with updated return
		 *	codes.
		 */
	} else {
		CONF_PAIR *cp = cf_item_to_pair(ci);
		modrefname = cf_pair_attr(cp);

		/*
		 *	Actions (ok = 1), etc. are orthogonal to just
		 *	about everything else.
		 */
		if (cf_pair_value(cp) != NULL) {
			cf_log_err(ci, "Entry is not a reference to a module");
			return NULL;
		}

		/*
		 *	In-place xlat's via %{...}.
		 *
		 *	This should really be removed from the server.
		 */
		if (((modrefname[0] == '%') && (modrefname[1] == '{')) ||
		    (modrefname[0] == '`')) {
			return compile_xlat(parent, component, modrefname);
		}
	}

#ifdef WITH_UNLANG
	/*
	 *	These can't be over-ridden.
	 */
	if (strcmp(modrefname, "break") == 0) {
		return compile_break(parent, component, ci);
	}

	if (strcmp(modrefname, "return") == 0) {
		return compile_empty(parent, component, NULL, GROUPTYPE_SIMPLE, GROUPTYPE_SIMPLE, MOD_RETURN, COND_TYPE_INVALID);
	}
#endif

	/*
	 *	We now have a name.  It can be one of two forms.  A
	 *	bare module name, or a section named for the module,
	 *	with over-rides for the return codes.
	 *
	 *	The name can refer to a real module, in the "modules"
	 *	section.  In that case, the name will be either the
	 *	first or second name of the sub-section of "modules".
	 *
	 *	Or, the name can refer to a policy, in the "policy"
	 *	section.  In that case, the name will be first of the
	 *	sub-section of "policy".
	 *
	 *	Or, the name can refer to a "module.method", in which
	 *	case we're calling a different method than normal for
	 *	this section.
	 *
	 *	Or, the name can refer to a virtual module, in the
	 *	"instantiate" section.  In that case, the name will be
	 *	the first of the sub-section of "instantiate".
	 *
	 *	We try these in sequence, from the bottom up.  This is
	 *	so that things in "instantiate" and "policy" can
	 *	over-ride calls to real modules.
	 */


	/*
	 *	Try:
	 *
	 *	instantiate { ... name { ...} ... }
	 *	policy { ... name { .. } .. }
	 *	policy { ... name.method { .. } .. }
	 *
	 *	The only difference between things in "instantiate"
	 *	and "policy" is that "instantiate" will cause modules
	 *	to be instantiated in a particular order.
	 */
	subcs = NULL;
	p = strrchr(modrefname, '.');
	if (!p) {
		subcs = virtual_module_find_cs(&method, modrefname, modrefname, NULL);
	} else {
		char buffer[256];

		strlcpy(buffer, modrefname, sizeof(buffer));
		buffer[p - modrefname] = '\0';

		subcs = virtual_module_find_cs(&method, modrefname, buffer, buffer + (p - modrefname) + 1);
	}

	/*
	 *	Check that we're not creating a loop.  We may
	 *	be compiling an "sql" module reference inside
	 *	of an "sql" policy.  If so, we allow the
	 *	second "sql" to refer to the module.
	 */
	for (loop = cf_item_parent(ci);
	     loop && subcs;
	     loop = cf_item_parent(cf_section_to_item(loop))) {
		if (loop == subcs) {
			subcs = NULL;
		}
	}

	/*
	 *	We've found the relevant entry.  It MUST be a
	 *	sub-section.
	 *
	 *	However, it can be a "redundant" block, or just
	 */
	if (subcs) {
		/*
		 *	modules.c takes care of ensuring that this is:
		 *
		 *	group foo { ...
		 *	load-balance foo { ...
		 *	redundant foo { ...
		 *	redundant-load-balance foo { ...
		 *
		 *	We can just recurs to compile the section as
		 *	if it was found here.
		 */
		if (cf_section_name2(subcs)) {
			c = compile_item(parent, method, cf_section_to_item(subcs), parent_grouptype, modname);
			if (!c) return NULL;

		} else {
			/*
			 *	We have:
			 *
			 *	foo { ...
			 *
			 *	So we compile it like it was:
			 *
			 *	group foo { ...
			 */
			c = compile_group(parent, method, subcs, GROUPTYPE_SIMPLE, parent_grouptype, MOD_GROUP);
			if (!c) return NULL;

			c->name = cf_section_name1(subcs);
			c->debug_name = c->name;
		}

		/*
		 *	Return the compiled thing if we can.
		 */
		if (cf_item_is_pair(ci)) return c;

		/*
		 *	Else we have a reference to a policy, and that reference
		 *	over-rides the return codes for the policy!
		 */
		if (!compile_action_section(c, ci)) {
			talloc_free(c);
			return NULL;
		}

		return c;
	}

	/*
	 *	Not a virtual module.  It must be a real module.
	 */
	modules = cf_section_sub_find(main_config.config, "modules");
	if (!modules) goto fail;

	this = NULL;
	realname = modrefname;

	/*
	 *	Try to load the optional module.
	 */
	if (realname[0] == '-') realname++;

	/*
	 *	As of v3, the "modules" section contains
	 *	modules we use.  Configuration for other
	 *	modules belongs in raddb/mods-available/,
	 *	which isn't loaded into the "modules" section.
	 */
	this = module_instantiate_method(modules, realname, &method);
	if (this) {
		*modname = this->module->name;
		return compile_csingle(parent, method, ci, this, parent_grouptype, realname);
	}

	/*
	 *	We were asked to MAYBE load it and it
	 *	doesn't exist.  Return a soft error.
	 */
	if (realname != modrefname) {
		*modname = modrefname;
		return NULL;
	}

	/*
	 *	Can't de-reference it to anything.  Ugh.
	 */
fail:
	*modname = NULL;
	cf_log_err(ci, "Failed to find \"%s\" as a module or policy.", modrefname);
	cf_log_err(ci, "Please verify that the configuration exists in %s/mods-enabled/%s.", get_radius_dir(), modrefname);
	return NULL;
}

modcallable *modcall_compile_section(modcallable *parent,
				     rlm_components_t component, CONF_SECTION *cs)
{
	char const *name1, *name2;
	modcallable *c;

	c = compile_group(parent, component, cs, GROUPTYPE_SIMPLE, GROUPTYPE_SIMPLE, MOD_GROUP);
	if (!c) return NULL;

	/*
	 *	The name / debug name are set to "group".  We want
	 *	that to be a little more informative.
	 */
	name1 = cf_section_name1(cs);
	name2 = cf_section_name2(cs);
	c->name = name1;

	if (!name2) {
		c->debug_name = name1;
	} else {
		c->debug_name = talloc_asprintf(c, "%s %s", name1, name2);
	}

	if (rad_debug_lvl > 3) {
		modcall_debug(c, 2);
	}

	/*
	 *	Associate the unlang with the configuration section.
	 */
	cf_data_add(cs, "unlang", c, NULL);

	dump_tree(component, c);
	return c;
}

int modcall_pass2_condition(fr_cond_t *c)
{
	if (!fr_condition_walk(c, pass2_cond_callback, NULL)) return -1;

	return 0;
}
