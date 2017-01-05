/*
 * commands of parm
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "catalog/mgr_host.h"
#include "catalog/mgr_cndnnode.h"
#include "catalog/mgr_parm.h"
#include "catalog/mgr_updateparm.h"
#include "commands/defrem.h"
#include "mgr/mgr_cmds.h"
#include "mgr/mgr_msg_type.h"
#include "miscadmin.h"
#include "funcapi.h"
#include "nodes/parsenodes.h"
#include "parser/mgr_node.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "miscadmin.h"
#include "utils/tqual.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "parser/scansup.h"

#include <stdlib.h>
#include <string.h>

#if (Natts_mgr_updateparm != 4)
#error "need change code"
#endif

static void mgr_send_show_parameters(char cmdtype, StringInfo infosendmsg, Oid hostoid, GetAgentCmdRst *getAgentCmdRst);
static bool mgr_recv_showparam_msg(ManagerAgent	*ma, GetAgentCmdRst *getAgentCmdRst);
static void mgr_add_givenname_updateparm(MGRUpdateparm *node, Name nodename, char nodetype, Relation rel_node, Relation rel_updateparm, Relation rel_parm, bool bneednotice);

/*if the parmeter in gtm or coordinator or datanode pg_settins, the nodetype in mgr_parm is '*'
 , if the parmeter in coordinator or datanode pg_settings, the nodetype in mgr_parm is '#'
*/
#define PARM_IN_GTM_CN_DN '*'
#define PARM_IN_CN_DN '#'
/*the string used to stand for value which be reset by force and the param table has '*' for this parameter*/
#define DEFAULT_VALUE "--"

typedef enum CheckInsertParmStatus
{
	PARM_NEED_INSERT=1,
	PARM_NEED_UPDATE,
	PARM_NEED_NONE
}CheckInsertParmStatus;

typedef struct InitNodeInfo
{
	Relation rel_node;
	HeapScanDesc rel_scan;
	ListCell  **lcp;
}InitNodeInfo;

/*
 * Displayable names for context types (enum GucContext)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const GucContext_Parmnames[] =
{
	 /* PGC_INTERNAL */ "internal",
	 /* PGC_POSTMASTER */ "postmaster",
	 /* PGC_SIGHUP */ "sighup",
	 /* PGC_BACKEND */ "backend",
	 /* PGC_SUSET */ "superuser",
	 /* PGC_USERSET */ "user"
};

static void mgr_check_parm_in_pgconf(Relation noderel, char parmtype, Name key, Name value, int *vartype, Name parmunit, Name parmmin, Name parmmax, int *effectparmstatus, StringInfo enumvalue, bool bneednotice);
static int mgr_check_parm_in_updatetbl(Relation noderel, char nodetype, Name nodename, Name key, char *value);
static void mgr_reload_parm(Relation noderel, char *nodename, char nodetype, StringInfo paramstrdata, int effectparmstatus, bool bforce);
static void mgr_updateparm_send_parm(GetAgentCmdRst *getAgentCmdRst, Oid hostoid, char *nodepath, StringInfo paramstrdata, int effectparmstatus, bool bforce);
static int mgr_delete_tuple_not_all(Relation noderel, char nodetype, Name key);
static int mgr_check_parm_value(char *name, char *value, int vartype, char *parmunit, char *parmmin, char *parmmax, StringInfo enumvalue);
static int mgr_get_parm_unit_type(char *nodename, char *parmunit);
static bool mgr_parm_enum_lookup_by_name(char *value, StringInfo valuelist);
static void mgr_string_add_single_quota(Name value);
/* 
* for command: set {datanode|coordinaotr}  {master|slave|extra} {nodename|ALL} {key1=value1,key2=value2...} , 
* set datanode all {key1=value1,key2=value2...},set gtm all {key1=value1,key2=value2...}, set gtm master|slave|extra
* gtmx {key1=value1,key2=value2...} to record the parameter in mgr_updateparm
*/
void mgr_add_updateparm(MGRUpdateparm *node, ParamListInfo params, DestReceiver *dest)
{
	if (mgr_has_priv_set())
	{
		DirectFunctionCall7(mgr_add_updateparm_func,
							CharGetDatum(node->parmtype),
							CStringGetDatum(node->nodename),
							CharGetDatum(node->nodetype),
							CStringGetDatum(node->key),
							CStringGetDatum(node->value),
							BoolGetDatum(node->is_force),
							PointerGetDatum(node->options));
		return;
	}
	else
	{
		ereport(ERROR, (errmsg("permission denied")));
		return ;
	}
}

Datum mgr_add_updateparm_func(PG_FUNCTION_ARGS)
{
	Relation rel_updateparm;
	Relation rel_parm;
	Relation rel_node;
	NameData nodename;
	ScanKeyData scankey[1];
	HeapScanDesc rel_scan;
	HeapTuple tuple;
	Form_mgr_node mgr_node;
	bool bneednotice = true;
	char nodetype; /*master/slave/extra*/

	MGRUpdateparm *parm_node;
	parm_node = palloc(sizeof(*parm_node));
	parm_node->parmtype = PG_GETARG_CHAR(0);
	parm_node->nodename = PG_GETARG_CSTRING(1);
	parm_node->nodetype = PG_GETARG_CHAR(2);
	parm_node->key = PG_GETARG_CSTRING(3);
	parm_node->value = PG_GETARG_CSTRING(4);
	parm_node->is_force = PG_GETARG_BOOL(5);
	parm_node->options = (List *)PG_GETARG_POINTER(6);

	Assert(parm_node && parm_node->nodename && parm_node->nodetype && parm_node->parmtype);
	nodetype = parm_node->nodetype;
	/*nodename*/
	namestrcpy(&nodename, parm_node->nodename);
	/*open systbl: mgr_parm*/
	rel_updateparm = heap_open(UpdateparmRelationId, RowExclusiveLock);
	rel_parm = heap_open(ParmRelationId, RowExclusiveLock);
	rel_node = heap_open(NodeRelationId, RowExclusiveLock);

	/*set datanode master/slave/extra all (key=value,...)*/
	if (strcmp(nodename.data, MACRO_STAND_FOR_ALL_NODENAME) == 0 && (CNDN_TYPE_DATANODE_MASTER == nodetype || CNDN_TYPE_DATANODE_SLAVE == nodetype || CNDN_TYPE_DATANODE_EXTRA == nodetype))
	{
		bneednotice = true;
		ScanKeyInit(&scankey[0]
				,Anum_mgr_node_nodetype
				,BTEqualStrategyNumber
				,F_CHAREQ
				,CharGetDatum(nodetype));
		rel_scan = heap_beginscan(rel_node, SnapshotNow, 1, scankey);
		while ((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
		{
			if(!HeapTupleIsValid(tuple))
				break;
			mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
			Assert(mgr_node);
			mgr_add_givenname_updateparm(parm_node, &(mgr_node->nodename), mgr_node->nodetype, rel_node, rel_updateparm, rel_parm, bneednotice);
			bneednotice = false;
		}
		heap_endscan(rel_scan);
	}
	/*set datanode/gtm all (key=value,...), set nodetype nodname (key=value,...)*/
	else
	{
		bneednotice = true;
		mgr_add_givenname_updateparm(parm_node, &nodename, nodetype, rel_node, rel_updateparm, rel_parm, bneednotice);
	}
	/*close relation */
	heap_close(rel_updateparm, RowExclusiveLock);
	heap_close(rel_parm, RowExclusiveLock);
	heap_close(rel_node, RowExclusiveLock);
	pfree(parm_node);
	PG_RETURN_BOOL(true);
}

static void mgr_add_givenname_updateparm(MGRUpdateparm *parm_node, Name nodename, char nodetype, Relation rel_node, Relation rel_updateparm, Relation rel_parm, bool bneednotice)
{
	HeapTuple newtuple;
	Datum datum[Natts_mgr_updateparm];
	List *param_keyvules_list = NIL;
	ListCell *cell;
	ListCell *lc;
	DefElem *def;
	NameData key;
	NameData value;
	NameData defaultvalue;
	NameData parmunit;
	NameData parmmin;
	NameData parmmax;
	NameData valuetmp;
	StringInfoData enumvalue;
	StringInfoData paramstrdata;
	bool isnull[Natts_mgr_updateparm];
	bool bsighup = false;
	char parmtype; /*coordinator or datanode or gtm */
	char *pvalue;
	int insertparmstatus;
	int effectparmstatus;
	int vartype;  /*the parm value type: bool, string, enum, int*/
	int ipoint = 0;
	const int namemaxlen = 64;
	struct keyvalue
	{
		char key[namemaxlen];
		char value[namemaxlen];
	};
	struct keyvalue *key_value = NULL;
	Assert(parm_node && parm_node->parmtype);
	parmtype =  parm_node->parmtype;
	memset(datum, 0, sizeof(datum));
	memset(isnull, 0, sizeof(isnull));
	initStringInfo(&enumvalue);

	/*check the key and value*/
	foreach(lc, parm_node->options)
	{
		def = lfirst(lc);
		Assert(def && IsA(def, DefElem));
		namestrcpy(&key, def->defname);	
		namestrcpy(&value, defGetString(def));
		if (strcmp(key.data, "port") == 0 || strcmp(key.data, "synchronous_standby_names") == 0)
		{
			pfree(enumvalue.data);
			ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE)
				, errmsg("permission denied: \"%s\" shoule be modified in \"node\" table before init all, \nuse \"list node\" to get information", key.data)));
		}
		if (!parm_node->is_force)
		{
			/*check the parameter is right for the type node of postgresql.conf*/
			resetStringInfo(&enumvalue);
			mgr_check_parm_in_pgconf(rel_parm, parmtype, &key, &defaultvalue, &vartype, &parmunit, &parmmin, &parmmax, &effectparmstatus, &enumvalue, bneednotice);
			if(PGC_SIGHUP == effectparmstatus)
				bsighup = true;

			if (PGC_STRING == vartype || (PGC_ENUM == vartype && strstr(value.data, " ") != NULL))
			{
				/*if the value of key is string or the type is enum and value include space, it need use signle quota*/
				mgr_string_add_single_quota(&value);
			}
			/* allow whitespace between integer and unit */
			if (PGC_INT == vartype)
			{
				/*check the value not include empty space*/
				pvalue=strchr(value.data, ' ');
				if (pvalue != NULL)
				{
					pvalue = value.data;
					ipoint = 0;
					/*skip head space*/
					while (isspace((unsigned char) *pvalue))
					{
						pvalue++;
					}
					while(*pvalue != '\0' && *pvalue != ' ')
					{
						valuetmp.data[ipoint] = *pvalue;
						ipoint++;
						pvalue++;
					}
					/*skip the space between value and unit*/
					while (isspace((unsigned char) *pvalue))
					{
						pvalue++;
					}
					if (*pvalue < '0' || *pvalue > '9')
					{
						/*get the unit*/
						while(*pvalue != '\0' && *pvalue != ' ')
						{
							valuetmp.data[ipoint] = *pvalue;
							ipoint++;
							pvalue++;
						}
						/*skip the space after unit*/
						while (isspace((unsigned char) *pvalue))
						{
							pvalue++;
						}
						if ('\0' == *pvalue)
						{
							valuetmp.data[ipoint]='\0';
							namestrcpy(&value, valuetmp.data);
						}
					}
				}
			}

			/*check the key's value*/
			if (mgr_check_parm_value(key.data, value.data, vartype, parmunit.data, parmmin.data, parmmax.data, &enumvalue) != 1)
			{
				return;
			}
		}
		else
		{
			/*add single quota for it if it not using single quota*/
			mgr_string_add_single_quota(&value);
		}
		key_value = palloc(sizeof(struct keyvalue));
		strncpy(key_value->key, key.data, namemaxlen-1);
		/*get key*/
		if (strlen(key.data) < namemaxlen)
			key_value->key[strlen(key.data)] = '\0';
		else
			key_value->key[namemaxlen-1] = '\0';
		/*get value*/
		strncpy(key_value->value, value.data, namemaxlen-1);
		if (strlen(value.data) < namemaxlen)
			key_value->value[strlen(value.data)] = '\0';
		else
			key_value->value[namemaxlen-1] = '\0';
		param_keyvules_list = lappend(param_keyvules_list,key_value); 
	}
	pfree(enumvalue.data);
	
	initStringInfo(&paramstrdata);
	/*refresh the param table*/
	foreach(cell, param_keyvules_list)
	{
		key_value = (struct keyvalue *)(lfirst(cell));
		namestrcpy(&key, key_value->key);
		namestrcpy(&value, key_value->value);
		/*add key, value to send string*/
		mgr_append_pgconf_paras_str_str(key.data, value.data, &paramstrdata);
		/*check the parm exists already in mgr_updateparm systbl*/
		insertparmstatus = mgr_check_parm_in_updatetbl(rel_updateparm, nodetype, nodename, &key, value.data);
		if (PARM_NEED_NONE == insertparmstatus)
			continue;
		else if (PARM_NEED_UPDATE == insertparmstatus)
		{
			continue;
		}
		datum[Anum_mgr_updateparm_nodename-1] = NameGetDatum(nodename);
		datum[Anum_mgr_updateparm_nodetype-1] = CharGetDatum(nodetype);
		datum[Anum_mgr_updateparm_key-1] = NameGetDatum(&key);
		datum[Anum_mgr_updateparm_value-1] = NameGetDatum(&value);
		/* now, we can insert record */
		newtuple = heap_form_tuple(RelationGetDescr(rel_updateparm), datum, isnull);
		simple_heap_insert(rel_updateparm, newtuple);
		CatalogUpdateIndexes(rel_updateparm, newtuple);
		heap_freetuple(newtuple);
	}
	list_free(param_keyvules_list);
	/*if the gtm/coordinator/datanode has inited, it will refresh the postgresql.conf of the node*/
	if (bsighup)
		effectparmstatus = PGC_SIGHUP;
	mgr_reload_parm(rel_node, nodename->data, nodetype, &paramstrdata, effectparmstatus, false);
	pfree(paramstrdata.data);
}

/*
*check the given parameter nodetype, key,value in mgr_parm, if not in, shows the parameter is not right in postgresql.conf
*/
static void mgr_check_parm_in_pgconf(Relation noderel, char parmtype, Name key, Name value, int *vartype, Name parmunit, Name parmmin, Name parmmax, int *effectparmstatus, StringInfo enumvalue, bool bneednotice)
{
	HeapTuple tuple;
	char *gucconntent;
	Form_mgr_parm mgr_parm;
	Datum datumparmunit;
	Datum datumparmmin;
	Datum datumparmmax;
	Datum datumenumvalues;
	bool isNull = false;
	
	/*check the name of key exist in mgr_parm system table, if the key only in gtm/coordinator/datanode, the parmtype in 
	* mgr_parm is PARM_TYPE_GTM/PARM_TYPE_COORDINATOR/PARM_TYPE_DATANODE; if the key only in cn or dn, the parmtype in 
	* mgr_parm is '#'; if the key in gtm or cn or dn, the parmtype in mgr_parm is '*'; 
	* first: check the parmtype the input parameter given; second: check the parmtype '#'; third check the parmtype '*'
	*/
	/*check the parm in mgr_parm, type is parmtype*/
	tuple = SearchSysCache2(PARMTYPENAME, CharGetDatum(parmtype), NameGetDatum(key));
	if(!HeapTupleIsValid(tuple))
	{
		if (PARM_TYPE_COORDINATOR == parmtype || PARM_TYPE_DATANODE ==parmtype)
		{
			/*check the parm in mgr_parm, type is '#'*/
			tuple = SearchSysCache2(PARMTYPENAME, CharGetDatum(PARM_IN_CN_DN), NameGetDatum(key));
			if(!HeapTupleIsValid(tuple))
			{
				/*check the parm in mgr_parm, type is '*'*/
				tuple = SearchSysCache2(PARMTYPENAME, CharGetDatum(PARM_IN_GTM_CN_DN), NameGetDatum(key));
				if(!HeapTupleIsValid(tuple))
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
						, errmsg("unrecognized configuration parameter \"%s\"", key->data)));
				mgr_parm = (Form_mgr_parm)GETSTRUCT(tuple);		
			}
			else
				mgr_parm = (Form_mgr_parm)GETSTRUCT(tuple);
		}
		else if (PARM_TYPE_GTM == parmtype)
		{
			/*check the parm in mgr_parm, type is '*'*/
			tuple = SearchSysCache2(PARMTYPENAME, CharGetDatum(PARM_IN_GTM_CN_DN), NameGetDatum(key));
			if(!HeapTupleIsValid(tuple))
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
					, errmsg("unrecognized configuration parameter \"%s\"", key->data)));
			mgr_parm = (Form_mgr_parm)GETSTRUCT(tuple);		
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
				, errmsg("the parm type \"%c\" does not exist", parmtype)));
		}
	}
	else
	{
		mgr_parm = (Form_mgr_parm)GETSTRUCT(tuple);
	}

	Assert(mgr_parm);
	gucconntent = NameStr(mgr_parm->parmcontext);
	if (strcmp(NameStr(mgr_parm->parmvartype), "string") == 0)
	{
		*vartype = PGC_STRING;
	}
	else if (strcmp(NameStr(mgr_parm->parmvartype), "real") == 0)
	{
		*vartype = PGC_REAL;
	}
	else if (strcmp(NameStr(mgr_parm->parmvartype), "enum") == 0)
	{
		*vartype = PGC_ENUM;
	}
	else if (strcmp(NameStr(mgr_parm->parmvartype), "bool") == 0)
	{
		*vartype = PGC_BOOL;
	}
	else if (strcmp(NameStr(mgr_parm->parmvartype), "integer") == 0)
	{
		*vartype = PGC_INT;
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
			, errmsg("the value type \"%s\" does not exist", NameStr(mgr_parm->parmvartype))));		
	}
		
	/*get the default value*/
	namestrcpy(value, NameStr(mgr_parm->parmvalue));
	if (PGC_STRING == *vartype || (PGC_ENUM == *vartype && strstr(value->data, " ") != NULL))
	{
		/*if the value of key is string or the type is enum and value includes space, it need use signle quota*/
		mgr_string_add_single_quota(value);
	}
	/*get parm unit*/
	datumparmunit = heap_getattr(tuple, Anum_mgr_parm_unit, RelationGetDescr(noderel), &isNull);
	if(isNull)
	{
		namestrcpy(parmunit, "");
	}
	else
	{
		namestrcpy(parmunit,TextDatumGetCString(datumparmunit));
	}
	/*get parm min*/
	datumparmmin = heap_getattr(tuple, Anum_mgr_parm_minval, RelationGetDescr(noderel), &isNull);
	if(isNull)
	{
		namestrcpy(parmmin, "0");
	}
	else
	{
		namestrcpy(parmmin,TextDatumGetCString(datumparmmin));
	}
	/*get parm max*/
	datumparmmax = heap_getattr(tuple, Anum_mgr_parm_maxval, RelationGetDescr(noderel), &isNull);
	if(isNull)
	{
		namestrcpy(parmmax, "0");
	}
	else
	{
		namestrcpy(parmmax,TextDatumGetCString(datumparmmax));
	}
	/*get enumvalues*/
	datumenumvalues = heap_getattr(tuple, Anum_mgr_parm_enumval, RelationGetDescr(noderel), &isNull);
	if(isNull)
	{
		/*never come here*/
		appendStringInfo(enumvalue, "%s", "{0}");
	}
	else
	{
		appendStringInfo(enumvalue, "%s", TextDatumGetCString(datumenumvalues));
	}
	
	if (strcasecmp(gucconntent, GucContext_Parmnames[PGC_USERSET]) == 0 || strcasecmp(gucconntent, GucContext_Parmnames[PGC_SUSET]) == 0 || strcasecmp(gucconntent, GucContext_Parmnames[PGC_SIGHUP]) == 0)
	{
		*effectparmstatus = PGC_SIGHUP;
	}
	else if (strcasecmp(gucconntent, GucContext_Parmnames[PGC_POSTMASTER]) == 0)
	{
		*effectparmstatus = PGC_POSTMASTER;
		if (bneednotice)
			ereport(NOTICE, (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM)
				, errmsg("parameter \"%s\" cannot be changed without restarting the server", key->data)));
	}
	else if (strcasecmp(gucconntent, GucContext_Parmnames[PGC_INTERNAL]) == 0)
	{
		*effectparmstatus = PGC_INTERNAL;
		if (bneednotice)
			ereport(NOTICE, (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM)
				, errmsg("parameter \"%s\" cannot be changed", key->data)));
	}
	else if (strcasecmp(gucconntent, GucContext_Parmnames[PGC_BACKEND]) == 0)
	{
		*effectparmstatus = PGC_BACKEND;
		if (bneednotice)
			ereport(NOTICE, (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM)
				, errmsg("parameter \"%s\" cannot be set after connection start", key->data)));
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
			, errmsg("unkown the content of this parameter \"%s\"", key->data)));
	}
	ReleaseSysCache(tuple);
}


/*
* check the parmeter exist in mgr_updateparm systbl or not. 
* 1. nodename is MACRO_STAND_FOR_ALL_NODENAME, does not in mgr_updateparm, clean the gtm or datanode param which name is not 
*	MACRO_STAND_FOR_ALL_NODENAME and has the same key, return PARM_NEED_INSERT
* 2. nodename is MACRO_STAND_FOR_ALL_NODENAME, exists in mgr_updateparm, clean the gtm or datanode param which name is not 
*	MACRO_STAND_FOR_ALL_NODENAME and has the same key, return PARM_NEED_UPDATE if the value need refresh or delnum != 0
*	(refresh value in this function if it need)
* 3. nodename is MACRO_STAND_FOR_ALL_NODENAME, exists in mgr_updateparm, clean the gtm or datanode param which name is not 
*	MACRO_STAND_FOR_ALL_NODENAME and has the same key, return PARM_NEED_DONE if the value does not need refresh and delnum == 0
* 4. nodename is not MACRO_STAND_FOR_ALL_NODENAME, MACRO_STAND_FOR_ALL_NODENAME its type include the nodename type-key, has the 
*	same value, if  nodename-type-key exists and has the same value, delete the tuple nodename-type-key return PARM_NEED_NONE
*	if  nodename-type-key exists and has not the same value, delete the tuple nodename-type-key return PARM_NEED_UPDATE,
*	if  nodename-type-key does not exists, return PARM_NEED_NONE
* 5. nodename is not MACRO_STAND_FOR_ALL_NODENAME, MACRO_STAND_FOR_ALL_NODENAME its type includes the nodename type-key, has not 
*	 the same value or not find the tuple for MACRO_STAND_FOR_ALL_NODENAME, then check the nodename-key-type exists in 
*	mgr_updateparm, if has same value, return PARM_NEED_NONE else 
*	PARM_NEED_UPDATE (refresh its value in this function)
* 6. nodename is not MACRO_STAND_FOR_ALL_NODENAME, MACRO_STAND_FOR_ALL_NODENAME its type includes the nodename type-key, has not 
*		 the same value or not find the tuple for MACRO_STAND_FOR_ALL_NODENAME, then check the nodename-key-type does not exists in 
*	mgr_updateparm, return PARM_NEED_INSERT    
*/

static int mgr_check_parm_in_updatetbl(Relation noderel, char nodetype, Name nodename, Name key, char *value)
{
	HeapTuple tuple;
	HeapTuple newtuple;
	HeapTuple alltype_tuple;
	Form_mgr_updateparm mgr_updateparm;
	Form_mgr_updateparm mgr_updateparm_alltype;
	NameData name_standall;
	NameData valuedata;
	HeapScanDesc rel_scan;
	HeapScanDesc rel_scanall;
	ScanKeyData scankey[3];
	TupleDesc tupledsc;
	Datum datum[Natts_mgr_updateparm];
	bool isnull[Natts_mgr_updateparm];
	bool got[Natts_mgr_updateparm];
	char allnodetype;
	int delnum = 0;
	int ret;

	if (GTM_TYPE_GTM_MASTER == nodetype || GTM_TYPE_GTM_SLAVE == nodetype || GTM_TYPE_GTM_EXTRA == nodetype)
		allnodetype = CNDN_TYPE_GTM;
	else if (CNDN_TYPE_DATANODE_MASTER == nodetype || CNDN_TYPE_DATANODE_SLAVE == nodetype || CNDN_TYPE_DATANODE_EXTRA == nodetype)
		allnodetype = CNDN_TYPE_DATANODE;
	else
		allnodetype = nodetype;
	/*nodename is MACRO_STAND_FOR_ALL_NODENAME*/
	if (namestrcmp(nodename, MACRO_STAND_FOR_ALL_NODENAME) == 0)
	{
		ScanKeyInit(&scankey[0]
			,Anum_mgr_updateparm_nodetype
			,BTEqualStrategyNumber
			,F_CHAREQ
			,CharGetDatum(nodetype));
		ScanKeyInit(&scankey[1]
			,Anum_mgr_updateparm_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,NameGetDatum(nodename));
		ScanKeyInit(&scankey[2]
			,Anum_mgr_updateparm_key
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,NameGetDatum(key));
		rel_scan = heap_beginscan(noderel, SnapshotNow, 3, scankey);
		tuple = heap_getnext(rel_scan, ForwardScanDirection);
		/*1.does not exist in mgr_updateparm*/
		if (!HeapTupleIsValid(tuple))
		{
			mgr_delete_tuple_not_all(noderel, nodetype, key);
			heap_endscan(rel_scan);
			return PARM_NEED_INSERT;
		}
		else
		{
			/*2,3. check need update*/
			mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(tuple);
			Assert(mgr_updateparm);
			if (strcmp(value, NameStr(mgr_updateparm->updateparmvalue)) == 0)
			{
				delnum += mgr_delete_tuple_not_all(noderel, nodetype, key);
				heap_endscan(rel_scan);
				if (delnum > 0)
					return PARM_NEED_UPDATE;
				else
					return PARM_NEED_NONE;
			}
			else
			{
				mgr_delete_tuple_not_all(noderel, nodetype, key);
				/*update parm's value*/
				memset(datum, 0, sizeof(datum));
				memset(isnull, 0, sizeof(isnull));
				memset(got, 0, sizeof(got));
				namestrcpy(&valuedata, value);
				datum[Anum_mgr_updateparm_value-1] = NameGetDatum(&valuedata);
				got[Anum_mgr_updateparm_value-1] = true;
				tupledsc = RelationGetDescr(noderel);
				newtuple = heap_modify_tuple(tuple, tupledsc, datum,isnull, got);
				simple_heap_update(noderel, &tuple->t_self, newtuple);
				CatalogUpdateIndexes(noderel, newtuple);
				heap_endscan(rel_scan);
				return PARM_NEED_UPDATE;
			}
		}
	}
	else
	{
		namestrcpy(&name_standall, MACRO_STAND_FOR_ALL_NODENAME);
		ScanKeyInit(&scankey[0]
			,Anum_mgr_updateparm_nodetype
			,BTEqualStrategyNumber
			,F_CHAREQ
			,CharGetDatum(allnodetype));
		ScanKeyInit(&scankey[1]
			,Anum_mgr_updateparm_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,NameGetDatum(&name_standall));
		ScanKeyInit(&scankey[2]
			,Anum_mgr_updateparm_key
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,NameGetDatum(key));
		rel_scanall = heap_beginscan(noderel, SnapshotNow, 3, scankey);
		alltype_tuple = heap_getnext(rel_scanall, ForwardScanDirection);
		ScanKeyInit(&scankey[0]
			,Anum_mgr_updateparm_nodetype
			,BTEqualStrategyNumber
			,F_CHAREQ
			,CharGetDatum(nodetype));
		ScanKeyInit(&scankey[1]
			,Anum_mgr_updateparm_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,NameGetDatum(nodename));
		ScanKeyInit(&scankey[2]
			,Anum_mgr_updateparm_key
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,NameGetDatum(key));
		rel_scan = heap_beginscan(noderel, SnapshotNow, 3, scankey);
		tuple = heap_getnext(rel_scan, ForwardScanDirection);
		if (HeapTupleIsValid(alltype_tuple))
		{
			mgr_updateparm_alltype = (Form_mgr_updateparm)GETSTRUCT(alltype_tuple);
			Assert(mgr_updateparm_alltype);
			/*4. MACRO_STAND_FOR_ALL_NODENAME has the same type-key-value */
			if (strcmp(NameStr(mgr_updateparm_alltype->updateparmvalue), value) == 0)
			{
				if (HeapTupleIsValid(tuple))
				{
					mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(tuple);
					Assert(mgr_updateparm_alltype);
					if (strcmp(NameStr(mgr_updateparm->updateparmvalue), value) != 0)
						ret = PARM_NEED_UPDATE;
					else
						ret = PARM_NEED_NONE;
					simple_heap_delete(noderel, &tuple->t_self);
					CatalogUpdateIndexes(noderel, tuple);
					heap_endscan(rel_scan);
					heap_endscan(rel_scanall);
					return ret;
				}
				else
				{
					heap_endscan(rel_scanall);
					heap_endscan(rel_scan);
					return PARM_NEED_NONE;
				}
			}
		}
		/*5,6*/
		if (HeapTupleIsValid(tuple))
		{
			mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(tuple);
			Assert(mgr_updateparm);
			if (strcmp(NameStr(mgr_updateparm->updateparmvalue), value) == 0)
			{
				heap_endscan(rel_scan);
				heap_endscan(rel_scanall);
				return PARM_NEED_NONE;
			}
			else
			{
				/*update parm's value*/
				memset(datum, 0, sizeof(datum));
				memset(isnull, 0, sizeof(isnull));
				memset(got, 0, sizeof(got));
				namestrcpy(&valuedata, value);
				datum[Anum_mgr_updateparm_value-1] = NameGetDatum(&valuedata);
				got[Anum_mgr_updateparm_value-1] = true;
				tupledsc = RelationGetDescr(noderel);
				newtuple = heap_modify_tuple(tuple, tupledsc, datum,isnull, got);
				simple_heap_update(noderel, &tuple->t_self, newtuple);
				CatalogUpdateIndexes(noderel, newtuple);
				heap_endscan(rel_scan);
				heap_endscan(rel_scanall);
				return PARM_NEED_UPDATE;
			}
		}
		else
		{
			heap_endscan(rel_scan);
			heap_endscan(rel_scanall);
			return PARM_NEED_INSERT;
		}
	}
}

/*
*get the parameters from mgr_updateparm, then add them to infosendparamsg,  used for initdb
*first, add the parameter which the nodename is '*' with given nodetype; second, add the parameter for given name with given nodetype 
*/
void mgr_add_parm(char *nodename, char nodetype, StringInfo infosendparamsg)
{
	Relation rel_updateparm;
	Form_mgr_updateparm mgr_updateparm;
	Form_mgr_updateparm mgr_updateparm_check;
	ScanKeyData key[2];
	HeapScanDesc rel_scan;
	HeapTuple tuple;
	HeapTuple checktuple;
	char *parmkey;
	char *parmvalue;
	char allnodetype;
	NameData nodenamedata;
	NameData nodenamedatacheck;
	
	if (GTM_TYPE_GTM_MASTER == nodetype || GTM_TYPE_GTM_SLAVE == nodetype || GTM_TYPE_GTM_EXTRA == nodetype)
		allnodetype = CNDN_TYPE_GTM;
	else if (CNDN_TYPE_DATANODE_MASTER == nodetype || CNDN_TYPE_DATANODE_SLAVE == nodetype || CNDN_TYPE_DATANODE_EXTRA == nodetype)
		allnodetype = CNDN_TYPE_DATANODE;
	else
		allnodetype = CNDN_TYPE_COORDINATOR_MASTER;
	/*first: add the parameter which the nodename is '*' with allnodetype*/
	namestrcpy(&nodenamedata, MACRO_STAND_FOR_ALL_NODENAME);
	namestrcpy(&nodenamedatacheck, nodename);
	ScanKeyInit(&key[0],
		Anum_mgr_updateparm_nodetype
		,BTEqualStrategyNumber
		,F_CHAREQ
		,CharGetDatum(allnodetype));
	ScanKeyInit(&key[1],
		Anum_mgr_updateparm_nodename
		,BTEqualStrategyNumber
		,F_NAMEEQ
		,NameGetDatum(&nodenamedata));	
	rel_updateparm = heap_open(UpdateparmRelationId, RowExclusiveLock);
	rel_scan = heap_beginscan(rel_updateparm, SnapshotNow, 2, key);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(tuple);
		Assert(mgr_updateparm);
		/*get key, value*/
		checktuple = SearchSysCache3(MGRUPDATAPARMNODENAMENODETYPEKEY, NameGetDatum(&nodenamedatacheck), CharGetDatum(nodetype), NameGetDatum(&(mgr_updateparm->updateparmkey)));
		if(HeapTupleIsValid(checktuple))
		{
			mgr_updateparm_check = (Form_mgr_updateparm)GETSTRUCT(checktuple);
			Assert(mgr_updateparm_check);
			if (strcmp(NameStr(mgr_updateparm_check->updateparmvalue), DEFAULT_VALUE) == 0)
			{
				ReleaseSysCache(checktuple);
				continue;
			}
			ReleaseSysCache(checktuple);
		}
		parmkey = NameStr(mgr_updateparm->updateparmkey);
		parmvalue = NameStr(mgr_updateparm->updateparmvalue);
		mgr_append_pgconf_paras_str_str(parmkey, parmvalue, infosendparamsg);
	}
	heap_endscan(rel_scan);
	/*second: add the parameter for given name with given nodetype*/
	namestrcpy(&nodenamedata, nodename);
	ScanKeyInit(&key[0],
		Anum_mgr_updateparm_nodetype
		,BTEqualStrategyNumber
		,F_CHAREQ
		,CharGetDatum(nodetype));
	ScanKeyInit(&key[1],
		Anum_mgr_updateparm_nodename
		,BTEqualStrategyNumber
		,F_NAMEEQ
		,NameGetDatum(&nodenamedata));
	rel_scan = heap_beginscan(rel_updateparm, SnapshotNow, 2, key);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(tuple);
		Assert(mgr_updateparm);
		if (strcmp(NameStr(mgr_updateparm->updateparmvalue), DEFAULT_VALUE) == 0)
		{
			continue;
		}
		/*get key, value*/
		parmkey = NameStr(mgr_updateparm->updateparmkey);
		parmvalue = NameStr(mgr_updateparm->updateparmvalue);
		mgr_append_pgconf_paras_str_str(parmkey, parmvalue, infosendparamsg);
	}
	heap_endscan(rel_scan);
	heap_close(rel_updateparm, RowExclusiveLock);
}

/*
* according to "set datanode|coordinator|gtm master|slave|extra nodename(key1=value1,...)" , get the nodename, key and value, 
* then from node systbl to get ip and path, then reload the key for the node(datanode or coordinator or gtm) when 
* the type of the key does not need restart to make effective
*/

static void mgr_reload_parm(Relation noderel, char *nodename, char nodetype, StringInfo paramstrdata, int effectparmstatus, bool bforce)
{
	HeapTuple tuple;
	Form_mgr_node mgr_node;
	GetAgentCmdRst getAgentCmdRst;
	Datum datumpath;
	HeapScanDesc rel_scan;
	char *nodepath;
	char *nodetypestr;
	bool isNull;

	initStringInfo(&(getAgentCmdRst.description));
	/*nodename is MACRO_STAND_FOR_ALL_NODENAME*/
	if (strcmp(nodename, MACRO_STAND_FOR_ALL_NODENAME) == 0)
	{
		rel_scan = heap_beginscan(noderel, SnapshotNow, 0, NULL);
		while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
		{
			mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
			Assert(mgr_node);
			if(!mgr_node->nodeincluster)
				continue;
			/*all gtm type: master/slave/extra*/
			if (CNDN_TYPE_GTM == nodetype)
			{
				if (mgr_node->nodetype != GTM_TYPE_GTM_MASTER && mgr_node->nodetype != GTM_TYPE_GTM_SLAVE && mgr_node->nodetype != GTM_TYPE_GTM_EXTRA)
					continue;
			}
			/*all datanode type: master/slave/extra*/
			else if (CNDN_TYPE_DATANODE == nodetype)
			{
				if (mgr_node->nodetype != CNDN_TYPE_DATANODE_MASTER && mgr_node->nodetype != CNDN_TYPE_DATANODE_SLAVE && mgr_node->nodetype != CNDN_TYPE_DATANODE_EXTRA)
					continue;
			}
			/*for coordinator all*/
			else
			{
				if (nodetype != mgr_node->nodetype)
					continue;
			}
			datumpath = heap_getattr(tuple, Anum_mgr_node_nodepath, RelationGetDescr(noderel), &isNull);
			if(isNull)
			{
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR)
					, err_generic_string(PG_DIAG_TABLE_NAME, "mgr_node")
					, errmsg("column cndnpath is null")));
			}
			nodepath = TextDatumGetCString(datumpath);
			ereport(LOG,
				(errmsg("send parameter %s ... to %s", paramstrdata->data, nodepath)));
			mgr_updateparm_send_parm(&getAgentCmdRst, mgr_node->nodehost, nodepath, paramstrdata, effectparmstatus, bforce);
		}
		heap_endscan(rel_scan);
	}
	else	/*for given nodename*/
	{
		tuple = mgr_get_tuple_node_from_name_type(noderel, nodename, nodetype);
		if(!(HeapTupleIsValid(tuple)))
		{
			nodetypestr = mgr_nodetype_str(nodetype);
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
				 ,errmsg("%s \"%s\" does not exist", nodetypestr, nodename)));
		}
		mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
		Assert(mgr_node);
		if(!mgr_node->nodeincluster)
		{
			pfree(getAgentCmdRst.description.data);
			heap_freetuple(tuple);
			return;
		}
		/*get path*/
		datumpath = heap_getattr(tuple, Anum_mgr_node_nodepath, RelationGetDescr(noderel), &isNull);
		if(isNull)
		{
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR)
				, err_generic_string(PG_DIAG_TABLE_NAME, "mgr_node")
				, errmsg("column cndnpath is null")));
		}
		nodepath = TextDatumGetCString(datumpath);	
		/*send the parameter to node path, then reload it*/
		ereport(LOG,
			(errmsg("send parameter %s ... to %s", paramstrdata->data, nodepath)));
		mgr_updateparm_send_parm(&getAgentCmdRst, mgr_node->nodehost, nodepath, paramstrdata, effectparmstatus, bforce);
		heap_freetuple(tuple);
	}
	pfree(getAgentCmdRst.description.data);
}

/*
* send parameter to node, refresh its postgresql.conf, if the guccontent of parameter is superuser/user/sighup, will reload the parameter
*/
static void mgr_updateparm_send_parm(GetAgentCmdRst *getAgentCmdRst, Oid hostoid, char *nodepath, StringInfo paramstrdata, int effectparmstatus, bool bforce)
{	
	/*send the parameter to node path, then reload it*/
	resetStringInfo(&(getAgentCmdRst->description));
	if(effectparmstatus == PGC_SIGHUP)
	{
		if (!bforce)
			mgr_send_conf_parameters(AGT_CMD_CNDN_REFRESH_PGSQLCONF_RELOAD, nodepath, paramstrdata, hostoid, getAgentCmdRst);
		else
			mgr_send_conf_parameters(AGT_CMD_CNDN_DELPARAM_PGSQLCONF_FORCE, nodepath, paramstrdata, hostoid, getAgentCmdRst);
	}
	else
	{
		if (!bforce)
			mgr_send_conf_parameters(AGT_CMD_CNDN_REFRESH_PGSQLCONF, nodepath, paramstrdata, hostoid, getAgentCmdRst);
		else
			mgr_send_conf_parameters(AGT_CMD_CNDN_DELPARAM_PGSQLCONF_FORCE, nodepath, paramstrdata, hostoid, getAgentCmdRst);
	}

	if (getAgentCmdRst->ret != true)
	{
		ereport(ERROR, (errcode(ERRCODE_CONNECTION_FAILURE)
			 ,errmsg("reload parameter fail: %s", (getAgentCmdRst->description).data))); 
	}
}

static int mgr_delete_tuple_not_all(Relation noderel, char nodetype, Name key)
{
	HeapTuple looptuple;
	Form_mgr_updateparm mgr_updateparm;
	HeapScanDesc rel_scan;
	int delnum = 0;
	
	/*check the nodename in mgr_updateparm nodetype and key are not the same with MACRO_STAND_FOR_ALL_NODENAME*/
	rel_scan = heap_beginscan(noderel, SnapshotNow, 0, NULL);
	while((looptuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(looptuple);
		Assert(mgr_updateparm);
		if (strcmp(NameStr(mgr_updateparm->updateparmkey), key->data) != 0)
			continue;
		if (strcmp(NameStr(mgr_updateparm->updateparmnodename), MACRO_STAND_FOR_ALL_NODENAME) == 0)
			continue;
		/*all gtm type: master/slave/extra*/
		if (CNDN_TYPE_GTM == nodetype)
		{
			if (mgr_updateparm->updateparmnodetype != GTM_TYPE_GTM_MASTER && mgr_updateparm->updateparmnodetype != GTM_TYPE_GTM_SLAVE && mgr_updateparm->updateparmnodetype != GTM_TYPE_GTM_EXTRA)
				continue;
		}
		/*all datanode type: master/slave/extra*/
		else if (CNDN_TYPE_DATANODE == nodetype)
		{
			if (mgr_updateparm->updateparmnodetype != CNDN_TYPE_DATANODE_MASTER && mgr_updateparm->updateparmnodetype != CNDN_TYPE_DATANODE_SLAVE && mgr_updateparm->updateparmnodetype != CNDN_TYPE_DATANODE_EXTRA)
				continue;
		}
		/*for coordinator all*/
		else
		{
			if (mgr_updateparm->updateparmnodetype != nodetype)
				continue;
		}
		/*delete the tuple which nodename is not MACRO_STAND_FOR_ALL_NODENAME and has the same nodetype and key*/
		delnum++;
		simple_heap_delete(noderel, &looptuple->t_self);
		CatalogUpdateIndexes(noderel, looptuple);
	}
	heap_endscan(rel_scan);
	return delnum;
}

/* 
* for command: reset {datanode|coordinaotr} {master|slave|extra} {nodename | all}{key1,key2...} , reset datanode 
* all {key1,key2...}, reset gtm all{key1,key2...}, reset gtm master|slave|extra gtmx {key1,key2...}, to remove the 
* parameter in mgr_updateparm; if the reset parameters not in mgr_updateparm, report error; otherwise use the values 
* which come from mgr_parm to replace the old values;
*/
void mgr_reset_updateparm(MGRUpdateparmReset *node, ParamListInfo params, DestReceiver *dest)
{
	if (mgr_has_priv_reset())
	{
		DirectFunctionCall6(mgr_reset_updateparm_func,
							CharGetDatum(node->parmtype),
							CStringGetDatum(node->nodename),
							CharGetDatum(node->nodetype),
							CStringGetDatum(node->key),
							BoolGetDatum(node->is_force),
							PointerGetDatum(node->options));
		return;
	}
	else
	{
		ereport(ERROR, (errmsg("permission denied")));
		return ;
	}
}

Datum mgr_reset_updateparm_func(PG_FUNCTION_ARGS)
{
	Relation rel_updateparm;
	Relation rel_parm;
	Relation rel_node;
	HeapTuple newtuple;
	HeapTuple looptuple;
	HeapTuple tuple;
	NameData nodename;
	NameData nodenametmp;
	NameData parmmin;
	NameData parmmax;
	Datum datum[Natts_mgr_updateparm];
	ListCell *lc;
	DefElem *def;
	NameData key;
	NameData defaultvalue;
	NameData allnodevalue;
	NameData parmunit;
	ScanKeyData scankey[3];
	HeapScanDesc rel_scan;
	Form_mgr_node mgr_node;
	StringInfoData enumvalue;
	StringInfoData paramstrdata;
	bool isnull[Natts_mgr_updateparm];
	bool got[Natts_mgr_updateparm];
	bool bneedinsert = false;
	bool bneednotice = true;
	bool bsighup = false;
	char parmtype;			/*coordinator or datanode or gtm */
	char nodetype;			/*master/slave/extra*/
	char nodetypetmp;
	char allnodetype;
	int effectparmstatus;
	int vartype; /*the parm value type: bool, string, enum, int*/
	Form_mgr_updateparm mgr_updateparm;

	MGRUpdateparmReset *parm_node;
	parm_node = palloc(sizeof(*parm_node));
	parm_node->parmtype = PG_GETARG_CHAR(0);
	parm_node->nodename = PG_GETARG_CSTRING(1);
	parm_node->nodetype = PG_GETARG_CHAR(2);
	parm_node->key = PG_GETARG_CSTRING(3);
	parm_node->is_force = PG_GETARG_BOOL(4);
	parm_node->options = (List *)PG_GETARG_POINTER(5);

	initStringInfo(&enumvalue);
	Assert(parm_node && parm_node->nodename && parm_node->nodetype && parm_node->parmtype);
	nodetype = parm_node->nodetype;
	parmtype =  parm_node->parmtype;
	/*nodename*/
	namestrcpy(&nodename, parm_node->nodename);
	
	/*open systbl: mgr_parm*/
	rel_updateparm = heap_open(UpdateparmRelationId, RowExclusiveLock);
	rel_parm = heap_open(ParmRelationId, RowExclusiveLock);
	rel_node = heap_open(NodeRelationId, RowExclusiveLock);

	memset(datum, 0, sizeof(datum));
	memset(isnull, 0, sizeof(isnull));
	memset(got, 0, sizeof(got));

	initStringInfo(&paramstrdata);
	/*check the key*/
	foreach(lc, parm_node->options)
	{
		def = lfirst(lc);
		Assert(def && IsA(def, DefElem));
		namestrcpy(&key, def->defname);
		if (strcmp(key.data, "port") == 0 || strcmp(key.data, "synchronous_standby_names") == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE)
				, errmsg("permission denied: \"%s\" shoule be modified in \"node\" table before init all, \nuse \"list node\" to get information", key.data)));
		}
		/*check the parameter is right for the type node of postgresql.conf*/
		if (!parm_node->is_force)
		{
			resetStringInfo(&enumvalue);
			mgr_check_parm_in_pgconf(rel_parm, parmtype, &key, &defaultvalue, &vartype, &parmunit, &parmmin, &parmmax, &effectparmstatus, &enumvalue, bneednotice);
		}
		if (PGC_SIGHUP == effectparmstatus)
			bsighup = true;
		/*get key, value to send string*/
		if (!parm_node->is_force)
			mgr_append_pgconf_paras_str_str(key.data, defaultvalue.data, &paramstrdata);
		else
			mgr_append_pgconf_paras_str_str(key.data, "force", &paramstrdata);
	}
	/*refresh param table*/
	foreach(lc,parm_node->options)
	{
		def = lfirst(lc);
		Assert(def && IsA(def, DefElem));
		namestrcpy(&key, def->defname);
		if (parm_node->is_force)
		{
			/*use "none" to label the row is no use, just to show the node does not set this parameter in its postgresql.conf*/
			namestrcpy(&defaultvalue, DEFAULT_VALUE);
		}
		/*if nodename is '*', delete the tuple in mgr_updateparm which nodetype is given and reload the parm if the cluster inited
		* reset gtm all (key=value,...)
		* reset coordinator all (key=value,...)
		* reset datanode all (key=value,...)
		*/
		if (strcmp(nodename.data, MACRO_STAND_FOR_ALL_NODENAME) == 0 && (CNDN_TYPE_GTM == nodetype || CNDN_TYPE_DATANODE == nodetype ||CNDN_TYPE_COORDINATOR_MASTER == nodetype))
		{
			ScanKeyInit(&scankey[0],
				Anum_mgr_updateparm_key
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,NameGetDatum(&key));
			rel_scan = heap_beginscan(rel_updateparm, SnapshotNow, 1, scankey);
			while((looptuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
			{
				mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(looptuple);
				Assert(mgr_updateparm);
				nodetypetmp = mgr_updateparm->updateparmnodetype;
				if (CNDN_TYPE_GTM == nodetype)
				{
					if (GTM_TYPE_GTM_MASTER != nodetypetmp && GTM_TYPE_GTM_SLAVE != nodetypetmp && GTM_TYPE_GTM_EXTRA != nodetypetmp && CNDN_TYPE_GTM != nodetypetmp)
						continue;
				}
				else if (CNDN_TYPE_DATANODE == nodetype)
				{
					if (CNDN_TYPE_DATANODE_MASTER != nodetypetmp && CNDN_TYPE_DATANODE_SLAVE != nodetypetmp && CNDN_TYPE_DATANODE_EXTRA != nodetypetmp && CNDN_TYPE_DATANODE != nodetypetmp)
						continue;
				}
				else  /*for coordinator all*/
				{
					if (nodetypetmp != nodetype)
						continue;
				}
				/*delete the tuple which nodetype is the given nodetype*/
				simple_heap_delete(rel_updateparm, &looptuple->t_self);
				CatalogUpdateIndexes(rel_updateparm, looptuple);
			}
			heap_endscan(rel_scan);
		}
		/*the nodename is not MACRO_STAND_FOR_ALL_NODENAME or nodetype is datanode master/slave/extra, refresh the postgresql.conf 
		* of the node, and delete the tuple in mgr_updateparm which nodetype and nodename is given;if MACRO_STAND_FOR_ALL_NODENAME 
		* in mgr_updateparm has the same nodetype, insert one tuple to mgr_updateparm for record
		*/
		else 
		{
			/*check the MACRO_STAND_FOR_ALL_NODENAME has the same nodetype*/
			if (GTM_TYPE_GTM_MASTER == nodetype || GTM_TYPE_GTM_SLAVE == nodetype || GTM_TYPE_GTM_EXTRA == nodetype)
				allnodetype = CNDN_TYPE_GTM;
			else if (CNDN_TYPE_DATANODE_MASTER == nodetype || CNDN_TYPE_DATANODE_SLAVE == nodetype || CNDN_TYPE_DATANODE_EXTRA == nodetype)
				allnodetype = CNDN_TYPE_DATANODE;
			else
				allnodetype = CNDN_TYPE_COORDINATOR_MASTER;
			namestrcpy(&nodenametmp, MACRO_STAND_FOR_ALL_NODENAME);
			bneedinsert = false;
			ScanKeyInit(&scankey[0],
				Anum_mgr_updateparm_nodename
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,NameGetDatum(&nodenametmp));
			ScanKeyInit(&scankey[1],
				Anum_mgr_updateparm_nodetype
				,BTEqualStrategyNumber
				,F_CHAREQ
				,CharGetDatum(allnodetype));
			ScanKeyInit(&scankey[2],
				Anum_mgr_updateparm_key
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,NameGetDatum(&key));
			rel_scan = heap_beginscan(rel_updateparm, SnapshotNow, 3, scankey);
			while((looptuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
			{
				mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(looptuple);
				Assert(mgr_updateparm);
				strcpy(allnodevalue.data, NameStr(mgr_updateparm->updateparmvalue));
				if (strcmp(allnodevalue.data, defaultvalue.data) == 0)
					bneedinsert = false;
				else
					bneedinsert = true;
				break;
			}
			heap_endscan(rel_scan);

			/*delete the tuple*/
			ScanKeyInit(&scankey[0],
				Anum_mgr_updateparm_nodetype
				,BTEqualStrategyNumber
				,F_CHAREQ
				,CharGetDatum(nodetype));
			ScanKeyInit(&scankey[1],
				Anum_mgr_updateparm_key
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,NameGetDatum(&key));
			rel_scan = heap_beginscan(rel_updateparm, SnapshotNow, 2, scankey);
			while((looptuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
			{
				mgr_updateparm = (Form_mgr_updateparm)GETSTRUCT(looptuple);
				Assert(mgr_updateparm);
				/*for reset datanode master|slave|extra all (key,...), 
				* reset gtm master gtmname(key,...), 
				* reset datanode master|slave|extra dnname(key), 
				* reset coordinator cnname (key,...)
				*/
				if (strcmp(NameStr(nodename), MACRO_STAND_FOR_ALL_NODENAME) == 0 || strcmp(NameStr(mgr_updateparm->updateparmnodename), NameStr(nodename)) ==0)
				{
					simple_heap_delete(rel_updateparm, &looptuple->t_self);
					CatalogUpdateIndexes(rel_updateparm, looptuple);
				}
				else
				{
					/*do nothing*/
				}
			}
			heap_endscan(rel_scan);

			/*insert tuple*/
			if (bneedinsert)
			{
				ScanKeyInit(&scankey[0]
					,Anum_mgr_node_nodetype
					,BTEqualStrategyNumber
					,F_CHAREQ
					,CharGetDatum(nodetype));
				rel_scan = heap_beginscan(rel_node, SnapshotNow, 1, scankey);
				while ((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
				{
					mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
					Assert(mgr_node);
					if(strcmp(NameStr(nodename), MACRO_STAND_FOR_ALL_NODENAME) != 0)
					{
						if (strcmp(NameStr(nodename), NameStr(mgr_node->nodename)) != 0)
							continue;
					}
					datum[Anum_mgr_updateparm_nodename-1] = NameGetDatum(&(mgr_node->nodename));
					datum[Anum_mgr_updateparm_nodetype-1] = CharGetDatum(nodetype);
					datum[Anum_mgr_updateparm_key-1] = NameGetDatum(&key);
					datum[Anum_mgr_updateparm_value-1] = NameGetDatum(&defaultvalue);
					/* now, we can insert record */
					newtuple = heap_form_tuple(RelationGetDescr(rel_updateparm), datum, isnull);
					simple_heap_insert(rel_updateparm, newtuple);
					CatalogUpdateIndexes(rel_updateparm, newtuple);
					heap_freetuple(newtuple);
				}
				heap_endscan(rel_scan);
			}
		}
	}
	/*if the gtm/coordinator/datanode has inited, it will refresh the postgresql.conf of the node*/
	if (bsighup)
		effectparmstatus = PGC_SIGHUP;
	if (!parm_node->is_force)
		mgr_reload_parm(rel_node, nodename.data, nodetype, &paramstrdata, effectparmstatus, false);
	else
		mgr_reload_parm(rel_node, nodename.data, nodetype, &paramstrdata, PGC_POSTMASTER, true);
	pfree(enumvalue.data);
	pfree(paramstrdata.data);
	/*close relation */
	heap_close(rel_updateparm, RowExclusiveLock);
	heap_close(rel_parm, RowExclusiveLock);
	heap_close(rel_node, RowExclusiveLock);
	pfree(parm_node);
	PG_RETURN_BOOL(true);
}

/*
* check the guc value for postgresql.conf
*/
static int mgr_check_parm_value(char *name, char *value, int vartype, char *parmunit, char *parmmin, char *parmmax, StringInfo enumvalue)
{
	int elevel = ERROR;
	int flags;

	switch (vartype)
	{
		case PGC_BOOL:
			{
				bool		newval;

				if (value)
				{
					if (!parse_bool(value, &newval))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						  errmsg("parameter \"%s\" requires a Boolean value",
								 name)));
						return 0;
					}
				}
				break;
			}

		case PGC_INT:
			{
				int			newval;
				int min;
				int max;
				
				if (value)
				{
					const char *hintmsg;
					flags = mgr_get_parm_unit_type(name, parmunit);
					if (!parse_int(value, &newval, flags, &hintmsg))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for parameter \"%s\": \"%s\"",
								name, value),
								 hintmsg ? errhint("%s", _(hintmsg)) : 0));
						return 0;
					}
					if (strcmp(parmmin, "") ==0 || strcmp(parmmax, "") ==0)
					{
						return 1;
					}
					min = atoi(parmmin);
					max = atoi(parmmax);
					if (newval < min || newval > max)
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("%d is outside the valid range for parameter \"%s\" (%d .. %d)",
										newval, name, min, max)));
						return 0;
					}
				}
				break;
			}

		case PGC_REAL:
			{
				double		newval;
				double min;
				double max;
				
				if (value)
				{
					if (!parse_real(value, &newval))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						  errmsg("parameter \"%s\" requires a numeric value",
								 name)));
						return 0;
					}
					
					if (strcmp(parmmin, "") == 0 || strcmp(parmmax, "") == 0)
					{
						return 1;
					}
					min = atof(parmmin);
					max = atof(parmmax);
					
					if (newval < min || newval > max)
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("%g is outside the valid range for parameter \"%s\" (%g .. %g)",
										newval, name, min, max)));
						return 0;
					}
				}
				break;
			}

		case PGC_STRING:
			{
				/*nothing to do,only need check some name will be truncated*/
				break;
			}

		case PGC_ENUM:
			{
				if (!mgr_parm_enum_lookup_by_name(value, enumvalue))
				{
					ereport(elevel,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for parameter \"%s\": \"%s\"",
						name, value),
						 errhint("Available values: %s", _(enumvalue->data))));
					return 0;
				}
				break;
			}
		default:
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
					, errmsg("the param type \"d\" does not exist")));
	}
	return 1;
}

/*
* get unit type from unit and parm name(see guc.c)
*/

static int mgr_get_parm_unit_type(char *nodename, char *parmunit)
{
	if (strcmp(parmunit, "ms") == 0)
	{
		return GUC_UNIT_MS;
	}
	else if (strcmp(parmunit, "s") == 0)
	{
		if(strcmp(nodename, "post_auth_delay") ==0 || strcmp(nodename, "pre_auth_delay") ==0)
		{
			return (GUC_NOT_IN_SAMPLE | GUC_UNIT_S);
		}
		else
			return GUC_UNIT_S;
	}
	else if (strcmp(parmunit, "ms") ==0)
	{
		return GUC_UNIT_MS;
	}
	else if (strcmp(parmunit, "min") ==0)
	{
		return GUC_UNIT_MIN;
	}
	else if (strcmp(parmunit, "kB") ==0)
	{
		return GUC_UNIT_KB;
	}
	else if (strcmp(parmunit, "8kB") ==0)
	{
		if (strcmp(nodename, "wal_buffers") ==0)
		{
			return GUC_UNIT_XBLOCKS;
		}
		else if (strcmp(nodename, "wal_segment_size") ==0)
		{
			return (GUC_UNIT_XBLOCKS | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE);
		}
		else
			return GUC_UNIT_KB;
		
	}
	else
		return 0;
}

/*check enum type of parm's value is right*/
static bool mgr_parm_enum_lookup_by_name(char *value, StringInfo valuelist)
{
	char pvaluearray[256]="";		/*the max length of enumvals in pg_settings less than 256*/
	char *pvaluetmp;
	char *ptr;
	char *pvalue;
	char *pvaluespecial=",debug2,";	/*debug equals debug2*/
	bool ret = false;
	int ipos = 0;
	NameData valueinputdata;
	NameData valuecheckdata;
	
	Assert(value != NULL);
	Assert(valuelist->data != NULL);
	/*valuelist replaces double quota to single quota*/
	if (strstr(valuelist->data, "\"") != NULL)
	{
		while(ipos < valuelist->len)
		{
			if ('\"' == valuelist->data[ipos])
				valuelist->data[ipos] = '\'';
			ipos++;
		}
	}
	namestrcpy(&valueinputdata, value);
	mgr_string_add_single_quota(&valueinputdata);
	/*special handling, because "debug" equals "debug2"*/
	if (strcmp("'debug'", valueinputdata.data) == 0)
	{
		pvalue = strstr(valuelist->data, pvaluespecial);
		if (pvalue != NULL)
			return true;
	}
	/*the content of valuelist like this "{xx,xx,xx}", so it need copy from 1 to len -2*/
	if (valuelist->len > 2)
	{
		strncpy(pvaluearray, &(valuelist->data[1]), (valuelist->len-2) < 255 ? (valuelist->len-2):255);
		pvaluearray[(valuelist->len-2) < 255 ? (valuelist->len-2):255] = '\0';
		resetStringInfo(valuelist);
		appendStringInfoString(valuelist, pvaluearray);
	}
	ptr = strtok_r(pvaluearray, ",", &pvaluetmp); 
	while(ptr != NULL)
	{
		namestrcpy(&valuecheckdata, ptr);
		mgr_string_add_single_quota(&valuecheckdata);
		if (strcmp(valuecheckdata.data, valueinputdata.data) == 0)
		{
			ret = true;
			break;
		}
		ptr = strtok_r(NULL, ",", &pvaluetmp);
	}

	return ret;
}

/*delete the tuple for given nodename and nodetype*/
void mgr_parmr_delete_tuple_nodename_nodetype(Relation noderel, Name nodename, char nodetype)
{
	HeapTuple looptuple;
	ScanKeyData scankey[2];
	HeapScanDesc rel_scan;
	
	/*for nodename is MACRO_STAND_FOR_ALL_NODENAME, only when type if master then delete the tuple*/
	if (strcmp(MACRO_STAND_FOR_ALL_NODENAME, nodename->data) == 0)
	{
		if (CNDN_TYPE_COORDINATOR_MASTER == nodetype || CNDN_TYPE_DATANODE_MASTER == nodetype || GTM_TYPE_GTM_MASTER == nodetype)
		{
				if (GTM_TYPE_GTM_MASTER == nodetype || GTM_TYPE_GTM_SLAVE == nodetype || GTM_TYPE_GTM_EXTRA == nodetype)
					nodetype = CNDN_TYPE_GTM;
				else if (CNDN_TYPE_DATANODE_MASTER == nodetype || CNDN_TYPE_DATANODE_SLAVE == nodetype || CNDN_TYPE_DATANODE_EXTRA == nodetype)
					nodetype = CNDN_TYPE_DATANODE;
				else
					nodetype = CNDN_TYPE_COORDINATOR_MASTER;
		}
		else
			return;
	}
	ScanKeyInit(&scankey[0],
		Anum_mgr_updateparm_nodename
		,BTEqualStrategyNumber
		,F_NAMEEQ
		,NameGetDatum(nodename));
	ScanKeyInit(&scankey[1],
		Anum_mgr_updateparm_nodetype
		,BTEqualStrategyNumber
		,F_CHAREQ
		,CharGetDatum(nodetype));
	rel_scan = heap_beginscan(noderel, SnapshotNow, 2, scankey);
	while((looptuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		simple_heap_delete(noderel, &looptuple->t_self);
		CatalogUpdateIndexes(noderel, looptuple);
	}
	heap_endscan(rel_scan);
}

/*update the tuple for given nodename and nodetype*/
void mgr_parmr_update_tuple_nodename_nodetype(Relation noderel, Name nodename, char oldnodetype, char newnodetype)
{
	HeapTuple looptuple;
	HeapTuple newtuple;
	ScanKeyData scankey[2];
	HeapScanDesc rel_scan;
	TupleDesc tupledsc;
	Datum datum[Natts_mgr_updateparm];
	bool isnull[Natts_mgr_updateparm];
	bool got[Natts_mgr_updateparm];
	
	ScanKeyInit(&scankey[0],
		Anum_mgr_updateparm_nodename
		,BTEqualStrategyNumber
		,F_NAMEEQ
		,NameGetDatum(nodename));
	ScanKeyInit(&scankey[1],
		Anum_mgr_updateparm_nodetype
		,BTEqualStrategyNumber
		,F_CHAREQ
		,CharGetDatum(oldnodetype));
	rel_scan = heap_beginscan(noderel, SnapshotNow, 2, scankey);
	tupledsc = RelationGetDescr(noderel);
	while((looptuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		/*update parm's type*/
		memset(datum, 0, sizeof(datum));
		memset(isnull, 0, sizeof(isnull));
		memset(got, 0, sizeof(got));
		datum[Anum_mgr_updateparm_nodetype-1] = CharGetDatum(newnodetype);
		got[Anum_mgr_updateparm_nodetype-1] = true;
		newtuple = heap_modify_tuple(looptuple, tupledsc, datum,isnull, got);	
		simple_heap_update(noderel, &looptuple->t_self, newtuple);
		CatalogUpdateIndexes(noderel, newtuple);
	}
	heap_endscan(rel_scan);
}

/*update mgr_updateparm, change * to newmaster name and change its nodetype to mastertype*/
void mgr_update_parm_after_dn_failover(Name oldmastername, char oldmastertype, Name oldslavename,  char oldslavetype)
{
	Relation rel_updateparm;
	
	rel_updateparm = heap_open(UpdateparmRelationId, RowExclusiveLock);
	/*delete old master parameters*/	
	mgr_parmr_delete_tuple_nodename_nodetype(rel_updateparm, oldmastername, oldmastertype);
	/*update the old slave parameters to new master type*/
	mgr_parmr_update_tuple_nodename_nodetype(rel_updateparm, oldslavename, oldslavetype, oldmastertype);

	heap_close(rel_updateparm, RowExclusiveLock);
}

/*when gtm failover, the mgr_updateparm need modify: delete oldmaster parm and update slavetype to master for new master*/
void mgr_parm_after_gtm_failover_handle(Name mastername, char mastertype, Name slavename, char slavetype)
{
	Relation rel_updateparm;
	
	rel_updateparm = heap_open(UpdateparmRelationId, RowExclusiveLock);
	/*delete old master parameters*/	
	mgr_parmr_delete_tuple_nodename_nodetype(rel_updateparm, mastername, mastertype);
	/*update the old slave parameters to new master type*/
	mgr_parmr_update_tuple_nodename_nodetype(rel_updateparm, slavename, slavetype, mastertype);
	
	heap_close(rel_updateparm, RowExclusiveLock);
}

/*
* show parameter, command: SHOW NODENAME PARAMETER
*/
Datum mgr_show_var_param(PG_FUNCTION_ARGS)
{
	HeapTuple tuple;
	HeapTuple out;
	FuncCallContext *funcctx;
	InitNodeInfo *info;
	StringInfoData buf;
	StringInfoData infosendmsg;
	NameData nodename;
	NameData param;
	NameData nodetypedata;
	ScanKeyData key[2];
	Form_mgr_node mgr_node;
	GetAgentCmdRst getAgentCmdRst;
	char *nodetypestr;
	/*max port is 65535,so the length of portstr is 6*/
	char portstr[6]="00000";

	/*get node name and parameter name*/
	namestrcpy(&nodename, PG_GETARG_CSTRING(0));
	namestrcpy(&param, PG_GETARG_CSTRING(1));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		check_nodename_isvalid(nodename.data);
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		info = palloc(sizeof(*info));
		info->rel_node = heap_open(NodeRelationId, AccessShareLock);
		ScanKeyInit(&key[0]
			,Anum_mgr_node_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(nodename.data));
		ScanKeyInit(&key[1]
				,Anum_mgr_node_nodeincluster
				,BTEqualStrategyNumber
				,F_BOOLEQ
				,BoolGetDatum(true));
		info->rel_scan = heap_beginscan(info->rel_node, SnapshotNow, 2, key);
		info->lcp =NULL;

		/* save info */
		funcctx->user_fctx = info;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	Assert(funcctx);
	info = funcctx->user_fctx;
	Assert(info);
	initStringInfo(&buf);

	initStringInfo(&(getAgentCmdRst.description));
	/*find the tuple ,which the node name is equal nodename*/

	initStringInfo(&infosendmsg);
	while((tuple = heap_getnext(info->rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
		Assert(mgr_node);
		/*send the command string to agent to get the value*/
		sprintf(portstr, "%d", mgr_node->nodeport);
		resetStringInfo(&(getAgentCmdRst.description));
		resetStringInfo(&infosendmsg);
		appendStringInfo(&infosendmsg, "%s", portstr);
		appendStringInfoCharMacro(&infosendmsg, '\0');
		appendStringInfo(&infosendmsg, "%s", param.data);
		appendStringInfoCharMacro(&infosendmsg, '\0');
		if (GTM_TYPE_GTM_MASTER == mgr_node->nodetype || GTM_TYPE_GTM_SLAVE == mgr_node->nodetype || GTM_TYPE_GTM_EXTRA == mgr_node->nodetype)
			mgr_send_show_parameters(AGT_CMD_SHOW_AGTM_PARAM, &infosendmsg, mgr_node->nodehost, &getAgentCmdRst);
		else
			mgr_send_show_parameters(AGT_CMD_SHOW_CNDN_PARAM, &infosendmsg, mgr_node->nodehost, &getAgentCmdRst);

		nodetypestr = mgr_nodetype_str(mgr_node->nodetype);
		namestrcpy(&nodetypedata, nodetypestr);
		pfree(nodetypestr);
		out = build_common_command_tuple(&nodetypedata, getAgentCmdRst.ret, getAgentCmdRst.description.data);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(out));
	}

	heap_endscan(info->rel_scan);
	heap_close(info->rel_node, AccessShareLock);
	pfree(infosendmsg.data);
	pfree(getAgentCmdRst.description.data);

	SRF_RETURN_DONE(funcctx);
}

static void mgr_send_show_parameters(char cmdtype, StringInfo infosendmsg, Oid hostoid, GetAgentCmdRst *getAgentCmdRst)
{
	ManagerAgent *ma;
	StringInfoData sendstrmsg;
	StringInfoData buf;
	
	initStringInfo(&sendstrmsg);
	mgr_append_infostr_infostr(&sendstrmsg, infosendmsg);
	ma = ma_connect_hostoid(hostoid);
	if(!ma_isconnected(ma))
	{
		/* report error message */
		getAgentCmdRst->ret = false;
		appendStringInfoString(&(getAgentCmdRst->description), ma_last_error_msg(ma));
		ma_close(ma);
		return;
	}
	getAgentCmdRst->ret = false;
	ma_beginmessage(&buf, AGT_MSG_COMMAND);
	ma_sendbyte(&buf, cmdtype);
	mgr_append_infostr_infostr(&buf, &sendstrmsg);
	pfree(sendstrmsg.data);
	ma_endmessage(&buf, ma);
	if (! ma_flush(ma, true))
	{
		getAgentCmdRst->ret = false;
		appendStringInfoString(&(getAgentCmdRst->description), ma_last_error_msg(ma));
		ma_close(ma);
		return;
	}
	/*check the receive msg*/
	mgr_recv_showparam_msg(ma, getAgentCmdRst);
	ma_close(ma);	
}

/*
* get msg from agent
*/
static bool mgr_recv_showparam_msg(ManagerAgent	*ma, GetAgentCmdRst *getAgentCmdRst)
{
	char			msg_type;
	StringInfoData recvbuf;
	bool initdone = false;
	initStringInfo(&recvbuf);
	for(;;)
	{
		msg_type = ma_get_message(ma, &recvbuf);
		if(msg_type == AGT_MSG_IDLE)
		{
			/* message end */
			break;
		}else if(msg_type == '\0')
		{
			/* has an error */
			break;
		}else if(msg_type == AGT_MSG_ERROR)
		{
			/* error message */
			getAgentCmdRst->ret = false;
			appendStringInfoString(&(getAgentCmdRst->description), ma_get_err_info(&recvbuf, AGT_MSG_RESULT));
			ereport(LOG, (errmsg("receive msg: %s", ma_get_err_info(&recvbuf, AGT_MSG_RESULT))));
			break;
		}else if(msg_type == AGT_MSG_NOTICE)
		{
			/* ignore notice message */
			ereport(LOG, (errmsg("receive msg: %s", recvbuf.data)));
		}
		else if(msg_type == AGT_MSG_RESULT)
		{
			getAgentCmdRst->ret = true;
			appendStringInfoString(&(getAgentCmdRst->description), recvbuf.data);
			ereport(DEBUG1, (errmsg("receive msg: %s", recvbuf.data)));
			initdone = true;
			break;
		}
	}
	pfree(recvbuf.data);
	return initdone;
}

/*
* if the value is string and not using single quota, add single quota for it
*/
static void mgr_string_add_single_quota(Name value)
{
	int len;
	NameData valuetmp;

	/*if the value of key is string, it need use single quota*/
	len = strlen(value->data);
	if (0 == len)
	{
		namestrcpy(value, "''");
	}
	else if (value->data[0] != '\'' || value->data[len-1] != '\'')
	{
		valuetmp.data[0]='\'';
		strcpy(valuetmp.data+sizeof(char),value->data);
		valuetmp.data[1+len]='\'';
		valuetmp.data[2+len]='\0';
		if (len > sizeof(value->data)-2-1)
		{
			valuetmp.data[sizeof(value->data)-2]='\'';
			valuetmp.data[sizeof(value->data)-1]='\0';
		}
		namestrcpy(value, valuetmp.data);
	}
	else
	{
		/*do nothing*/
	}
}
