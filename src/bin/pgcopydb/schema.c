/*
 * src/bin/pgcopydb/schema.c
 *	 SQL queries to discover the source database schema
 */

#include <inttypes.h>
#include <limits.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "parson.h"

#include "postgres_fe.h"
#include "pqexpbuffer.h"

#include "defaults.h"
#include "env_utils.h"
#include "file_utils.h"
#include "filtering.h"
#include "log.h"
#include "parsing_utils.h"
#include "pg_depend_sql.h"
#include "pgsql.h"
#include "schema.h"
#include "signals.h"
#include "string_utils.h"


static bool prepareFilters(PGSQL *pgsql, SourceFilters *filters);

static bool prepareFilterCopyIncludeOnlySchema(PGSQL *pgsql,
											   SourceFilters *filters);

static bool prepareFilterCopyExcludeSchema(PGSQL *pgsql,
										   SourceFilters *filters);

static bool prepareFilterCopyTableList(PGSQL *pgsql,
									   SourceFilterTableList *tableList,
									   const char *temp_table_name);


/* Context used when fetching database definitions */
typedef struct SourceDatabaseArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceDatabaseArray *databaseArray;
	bool parsedOk;
} SourceDatabaseArrayContext;

/* Context used when fetching schema definitions */
typedef struct SourceSchemaArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceSchemaArray *schemaArray;
	bool parsedOk;
} SourceSchemaArrayContext;

/* Context used when fetching role definitions */
typedef struct SourceRoleArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceRoleArray *rolesArray;
	bool parsedOk;
} SourceRoleArrayContext;

/* Context used when fetching database properties */
typedef struct SourcePropertiesArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourcePropertiesArray *gucsArray;
	bool parsedOk;
} SourcePropertiesArrayContext;

/* Context used when fetching all the extension definitions */
typedef struct SourceExtensionArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceExtensionArray *extensionArray;
	bool parsedOk;
} SourceExtensionArrayContext;

/* Context used when fetching extension versions as a json array */
typedef struct ExtensionsVersionsArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	ExtensionsVersionsArray *evArray;
	bool parsedOk;
} ExtensionsVersionsArrayContext;

/* Context used when fetching collation definitions */
typedef struct SourceCollationArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceCollationArray *collationArray;
	bool parsedOk;
} SourceCollationArrayContext;

/* Context used when fetching all the table definitions */
typedef struct SourceTableArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceTableArray *tableArray;
	bool parsedOk;
} SourceTableArrayContext;

/* Context used when fetching all the sequence definitions */
typedef struct SourceSequenceArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceSequenceArray *sequenceArray;
	bool parsedOk;
} SourceSequenceArrayContext;

/* Context used when fetching all the indexes definitions */
typedef struct SourceIndexArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceIndexArray *indexArray;
	bool parsedOk;
} SourceIndexArrayContext;

/* Context used when fetching all the table dependencies */
typedef struct SourceDependArrayContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceDependArray *dependArray;
	bool parsedOk;
} SourceDependArrayContext;

/* Context used when fetching a list of COPY partitions for a table */
typedef struct SourcePartitionContext
{
	char sqlstate[SQLSTATE_LENGTH];
	SourceTable *table;
	bool parsedOk;
} SourcePartitionContext;

/* Context used when fetching a table's rowcount and checksum */
typedef struct ChecksumContext
{
	char sqlstate[SQLSTATE_LENGTH];
	TableChecksum *sum;
	bool parsedOk;
} ChecksumContext;


static void getSchemaList(void *ctx, PGresult *result);

static void getRoleList(void *ctx, PGresult *result);

static void getDatabaseList(void *ctx, PGresult *result);

static void getDatabaseProperties(void *ctx, PGresult *result);

static void getExtensionList(void *ctx, PGresult *result);

static bool parseCurrentExtension(PGresult *result,
								  int rowNumber,
								  SourceExtension *extension,
								  int *confIndex);

static bool parseCurrentExtensionConfig(PGresult *result,
										int rowNumber,
										SourceExtensionConfig *extConfig);

static void getExtensionsVersions(void *ctx, PGresult *result);

static void getCollationList(void *ctx, PGresult *result);

static void getTableArray(void *ctx, PGresult *result);

static bool parseCurrentSourceTable(PGresult *result,
									int rowNumber,
									SourceTable *table);

static bool parseAttributesArray(SourceTable *table, JSON_Value *json);

static void getSequenceArray(void *ctx, PGresult *result);

static bool parseCurrentSourceSequence(PGresult *result,
									   int rowNumber,
									   SourceSequence *table);

static void getIndexArray(void *ctx, PGresult *result);

static bool parseCurrentSourceIndex(PGresult *result,
									int rowNumber,
									SourceIndex *index);

static void getDependArray(void *ctx, PGresult *result);

static bool parseCurrentSourceDepend(PGresult *result,
									 int rowNumber,
									 SourceDepend *depend);

static void getPartitionList(void *ctx, PGresult *result);

static bool parseCurrentPartition(PGresult *result,
								  int rowNumber,
								  SourceTableParts *parts);

static void getTableChecksum(void *ctx, PGresult *result);

struct FilteringQueries
{
	SourceFilterType type;
	char *sql;
};


/*
 * schema_query_privileges queries the given database connection to figure out
 * if we can create a schema, and if we can create temporary objects.
 */
bool
schema_query_privileges(PGSQL *pgsql,
						bool *hasDBCreatePrivilage,
						bool *hasDBTempPrivilege)
{
	if (!pgsql_has_database_privilege(pgsql, "create", hasDBCreatePrivilage))
	{
		/* errors have already been logged */
		return false;
	}

	if (!pgsql_has_database_privilege(pgsql, "temp", hasDBTempPrivilege))
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * schema_list_databases grabs the list of databases from the given source
 * Postgres instance and allocates a SourceDatabase array with the result of
 * the query.
 */
bool
schema_list_databases(PGSQL *pgsql, SourceDatabaseArray *catArray)
{
	SourceDatabaseArrayContext parseContext = { { 0 }, catArray, false };

	char *sql =
		"select d.oid, datname, pg_database_size(d.oid) as bytes, "
		"       pg_size_pretty(pg_database_size(d.oid)) "
		"  from pg_database d "
		" where datname not in ('template0', 'template1') "
		"order by datname";

	if (!pgsql_execute_with_params(pgsql, sql,
								   0, NULL, NULL,
								   &parseContext, &getDatabaseList))
	{
		log_error("Failed to list databases");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list databases");
		return false;
	}

	return true;
}


/*
 * schema_list_database_properties grabs the list of GUC settings attached to a
 * given database with either ALTER DATABASE SET or ALTER ROLE IN DATABASE SET
 * commands.
 */
bool
schema_list_database_properties(PGSQL *pgsql, SourcePropertiesArray *gucsArray)
{
	SourcePropertiesArrayContext parseContext = { { 0 }, gucsArray, false };

	char *sql =
		"select d.datname, NULL as rolname, "
		"       unnest(rs.setconfig) as setconfig "
		"  from pg_db_role_setting rs "
		"       join pg_database d on d.oid = rs.setdatabase "
		" where d.datname = current_database() "
		"   and setrole = 0 "

		"union all "

		"select d.datname, format('%I', rolname) as rolname, "
		"       unnest(rs.setconfig)  as setconfig "
		"  from pg_db_role_setting rs "
		"       join pg_database d on d.oid = rs.setdatabase "
		"       join pg_roles r on r.oid = rs.setrole "
		" where d.datname = current_database()";

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &parseContext, &getDatabaseProperties))
	{
		log_error("Failed to list databases properties");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list databases properties");
		return false;
	}

	return true;
}


/*
 * schema_list_schemas grabs the list of schema from the given Postgres
 * instance and allocates a SourceSchemaArray array with the result of the
 * query.
 */
bool
schema_list_schemas(PGSQL *pgsql, SourceSchemaArray *array)
{
	SourceSchemaArrayContext parseContext = { { 0 }, array, false };

	char *sql =
		"select n.oid, n.nspname, "
		"       format('- %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"  from pg_namespace n "
		"       join pg_roles auth ON auth.oid = n.nspowner "
		" where nspname <> 'information_schema' and nspname !~ '^pg_'";

	if (!pgsql_execute_with_params(pgsql, sql,
								   0, NULL, NULL,
								   &parseContext, &getSchemaList))
	{
		log_error("Failed to list schemas that extensions depend on");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list schemas that extensions depend on");
		return false;
	}

	return true;
}


/*
 * schema_list_roles grabs the list of role from the given Postgres
 * instance and allocates a SourceRoleArray array with the result of the
 * query.
 */
bool
schema_list_roles(PGSQL *pgsql, SourceRoleArray *rolesArray)
{
	SourceRoleArrayContext parseContext = { { 0 }, rolesArray, false };

	char *sql =
		"select oid, format('%I', rolname) as rolname from pg_roles";

	if (!pgsql_execute_with_params(pgsql, sql,
								   0, NULL, NULL,
								   &parseContext, &getRoleList))
	{
		log_error("Failed to list roles");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list roles");
		return false;
	}

	return true;
}


/*
 * schema_list_extensions grabs the list of extensions from the given source
 * Postgres instance and allocates a SourceExtension array with the result of
 * the query.
 */
bool
schema_list_extensions(PGSQL *pgsql, SourceExtensionArray *extArray)
{
	SourceExtensionArrayContext parseContext = { { 0 }, extArray, false };

	char *sql =
		"select e.oid, extname, extnamespace::regnamespace, extrelocatable, "
		"       0 as count, null as n, "
		"       null as extconfig, null as nspname, null as relname, "
		"       null as extcondition "
		"  from pg_extension e "
		" where extconfig is null "

		" UNION ALL "

		"  select e.oid, extname, extnamespace::regnamespace, extrelocatable, "
		"         array_length(e.extconfig, 1) as count, "
		"         extconfig.n, "
		"         extconfig.extconfig, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         extcondition[extconfig.n] "
		"    from pg_extension e, "
		"         unnest(extconfig) with ordinality as extconfig(extconfig, n) "
		"          left join pg_class c on c.oid = extconfig.extconfig "
		"          join pg_namespace n on c.relnamespace = n.oid "
		"   where extconfig.extconfig is not null "

		"order by oid, n";

	if (!pgsql_execute_with_params(pgsql, sql,
								   0, NULL, NULL,
								   &parseContext, &getExtensionList))
	{
		log_error("Failed to list extensions");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list extensions");
		return false;
	}

	return true;
}


/*
 * schema_list_ext_schemas grabs the list of schema that extensions depend on
 * from the given source Postgres instance and allocates a SourceSchemaArray
 * array with the result of the query.
 */
bool
schema_list_ext_schemas(PGSQL *pgsql, SourceSchemaArray *array)
{
	SourceSchemaArrayContext parseContext = { { 0 }, array, false };

	char *sql =
		"select n.oid, n.nspname, "
		"       format('- %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"  from pg_namespace n "
		"       join pg_roles auth ON auth.oid = n.nspowner "
		"       join pg_depend d "
		"         on d.refclassid = 'pg_namespace'::regclass "
		"        and d.refobjid = n.oid "
		"        and d.classid = 'pg_extension'::regclass "
		" where nspname <> 'public' and nspname !~ '^pg_'";

	if (!pgsql_execute_with_params(pgsql, sql,
								   0, NULL, NULL,
								   &parseContext, &getSchemaList))
	{
		log_error("Failed to list schemas that extensions depend on");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list schemas that extensions depend on");
		return false;
	}

	return true;
}


/*
 * schema_list_ext_versions lists available extensions versions.
 */
bool
schema_list_ext_versions(PGSQL *pgsql, ExtensionsVersionsArray *array)
{
	ExtensionsVersionsArrayContext parseContext = { { 0 }, array, false };

	char *sql =
		"select e.name, e.default_version, e.installed_version, "
		"       u.versions "

		"from pg_available_extensions e "
		"     left join lateral "
		"     ( "
		"       with updates as "
		"       ( "
		"         select source, "
		"                array_length(regexp_split_to_array(path, '--'), 1) as steps "
		"           from pg_extension_update_paths(e.name) "
		"          where (   target = e.default_version "
		"                 or source = e.default_version) "
		"           and source not in ('unpackaged', 'ANY') "
		"           and path is not null "

		"     union all "

		"        select e.default_version, 0"

		"      order by steps, source desc "
		"       ) "
		"       select coalesce(jsonb_agg(source), "
		"                       jsonb_build_array(e.default_version)) "
		"       from updates "
		"     ) "
		"     as u(versions) on true "

		"group by e.name, e.default_version, e.installed_version, u.versions "
		"order by e.name";

	if (!pgsql_execute_with_params(pgsql, sql,
								   0, NULL, NULL,
								   &parseContext, &getExtensionsVersions))
	{
		log_error("Failed to list available extensions versions");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list available extensions versions");
		return false;
	}

	return true;
}


/*
 * schema_list_collations grabs the list of collations used in the given
 * database connection. Collations listed may be used in the database
 * definition itself, in a column in any table in that database, or in an index
 * definition.
 */
bool
schema_list_collations(PGSQL *pgsql, SourceCollationArray *array)
{
	SourceCollationArrayContext parseContext = { { 0 }, array, false };

	char *sql =
		"with indcols as "
		" ( "
		"   select indexrelid, n, colloid "
		"     from pg_index i "
		"     join pg_class c on c.oid = i.indexrelid "
		"     join pg_namespace n on n.oid = c.relnamespace, "
		"          unnest(indcollation) with ordinality as t (colloid, n) "
		"    where n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		" ) "
		"select colloid, collname, "
		"       pg_describe_object('pg_class'::regclass, indexrelid, 0), "
		"       format('%s %s %s', "
		"              regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"              regexp_replace(c.collname, '[\\n\\r]', ' '), "
		"              regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"  from indcols "
		"       join pg_collation c on c.oid = colloid "
		"       join pg_roles auth ON auth.oid = c.collowner "
		"       join pg_namespace n on n.oid = c.collnamespace "
		" where colloid <> 0 "
		"   and collname <> 'default' "

		"union "

		"select c.oid as colloid, c.collname, "
		"       format('database %s', d.datname) as desc, "
		"       format('%s %s %s', "
		"              regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"              regexp_replace(c.collname, '[\\n\\r]', ' '), "
		"              regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"  from pg_database d "
		"       join pg_collation c on c.collname = d.datcollate "
		"       join pg_roles auth ON auth.oid = c.collowner "
		"       join pg_namespace n on n.oid = c.collnamespace "
		" where d.datname = current_database() "

		"union "

		"select coll.oid as colloid, coll.collname, "
		"       pg_describe_object('pg_class'::regclass, attrelid, attnum), "
		"       format('%s %s %s', "
		"              regexp_replace(cn.nspname, '[\\n\\r]', ' '), "
		"              regexp_replace(coll.collname, '[\\n\\r]', ' '), "
		"              regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"  from pg_attribute a "
		"       join pg_class c on c.oid = a.attrelid "
		"       join pg_namespace n on n.oid = c.relnamespace "
		"       join pg_collation coll on coll.oid = attcollation "
		"       join pg_roles auth ON auth.oid = coll.collowner "
		"       join pg_namespace cn on cn.oid = coll.collnamespace "
		" where collname <> 'default' "
		"   and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"order by colloid";

	if (!pgsql_execute_with_params(pgsql, sql,
								   0, NULL, NULL,
								   &parseContext, &getCollationList))
	{
		log_error("Failed to list non-default collations in use in database");
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list non-default collations in use in database");
		return false;
	}

	return true;
}


/*
 * For code simplicity the index array is also the SourceFilterType enum value.
 */
struct FilteringQueries listSourceTableSizeSQL[] = {
	{
		SOURCE_FILTER_TYPE_NONE,

		"  select c.oid, pg_table_size(c.oid) as bytes "
		"    from pg_catalog.pg_class c"
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid"

		"   where relkind = 'r' and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "
	},

	{
		SOURCE_FILTER_TYPE_INCL,

		"  select c.oid, pg_table_size(c.oid) as bytes "
		"    from pg_catalog.pg_class c"
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid"

		/* include-only-table */
		"         join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and c.relname = inc.relname "

		"   where relkind = 'r' and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "
	},

	{
		SOURCE_FILTER_TYPE_EXCL,

		"  select c.oid, pg_table_size(c.oid) as bytes "
		"    from pg_catalog.pg_class c"
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on n.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on n.nspname = ft.nspname "
		"               and c.relname = ft.relname "

		/* exclude-table-data */
		"         left join pg_temp.filter_exclude_table_data ftd "
		"                on n.nspname = ftd.nspname "
		"               and c.relname = ftd.relname "

		"   where relkind = 'r' and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "

		/* WHERE clause for exclusion filters */
		"     and fn.nspname is null "
		"     and ft.relname is null "
		"     and ftd.relname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "
	},

	{
		SOURCE_FILTER_TYPE_LIST_NOT_INCL,

		"  select c.oid, pg_table_size(c.oid) as bytes "
		"    from pg_catalog.pg_class c"
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid"

		/* include-only-table */
		"    left join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and c.relname = inc.relname "

		"   where relkind in ('r', 'p') and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "

		/* WHERE clause for exclusion filters */
		"     and inc.nspname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "
	},

	{
		SOURCE_FILTER_TYPE_LIST_EXCL,

		"  select c.oid, pg_table_size(c.oid) as bytes "
		"    from pg_catalog.pg_class c"
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on n.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on n.nspname = ft.nspname "
		"               and c.relname = ft.relname "

		/* WHERE clause for exclusion filters */
		"     and (   fn.nspname is not null "
		"          or ft.relname is not null ) "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "
	}
};


/*
 * schema_prepare_pgcopydb_table_size creates a table named pgcopydb_table_size
 * on the given connection (typically, the source database). The creation is
 * skipped if the table already exists.
 */
bool
schema_prepare_pgcopydb_table_size(PGSQL *pgsql,
								   SourceFilters *filters,
								   bool hasDBCreatePrivilege,
								   bool cache,
								   bool dropCache,
								   bool *createdTableSizeTable)
{
	log_trace("schema_prepare_pgcopydb_table_size");

	switch (filters->type)
	{
		case SOURCE_FILTER_TYPE_NONE:
		{
			/* skip filters preparing (temp tables) */
			break;
		}

		case SOURCE_FILTER_TYPE_INCL:
		case SOURCE_FILTER_TYPE_EXCL:
		case SOURCE_FILTER_TYPE_LIST_NOT_INCL:
		case SOURCE_FILTER_TYPE_LIST_EXCL:
		{
			if (!prepareFilters(pgsql, filters))
			{
				log_error("Failed to prepare pgcopydb filters, "
						  "see above for details");
				return false;
			}
			break;
		}

		/* SOURCE_FILTER_TYPE_EXCL_INDEX etc */
		default:
		{
			log_error("BUG: schema_list_ordinary_tables called with "
					  "filtering type %d",
					  filters->type);
			return false;
		}
	}

	if ((cache || dropCache) && !hasDBCreatePrivilege)
	{
		log_fatal("Connecting with a role that does not have CREATE privileges "
				  "on the source database prevents pg_table_size() caching");
		return false;
	}

	/*
	 * See if a pgcopydb.pgcopydb_table_size table already exists.
	 */
	bool exists = false;

	if (dropCache)
	{
		if (!schema_drop_pgcopydb_table_size(pgsql))
		{
			/* errors have already been logged */
			return false;
		}
	}
	else
	{
		if (!pgsql_table_exists(pgsql, "pgcopydb", "pgcopydb_table_size", &exists))
		{
			/* errors have already been logged */
			return false;
		}

		if (exists)
		{
			log_notice("Table pgcopydb.pgcopydb_table_size already exists, "
					   "re-using it");
			return true;
		}
	}

	/*
	 * Now the table does not exists, and we have to decide if we want to make
	 * it a persitent table in the possibly new schema "pgcopydb" (cache ==
	 * true), or a temporary table (cache == false).
	 */
	if (cache)
	{
		char *createSchema = "create schema if not exists pgcopydb";

		if (!pgsql_execute(pgsql, createSchema))
		{
			log_error("Failed to compute table size, see above for details");
			return false;
		}
	}

	char *tablename = "pgcopydb_table_size";
	PQExpBuffer sql = createPQExpBuffer();

	if (cache)
	{
		appendPQExpBuffer(sql,
						  "create table if not exists pgcopydb.%s as %s",
						  tablename,
						  listSourceTableSizeSQL[filters->type].sql);
	}
	else
	{
		appendPQExpBuffer(sql,
						  "create temp table %s  on commit drop as %s",
						  tablename,
						  listSourceTableSizeSQL[filters->type].sql);
	}

	if (PQExpBufferBroken(sql))
	{
		log_error("Failed to prepare create pgcopydb_table_size query "
				  "buffer: Out of Memory");
		(void) destroyPQExpBuffer(sql);
		return false;
	}

	if (!pgsql_execute(pgsql, sql->data))
	{
		log_error("Failed to compute table size, see above for details");
		(void) destroyPQExpBuffer(sql);
		return false;
	}

	(void) destroyPQExpBuffer(sql);

	char *createIndex = "create index on pgcopydb_table_size(oid)";

	if (!pgsql_execute(pgsql, createIndex))
	{
		log_error("Failed to compute table size, see above for details");
		return false;
	}

	/* we only consider that we created the cache when cache is true */
	*createdTableSizeTable = cache;

	return true;
}


/*
 * schema_drop_pgcopydb_table_size drops the pgcopydb.pgcopydb_table_size
 * table.
 */
bool
schema_drop_pgcopydb_table_size(PGSQL *pgsql)
{
	char *sql = "drop table if exists pgcopydb.pgcopydb_table_size cascade";

	if (!pgsql_execute(pgsql, sql))
	{
		log_error("Failed to compute table size, see above for details");
		return false;
	}

	return true;
}


/*
 * For code simplicity the index array is also the SourceFilterType enum value.
 */
struct FilteringQueries listSourceTablesSQL[] = {
	{
		SOURCE_FILTER_TYPE_NONE,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         false as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(c.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')), "
		"         case when pkeys.attname is not null "
		"              then format('%I', pkeys.attname) "
		"               end as partkey, "
		"         attrs.js as attributes "

		"    from pg_catalog.pg_class c"
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid"
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = c.relowner"
		"         join lateral ( "
		"               with atts as "
		"               ("
		"                  select attnum, atttypid::integer, "
		"                         format('%I', attname) as attname, "
		"                         i.indrelid is not null as attisprimary, "
		"						  col.is_generated = 'ALWAYS' as attisgenerated "
		"                    from pg_attribute a "
		"                         left join pg_index i "
		"                                on i.indrelid = a.attrelid "
		"                               and a.attnum = ANY(i.indkey) "
		"                               and i.indisprimary "
		"					 	  left join information_schema.columns col "
        "                    			on col.column_name = a.attname "
		"					  			and col.table_name = c.relname "
		"          			 			and col.table_schema = n.nspname "
		"                   where a.attrelid = c.oid and not a.attisdropped "
		"                     and a.attnum > 0 "
		"                order by attnum "
		"               ) "
		"               select json_agg(row_to_json(atts)) as js "
		"                from atts "
		"              ) as attrs on true"
		"         left join pgcopydb_table_size ts on ts.oid = c.oid"

		/* find a copy partition key candidate */
		"         left join lateral ("
		"             select indrelid, indexrelid, a.attname"

		"               from pg_index x"
		"               join pg_class i on i.oid = x.indexrelid"
		"               join pg_attribute a "
		"                 on a.attrelid = c.oid and attnum = indkey[0]"

		"              where x.indrelid = c.oid"
		"                and (indisprimary or indisunique)"
		"                and array_length(indkey::integer[], 1) = 1"
		"                and atttypid in ('smallint'::regtype,"
		"                                 'int'::regtype,"
		"                                 'bigint'::regtype)"
		"           order by not indisprimary, not indisunique"
		"              limit 1"
		"         ) as pkeys on true"

		"   where relkind = 'r' and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and n.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by bytes desc, n.nspname, c.relname"
	},

	{
		SOURCE_FILTER_TYPE_INCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         exists(select 1 "
		"                  from pg_temp.filter_exclude_table_data ftd "
		"                 where n.nspname = ftd.nspname "
		"                   and c.relname = ftd.relname) as excludedata,"
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(c.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')), "
		"         case when pkeys.attname is not null "
		"              then format('%I', pkeys.attname) "
		"               end as partkey, "
		"         attrs.js as attributes "

		"    from pg_catalog.pg_class c "
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = c.relowner"
		"         join lateral ( "
		"               with atts as "
		"               ("
		"                  select attnum, atttypid::integer, "
		"                         format('%I', attname) as attname, "
		"                         i.indrelid is not null as attisprimary, "
		"						  col.is_generated = 'ALWAYS' as attisgenerated "
		"                    from pg_attribute a "
		"                         left join pg_index i "
		"                                on i.indrelid = a.attrelid "
		"                               and a.attnum = ANY(i.indkey) "
		"                               and i.indisprimary "
		"					 	  left join information_schema.columns col "
        "                    			on col.column_name = a.attname "
		"					  			and col.table_name = c.relname "
		"          			 			and col.table_schema = n.nspname "	
		"                   where a.attrelid = c.oid and not a.attisdropped "
		"                     and a.attnum > 0 "
		"                order by attnum "
		"               ) "
		"               select json_agg(row_to_json(atts)) as js "
		"                from atts "
		"              ) as attrs on true"
		"         left join pgcopydb_table_size ts on ts.oid = c.oid"

		/* include-only-table */
		"         join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and c.relname = inc.relname "

		/* find a copy partition key candidate */
		"         left join lateral ("
		"             select indrelid, indexrelid, a.attname"

		"               from pg_index x"
		"               join pg_class i on i.oid = x.indexrelid"
		"               join pg_attribute a "
		"                 on a.attrelid = c.oid and attnum = indkey[0]"

		"              where x.indrelid = c.oid"
		"                and (indisprimary or indisunique)"
		"                and array_length(indkey::integer[], 1) = 1"
		"                and atttypid in ('smallint'::regtype,"
		"                                 'int'::regtype,"
		"                                 'bigint'::regtype)"
		"           order by not indisprimary, not indisunique"
		"              limit 1"
		"         ) as pkeys on true"

		"   where relkind = 'r' and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and n.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by bytes desc, n.nspname, c.relname"
	},

	{
		SOURCE_FILTER_TYPE_EXCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         ftd.relname is not null as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(c.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')), "
		"         case when pkeys.attname is not null "
		"              then format('%I', pkeys.attname) "
		"               end as partkey, "
		"         attrs.js as attributes "

		"    from pg_catalog.pg_class c "
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = c.relowner"
		"         join lateral ( "
		"               with atts as "
		"               ("
		"                  select attnum, atttypid::integer, "
		"                         format('%I', attname) as attname, "
		"                         i.indrelid is not null as attisprimary, "
		"						  col.is_generated = 'ALWAYS' as attisgenerated "
		"                    from pg_attribute a "
		"                         left join pg_index i "
		"                                on i.indrelid = a.attrelid "
		"                               and a.attnum = ANY(i.indkey) "
		"                               and i.indisprimary "
		"					 	  left join information_schema.columns col "
        "                    			on col.column_name = a.attname "
		"					  			and col.table_name = c.relname "
		"          			 			and col.table_schema = n.nspname "
		"                   where a.attrelid = c.oid and not a.attisdropped "
		"                     and a.attnum > 0 "
		"                order by attnum "
		"               ) "
		"               select json_agg(row_to_json(atts)) as js "
		"                from atts "
		"              ) as attrs on true"
		"         left join pgcopydb_table_size ts on ts.oid = c.oid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on n.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on n.nspname = ft.nspname "
		"               and c.relname = ft.relname "

		/* exclude-table-data */
		"         left join pg_temp.filter_exclude_table_data ftd "
		"                on n.nspname = ftd.nspname "
		"               and c.relname = ftd.relname "

		/* find a copy partition key candidate */
		"         left join lateral ("
		"             select indrelid, indexrelid, a.attname"

		"               from pg_index x"
		"               join pg_class i on i.oid = x.indexrelid"
		"               join pg_attribute a "
		"                 on a.attrelid = c.oid and attnum = indkey[0]"

		"              where x.indrelid = c.oid"
		"                and (indisprimary or indisunique)"
		"                and array_length(indkey::integer[], 1) = 1"
		"                and atttypid in ('smallint'::regtype,"
		"                                 'int'::regtype,"
		"                                 'bigint'::regtype)"
		"           order by not indisprimary, not indisunique"
		"              limit 1"
		"         ) as pkeys on true"

		"   where relkind in ('r', 'p') and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and n.nspname !~ 'pgcopydb' "

		/* WHERE clause for exclusion filters */
		"     and fn.nspname is null "
		"     and ft.relname is null "
		"     and ftd.relname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by bytes desc, n.nspname, c.relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_NOT_INCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         false as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(c.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')), "
		"         case when pkeys.attname is not null "
		"              then format('%I', pkeys.attname) "
		"               end as partkey, "
		"         attrs.js as attributes "

		"    from pg_catalog.pg_class c "
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = c.relowner"
		"         join lateral ( "
		"               with atts as "
		"               ("
		"                  select attnum, atttypid::integer, "
		"                         format('%I', attname) as attname, "
		"                         i.indrelid is not null as attisprimary, "
		"						  col.is_generated = 'ALWAYS' as attisgenerated "
		"                    from pg_attribute a "
		"                         left join pg_index i "
		"                                on i.indrelid = a.attrelid "
		"                               and a.attnum = ANY(i.indkey) "
		"                               and i.indisprimary "
		"					 	  left join information_schema.columns col "
        "                    			on col.column_name = a.attname "
		"					  			and col.table_name = c.relname "
		"          			 			and col.table_schema = n.nspname "	
		"                   where a.attrelid = c.oid and not a.attisdropped "
		"                     and a.attnum > 0 "
		"                order by attnum "
		"               ) "
		"               select json_agg(row_to_json(atts)) as js "
		"                from atts "
		"              ) as attrs on true"
		"         left join pgcopydb_table_size ts on ts.oid = c.oid"

		/* include-only-table */
		"    left join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and c.relname = inc.relname "

		/* find a copy partition key candidate */
		"         left join lateral ("
		"             select indrelid, indexrelid, a.attname"

		"               from pg_index x"
		"               join pg_class i on i.oid = x.indexrelid"
		"               join pg_attribute a "
		"                 on a.attrelid = c.oid and attnum = indkey[0]"

		"              where x.indrelid = c.oid"
		"                and (indisprimary or indisunique)"
		"                and array_length(indkey::integer[], 1) = 1"
		"                and atttypid in ('smallint'::regtype,"
		"                                 'int'::regtype,"
		"                                 'bigint'::regtype)"
		"           order by not indisprimary, not indisunique"
		"              limit 1"
		"         ) as pkeys on true"

		"   where relkind in ('r', 'p') and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and n.nspname !~ 'pgcopydb' "

		/* WHERE clause for exclusion filters */
		"     and inc.nspname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by bytes desc, n.nspname, c.relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_EXCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         false as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(c.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')), "
		"         case when pkeys.attname is not null "
		"              then format('%I', pkeys.attname) "
		"               end as partkey, "
		"         attrs.js as attributes "

		"    from pg_catalog.pg_class c "
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = c.relowner"
		"         join lateral ( "
		"               with atts as "
		"               ("
		"                  select attnum, atttypid::integer, "
		"                         format('%I', attname) as attname, "
		"                         i.indrelid is not null as attisprimary, "
		"						  col.is_generated = 'ALWAYS' as attisgenerated "
		"                    from pg_attribute a "
		"                         left join pg_index i "
		"                                on i.indrelid = a.attrelid "
		"                               and a.attnum = ANY(i.indkey) "
		"                               and i.indisprimary "
		"					 	  left join information_schema.columns col "
        "                    			on col.column_name = a.attname "
		"					  			and col.table_name = c.relname "
		"          			 			and col.table_schema = n.nspname "	
		"                   where a.attrelid = c.oid and not a.attisdropped "
		"                     and a.attnum > 0 "
		"                order by attnum "
		"               ) "
		"               select json_agg(row_to_json(atts)) as js "
		"                from atts "
		"              ) as attrs on true"
		"         left join pgcopydb_table_size ts on ts.oid = c.oid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on n.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on n.nspname = ft.nspname "
		"               and c.relname = ft.relname "

		/* find a copy partition key candidate */
		"         left join lateral ("
		"             select indrelid, indexrelid, a.attname"

		"               from pg_index x"
		"               join pg_class i on i.oid = x.indexrelid"
		"               join pg_attribute a "
		"                 on a.attrelid = c.oid and attnum = indkey[0]"

		"              where x.indrelid = c.oid"
		"                and (indisprimary or indisunique)"
		"                and array_length(indkey::integer[], 1) = 1"
		"                and atttypid in ('smallint'::regtype,"
		"                                 'int'::regtype,"
		"                                 'bigint'::regtype)"
		"           order by not indisprimary, not indisunique"
		"              limit 1"
		"         ) as pkeys on true"

		"   where relkind in ('r', 'p') and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and n.nspname !~ 'pgcopydb' "

		/* WHERE clause for exclusion filters */
		"     and (   fn.nspname is not null "
		"          or ft.relname is not null ) "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by bytes desc, n.nspname, c.relname"
	}
};


/*
 * schema_list_ordinary_tables grabs the list of tables from the given source
 * Postgres instance and allocates a SourceTable array with the result of the
 * query.
 */
bool
schema_list_ordinary_tables(PGSQL *pgsql,
							SourceFilters *filters,
							SourceTableArray *tableArray)
{
	SourceTableArrayContext context = { { 0 }, tableArray, false };

	log_trace("schema_list_ordinary_tables");

	switch (filters->type)
	{
		case SOURCE_FILTER_TYPE_NONE:
		{
			/* skip filters preparing (temp tables) */
			break;
		}

		case SOURCE_FILTER_TYPE_INCL:
		case SOURCE_FILTER_TYPE_EXCL:
		case SOURCE_FILTER_TYPE_LIST_NOT_INCL:
		case SOURCE_FILTER_TYPE_LIST_EXCL:
		{
			if (!prepareFilters(pgsql, filters))
			{
				log_error("Failed to prepare pgcopydb filters, "
						  "see above for details");
				return false;
			}
			break;
		}

		/* SOURCE_FILTER_TYPE_EXCL_INDEX etc */
		default:
		{
			log_error("BUG: schema_list_ordinary_tables called with "
					  "filtering type %d",
					  filters->type);
			return false;
		}
	}

	log_debug("listSourceTablesSQL[%s]", filterTypeToString(filters->type));

	char *sql = listSourceTablesSQL[filters->type].sql;

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &getTableArray))
	{
		log_error("Failed to list tables");
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to list tables");
		return false;
	}

	return true;
}


/*
 * For code simplicity the index array is also the SourceFilterType enum value.
 */
struct FilteringQueries listSourceTablesNoPKSQL[] = {
	{
		SOURCE_FILTER_TYPE_NONE,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         false as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(r.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')),"
		"         NULL as partkey,"
		"         NULL as attributes"

		"    from pg_class r "
		"         join pg_namespace n ON n.oid = r.relnamespace "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = r.relowner"
		"         left join pgcopydb_table_size ts on ts.oid = r.oid"

		"   where r.relkind = 'r' and r.relpersistence in ('p', 'u')  "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and not exists "
		"         ( "
		"           select c.oid "
		"             from pg_constraint c "
		"            where c.conrelid = r.oid "
		"              and c.contype = 'p' "
		"         ) "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by n.nspname, r.relname"
	},

	{
		SOURCE_FILTER_TYPE_INCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         false as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(r.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')),"
		"         NULL as partkey,"
		"         NULL as attributes"

		"    from pg_class r "
		"         join pg_namespace n ON n.oid = r.relnamespace "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = r.relowner"
		"         left join pgcopydb_table_size ts on ts.oid = r.oid"

		/* include-only-table */
		"         join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and r.relname = inc.relname "

		"   where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and not exists "
		"         ( "
		"           select c.oid "
		"             from pg_constraint c "
		"            where c.conrelid = r.oid "
		"              and c.contype = 'p' "
		"         ) "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by n.nspname, r.relname"
	},

	{
		SOURCE_FILTER_TYPE_EXCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         ftd.relname is not null as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(r.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')),"
		"         NULL as partkey,"
		"         NULL as attributes"

		"    from pg_class r "
		"         join pg_namespace n ON n.oid = r.relnamespace "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = r.relowner"
		"         left join pgcopydb_table_size ts on ts.oid = r.oid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on n.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on n.nspname = ft.nspname "
		"               and r.relname = ft.relname "

		/* exclude-table-data */
		"         left join pg_temp.filter_exclude_table_data ftd "
		"                on n.nspname = ftd.nspname "
		"               and r.relname = ftd.relname "

		"   where r.relkind = 'r' and r.relpersistence in ('p', 'u')  "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and not exists "
		"         ( "
		"           select c.oid "
		"             from pg_constraint c "
		"            where c.conrelid = r.oid "
		"              and c.contype = 'p' "
		"         ) "

		/* WHERE clause for exclusion filters */
		"     and fn.nspname is null "
		"     and ft.relname is null "
		"     and ftd.relname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by n.nspname, r.relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_NOT_INCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         false as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(r.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')),"
		"         NULL as partkey,"
		"         NULL as attributes"

		"    from pg_class r "
		"         join pg_namespace n ON n.oid = r.relnamespace "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = r.relowner"
		"         left join pgcopydb_table_size ts on ts.oid = r.oid"

		/* include-only-table */
		"    left join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and r.relname = inc.relname "

		"   where r.relkind = 'r' and r.relpersistence in ('p', 'u')  "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and not exists "
		"         ( "
		"           select c.oid "
		"             from pg_constraint c "
		"            where c.conrelid = r.oid "
		"              and c.contype = 'p' "
		"         ) "

		/* WHERE clause for exclusion filters */
		"     and inc.nspname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by n.nspname, r.relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_EXCL,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         pg_am.amname, "
		"         c.relpages, c.reltuples::bigint, "
		"         ts.bytes as bytes, "
		"         pg_size_pretty(ts.bytes), "
		"         false as excludedata, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(r.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')),"
		"         NULL as partkey,"
		"         NULL as attributes"

		"    from pg_class r "
		"         join pg_namespace n ON n.oid = r.relnamespace "
		"         left join pg_catalog.pg_am on c.relam = pg_am.oid"
		"         join pg_roles auth ON auth.oid = r.relowner"
		"         left join pgcopydb_table_size ts on ts.oid = r.oid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on n.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on n.nspname = ft.nspname "
		"               and r.relname = ft.relname "

		"   where r.relkind = 'r' and r.relpersistence in ('p', 'u')  "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and not exists "
		"         ( "
		"           select c.oid "
		"             from pg_constraint c "
		"            where c.conrelid = r.oid "
		"              and c.contype = 'p' "
		"         ) "

		/* WHERE clause for exclusion filters */
		"     and (   fn.nspname is not null "
		"          or ft.relname is not null ) "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by n.nspname, r.relname"
	}
};

/*
 * schema_list_ordinary_tables_without_pk lists all tables that do not have a
 * primary key. This is useful to prepare a migration when some kind of change
 * data capture technique is considered.
 */
bool
schema_list_ordinary_tables_without_pk(PGSQL *pgsql,
									   SourceFilters *filters,
									   SourceTableArray *tableArray)
{
	SourceTableArrayContext context = { { 0 }, tableArray, false };

	log_trace("schema_list_ordinary_tables_without_pk");

	switch (filters->type)
	{
		case SOURCE_FILTER_TYPE_NONE:
		{
			/* skip filters preparing (temp tables) */
			break;
		}

		case SOURCE_FILTER_TYPE_INCL:
		case SOURCE_FILTER_TYPE_EXCL:
		case SOURCE_FILTER_TYPE_LIST_NOT_INCL:
		case SOURCE_FILTER_TYPE_LIST_EXCL:
		{
			if (!prepareFilters(pgsql, filters))
			{
				log_error("Failed to prepare pgcopydb filters, "
						  "see above for details");
				return false;
			}
			break;
		}

		/* SOURCE_FILTER_TYPE_EXCL_INDEX etc */
		default:
		{
			log_error("BUG: schema_list_ordinary_tables_without_pk called with "
					  "filtering type %d",
					  filters->type);
			return false;
		}
	}

	log_debug("listSourceTablesNoPKSQL[%s]", filterTypeToString(filters->type));

	char *sql = listSourceTablesNoPKSQL[filters->type].sql;

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &getTableArray))
	{
		log_error("Failed to list tables without primary key");
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to list tables without primary key");
		return false;
	}

	return true;
}


/*
 * For code simplicity the index array is also the SourceFilterType enum value.
 */
struct FilteringQueries listSourceSequencesSQL[] = {
	{
		SOURCE_FILTER_TYPE_NONE,

		"  select c.oid, "
		"         format('%I', n.nspname) as nspname, "
		"         format('%I', c.relname) as relname, "
		"         format('%s %s %s', "
		"                regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                regexp_replace(c.relname, '[\\n\\r]', ' '), "
		"                regexp_replace(auth.rolname, '[\\n\\r]', ' ')), "
		"         NULL as ownedby, "
		"         NULL as attrelid, "
		"         NULL as attroid "

		"    from pg_catalog.pg_class c "
		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid "
		"         join pg_roles auth ON auth.oid = c.relowner"

		"   where c.relkind = 'S' and c.relpersistence in ('p', 'u') "
		"     and n.nspname !~ '^pg_' and n.nspname <> 'information_schema' "
		"     and n.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = c.oid "
		"            and d.deptype = 'e' "
		"       ) "

		"order by n.nspname, c.relname"
	},

	{
		SOURCE_FILTER_TYPE_INCL,

		"with "
		" seqs(seqoid, nspname, relname, restore_list_name) as "
		" ( "
		"    select s.oid as seqoid, "
		"           format('%I', sn.nspname) as nspname, "
		"           format('%I', s.relname) as relname, "
		"           format('%s %s %s', "
		"                  regexp_replace(sn.nspname, '[\\n\\r]', ' '), "
		"                  regexp_replace(s.relname, '[\\n\\r]', ' '), "
		"                  regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"      from pg_class s "
		"           join pg_namespace sn on sn.oid = s.relnamespace "
		"           join pg_roles auth ON auth.oid = s.relowner "

		"     where s.relkind = 'S' "
		"       and sn.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */

		"       and not exists "
		"         ( "
		"           select 1 "
		"             from pg_depend d "
		"            where d.classid = 'pg_class'::regclass "
		"              and d.objid = s.oid "
		"              and d.deptype = 'e' "
		"         ) "
		"    ) "

		/*
		 * pg_depend links SEQUENCED OWNED sequence and table with AUTO deptype
		 * for serial, and INTERNAL deptype for identity columns.
		 *
		 * Also, sequences can be used on tables that do not "own" them, just
		 * by using a DEFAULT value.
		 */
		"    select s.seqoid, s.nspname, s.relname, s.restore_list_name, "
		"           r1.oid as ownedby, "
		"           r2.oid as attrelid, "
		"           a.oid as attroid "
		"      from seqs as s "

		"       left join pg_depend d1 on d1.objid = s.seqoid "
		"        and d1.classid = 'pg_class'::regclass "
		"        and d1.refclassid = 'pg_class'::regclass "
		"        and d1.deptype in ('i', 'a') "

		"       left join pg_depend d2 on d2.refobjid = s.seqoid "
		"        and d2.refclassid = 'pg_class'::regclass "
		"        and d2.classid = 'pg_attrdef'::regclass "
		"       join pg_attrdef a on a.oid = d2.objid "
		"       join pg_attribute at "
		"         on at.attrelid = a.adrelid "
		"        and at.attnum = a.adnum "

		"       left join pg_class r1 on r1.oid = d1.refobjid "
		"       left join pg_namespace rn1 on rn1.oid = r1.relnamespace "

		"       left join pg_class r2 on r2.oid = at.attrelid  "
		"       left join pg_namespace rn2 on rn2.oid = r2.relnamespace "

		/* include-only-table */
		"       left join pg_temp.filter_include_only_table inc1 "
		"         on rn1.nspname = inc1.nspname "
		"        and r1.relname = inc1.relname "

		"       left join pg_temp.filter_include_only_table inc2 "
		"         on rn2.nspname = inc2.nspname "
		"        and r2.relname = inc2.relname "

		"      where (r1.relname is not null and inc1.relname is not null) "
		"         or (r2.relname is not null and inc2.relname is not null) "

		"   order by nspname, relname"
	},

	{
		SOURCE_FILTER_TYPE_EXCL,

		"with "
		" seqs(seqoid, nspname, relname, restore_list_name) as "
		" ( "
		"    select s.oid as seqoid, "
		"           format('%I', sn.nspname) as nspname, "
		"           format('%I', s.relname) as relname, "
		"           format('%s %s %s', "
		"                  regexp_replace(sn.nspname, '[\\n\\r]', ' '), "
		"                  regexp_replace(s.relname, '[\\n\\r]', ' '), "
		"                  regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"      from pg_class s "
		"           join pg_namespace sn on sn.oid = s.relnamespace "
		"           join pg_roles auth ON auth.oid = s.relowner "

		"     where s.relkind = 'S' "
		"       and sn.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */

		"       and not exists "
		"         ( "
		"           select 1 "
		"             from pg_depend d "
		"            where d.classid = 'pg_class'::regclass "
		"              and d.objid = s.oid "
		"              and d.deptype = 'e' "
		"         ) "
		"    ) "

		/*
		 * pg_depend links SEQUENCED OWNED sequence and table with AUTO deptype
		 * for serial, and INTERNAL deptype for identity columns.
		 *
		 * Also, sequences can be used on tables that do not "own" them, just
		 * by using a DEFAULT value.
		 */
		"    select s.seqoid, s.nspname, s.relname, s.restore_list_name, "
		"           r1.oid as ownedby, "
		"           r2.oid as attrelid, "
		"           a.oid as attroid "
		"      from seqs as s "

		"       left join pg_depend d1 on d1.objid = s.seqoid "
		"        and d1.classid = 'pg_class'::regclass "
		"        and d1.refclassid = 'pg_class'::regclass "
		"        and d1.deptype in ('i', 'a') "

		"       left join pg_depend d2 on d2.refobjid = s.seqoid "
		"        and d2.refclassid = 'pg_class'::regclass "
		"        and d2.classid = 'pg_attrdef'::regclass "
		"       join pg_attrdef a on a.oid = d2.objid "
		"       join pg_attribute at "
		"         on at.attrelid = a.adrelid "
		"        and at.attnum = a.adnum "

		"       left join pg_class r1 on r1.oid = d1.refobjid "
		"       join pg_namespace rn1 on rn1.oid = r1.relnamespace "

		"       left join pg_class r2 on r2.oid = at.attrelid  "
		"       join pg_namespace rn2 on rn2.oid = r2.relnamespace "

		/* exclude-schema */
		"      left join pg_temp.filter_exclude_schema fn1 "
		"             on rn1.nspname = fn1.nspname "

		"      left join pg_temp.filter_exclude_schema fn2 "
		"             on rn2.nspname = fn2.nspname "

		/* exclude-table */
		"      left join pg_temp.filter_exclude_table ft1 "
		"             on rn1.nspname = ft1.nspname "
		"            and r1.relname = ft1.relname "

		"      left join pg_temp.filter_exclude_table ft2 "
		"             on rn2.nspname = ft2.nspname "
		"            and r2.relname = ft2.relname "

		/* exclude-table-data */
		"      left join pg_temp.filter_exclude_table_data ftd1 "
		"             on rn1.nspname = ftd1.nspname "
		"            and r1.relname = ftd1.relname "

		"      left join pg_temp.filter_exclude_table_data ftd2 "
		"             on rn2.nspname = ftd2.nspname "
		"            and r2.relname = ftd2.relname "

		/* WHERE clause for exclusion filters */
		"     where case when r1.oid = r2.oid "
		"           then rn1.nspname is not null and fn1.nspname is null "
		"            and r1.relname is not null and ft1.relname is null "
		"            and r1.relname is not null and ftd1.relname is null "

		"           else rn2.nspname is not null and fn2.nspname is null "
		"            and r2.relname is not null and ft2.relname is null "
		"            and r2.relname is not null and ftd2.relname is null "
		"           end"

		"   order by nspname, relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_NOT_INCL,

		"with "
		" seqs(seqoid, nspname, relname, restore_list_name) as "
		" ( "
		"    select s.oid as seqoid, "
		"           format('%I', sn.nspname) as nspname, "
		"           format('%I', s.relname) as relname, "
		"           format('%s %s %s', "
		"                  regexp_replace(sn.nspname, '[\\n\\r]', ' '), "
		"                  regexp_replace(s.relname, '[\\n\\r]', ' '), "
		"                  regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"      from pg_class s "
		"           join pg_namespace sn on sn.oid = s.relnamespace "
		"           join pg_roles auth ON auth.oid = s.relowner "

		"     where s.relkind = 'S' "
		"       and sn.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */

		"       and not exists "
		"         ( "
		"           select 1 "
		"             from pg_depend d "
		"            where d.classid = 'pg_class'::regclass "
		"              and d.objid = s.oid "
		"              and d.deptype = 'e' "
		"         ) "
		"    ) "

		/*
		 * pg_depend links SEQUENCED OWNED sequence and table with AUTO deptype
		 * for serial, and INTERNAL deptype for identity columns.
		 *
		 * Also, sequences can be used on tables that do not "own" them, just
		 * by using a DEFAULT value.
		 */
		"    select s.seqoid, s.nspname, s.relname, s.restore_list_name, "
		"           r1.oid as ownedby, "
		"           r2.oid as attrelid, "
		"           a.oid as attroid "
		"      from seqs as s "

		"       left join pg_depend d1 on d1.objid = s.seqoid "
		"        and d1.classid = 'pg_class'::regclass "
		"        and d1.refclassid = 'pg_class'::regclass "
		"        and d1.deptype in ('i', 'a') "

		"       left join pg_depend d2 on d2.refobjid = s.seqoid "
		"        and d2.refclassid = 'pg_class'::regclass "
		"        and d2.classid = 'pg_attrdef'::regclass "
		"       join pg_attrdef a on a.oid = d2.objid "
		"       join pg_attribute at "
		"         on at.attrelid = a.adrelid "
		"        and at.attnum = a.adnum "

		"       left join pg_class r1 on r1.oid = d1.refobjid "
		"       left join pg_namespace rn1 on rn1.oid = r1.relnamespace "

		"       left join pg_class r2 on r2.oid = at.attrelid  "
		"       left join pg_namespace rn2 on rn2.oid = r2.relnamespace "

		/* include-only-table */
		"       left join pg_temp.filter_include_only_table inc1 "
		"              on rn1.nspname = inc1.nspname "
		"             and r1.relname = inc1.relname "

		"       left join pg_temp.filter_include_only_table inc2 "
		"              on rn2.nspname = inc2.nspname "
		"             and r2.relname = inc2.relname "

		"      where (r1.relname is not null and inc1.relname is null) "
		"         or (r2.relname is not null and inc2.relname is null) "

		"   order by nspname, relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_EXCL,

		"with "
		" seqs(seqoid, nspname, relname, restore_list_name) as "
		" ( "
		"    select s.oid as seqoid, "
		"           format('%I', sn.nspname) as nspname, "
		"           format('%I', s.relname) as relname, "
		"           format('%s %s %s', "
		"                  regexp_replace(sn.nspname, '[\\n\\r]', ' '), "
		"                  regexp_replace(s.relname, '[\\n\\r]', ' '), "
		"                  regexp_replace(auth.rolname, '[\\n\\r]', ' ')) "
		"      from pg_class s "
		"           join pg_namespace sn on sn.oid = s.relnamespace "
		"           join pg_roles auth ON auth.oid = s.relowner "

		"     where s.relkind = 'S' "
		"       and sn.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */

		"       and not exists "
		"         ( "
		"           select 1 "
		"             from pg_depend d "
		"            where d.classid = 'pg_class'::regclass "
		"              and d.objid = s.oid "
		"              and d.deptype = 'e' "
		"         ) "
		"    ) "

		/*
		 * pg_depend links SEQUENCED OWNED sequence and table with AUTO deptype
		 * for serial, and INTERNAL deptype for identity columns.
		 *
		 * Also, sequences can be used on tables that do not "own" them, just
		 * by using a DEFAULT value.
		 */
		"    select s.seqoid, s.nspname, s.relname, s.restore_list_name, "
		"           r1.oid as ownedby, "
		"           r2.oid as attrelid, "
		"           a.oid as attroid "
		"      from seqs as s "

		"       left join pg_depend d1 on d1.objid = s.seqoid "
		"        and d1.classid = 'pg_class'::regclass "
		"        and d1.refclassid = 'pg_class'::regclass "
		"        and d1.deptype in ('i', 'a') "

		"       left join pg_depend d2 on d2.refobjid = s.seqoid "
		"        and d2.refclassid = 'pg_class'::regclass "
		"        and d2.classid = 'pg_attrdef'::regclass "
		"       join pg_attrdef a on a.oid = d2.objid "
		"       join pg_attribute at "
		"         on at.attrelid = a.adrelid "
		"        and at.attnum = a.adnum "

		"       left join pg_class r1 on r1.oid = d1.refobjid "
		"       join pg_namespace rn1 on rn1.oid = r1.relnamespace "

		"       left join pg_class r2 on r2.oid = at.attrelid  "
		"       join pg_namespace rn2 on rn2.oid = r2.relnamespace "

		/* exclude-schema */
		"      left join pg_temp.filter_exclude_schema fn "
		"             on rn1.nspname = fn.nspname "

		/* exclude-table */
		"      left join pg_temp.filter_exclude_table ft "
		"             on rn1.nspname = ft.nspname "
		"            and r1.relname = ft.relname "


		/* WHERE clause for exclusion filters */
		"     where (   fn.nspname is not null "
		"            or ft.relname is not null) "

		"   order by nspname, relname"
	},
};


/*
 * schema_list_sequences grabs the list of sequences from the given source
 * Postgres instance and allocates a SourceSequence array with the result of
 * the query.
 */
bool
schema_list_sequences(PGSQL *pgsql,
					  SourceFilters *filters,
					  SourceSequenceArray *seqArray)
{
	SourceSequenceArrayContext context = { { 0 }, seqArray, false };

	log_trace("schema_list_sequences");

	switch (filters->type)
	{
		case SOURCE_FILTER_TYPE_NONE:
		{
			/* skip filters preparing (temp tables) */
			break;
		}

		case SOURCE_FILTER_TYPE_INCL:
		case SOURCE_FILTER_TYPE_EXCL:
		case SOURCE_FILTER_TYPE_LIST_NOT_INCL:
		case SOURCE_FILTER_TYPE_LIST_EXCL:
		{
			if (!prepareFilters(pgsql, filters))
			{
				log_error("Failed to prepare pgcopydb filters, "
						  "see above for details");
				return false;
			}
			break;
		}

		/* SOURCE_FILTER_TYPE_EXCL_INDEX etc */
		default:
		{
			log_error("BUG: schema_list_sequences called with "
					  "filtering type %d",
					  filters->type);
			return false;
		}
	}

	log_debug("listSourceSequencesSQL[%s]", filterTypeToString(filters->type));

	char *sql = listSourceSequencesSQL[filters->type].sql;

	/*
	 * A single sequence can be attached to more than one table, and it could
	 * be that some of the tables are excluded and some of the tables are
	 * included in our filtering. In that case we want to remove from the
	 * SOURCE_FILTER_TYPE_LIST_EXCL list of sequences the sequences from the
	 * SOURCE_FILTER_TYPE_EXCL list.
	 */
	PQExpBuffer buffer = NULL;

	if (filters->type == SOURCE_FILTER_TYPE_LIST_EXCL)
	{
		buffer = createPQExpBuffer();

		char *exclude = sql;
		char *keep = listSourceSequencesSQL[SOURCE_FILTER_TYPE_EXCL].sql;

		char *sqlTmpl =
			"select seqoid, "
			"       format('%%I', nspname) as nspname, "
			"       format('%%I', relname) as relname, "
			"       restore_list_name, "
			"       ownedby, attrelid, attroid "
			"  from (%s) as exclude "
			" where not exists "
			" ( "
			"   select 1 "
			"     from (%s) as keep "
			"    where keep.seqoid = exclude.seqoid "
			"      and keep.ownedby is not distinct from exclude.ownedby "
			"      and keep.attrelid is not distinct from exclude.attrelid "
			"      and keep.attroid is not distinct from exclude.attroid "
			" ) ";

		appendPQExpBuffer(buffer, sqlTmpl, exclude, keep);

		if (PQExpBufferBroken(buffer))
		{
			log_error("Failed to create SQL query: out of memory");
			(void) destroyPQExpBuffer(buffer);
			return false;
		}

		sql = buffer->data;
	}

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &getSequenceArray))
	{
		log_error("Failed to list sequences");
		(void) destroyPQExpBuffer(buffer);
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to list sequences");
		(void) destroyPQExpBuffer(buffer);
		return false;
	}

	(void) destroyPQExpBuffer(buffer);

	return true;
}


/*
 * schema_get_sequence_value fetches sequence metadata last_value and
 * is_called for the given sequence.
 */
bool
schema_get_sequence_value(PGSQL *pgsql, SourceSequence *seq)
{
	return pgsql_get_sequence(pgsql,
							  seq->nspname,
							  seq->relname,
							  &(seq->lastValue),
							  &(seq->isCalled));
}


/*
 * schema_set_sequence_value calls pg_catalog.setval() on the given sequence.
 */
bool
schema_set_sequence_value(PGSQL *pgsql, SourceSequence *seq)
{
	SingleValueResultContext parseContext = { { 0 }, PGSQL_RESULT_BIGINT, false };
	char *sql = "select pg_catalog.setval($1::regclass, $2, $3)";

	int paramCount = 3;
	Oid paramTypes[3] = { TEXTOID, INT8OID, BOOLOID };
	const char *paramValues[3];

	paramValues[0] = seq->qname;
	paramValues[1] = intToString(seq->lastValue).strValue;
	paramValues[2] = seq->isCalled ? "true" : "false";

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, &parseSingleValueResult))
	{
		log_error("Failed to set sequence %s last value to %lld",
				  seq->qname, (long long) seq->lastValue);
		return false;
	}

	if (!parseContext.parsedOk)
	{
		log_error("Failed to set sequence %s last value to %lld",
				  seq->qname, (long long) seq->lastValue);
		return false;
	}

	return true;
}


/*
 * For code simplicity the index array is also the SourceFilterType enum value.
 */
struct FilteringQueries listSourceIndexesSQL[] = {
	{
		SOURCE_FILTER_TYPE_NONE,

		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"

		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"

		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		" order by n.nspname, r.relname, i.relname"
	},

	{
		SOURCE_FILTER_TYPE_INCL,

		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"
		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"

		/* include-only-table */
		"         join pg_temp.filter_include_only_table inc "
		"           on rn.nspname = inc.nspname "
		"          and r.relname = inc.relname "

		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		" order by n.nspname, r.relname, i.relname"
	},

	{
		SOURCE_FILTER_TYPE_EXCL,

		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"
		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on rn.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on rn.nspname = ft.nspname "
		"               and r.relname = ft.relname "

		/* exclude-table-data */
		"         left join pg_temp.filter_exclude_table_data ftd "
		"                on rn.nspname = ftd.nspname "
		"               and r.relname = ftd.relname "

		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "

		/* WHERE clause for exclusion filters */
		"     and fn.nspname is null "
		"     and ft.relname is null "
		"     and ftd.relname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		" order by n.nspname, r.relname, i.relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_NOT_INCL,

		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"
		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"

		/* include-only-table */
		"    left join pg_temp.filter_include_only_table inc "
		"           on rn.nspname = inc.nspname "
		"          and r.relname = inc.relname "

		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "

		/* WHERE clause for exclusion filters */
		"     and inc.relname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		" order by n.nspname, r.relname, i.relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_EXCL,

		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"
		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on rn.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on rn.nspname = ft.nspname "
		"               and r.relname = ft.relname "

		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "

		/* WHERE clause for exclusion filters */
		"     and (   fn.nspname is not null "
		"          or ft.relname is not null ) "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		" order by n.nspname, r.relname, i.relname"
	},

	{
		SOURCE_FILTER_TYPE_EXCL_INDEX,

		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"
		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"

		/* exclude-index */
		"          left join filter_exclude_index ft "
		"                 on n.nspname = ft.nspname "
		"                and i.relname = ft.relname "

		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "

		/* WHERE clause for exclusion filters */
		"     and ft.relname is null "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		" order by n.nspname, r.relname, i.relname"
	},

	{
		SOURCE_FILTER_TYPE_LIST_EXCL_INDEX,

		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"
		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"

		/* list only exclude-index */
		"               join filter_exclude_index ft "
		"                 on n.nspname = ft.nspname "
		"                and i.relname = ft.relname "

		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "

		/* avoid pg_class entries which belong to extensions */
		"     and not exists "
		"       ( "
		"         select 1 "
		"           from pg_depend d "
		"          where d.classid = 'pg_class'::regclass "
		"            and d.objid = r.oid "
		"            and d.deptype = 'e' "
		"       ) "

		" order by n.nspname, r.relname, i.relname"
	}
};


/*
 * schema_list_all_indexes grabs the list of indexes from the given source
 * Postgres instance and allocates a SourceIndex array with the result of the
 * query.
 */
bool
schema_list_all_indexes(PGSQL *pgsql,
						SourceFilters *filters,
						SourceIndexArray *indexArray)
{
	SourceIndexArrayContext context = { { 0 }, indexArray, false };

	log_trace("schema_list_all_indexes");

	if (filters->type != SOURCE_FILTER_TYPE_NONE)
	{
		if (!prepareFilters(pgsql, filters))
		{
			log_error("Failed to prepare pgcopydb filters, "
					  "see above for details");
			return false;
		}
	}

	log_debug("listSourceIndexesSQL[%s]", filterTypeToString(filters->type));

	char *sql = listSourceIndexesSQL[filters->type].sql;

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &getIndexArray))
	{
		log_error("Failed to list all indexes");
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to list all indexes");
		return false;
	}

	return true;
}


/*
 * schema_list_all_indexes grabs the list of indexes from the given source
 * Postgres instance and allocates a SourceIndex array with the result of the
 * query.
 */
bool
schema_list_table_indexes(PGSQL *pgsql,
						  const char *schemaName,
						  const char *tableName,
						  SourceIndexArray *indexArray)
{
	SourceIndexArrayContext context = { { 0 }, indexArray, false };

	char *sql =
		"   select i.oid, "
		"          format('%I', n.nspname) as inspname, "
		"          format('%I', i.relname) as irelname,"
		"          r.oid, "
		"          format('%I', rn.nspname) as rnspname, "
		"          format('%I', r.relname) as rrelname, "
		"          indisprimary,"
		"          indisunique,"
		"          (select string_agg(format('%I', attname), ',')"
		"             from pg_attribute"
		"            where attrelid = r.oid"
		"              and array[attnum::integer] <@ indkey::integer[]"
		"          ) as cols,"
		"          pg_get_indexdef(indexrelid),"
		"          c.oid,"
		"          c.condeferrable,"
		"          c.condeferred,"
		"          case when conname is not null "
		"               then format('%I', c.conname) "
		"           end as conname,"
		"          pg_get_constraintdef(c.oid),"
		"          format('%s %s %s', "
		"                 regexp_replace(n.nspname, '[\\n\\r]', ' '), "
		"                 regexp_replace(i.relname, '[\\n\\r]', ' '), "
		"                 regexp_replace(auth.rolname, '[\\n\\r]', ' '))"
		"     from pg_index x"
		"          join pg_class i ON i.oid = x.indexrelid"
		"          join pg_class r ON r.oid = x.indrelid"
		"          join pg_namespace n ON n.oid = i.relnamespace"
		"          join pg_namespace rn ON rn.oid = r.relnamespace"
		"          join pg_roles auth ON auth.oid = i.relowner"
		"          left join pg_depend d "
		"                 on d.classid = 'pg_class'::regclass"
		"                and d.objid = i.oid"
		"                and d.refclassid = 'pg_constraint'::regclass"
		"                and d.deptype = 'i'"
		"          left join pg_constraint c ON c.oid = d.refobjid"
		"    where r.relkind = 'r' and r.relpersistence in ('p', 'u') "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "
		"      and rn.nspname = $1 and r.relname = $2"
		" order by n.nspname, r.relname, i.relname";

	int paramCount = 2;
	Oid paramTypes[2] = { TEXTOID, TEXTOID };
	const char *paramValues[2] = { schemaName, tableName };

	log_trace("schema_list_table_indexes");

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &getIndexArray))
	{
		log_error("Failed to list all indexes for table \"%s\".\"%s\"",
				  schemaName, tableName);
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to list all indexes for table \"%s\".\"%s\"",
				  schemaName, tableName);
		return false;
	}

	return true;
}


/*
 * For code simplicity the index array is also the SourceFilterType enum value.
 */
struct FilteringQueries listSourceDependSQL[] = {
	{
		SOURCE_FILTER_TYPE_NONE, ""
	},

	{
		SOURCE_FILTER_TYPE_INCL,

		PG_DEPEND_SQL
		"  SELECT n.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, "
		"         deptype, type, identity "
		"    FROM unconcat "

		/* include-only-table */
		"         join pg_class c "
		"           on unconcat.refclassid = 'pg_class'::regclass "
		"          and unconcat.refobjid = c.oid "

		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid "

		"         join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and c.relname = inc.relname "

		"         , pg_identify_object(classid, objid, objsubid) "

		"   WHERE NOT (refclassid = classid AND refobjid = objid) "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "
		"      and type not in ('toast table column', 'default value') "

		/* remove duplicates due to multiple refobjsubid / objsubid */
		"GROUP BY n.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, deptype, type, identity"
	},

	{
		SOURCE_FILTER_TYPE_EXCL,

		PG_DEPEND_SQL
		"  SELECT cn.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, "
		"         deptype, type, identity "
		"    FROM unconcat "

		"         join pg_class c "
		"           on unconcat.refclassid = 'pg_class'::regclass "
		"          and unconcat.refobjid = c.oid "

		"         join pg_catalog.pg_namespace cn "
		"           on c.relnamespace = cn.oid "

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on cn.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on cn.nspname = ft.nspname "
		"               and c.relname = ft.relname "

		"         , pg_identify_object(classid, objid, objsubid) "

		"   WHERE NOT (refclassid = classid AND refobjid = objid) "
		"      and cn.nspname !~ '^pg_' and cn.nspname <> 'information_schema'"
		"      and cn.nspname !~ 'pgcopydb' "
		"      and type not in ('toast table column', 'default value') "

		/* WHERE clause for exclusion filters */
		"     and fn.nspname is null "
		"     and ft.relname is null "

		/* remove duplicates due to multiple refobjsubid / objsubid */
		"GROUP BY cn.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, deptype, type, identity"
	},

	{
		SOURCE_FILTER_TYPE_LIST_NOT_INCL,

		PG_DEPEND_SQL
		"  SELECT n.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, "
		"         deptype, type, identity "
		"    FROM unconcat "

		"         join pg_class c "
		"           on unconcat.refclassid = 'pg_class'::regclass "
		"          and unconcat.refobjid = c.oid "

		"         join pg_catalog.pg_namespace n on c.relnamespace = n.oid "

		/* include-only-table */
		"    left join pg_temp.filter_include_only_table inc "
		"           on n.nspname = inc.nspname "
		"          and c.relname = inc.relname "

		"         , pg_identify_object(classid, objid, objsubid) "

		"   WHERE NOT (refclassid = classid AND refobjid = objid) "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "
		"      and type not in ('toast table column', 'default value') "

		/* WHERE clause for exclusion filters */
		"     and inc.nspname is null "

		/* remove duplicates due to multiple refobjsubid / objsubid */
		"GROUP BY n.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, deptype, type, identity"
	},

	{
		SOURCE_FILTER_TYPE_LIST_EXCL,

		PG_DEPEND_SQL
		"  SELECT n.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, "
		"         deptype, type, identity "
		"    FROM unconcat "

		"         join pg_class c "
		"           on unconcat.refclassid = 'pg_class'::regclass "
		"          and unconcat.refobjid = c.oid "

		"         join pg_catalog.pg_namespace n "
		"           on c.relnamespace = n.oid "

		/* exclude-schema */
		"         left join pg_temp.filter_exclude_schema fn "
		"                on n.nspname = fn.nspname "

		/* exclude-table */
		"         left join pg_temp.filter_exclude_table ft "
		"                on n.nspname = ft.nspname "
		"               and c.relname = ft.relname "

		"         , pg_identify_object(classid, objid, objsubid) "

		"   WHERE NOT (refclassid = classid AND refobjid = objid) "
		"      and n.nspname !~ '^pg_' and n.nspname <> 'information_schema'"
		"      and n.nspname !~ 'pgcopydb' "
		"      and type not in ('toast table column', 'default value') "

		/* WHERE clause for exclusion filters */
		"     and (   fn.nspname is not null "
		"          or ft.relname is not null ) "

		/* remove duplicates due to multiple refobjsubid / objsubid */
		"GROUP BY n.nspname, c.relname, "
		"         refclassid, refobjid, classid, objid, deptype, type, identity"
	}
};


/*
 * schema_list_pg_depend recursively walks the pg_catalog.pg_depend view and
 * builds the list of objects that depend on tables that are filtered-out from
 * our operations.
 */
bool
schema_list_pg_depend(PGSQL *pgsql,
					  SourceFilters *filters,
					  SourceDependArray *dependArray)
{
	SourceDependArrayContext context = { { 0 }, dependArray, false };

	log_trace("schema_list_pg_depend");

	switch (filters->type)
	{
		case SOURCE_FILTER_TYPE_INCL:
		case SOURCE_FILTER_TYPE_EXCL:
		case SOURCE_FILTER_TYPE_LIST_NOT_INCL:
		case SOURCE_FILTER_TYPE_LIST_EXCL:
		{
			if (!prepareFilters(pgsql, filters))
			{
				log_error("Failed to prepare pgcopydb filters, "
						  "see above for details");
				return false;
			}
			break;
		}

		/* SOURCE_FILTER_TYPE_EXCL_INDEX etc */
		default:
		{
			log_error("BUG: schema_list_ordinary_tables called with "
					  "filtering type %d",
					  filters->type);
			return false;
		}
	}

	log_debug("listSourceDependSQL[%s]", filterTypeToString(filters->type));

	char *sql = listSourceDependSQL[filters->type].sql;

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &getDependArray))
	{
		log_error("Failed to list table dependencies");
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to list table dependencies");
		return false;
	}

	return true;
}


/*
 * schema_list_partitions prepares the list of partitions that we can drive
 * from our parameters: table size, --split-tables-larger-than.
 */
bool
schema_list_partitions(PGSQL *pgsql, SourceTable *table, uint64_t partSize)
{
	/* no partKey, no partitions, done. */
	if (IS_EMPTY_STRING_BUFFER(table->partKey))
	{
		table->partsArray.count = 0;
		return true;
	}

	/* when partSize is zero, just don't partition the COPY */
	if (partSize == 0)
	{
		table->partsArray.count = 0;
		return true;
	}

	PQExpBuffer sql = createPQExpBuffer();

	SourcePartitionContext parseContext = { { 0 }, table, false };

	if (streq(table->partKey, "ctid"))
	{
		char *sqlTemplate =
			" with "
			" relpage_bounds (min, max) as "
			" ( "
			"   select 0, relpages "
			"     from pg_class "
			"    where pg_class.oid = '%s'::regclass "
			" ), "
			" t (parts) as "
			" ( "
			"   select ceil(bytes::float / $1) as parts "
			"     from pgcopydb_table_size "
			"     where oid = $2 "
			"	union all "
			"	select 1 as parts "
			"	order by parts desc "
			"	limit 1 "
			" ), "
			" ranges(n, parts, a, b) as "
			" ( "
			"   select n, "
			"          parts + 1, "
			"          x as a, "
			"          coalesce((lead(x, 1) over(order by n)) - 1, max) as b "
			"     from relpage_bounds, t, "
			"          generate_series(min, max, ((max-min+1)/parts)::bigint + 1) "
			"          with ordinality as s(x, n) "
			"   union all "
			"   select parts + 1, "
			"          parts + 1, "
			"          max, "
			"          NULL "
			"     from relpage_bounds, t "
			" ) "
			" "
			"  select n, parts, a, b, b-a+1 as pages "
			"    from ranges "
			"order by n";

		appendPQExpBuffer(sql, sqlTemplate, table->qname);
	}
	else
	{
		char *sqlTemplate =
			" with "
			" key_bounds (min, max) as "
			" ( "
			"   select min(%s), max(%s) "
			"     from %s "
			" ), "
			" t (parts) as "
			" ( "
			"   select ceil(bytes::float / $1) as parts "
			"     from pgcopydb_table_size "
			"     where oid = $2 "
			"	union all "
			"	select 1 as parts "
			"	order by parts desc "
			"	limit 1 "
			" ), "
			" ranges(n, parts, a, b) as "
			" ( "
			"   select n, "
			"          parts, "
			"          x as a, "
			"          coalesce((lead(x, 1) over(order by n)) - 1, max) as b "
			"     from key_bounds, t, "
			"          generate_series(min, max, ((max-min+1)/parts)::bigint + 1) "
			"          with ordinality as s(x, n) "
			" ) "
			" "
			"  select n, parts, a, b, b-a+1 as count "
			"    from ranges "
			"order by n";

		appendPQExpBuffer(sql, sqlTemplate,
						  table->partKey, table->partKey, table->qname);
	}

	if (PQExpBufferBroken(sql))
	{
		(void) destroyPQExpBuffer(sql);
		log_error("Failed to prepare partition query for table %s: out of memory",
				  table->qname);
		return false;
	}

	int paramCount = 2;
	Oid paramTypes[2] = { INT8OID, OIDOID };
	const char *paramValues[2];

	paramValues[0] = intToString(partSize).strValue;
	paramValues[1] = intToString(table->oid).strValue;

	if (!pgsql_execute_with_params(pgsql, sql->data,
								   paramCount, paramTypes, paramValues,
								   &parseContext, &getPartitionList))
	{
		(void) destroyPQExpBuffer(sql);
		log_error("Failed to compute partition list for table %s",
				  table->qname);
		return false;
	}

	(void) destroyPQExpBuffer(sql);

	if (!parseContext.parsedOk)
	{
		log_error("Failed to list table COPY partition list");
		return false;
	}

	return true;
}


/*
 * schema_checksum_table runs a SQL query that computes the number of rows of a
 * table and also a checksum for all the rows contents.
 */
bool
schema_send_table_checksum(PGSQL *pgsql, SourceTable *table)
{
	if (table->attributes.count == 0)
	{
		char sql[BUFSIZE] = { 0 };

		sformat(sql, sizeof(sql),
				"select count(1) as cnt, 0 as chksum from only %s",
				table->qname);

		if (!pgsql_send_with_params(pgsql, sql, 0, NULL, NULL))
		{
			log_error("Failed to compute checksum for table %s", table->qname);
			return false;
		}

		return true;
	}

	/* first prepare the column list */
	PQExpBuffer attrList = createPQExpBuffer();

	appendPQExpBuffer(attrList, "(");

	for (int c = 0; c < table->attributes.count; c++)
	{
		char *srcAttName = table->attributes.array[c].attname;

		appendPQExpBuffer(attrList, "%s%s",
						  c > 0 ? ", " : "",
						  srcAttName);
	}

	appendPQExpBuffer(attrList, ")");

	if (PQExpBufferBroken(attrList))
	{
		(void) destroyPQExpBuffer(attrList);
		log_error("Failed to build attribute list: Out of Memory");
		return false;
	}

	/* now prepare the actual query */
	PQExpBuffer sql = createPQExpBuffer();

	/*
	 * Compute the hashtext of every single row in the table, and aggregate the
	 * results as a sum of bigint numbers. Because the sum of bigint could
	 * overflow to numeric, the aggregated sum is then hashed into an MD5
	 * value: bigint is 64 bits, MD5 is 128 bits.
	 *
	 * Also, to lower the chances of a collision, include the row count in the
	 * computation of the MD5 by appending it to the input string of the MD5
	 * function.
	 */
	appendPQExpBuffer(sql,
					  "select count(1) as cnt, "
					  "md5(format('%%s-%%s', "
					  "      sum(hashtext(%s::text)::bigint),"
					  "      count(1))"
					  ")::uuid as chksum "
					  "from only %s",
					  attrList->data,
					  table->qname);

	(void) destroyPQExpBuffer(attrList);

	if (PQExpBufferBroken(sql))
	{
		(void) destroyPQExpBuffer(sql);
		log_error("Failed to build attribute list: Out of Memory");
		return false;
	}

	if (!pgsql_send_with_params(pgsql, sql->data, 0, NULL, NULL))
	{
		log_error("Failed to compute checksum for table %s", table->qname);
		(void) destroyPQExpBuffer(sql);
		return false;
	}

	(void) destroyPQExpBuffer(sql);

	return true;
}


/*
 * schema_fetch_table_checksum fetches the results from the
 * schema_send_table_checksum async query.
 */
bool
schema_fetch_table_checksum(PGSQL *pgsql, TableChecksum *sum, bool *done)
{
	ChecksumContext parseContext = { { 0 }, sum, false };

	if (!pgsql_fetch_results(pgsql, done, &parseContext, &getTableChecksum))
	{
		log_error("Failed to fetch table checksum results");
		return false;
	}

	return true;
}


/*
 * prepareFilters prepares the temporary tables that are needed on the Postgres
 * session where we want to implement a catalog query with filtering. The
 * filtering rules are then uploaded in those temp tables, and the filtering is
 * implemented with SQL joins.
 */
static bool
prepareFilters(PGSQL *pgsql, SourceFilters *filters)
{
	/*
	 * Temporary tables only are available within a session, so we need a
	 * multi-statement connection here.
	 */
	if (pgsql->connection == NULL)
	{
		/* open a multi-statements connection then */
		pgsql->connectionStatementType = PGSQL_CONNECTION_MULTI_STATEMENT;
	}
	else if (pgsql->connectionStatementType != PGSQL_CONNECTION_MULTI_STATEMENT)
	{
		log_error("BUG: calling prepareFilters with a "
				  "non PGSQL_CONNECTION_MULTI_STATEMENT connection");
		pgsql_finish(pgsql);
		return false;
	}

	/* if the filters have already been prepared, we're good */
	if (filters->prepared)
	{
		return true;
	}

	/*
	 * First, create the temp tables.
	 */
	char *tempTables[] = {
		"create temp table filter_exclude_schema(nspname name)",
		"create temp table filter_include_only_schema(nspname name)",
		"create temp table filter_include_only_table(nspname name, relname name)",
		"create temp table filter_exclude_table(nspname name, relname name)",
		"create temp table filter_exclude_table_data(nspname name, relname name)",
		"create temp table filter_exclude_index(nspname name, relname name)",
		NULL
	};

	for (int i = 0; tempTables[i] != NULL; i++)
	{
		if (!pgsql_execute(pgsql, tempTables[i]))
		{
			/* errors have already been logged */
			return false;
		}
	}

	/*
	 * Now, fill-in the temp tables with the data that we have.
	 */
	if (!prepareFilterCopyIncludeOnlySchema(pgsql, filters))
	{
		/* errors have already been logged */
		return false;
	}

	if (!prepareFilterCopyExcludeSchema(pgsql, filters))
	{
		/* errors have already been logged */
		return false;
	}

	struct name_list_pair
	{
		char *name;
		SourceFilterTableList *list;
	}
	nameListPair[] =
	{
		{ "filter_include_only_table", &(filters->includeOnlyTableList) },
		{ "filter_exclude_table", &(filters->excludeTableList) },
		{ "filter_exclude_table_data", &(filters->excludeTableDataList) },
		{ "filter_exclude_index", &(filters->excludeIndexList) },
		{ NULL, NULL },
	};

	for (int i = 0; nameListPair[i].name != NULL; i++)
	{
		if (!prepareFilterCopyTableList(pgsql,
										nameListPair[i].list,
										nameListPair[i].name))
		{
			/* errors have already been logged */
			return false;
		}
	}

	/* mark the filters as prepared already */
	filters->prepared = true;

	return true;
}


/*
 * prepareFilterCopyExcludeSchema sends a COPY from STDIN query and then
 * uploads the local filters that we have in the pg_temp.filter_exclude_schema
 * table.
 */
static bool
prepareFilterCopyExcludeSchema(PGSQL *pgsql, SourceFilters *filters)
{
	if (filters->excludeSchemaList.count == 0)
	{
		return true;
	}

	char *qname = "\"pg_temp\".\"filter_exclude_schema\"";

	if (!pg_copy_from_stdin(pgsql, qname))
	{
		/* errors have already been logged */
		return false;
	}

	for (int i = 0; i < filters->excludeSchemaList.count; i++)
	{
		char *nspname = filters->excludeSchemaList.array[i].nspname;

		if (!pg_copy_row_from_stdin(pgsql, "s", nspname))
		{
			/* errors have already been logged */
			return false;
		}
	}

	if (!pg_copy_end(pgsql))
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * prepareFilterCopyIncludeOnlySchema sends a COPY from STDIN query and then
 * uploads the local filters that we have in the
 * pg_temp.filter_include_only_schema table.
 *
 * Then it prepares the pg_temp.filter_exclude_schema table with all the schema
 * names found in pg_namespace that are not in the include-only-schema list.
 */
static bool
prepareFilterCopyIncludeOnlySchema(PGSQL *pgsql, SourceFilters *filters)
{
	if (filters->includeOnlySchemaList.count == 0)
	{
		return true;
	}

	char *qname = "\"pg_temp\".\"filter_include_only_schema\"";

	if (!pg_copy_from_stdin(pgsql, qname))
	{
		/* errors have already been logged */
		return false;
	}

	for (int i = 0; i < filters->includeOnlySchemaList.count; i++)
	{
		char *nspname = filters->includeOnlySchemaList.array[i].nspname;

		log_trace("prepareFilterCopyIncludeOnlySchema: \"%s\"", nspname);

		if (!pg_copy_row_from_stdin(pgsql, "s", nspname))
		{
			/* errors have already been logged */
			return false;
		}
	}

	if (!pg_copy_end(pgsql))
	{
		/* errors have already been logged */
		return false;
	}

	char *sql =
		"insert into \"pg_temp\".\"filter_exclude_schema\" "
		"     select n.nspname "
		"       from pg_namespace n "
		"  left join \"pg_temp\".\"filter_include_only_schema\" inc "
		"         on n.nspname = inc.nspname "
		"      where inc.nspname is null ";

	if (!pgsql_execute(pgsql, sql))
	{
		log_error("Failed to prepare include-only-schema filters, "
				  "see above for details");
		return false;
	}

	return true;
}


/*
 * prepareFilterCopyTableList sends a COPY from STDIN query and then uploads
 * the local filters that we have in the given target table.
 */
static bool
prepareFilterCopyTableList(PGSQL *pgsql,
						   SourceFilterTableList *tableList,
						   const char *temp_table_name)
{
	char qname[BUFSIZE] = { 0 };

	sformat(qname, sizeof(qname), "\"pg_temp\".\"%s\"", temp_table_name);

	if (!pg_copy_from_stdin(pgsql, qname))
	{
		/* errors have already been logged */
		return false;
	}

	for (int i = 0; i < tableList->count; i++)
	{
		char *nspname = tableList->array[i].nspname;
		char *relname = tableList->array[i].relname;

		log_trace("\"%s\"\t\"%s\"", nspname, relname);

		if (!pg_copy_row_from_stdin(pgsql, "ss", nspname, relname))
		{
			/* errors have already been logged */
			return false;
		}
	}

	if (!pg_copy_end(pgsql))
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * getSchemaList loops over the SQL result for the schema array query and
 * allocates an array of schemas then populates it with the query result.
 */
static void
getSchemaList(void *ctx, PGresult *result)
{
	SourceSchemaArrayContext *context = (SourceSchemaArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getSchemaList: %d", nTuples);

	if (PQnfields(result) != 3)
	{
		log_error("Query returned %d columns, expected 3", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->schemaArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getSchemaList");

		free(context->schemaArray->array);
		context->schemaArray->array = NULL;
	}

	context->schemaArray->count = nTuples;
	context->schemaArray->array =
		(SourceSchema *) calloc(nTuples, sizeof(SourceSchema));

	if (context->schemaArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	int errors = 0;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceSchema *schema = &(context->schemaArray->array[rowNumber]);

		/* 1. oid */
		char *value = PQgetvalue(result, rowNumber, 0);

		if (!stringToUInt32(value, &(schema->oid)) || schema->oid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}

		/* 2. nspname */
		value = PQgetvalue(result, rowNumber, 1);
		int length = strlcpy(schema->nspname, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Schema name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		/* 3. restoreListName */
		value = PQgetvalue(result, rowNumber, 2);
		length = strlcpy(schema->restoreListName, value, RESTORE_LIST_NAMEDATALEN);

		if (length >= RESTORE_LIST_NAMEDATALEN)
		{
			log_error("Schema restore list name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (RESTORE_LIST_NAMEDATALEN - 1)",
					  value, length, RESTORE_LIST_NAMEDATALEN - 1);
			++errors;
		}

		log_trace("getSchemaList: %u \"%s\" %s",
				  schema->oid,
				  schema->nspname,
				  schema->restoreListName);
	}

	context->parsedOk = errors == 0;
}


/*
 * getRoleList loops over the SQL result for the role array query and
 * allocates an array of roles then populates it with the query result.
 */
static void
getRoleList(void *ctx, PGresult *result)
{
	SourceRoleArrayContext *context = (SourceRoleArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getRoleList: %d", nTuples);

	if (PQnfields(result) != 2)
	{
		log_error("Query returned %d columns, expected 2", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->rolesArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getRoleList");

		free(context->rolesArray->array);
		context->rolesArray->array = NULL;
	}

	context->rolesArray->count = nTuples;
	context->rolesArray->array =
		(SourceRole *) calloc(nTuples, sizeof(SourceRole));

	if (context->rolesArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	int errors = 0;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceRole *role = &(context->rolesArray->array[rowNumber]);

		/* 1. oid */
		char *value = PQgetvalue(result, rowNumber, 0);

		if (!stringToUInt32(value, &(role->oid)) || role->oid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}

		/* 2. rolname */
		value = PQgetvalue(result, rowNumber, 1);
		int length = strlcpy(role->rolname, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Role name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		log_trace("getRoleList: %u %s", role->oid, role->rolname);
	}

	context->parsedOk = errors == 0;
}


/*
 * getDatabaseList loops over the SQL result for the database array query and
 * allocates an array of databases then populates it with the query result.
 */
static void
getDatabaseList(void *ctx, PGresult *result)
{
	SourceDatabaseArrayContext *context = (SourceDatabaseArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getDatabaseList: %d", nTuples);

	if (PQnfields(result) != 4)
	{
		log_error("Query returned %d columns, expected 4", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->databaseArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getDatabaseList");

		free(context->databaseArray->array);
		context->databaseArray->array = NULL;
	}

	context->databaseArray->count = nTuples;
	context->databaseArray->array =
		(SourceDatabase *) calloc(nTuples, sizeof(SourceDatabase));

	if (context->databaseArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	int errors = 0;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceDatabase *database = &(context->databaseArray->array[rowNumber]);

		/* 1. oid */
		char *value = PQgetvalue(result, rowNumber, 0);

		if (!stringToUInt32(value, &(database->oid)) || database->oid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}

		/* 2. datname */
		value = PQgetvalue(result, rowNumber, 1);
		int length = strlcpy(database->datname, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Database name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		/* 3. bytes */
		value = PQgetvalue(result, rowNumber, 2);
		if (PQgetisnull(result, rowNumber, 2))
		{
			/*
			 * It may happen that pg_table_size() returns NULL (when failing to
			 * open the given relation).
			 */
			database->bytes = 0;
		}
		else
		{
			value = PQgetvalue(result, rowNumber, 2);

			if (!stringToInt64(value, &(database->bytes)))
			{
				log_error("Invalid pg_database_size: \"%s\"", value);
				++errors;
			}
		}

		/* 4. pg_size_pretty */
		value = PQgetvalue(result, rowNumber, 3);
		length = strlcpy(database->bytesPretty, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Pretty printed byte size \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}
	}

	context->parsedOk = errors == 0;
}


/*
 * getDatabaseList loops over the SQL result for the database properties array
 * query and allocates an array of GUC settings then populates it with the
 * query result.
 */
static void
getDatabaseProperties(void *ctx, PGresult *result)
{
	SourcePropertiesArrayContext *context = (SourcePropertiesArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getDatabaseProperties: %d", nTuples);

	if (PQnfields(result) != 3)
	{
		log_error("Query returned %d columns, expected 3", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->gucsArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getDatabaseProperties");

		free(context->gucsArray->array);
		context->gucsArray->array = NULL;
	}

	context->gucsArray->count = nTuples;
	context->gucsArray->array =
		(SourceProperty *) calloc(nTuples, sizeof(SourceProperty));

	if (context->gucsArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	int errors = 0;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceProperty *property = &(context->gucsArray->array[rowNumber]);

		/* 1. datname */
		char *value = PQgetvalue(result, rowNumber, 0);
		int length = strlcpy(property->datname, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Properties role name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		/* 2. rolname */
		if (PQgetisnull(result, rowNumber, 1))
		{
			property->roleInDatabase = false;
		}
		else
		{
			property->roleInDatabase = true;

			value = PQgetvalue(result, rowNumber, 1);
			int length = strlcpy(property->rolname, value, PG_NAMEDATALEN);

			if (length >= PG_NAMEDATALEN)
			{
				log_error("Properties role name \"%s\" is %d bytes long, "
						  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
						  value, length, PG_NAMEDATALEN - 1);
				++errors;
			}
		}

		/* 3. setconfig */
		value = PQgetvalue(result, rowNumber, 2);
		int len = strlen(value);
		int bytes = len + 1;

		property->setconfig = (char *) calloc(bytes, sizeof(char));

		if (property->setconfig == NULL)
		{
			log_fatal(ALLOCATION_FAILED_ERROR);
			return;
		}

		strlcpy(property->setconfig, value, bytes);
	}

	context->parsedOk = errors == 0;
}


/*
 * getExtensionList loops over the SQL result for the extension array query and
 * allocates an array of extensions then populates it with the query result.
 */
static void
getExtensionList(void *ctx, PGresult *result)
{
	SourceExtensionArrayContext *context = (SourceExtensionArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getExtensionList: %d", nTuples);

	if (PQnfields(result) != 10)
	{
		log_error("Query returned %d columns, expected 10", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->extensionArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getExtensionList");

		free(context->extensionArray->array);
		context->extensionArray->array = NULL;
	}

	context->extensionArray->count = 0;
	context->extensionArray->array =
		(SourceExtension *) calloc(nTuples, sizeof(SourceExtension));

	if (context->extensionArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	bool parsedOk = true;

	int extArrayIndex = 0;
	SourceExtension *extension = NULL;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceExtension rowExtension = { 0 };
		int confIndex = 0;

		parsedOk = parsedOk &&
				   parseCurrentExtension(result, rowNumber, &rowExtension, &confIndex);

		log_trace("getExtensionList: %s [%d/%d]",
				  rowExtension.extname,
				  confIndex,
				  rowExtension.config.count);

		/*
		 * Only the first extension of a series gets into the extension list.
		 *
		 * Each extension has an array of extconfig (pg_class oids) and an
		 * array of extcondition (WHERE clauses, as text) of the same
		 * dimensions.
		 *
		 * The arrays may be empty, in which case confIndex == 0, and we can
		 * skip the extension configuration parts.
		 *
		 * The arrays may contain a single entry, in which case parsing the
		 * current row is self-contained.
		 *
		 * The arrays may contain 2 or more values, in which case the first row
		 * we read in the loop is where we build the SourceExtension structure
		 * instance, and then the next rows of the SQL query have the same
		 * first columns values and vary only in their extconfig/extcondition
		 * columns. The arrays have been UNNESTed, so each row contains the
		 * next value from the array.
		 */
		if (confIndex == 0 || confIndex == 1)
		{
			/* copy the current rowExtension into the target array entry */
			extension = &(context->extensionArray->array[extArrayIndex++]);
			*extension = rowExtension;

			/* update the extension array count too, not just the index */
			context->extensionArray->count++;
		}

		/* now loop over extension configuration, if any */
		if (extension->config.count > 0)
		{
			/* SQL arrays indexes start at 1, C arrays index start at 0 */
			if (confIndex == 1)
			{
				extension->config.array =
					(SourceExtensionConfig *)
					calloc(extension->config.count,
						   sizeof(SourceExtensionConfig));

				if (extension->config.array == NULL)
				{
					log_fatal(ALLOCATION_FAILED_ERROR);
					parsedOk = false;
					return;
				}
			}

			/* SQL arrays indexes start at 1, C arrays index start at 0 */
			SourceExtensionConfig *extConfig =
				&(extension->config.array[confIndex - 1]);

			parsedOk = parsedOk &&
					   parseCurrentExtensionConfig(result, rowNumber, extConfig);
		}
	}

	if (!parsedOk)
	{
		free(context->extensionArray->array);
		context->extensionArray->array = NULL;
	}

	context->parsedOk = parsedOk;
}


/*
 * parseCurrentExtension parses a single row of the extension listing query
 * result.
 */
static bool
parseCurrentExtension(PGresult *result,
					  int rowNumber,
					  SourceExtension *extension,
					  int *confIndex)
{
	int errors = 0;

	/* 1. oid */
	char *value = PQgetvalue(result, rowNumber, 0);

	if (!stringToUInt32(value, &(extension->oid)) || extension->oid == 0)
	{
		log_error("Invalid OID \"%s\"", value);
		++errors;
	}

	/* 2. extname */
	value = PQgetvalue(result, rowNumber, 1);
	int length = strlcpy(extension->extname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Extension name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 3. extnamespace */
	value = PQgetvalue(result, rowNumber, 2);
	length = strlcpy(extension->extnamespace, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Extension extnamespace \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 4. extrelocatable */
	value = PQgetvalue(result, rowNumber, 3);
	extension->extrelocatable = (*value) == 't';

	/* 5. array_length(extconfig), or NULL */
	if (PQgetisnull(result, rowNumber, 4))
	{
		extension->config.count = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 4);

		if (!stringToInt(value, &(extension->config.count)))
		{
			log_error("Invalid extension configuration count \"%s\"", value);
			++errors;
		}
	}

	/* 6. n (position over count), or NULL */
	if (PQgetisnull(result, rowNumber, 5))
	{
		*confIndex = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 5);

		if (!stringToInt(value, confIndex))
		{
			log_error("Invalid extension configuration index \"%s\"", value);
			++errors;
		}
	}

	return errors == 0;
}


/*
 * parseCurrentExtensionConfig parses a single row of the extension listing
 * query and adds the extconfig and extcondition columns to the given array
 * entry of SourceExtensionConfig.
 */
static bool
parseCurrentExtensionConfig(PGresult *result,
							int rowNumber,
							SourceExtensionConfig *extConfig)
{
	int errors = 0;

	/* 7. extconfig (pg_class oid) */
	char *value = PQgetvalue(result, rowNumber, 6);

	if (!stringToUInt32(value, &(extConfig->oid)))
	{
		log_error("Invalid extension configuration OID \"%s\"", value);
		++errors;
	}

	/* 8. n.nspname */
	value = PQgetvalue(result, rowNumber, 7);
	int length = strlcpy(extConfig->nspname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Schema name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 9. c.relname */
	value = PQgetvalue(result, rowNumber, 8);
	length = strlcpy(extConfig->relname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Extension configuration table name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 10. extcondition */
	value = PQgetvalue(result, rowNumber, 9);
	extConfig->condition = strdup(value);

	if (extConfig->condition == NULL)
	{
		log_error(ALLOCATION_FAILED_ERROR);
		++errors;
	}

	return errors == 0;
}


/*
 * getExtensionsVersions loops over the SQL result for the available extension
 * versions list.
 */
static void
getExtensionsVersions(void *ctx, PGresult *result)
{
	ExtensionsVersionsArrayContext *context =
		(ExtensionsVersionsArrayContext *) ctx;

	int nTuples = PQntuples(result);

	log_debug("getExtensionsVersions: %d", nTuples);

	if (PQnfields(result) != 4)
	{
		log_error("Query returned %d columns, expected 4", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->evArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getExtensionsVersions");

		free(context->evArray->array);
		context->evArray->array = NULL;
	}

	context->evArray->count = nTuples;
	context->evArray->array =
		(ExtensionsVersions *) calloc(nTuples, sizeof(ExtensionsVersions));

	if (context->evArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	int errors = 0;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		ExtensionsVersions *ev = &(context->evArray->array[rowNumber]);

		/* 1. name */
		char *value = PQgetvalue(result, rowNumber, 0);
		int length = strlcpy(ev->name, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Extension name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		/* 2. defaultVersion */
		value = PQgetvalue(result, rowNumber, 1);
		length = strlcpy(ev->defaultVersion, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Extension version \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		/* 3. installedVersion */
		value = PQgetvalue(result, rowNumber, 2);
		length = strlcpy(ev->installedVersion, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Extension version \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		/* 4. versions JSON array */
		if (!PQgetisnull(result, rowNumber, 3))
		{
			value = PQgetvalue(result, rowNumber, 3);
			ev->json = json_parse_string(value);

			if (ev->json == NULL || json_type(ev->json) != JSONArray)
			{
				log_error("Failed to parse extension \"%s\" available versions "
						  "JSON array: %s",
						  ev->name, value);
				++errors;
			}
		}
	}

	context->parsedOk = errors == 0;
}


/*
 * getCollationList loops over the SQL result for the collation array query and
 * allocates an array of schemas then populates it with the query result.
 */
static void
getCollationList(void *ctx, PGresult *result)
{
	SourceCollationArrayContext *context = (SourceCollationArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getCollationList: %d", nTuples);

	if (PQnfields(result) != 4)
	{
		log_error("Query returned %d columns, expected 4", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->collationArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getCollationList");

		free(context->collationArray->array);
		context->collationArray->array = NULL;
	}

	context->collationArray->count = nTuples;
	context->collationArray->array =
		(SourceCollation *) calloc(nTuples, sizeof(SourceCollation));

	if (context->collationArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	int errors = 0;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceCollation *collation = &(context->collationArray->array[rowNumber]);

		/* 1. oid */
		char *value = PQgetvalue(result, rowNumber, 0);

		if (!stringToUInt32(value, &(collation->oid)) || collation->oid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}

		/* 2. collname */
		value = PQgetvalue(result, rowNumber, 1);
		int length = strlcpy(collation->collname, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Collation name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}

		/* 3. desc */
		value = PQgetvalue(result, rowNumber, 2);
		length = strlen(value) + 1;
		collation->desc = (char *) calloc(length, sizeof(char));

		if (collation->desc == NULL)
		{
			log_fatal(ALLOCATION_FAILED_ERROR);
			return;
		}

		strlcpy(collation->desc, value, length);

		/* 4. restoreListName */
		value = PQgetvalue(result, rowNumber, 3);
		length =
			strlcpy(collation->restoreListName,
					value,
					RESTORE_LIST_NAMEDATALEN);

		if (length >= RESTORE_LIST_NAMEDATALEN)
		{
			log_error("Collation restore list name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (RESTORE_LIST_NAMEDATALEN - 1)",
					  value, length, RESTORE_LIST_NAMEDATALEN - 1);
			++errors;
		}
	}

	context->parsedOk = errors == 0;
}


/*
 * getTableArray loops over the SQL result for the tables array query and
 * allocates an array of tables then populates it with the query result.
 */
static void
getTableArray(void *ctx, PGresult *result)
{
	SourceTableArrayContext *context = (SourceTableArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getTableArray: %d", nTuples);

	if (PQnfields(result) != 12)
	{
		log_error("Query returned %d columns, expected 12", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->tableArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getTableArray");

		free(context->tableArray->array);
		context->tableArray->array = NULL;
	}

	context->tableArray->count = nTuples;
	context->tableArray->array =
		(SourceTable *) calloc(nTuples, sizeof(SourceTable));

	if (context->tableArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	bool parsedOk = true;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceTable *table = &(context->tableArray->array[rowNumber]);

		parsedOk = parsedOk &&
				   parseCurrentSourceTable(result, rowNumber, table);
	}

	if (!parsedOk)
	{
		free(context->tableArray->array);
		context->tableArray->array = NULL;
	}

	context->parsedOk = parsedOk;
}


/*
 * parseCurrentSourceTable parses a single row of the table listing query
 * result.
 */
static bool
parseCurrentSourceTable(PGresult *result, int rowNumber, SourceTable *table)
{
	int errors = 0;

	int fnoid = PQfnumber(result, "oid");
	int fnnspname = PQfnumber(result, "nspname");
	int fnrelname = PQfnumber(result, "relname");
	int fnamname = PQfnumber(result, "amname");
	int fnrelpages = PQfnumber(result, "relpages");
	int fnreltuples = PQfnumber(result, "reltuples");
	int fnbytes = PQfnumber(result, "bytes");
	int fnbytespretty = PQfnumber(result, "pg_size_pretty");
	int fnexcldata = PQfnumber(result, "excludedata");
	int fnrestorelistname = PQfnumber(result, "format");
	int fnpartkey = PQfnumber(result, "partkey");
	int fnattrs = PQfnumber(result, "attributes");

	/* c.oid */
	char *value = PQgetvalue(result, rowNumber, fnoid);

	if (!stringToUInt32(value, &(table->oid)) || table->oid == 0)
	{
		log_error("Invalid OID \"%s\"", value);
		++errors;
	}

	/* n.nspname */
	value = PQgetvalue(result, rowNumber, fnnspname);
	int length = strlcpy(table->nspname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Schema name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* c.relname */
	value = PQgetvalue(result, rowNumber, fnrelname);
	length = strlcpy(table->relname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Table name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* compute the qualified name from the nspname and relname */
	length = sformat(table->qname, sizeof(table->qname), "%s.%s",
					 table->nspname,
					 table->relname);

	if (length >= sizeof(table->qname))
	{
		log_error("Qualified table name \"%s\".\"%s\" is %d bytes long, "
				  "the maximum expected is %lld",
				  table->nspname,
				  table->relname,
				  length,
				  (long long) sizeof(table->qname) - 1);
		++errors;
	}

	/* pgam_amname */
	if (PQgetisnull(result, rowNumber, fnamname))
	{
		/* table started having an amname in Postgres 12 */
		strlcpy(table->amname, "heap", PG_NAMEDATALEN);
	}
	else
	{
		value = PQgetvalue(result, rowNumber, fnamname);
		length = strlcpy(table->amname, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Access Method name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}
	}

	/* c.relpages */
	if (PQgetisnull(result, rowNumber, fnrelpages))
	{
		/*
		 * reltuples is NULL when table has never been ANALYZEd, just count
		 * zero then.
		 */
		table->relpages = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, fnrelpages);

		if (!stringToInt64(value, &(table->relpages)))
		{
			log_error("Invalid relpages \"%s\"", value);
			++errors;
		}
	}

	/* c.reltuples::bigint */
	if (PQgetisnull(result, rowNumber, fnreltuples))
	{
		/*
		 * reltuples is NULL when table has never been ANALYZEd, just count
		 * zero then.
		 */
		table->reltuples = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, fnreltuples);

		if (!stringToInt64(value, &(table->reltuples)))
		{
			log_error("Invalid reltuples::bigint \"%s\"", value);
			++errors;
		}
	}

	/* pg_table_size(c.oid) as bytes */
	if (PQgetisnull(result, rowNumber, fnbytes))
	{
		/*
		 * It may happen that pg_table_size() returns NULL (when failing to
		 * open the given relation).
		 */
		table->bytes = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, fnbytes);

		if (!stringToInt64(value, &(table->bytes)))
		{
			log_error("Invalid reltuples::bigint \"%s\"", value);
			++errors;
		}
	}

	/* pg_size_pretty(c.oid) */
	value = PQgetvalue(result, rowNumber, fnbytespretty);
	length = strlcpy(table->bytesPretty, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Pretty printed byte size \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* excludeData */
	value = PQgetvalue(result, rowNumber, fnexcldata);
	table->excludeData = (*value) == 't';

	/* restoreListName */
	value = PQgetvalue(result, rowNumber, fnrestorelistname);
	length = strlcpy(table->restoreListName, value, RESTORE_LIST_NAMEDATALEN);

	if (length >= RESTORE_LIST_NAMEDATALEN)
	{
		log_error("Table restore list name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (RESTORE_LIST_NAMEDATALEN - 1)",
				  value, length, RESTORE_LIST_NAMEDATALEN - 1);
		++errors;
	}

	/* partkey */
	if (PQgetisnull(result, rowNumber, fnpartkey))
	{
		log_debug("Table %s with oid %u has not part key column",
				  table->qname,
				  table->oid);
	}
	else
	{
		value = PQgetvalue(result, rowNumber, fnpartkey);
		length = strlcpy(table->partKey, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Partition key column name %s is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}
	}

	/* attributes */
	if (PQgetisnull(result, rowNumber, fnattrs))
	{
		/* the query didn't care to add the attributes, skip parsing them */
		table->attributes.count = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, fnattrs);

		JSON_Value *json = json_parse_string(value);

		if (!parseAttributesArray(table, json))
		{
			log_error("Failed to parse table %s attribute array: %s",
					  table->qname,
					  value);
			++errors;
		}

		json_value_free(json);
	}

	log_trace("parseCurrentSourceTable: %s.%s", table->nspname, table->relname);

	return errors == 0;
}


/*
 * parseAttributesArray parses a JSON representation of table list of
 * attributes and allocates the table's attribute array.
 */
static bool
parseAttributesArray(SourceTable *table, JSON_Value *json)
{
	if (json == NULL || json_type(json) != JSONArray)
	{
		return false;
	}

	JSON_Array *jsAttsArray = json_array(json);

	int count = json_array_get_count(jsAttsArray);

	table->attributes.count = count;
	table->attributes.array =
		(SourceTableAttribute *) calloc(count, sizeof(SourceTableAttribute));

	if (table->attributes.array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return false;
	}

	for (int i = 0; i < count; i++)
	{
		SourceTableAttribute *attr = &(table->attributes.array[i]);
		JSON_Object *jsAttr = json_array_get_object(jsAttsArray, i);

		attr->attnum = json_object_get_number(jsAttr, "attnum");
		attr->atttypid = json_object_get_number(jsAttr, "atttypid");

		strlcpy(attr->attname,
				json_object_get_string(jsAttr, "attname"),
				sizeof(attr->attname));

		attr->attisprimary = json_object_get_boolean(jsAttr, "attisprimary");
		attr->attisgenerated = json_object_get_boolean(jsAttr, "attisgenerated");
	}

	return true;
}


/*
 * getSequenceArray loops over the SQL result for the sequence array query and
 * allocates an array of tables then populates it with the query result.
 */
static void
getSequenceArray(void *ctx, PGresult *result)
{
	SourceSequenceArrayContext *context = (SourceSequenceArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getSequenceArray: %d", nTuples);

	if (PQnfields(result) != 7)
	{
		log_error("Query returned %d columns, expected 7", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->sequenceArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getSequenceArray");

		free(context->sequenceArray->array);
		context->sequenceArray->array = NULL;
	}

	context->sequenceArray->count = nTuples;
	context->sequenceArray->array =
		(SourceSequence *) calloc(nTuples, sizeof(SourceSequence));

	if (context->sequenceArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	bool parsedOk = true;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceSequence *sequence = &(context->sequenceArray->array[rowNumber]);

		parsedOk = parsedOk &&
				   parseCurrentSourceSequence(result, rowNumber, sequence);
	}

	if (!parsedOk)
	{
		free(context->sequenceArray->array);
		context->sequenceArray->array = NULL;
	}

	context->parsedOk = parsedOk;
}


/*
 * parseCurrentSourceSequence parses a single row of the sequence listing query
 * result.
 */
static bool
parseCurrentSourceSequence(PGresult *result, int rowNumber, SourceSequence *seq)
{
	int errors = 0;

	/* 1. c.oid */
	char *value = PQgetvalue(result, rowNumber, 0);

	if (!stringToUInt32(value, &(seq->oid)) || seq->oid == 0)
	{
		log_error("Invalid OID \"%s\"", value);
		++errors;
	}

	/* 2. n.nspname */
	value = PQgetvalue(result, rowNumber, 1);
	int length = strlcpy(seq->nspname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Schema name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 3. c.relname */
	value = PQgetvalue(result, rowNumber, 2);
	length = strlcpy(seq->relname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Sequence name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* compute the qualified name from the nspname and relname */
	length = sformat(seq->qname, sizeof(seq->qname), "%s.%s",
					 seq->nspname,
					 seq->relname);

	if (length >= sizeof(seq->qname))
	{
		log_error("Qualified seq name \"%s\".\"%s\" is %d bytes long, "
				  "the maximum expected is %lld",
				  seq->nspname,
				  seq->relname,
				  length,
				  (long long) sizeof(seq->qname) - 1);
		++errors;
	}

	/* 4. restoreListName */
	value = PQgetvalue(result, rowNumber, 3);
	length = strlcpy(seq->restoreListName, value, RESTORE_LIST_NAMEDATALEN);

	if (length >= RESTORE_LIST_NAMEDATALEN)
	{
		log_error("Table restore list name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (RESTORE_LIST_NAMEDATALEN - 1)",
				  value, length, RESTORE_LIST_NAMEDATALEN - 1);
		++errors;
	}

	/* 5. ownedby */
	if (PQgetisnull(result, rowNumber, 4))
	{
		seq->ownedby = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 4);

		if (!stringToUInt32(value, &(seq->ownedby)) || seq->ownedby == 0)
		{
			log_error("Invalid pg_class OID for ownedby: \"%s\"", value);
			++errors;
		}
	}

	/* 6. attrelid */
	if (PQgetisnull(result, rowNumber, 5))
	{
		seq->ownedby = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 5);

		if (!stringToUInt32(value, &(seq->attrelid)) || seq->attrelid == 0)
		{
			log_error("Invalid pg_class OID for attrelid: \"%s\"", value);
			++errors;
		}
	}

	/* 6. attroid */
	if (PQgetisnull(result, rowNumber, 6))
	{
		seq->attroid = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 6);

		if (!stringToUInt32(value, &(seq->attroid)) || seq->attroid == 0)
		{
			log_error("Invalid pg_attribute OID \"%s\"", value);
			++errors;
		}
	}

	return errors == 0;
}


/*
 * getIndexArray loops over the SQL result for the index array query and
 * allocates an array of tables then populates it with the query result.
 */
static void
getIndexArray(void *ctx, PGresult *result)
{
	SourceIndexArrayContext *context = (SourceIndexArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getIndexArray: %d", nTuples);

	if (PQnfields(result) != 16)
	{
		log_error("Query returned %d columns, expected 16", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->indexArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getIndexArray");

		free(context->indexArray->array);
		context->indexArray->array = NULL;
	}

	context->indexArray->count = nTuples;
	context->indexArray->array =
		(SourceIndex *) calloc(nTuples, sizeof(SourceIndex));

	if (context->indexArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	bool parsedOk = true;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceIndex *index = &(context->indexArray->array[rowNumber]);

		parsedOk = parsedOk &&
				   parseCurrentSourceIndex(result, rowNumber, index);
	}

	if (!parsedOk)
	{
		free(context->indexArray->array);
		context->indexArray->array = NULL;
	}

	context->parsedOk = parsedOk;
}


/*
 * parseCurrentSourceIndex parses a single row of the index listing query
 * result.
 */
static bool
parseCurrentSourceIndex(PGresult *result, int rowNumber, SourceIndex *index)
{
	int errors = 0;

	/* 1. i.oid */
	char *value = PQgetvalue(result, rowNumber, 0);

	if (!stringToUInt32(value, &(index->indexOid)) || index->indexOid == 0)
	{
		log_error("Invalid index OID \"%s\"", value);
		++errors;
	}

	/* 2. n.nspname */
	value = PQgetvalue(result, rowNumber, 1);
	int length = strlcpy(index->indexNamespace, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Schema name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 3. i.relname */
	value = PQgetvalue(result, rowNumber, 2);
	length = strlcpy(index->indexRelname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Index name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* compute the qualified name from the nspname and relname */
	length = sformat(index->indexQname, sizeof(index->indexQname), "%s.%s",
					 index->indexNamespace,
					 index->indexRelname);

	if (length >= sizeof(index->tableQname))
	{
		log_error("Qualified index name \"%s\".\"%s\" is %d bytes long, "
				  "the maximum expected is %lld",
				  index->indexNamespace,
				  index->indexRelname,
				  length,
				  (long long) sizeof(index->tableQname) - 1);
		++errors;
	}

	/* 4. r.oid */
	value = PQgetvalue(result, rowNumber, 3);

	if (!stringToUInt32(value, &(index->tableOid)) || index->tableOid == 0)
	{
		log_error("Invalid OID \"%s\"", value);
		++errors;
	}

	/* 5. rn.nspname */
	value = PQgetvalue(result, rowNumber, 4);
	length = strlcpy(index->tableNamespace, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Schema name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 6. r.relname */
	value = PQgetvalue(result, rowNumber, 5);
	length = strlcpy(index->tableRelname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Index name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* compute the qualified name from the nspname and relname */
	length = sformat(index->tableQname, sizeof(index->tableQname), "%s.%s",
					 index->tableNamespace,
					 index->tableRelname);

	if (length >= sizeof(index->tableQname))
	{
		log_error("Qualified table name \"%s\".\"%s\" is %d bytes long, "
				  "the maximum expected is %lld",
				  index->tableNamespace,
				  index->tableRelname,
				  length,
				  (long long) sizeof(index->tableQname) - 1);
		++errors;
	}

	/* 7. indisprimary */
	value = PQgetvalue(result, rowNumber, 6);
	if (value == NULL || ((*value != 't') && (*value != 'f')))
	{
		log_error("Invalid indisprimary value \"%s\"", value);
		++errors;
	}
	else
	{
		index->isPrimary = (*value) == 't';
	}

	/* 8. indisunique */
	value = PQgetvalue(result, rowNumber, 7);
	if (value == NULL || ((*value != 't') && (*value != 'f')))
	{
		log_error("Invalid indisunique value \"%s\"", value);
		++errors;
	}
	else
	{
		index->isUnique = (*value) == 't';
	}

	/* 9. cols */
	value = PQgetvalue(result, rowNumber, 8);
	length = strlen(value) + 1;
	index->indexColumns = (char *) calloc(length, sizeof(char));

	if (index->indexColumns == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return false;
	}

	strlcpy(index->indexColumns, value, length);

	/* 10. pg_get_indexdef() */
	value = PQgetvalue(result, rowNumber, 9);
	length = strlen(value) + 1;
	index->indexDef = (char *) calloc(length, sizeof(char));

	if (index->indexDef == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return false;
	}

	strlcpy(index->indexDef, value, length);

	/* 11. c.oid */
	if (PQgetisnull(result, rowNumber, 10))
	{
		index->constraintOid = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 10);

		if (!stringToUInt32(value, &(index->constraintOid)) ||
			index->constraintOid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}
	}

	/* 12. c.condeferrable */
	if (!PQgetisnull(result, rowNumber, 11))
	{
		value = PQgetvalue(result, rowNumber, 11);
		if (value == NULL || ((*value != 't') && (*value != 'f')))
		{
			log_error("Invalid condeferrable value \"%s\"", value);
			++errors;
		}
		else
		{
			index->condeferrable = (*value) == 't';
		}
	}

	/* 13. c.condeferred */
	if (!PQgetisnull(result, rowNumber, 12))
	{
		value = PQgetvalue(result, rowNumber, 12);
		if (value == NULL || ((*value != 't') && (*value != 'f')))
		{
			log_error("Invalid condeferred value \"%s\"", value);
			++errors;
		}
		else
		{
			index->condeferred = (*value) == 't';
		}
	}

	/* 14. conname */
	if (!PQgetisnull(result, rowNumber, 13))
	{
		value = PQgetvalue(result, rowNumber, 13);
		length = strlcpy(index->constraintName, value, PG_NAMEDATALEN);

		if (length >= PG_NAMEDATALEN)
		{
			log_error("Index name \"%s\" is %d bytes long, "
					  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
					  value, length, PG_NAMEDATALEN - 1);
			++errors;
		}
	}

	/* 15. pg_get_constraintdef */
	if (!PQgetisnull(result, rowNumber, 14))
	{
		value = PQgetvalue(result, rowNumber, 14);
		length = strlen(value) + 1;
		index->constraintDef = (char *) calloc(length, sizeof(char));

		if (index->constraintDef == NULL)
		{
			log_fatal(ALLOCATION_FAILED_ERROR);
			return false;
		}

		strlcpy(index->constraintDef, value, length);
	}

	/* 16. indexRestoreListName */
	value = PQgetvalue(result, rowNumber, 15);
	length =
		strlcpy(index->indexRestoreListName, value, RESTORE_LIST_NAMEDATALEN);

	if (length >= RESTORE_LIST_NAMEDATALEN)
	{
		log_error("Index restore list name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (RESTORE_LIST_NAMEDATALEN - 1)",
				  value, length, RESTORE_LIST_NAMEDATALEN - 1);
		++errors;
	}

	return errors == 0;
}


/*
 * getDependArray loops over the SQL result for the table dependencies array
 * query and allocates an array of tables then populates it with the query
 * result.
 */
static void
getDependArray(void *ctx, PGresult *result)
{
	SourceDependArrayContext *context = (SourceDependArrayContext *) ctx;
	int nTuples = PQntuples(result);

	log_debug("getDependArray: %d", nTuples);

	if (PQnfields(result) != 9)
	{
		log_error("Query returned %d columns, expected 9", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->dependArray->array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's array is not null in getDependArray");

		free(context->dependArray->array);
		context->dependArray->array = NULL;
	}

	context->dependArray->count = nTuples;
	context->dependArray->array =
		(SourceDepend *) calloc(nTuples, sizeof(SourceDepend));

	if (context->dependArray->array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	bool parsedOk = true;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceDepend *depend = &(context->dependArray->array[rowNumber]);

		parsedOk = parsedOk &&
				   parseCurrentSourceDepend(result, rowNumber, depend);
	}

	if (!parsedOk)
	{
		free(context->dependArray->array);
		context->dependArray->array = NULL;
	}

	context->parsedOk = parsedOk;
}


/*
 * parseCurrentSourceDepend parses a single row of the dependency listing query
 * result.
 */
static bool
parseCurrentSourceDepend(PGresult *result, int rowNumber, SourceDepend *depend)
{
	int errors = 0;

	/* 1. n.nspname */
	char *value = PQgetvalue(result, rowNumber, 0);
	int length = strlcpy(depend->nspname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Schema name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 2. c.relname */
	value = PQgetvalue(result, rowNumber, 1);
	length = strlcpy(depend->relname, value, PG_NAMEDATALEN);

	if (length >= PG_NAMEDATALEN)
	{
		log_error("Table name \"%s\" is %d bytes long, "
				  "the maximum expected is %d (PG_NAMEDATALEN - 1)",
				  value, length, PG_NAMEDATALEN - 1);
		++errors;
	}

	/* 3. refclassid */
	if (PQgetisnull(result, rowNumber, 2))
	{
		depend->refclassid = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 2);

		if (!stringToUInt32(value, &(depend->refclassid)) || depend->refclassid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}
	}

	/* 4. refobjid */
	if (PQgetisnull(result, rowNumber, 3))
	{
		depend->refobjid = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 3);

		if (!stringToUInt32(value, &(depend->refobjid)) || depend->refobjid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}
	}

	/* 5. classid */
	if (PQgetisnull(result, rowNumber, 4))
	{
		depend->classid = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 4);

		if (!stringToUInt32(value, &(depend->classid)) || depend->classid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}
	}

	/* 6. objid */
	if (PQgetisnull(result, rowNumber, 5))
	{
		depend->objid = 0;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 5);

		if (!stringToUInt32(value, &(depend->objid)) || depend->objid == 0)
		{
			log_error("Invalid OID \"%s\"", value);
			++errors;
		}
	}

	/* 7. deptype */
	if (PQgetisnull(result, rowNumber, 6))
	{
		depend->deptype = 's';  /* invent something for schemas */
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 6);
		depend->deptype = value[0];
	}

	/* 8. type */
	value = PQgetvalue(result, rowNumber, 7);
	length = strlcpy(depend->type, value, BUFSIZE);

	if (length >= BUFSIZE)
	{
		log_error("Table dependency type \"%s\" is %d bytes long, "
				  "the maximum expected is %d (BUFSIZE - 1)",
				  value, length, BUFSIZE - 1);
		++errors;
	}

	/* 9. identity */
	value = PQgetvalue(result, rowNumber, 8);
	length = strlcpy(depend->identity, value, BUFSIZE);

	if (length >= BUFSIZE)
	{
		log_error("Table dependency identity \"%s\" is %d bytes long, "
				  "the maximum expected is %d (BUFSIZE - 1)",
				  value, length, BUFSIZE - 1);
		++errors;
	}

	return errors == 0;
}


/*
 * getPartitionList loops over the SQL result for the COPY partitions query and
 * allocate an array of SourceTableParts and populates it with the query
 * results.
 */
static void
getPartitionList(void *ctx, PGresult *result)
{
	SourcePartitionContext *context = (SourcePartitionContext *) ctx;
	int nTuples = PQntuples(result);

	if (PQnfields(result) != 5)
	{
		log_error("Query returned %d columns, expected 5", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	/* we're not supposed to re-cycle arrays here */
	if (context->table->partsArray.array != NULL)
	{
		/* issue a warning but let's try anyway */
		log_warn("BUG? context's partsArray is not null in getPartitionList");

		free(context->table->partsArray.array);
		context->table->partsArray.array = NULL;
		context->table->partsArray.count = 0;
	}

	context->table->partsArray.count = nTuples;
	context->table->partsArray.array =
		(SourceTableParts *) calloc(nTuples, sizeof(SourceTableParts));

	if (context->table->partsArray.array == NULL)
	{
		log_fatal(ALLOCATION_FAILED_ERROR);
		return;
	}

	bool parsedOk = true;

	for (int rowNumber = 0; rowNumber < nTuples; rowNumber++)
	{
		SourceTableParts *parts = &(context->table->partsArray.array[rowNumber]);

		parsedOk = parsedOk &&
				   parseCurrentPartition(result, rowNumber, parts);
	}

	if (!parsedOk)
	{
		free(context->table->partsArray.array);
		context->table->partsArray.array = NULL;
		context->table->partsArray.count = 0;
	}

	context->parsedOk = parsedOk;
}


/*
 * parseCurrentPartition parses a single row of the table COPY partition
 * listing query result.
 */
static bool
parseCurrentPartition(PGresult *result, int rowNumber, SourceTableParts *parts)
{
	int errors = 0;

	/* 1. partNumber */
	char *value = PQgetvalue(result, rowNumber, 0);

	if (!stringToInt(value, &(parts->partNumber)))
	{
		log_error("Invalid part number \"%s\"", value);
		++errors;
	}

	/* 2. partCount */
	value = PQgetvalue(result, rowNumber, 1);

	if (!stringToInt(value, &(parts->partCount)))
	{
		log_error("Invalid part count \"%s\"", value);
		++errors;
	}

	/* 3. min */
	value = PQgetvalue(result, rowNumber, 2);

	if (!stringToInt64(value, &(parts->min)))
	{
		log_error("Invalid part min \"%s\"", value);
		++errors;
	}

	/* 4. max */
	if (PQgetisnull(result, rowNumber, 3))
	{
		parts->max = -1;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 3);

		if (!stringToInt64(value, &(parts->max)))
		{
			log_error("Invalid part max \"%s\"", value);
			++errors;
		}
	}

	/* 5. count */
	if (PQgetisnull(result, rowNumber, 4))
	{
		parts->count = -1;
	}
	else
	{
		value = PQgetvalue(result, rowNumber, 4);

		if (!stringToInt64(value, &(parts->count)))
		{
			log_error("Invalid part count \"%s\"", value);
			++errors;
		}
	}

	return errors == 0;
}


/*
 * getTableChecksum assigns the rowcount and checksum fields of a table from
 * the result of an SQL query.
 */
static void
getTableChecksum(void *ctx, PGresult *result)
{
	ChecksumContext *context = (ChecksumContext *) ctx;

	int nTuples = PQntuples(result);
	int errors = 0;

	if (nTuples != 1)
	{
		log_error("Query returned %d columns, expected 1", nTuples);
		context->parsedOk = false;
		return;
	}

	if (PQnfields(result) != 2)
	{
		log_error("Query returned %d columns, expected 2", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	TableChecksum *sum = context->sum;

	/* 1. count */
	char *value = PQgetvalue(result, 0, 0);

	if (!stringToUInt64(value, &(sum->rowcount)))
	{
		log_error("Invalid row count value: \"%s\"", value);
		++errors;
	}

	value = PQgetvalue(result, 0, 1);
	strlcpy(sum->checksum, value, CHECKSUMLEN);

	context->parsedOk = errors == 0;
}
