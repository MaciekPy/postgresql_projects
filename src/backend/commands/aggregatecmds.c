/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/aggregatecmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 *	DefineAggregate
 *
 * "oldstyle" signals the old (pre-8.2) style where the aggregate input type
 * is specified by a BASETYPE element in the parameters.  Otherwise,
 * "args" defines the input type(s).
 */
Oid
DefineAggregate(List *name, List *args, bool oldstyle, List *parameters)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	List	   *transfuncName = NIL;
	List	   *finalfuncName = NIL;
	List	   *sortoperatorName = NIL;
	List	   *transsortoperatorName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
	char	   *initval = NULL;
	Oid		   *aggArgTypes;
	int			numArgs;
	int			numOrderedArgs = 0;
	Oid			transTypeId = InvalidOid;
	char		transTypeType;
	ListCell   *pl;
	bool		ishypothetical = false;
	bool		isOrderedSet = false;
	Oid 		variadic_type = InvalidOid;
	Oid		ord_variadic_type = InvalidOid;

	/* Convert list of names to a name and namespace */
	aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(aggNamespace));

	if (list_length(args) > 1)
	{
		if (lsecond(args) != NULL)
		{
			//elog(WARNING,"second args is not NULL %d", list_length(args));
			isOrderedSet = true;
		}
	}

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete spellings
		 * for sfunc, stype, initcond.
		 */
		if (pg_strcasecmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sortop") == 0)
			sortoperatorName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "hypothetical") == 0)
			ishypothetical = true;
		else if (pg_strcasecmp(defel->defname, "transsortop") == 0)
			transsortoperatorName = defGetQualifiedName(defel);
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	if (!isOrderedSet)
	{
		/*
	 	* make sure we have our required definitions
	 	*/
		if (transType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						errmsg("aggregate stype must be specified")));
		if (transfuncName == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate sfunc must be specified")));
	}

	/*
	 * look up the aggregate's input datatype(s).
	 */
	if (oldstyle)
	{
		/*
		 * Old style: use basetype parameter.  This supports aggregates of
		 * zero or one input, with input type ANY meaning zero inputs.
		 *
		 * Historically we allowed the command to look like basetype = 'ANY'
		 * so we must do a case-insensitive comparison for the name ANY. Ugh.
		 */
		if (baseType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate input type must be specified")));

		if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		{
			numArgs = 0;
			aggArgTypes = NULL;
		}
		else
		{
			numArgs = 1;
			aggArgTypes = (Oid *) palloc(sizeof(Oid));
			aggArgTypes[0] = typenameTypeId(NULL, baseType);
		}
	}
	else
	{
		/*
		 * New style: args is a list of TypeNames (possibly zero of 'em).
		 */
		ListCell   *lc;
		int			i = 0;

		if (baseType != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("basetype is redundant with aggregate input type specification")));

		numArgs = list_length(linitial(args));
		if (isOrderedSet)
		{
			int totalnumArgs = list_length(linitial(args)) + list_length(lsecond(args));
			aggArgTypes = (Oid *) palloc(sizeof(Oid) * totalnumArgs);
			numOrderedArgs = list_length(lsecond(args));
		}
		else
		{
			aggArgTypes = (Oid *) palloc(sizeof(Oid) * numArgs);
		}

		//elog_node_display(NOTICE, "args", args, true);
		
		foreach(lc, linitial(args))
		{
			TypeName *curTypeName = NULL;
			if (IsA(lfirst(lc), List))
			{
				curTypeName = linitial(lfirst(lc));
				variadic_type = typenameTypeId(NULL, curTypeName);

				switch (variadic_type)
				{
					case ANYOID:
							variadic_type = ANYOID;
							break;
					case ANYARRAYOID:
							variadic_type = ANYELEMENTOID;
							break;
					default:
							if (!OidIsValid(get_element_type(variadic_type)))
								elog(ERROR, "variadic parameter is not an array");
							break;
				}

				aggArgTypes[i++] = variadic_type;

			}
			else
			{		
				curTypeName = (TypeName *) lfirst(lc);
				aggArgTypes[i++] = typenameTypeId(NULL, curTypeName);
			}

			
		}

		if (isOrderedSet)
		{
			if (variadic_type != InvalidOid)
			{
				if (!IsA(linitial(lsecond(args)), List))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 		errmsg("Ordered arguments must be variadic")));

				if (list_length(lsecond(args)) != 1)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 		errmsg("Invalid ordered arguments for variadic")));

				/*if (variadic_type == ANYOID)
					if (typenameTypeId(NULL, lfirst(lsecond(args))) != variadic_type)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 		errmsg("Variadic types do not match")));*/

				if (variadic_type != ANYOID)
					if (!OidIsValid(get_element_type(variadic_type)))
						elog(ERROR, "variadic parameter is not an array");
			}

			foreach(lc, lsecond(args))
			{
				//elog_node_display(NOTICE, "args", args, true);
				if (IsA(lfirst(lc), List))
				{
					//elog(WARNING,"In List condition");
					ord_variadic_type = typenameTypeId(NULL, linitial(linitial(lfirst(lc))));

					if (ord_variadic_type != ANYOID)
						if (!OidIsValid(get_element_type(ord_variadic_type)))
									elog(ERROR, "variadic parameter is not an array");

					aggArgTypes[i++] = ord_variadic_type;

					//elog(WARNING,"type is %d",ord_variadic_type);
				}
				else
				{
					//elog(WARNING,"In non List condition");
					TypeName *curTypeName = (TypeName *) lfirst(lc);

					aggArgTypes[i++] = typenameTypeId(NULL, curTypeName);
				}
			}

		}
	}

	/*
	 * look up the aggregate's transtype.The lookup happens only if the
	 * aggregate function is not an ordered set function.
	 *
	 * transtype can't be a pseudo-type, since we need to be able to store
	 * values of the transtype.  However, we can allow polymorphic transtype
	 * in some cases (AggregateCreate will check).	Also, we allow "internal"
	 * for functions that want to pass pointers to private data structures;
	 * but allow that only to superusers, since you could crash the system (or
	 * worse) by connecting up incompatible internal-using functions in an
	 * aggregate.
	 */
	if (transType)
	{
		transTypeId = typenameTypeId(NULL, transType);
		transTypeType = get_typtype(transTypeId);
		if (transTypeType == TYPTYPE_PSEUDO &&
			!IsPolymorphicType(transTypeId))
		{
			if (!isOrderedSet)
			{
				if (transTypeId == INTERNALOID && superuser())
					 /* okay */ ;
				else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate transition data type cannot be %s",
								format_type_be(transTypeId))));
			}
			else
			{
				if (transTypeId == INTERNALOID && superuser())
					ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate transition data type cannot be INTERNALOID for ordered set functions")));
			}
		}

	}

	/*
	 * If we have an initval, and it's not for a pseudotype (particularly a
	 * polymorphic type), make sure it's acceptable to the type's input
	 * function.  We will store the initval as text, because the input
	 * function isn't necessarily immutable (consider "now" for timestamp),
	 * and we want to use the runtime not creation-time interpretation of the
	 * value.  However, if it's an incorrect value it seems much more
	 * user-friendly to complain at CREATE AGGREGATE time.
	 */
	if (transType)
	{
		if (initval && transTypeType != TYPTYPE_PSEUDO)
		{
			Oid			typinput,
						typioparam;

			getTypeInputInfo(transTypeId, &typinput, &typioparam);
			(void) OidInputFunctionCall(typinput, initval, typioparam, -1);
		}
	}

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	return AggregateCreate(aggName,		/* aggregate name */
						   aggNamespace,		/* namespace */
						   aggArgTypes, /* input data type(s) */
						   numArgs,
						   numOrderedArgs,
						   transfuncName,		/* step function name */
						   finalfuncName,		/* final function name */
						   sortoperatorName,	/* sort operator name */
						   transsortoperatorName,  /* transsort operator name */
						   transTypeId, /* transition data type */
						   initval,  /* initial condition */
						   variadic_type,  /* The Oid of the variadic type in direct args, if applicable */
						   ord_variadic_type,   /* The Oid of the variadic type in ordered args, if applicable */
						   isOrderedSet,  /* If the function is an ordered set */
						   ishypothetical);  /* If the function is a hypothetical set */
}
