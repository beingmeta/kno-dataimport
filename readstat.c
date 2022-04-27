/* -*- Mode: C; Character-encoding: utf-8; -*- */

/* readstat.c
   This implements Kno bindings to the ReadStat C library
   Copyright (C) 2007-2019 beingmeta, inc.
   Copyright (C) 2020-2021 beingmeta, LLC
*/

#ifndef _FILEINFO
#define _FILEINFO __FILE__
#endif

#define U8_INLINE_IO 1
#define KNO_DEFINE_GETOPT 1

#include "kno/knosource.h"
#include "kno/lisp.h"
#include "kno/numbers.h"
#include "kno/eval.h"
#include "kno/sequences.h"
#include "kno/storage.h"
#include "kno/texttools.h"
#include "kno/cprims.h"

#include "kno/sql.h"

#include <libu8/libu8.h>
#include <libu8/u8printf.h>
#include <libu8/u8crypto.h>

#include <math.h>
#include <limits.h>
#include <readstat.h>

KNO_EXPORT int kno_init_readstat(void) KNO_LIBINIT_FN;

kno_lisp_type kno_readstat_type;
#define KNO_READSTAT_TYPE     0xEC7900L

static lispval readstat_module;

typedef struct KNO_READSTAT {
  KNO_ANNOTATED_HEADER;
  u8_string rs_source;
  u8_context rs_type;
  unsigned int rs_bits;
  int rs_n_vars;
  readstat_parser_t *rs_parser;
  struct KNO_SCHEMAP *rs_dataframe;
  lispval *rs_values;
  long long rs_obvsid;
  struct KNO_SCHEMAP *rs_observation;
  int rs_seen_count;
  lispval rs_output;
  lispval rs_callback;} *kno_readstat;

DEF_KNOSYM(nvars); DEF_KNOSYM(rows); DEF_KNOSYM(label); DEF_KNOSYM(tablename);
DEF_KNOSYM(encname); DEF_KNOSYM(is64bit);
DEF_KNOSYM(formatversion); DEF_KNOSYM(name);
DEF_KNOSYM(string); DEF_KNOSYM(stringref);
DEF_KNOSYM(int8); DEF_KNOSYM(int16); DEF_KNOSYM(int32);
DEF_KNOSYM(float); DEF_KNOSYM(double);
DEF_KNOSYM(callback); DEF_KNOSYM(aggregate);

static void store_string(lispval table,lispval slotid,u8_string value)
{
  lispval string = knostring(value);
  kno_store(table,slotid,string);
  kno_decref(string);
}

static u8_context get_readstat_typename(readstat_type_t type)
{
  switch (type) {
  case READSTAT_TYPE_STRING:
    return "READSTAT_TYPE_STRING";
  case READSTAT_TYPE_INT8:
    return "READSTAT_TYPE_INT8";
  case READSTAT_TYPE_INT16:
    return "READSTAT_TYPE_INT16";
  case READSTAT_TYPE_INT32:
    return "READSTAT_TYPE_INT32";
  case READSTAT_TYPE_FLOAT:
    return "READSTAT_TYPE_FLOAT";
  case READSTAT_TYPE_DOUBLE:
    return "READSTAT_TYPE_DOUBLE";
  case READSTAT_TYPE_STRING_REF:
    return "READSTAT_TYPE_STRING_REF";
  default:
    return "Bad type";
  }
}

static lispval get_readstat_typesym(readstat_type_t type)
{
  switch (type) {
  case READSTAT_TYPE_STRING:
    return KNOSYM(string);
  case READSTAT_TYPE_INT8:
    return KNOSYM(int8);
  case READSTAT_TYPE_INT16:
    return KNOSYM(int16);
  case READSTAT_TYPE_INT32:
    return KNOSYM(int32);
  case READSTAT_TYPE_FLOAT:
    return KNOSYM(float);
  case READSTAT_TYPE_DOUBLE:
    return KNOSYM(double);
  case READSTAT_TYPE_STRING_REF:
    return KNOSYM(stringref);
  default:
    return kno_intern("BadType");
  }
}

static u8_string get_valstring(readstat_value_t *val)
{
  readstat_type_t valtype = val->type;
  switch (valtype) {
  case READSTAT_TYPE_STRING:
    return u8_strdup(val->v.string_value);
  case READSTAT_TYPE_INT8:
    return u8_mkstring("%d",val->v.i8_value);
  case READSTAT_TYPE_INT16:
    return u8_mkstring("%d",val->v.i16_value);
  case READSTAT_TYPE_INT32:
    return u8_mkstring("%d",val->v.i32_value);
  case READSTAT_TYPE_FLOAT:
    return u8_mkstring("%f",val->v.float_value);
  case READSTAT_TYPE_DOUBLE:
    return u8_mkstring("%f",val->v.double_value);
  case READSTAT_TYPE_STRING_REF:
    return u8_strdup(val->v.string_value);
  default:
    return "Bad type";
  }
}

static lispval get_lisp_value(readstat_value_t *val)
{
  readstat_type_t valtype = val->type;
  switch (valtype) {
  case READSTAT_TYPE_STRING:
    return knostring(val->v.string_value);
  case READSTAT_TYPE_INT8:
    return KNO_INT(val->v.i8_value);
  case READSTAT_TYPE_INT16:
    return KNO_INT(val->v.i16_value);
  case READSTAT_TYPE_INT32:
    return KNO_INT(val->v.i32_value);
  case READSTAT_TYPE_FLOAT:
    return kno_make_flonum(val->v.float_value);
  case READSTAT_TYPE_DOUBLE:
    return kno_make_flonum(val->v.double_value);
  case READSTAT_TYPE_STRING_REF:
    return knostring(val->v.string_value);
  default:
    return KNO_VOID;
  }
}

#define KNO_DATAFRAME_TEMPLATE_FLAGS \
  (KNO_SCHEMAP_FIXED_SCHEMA|KNO_SCHEMAP_DATAFRAME)

static int metadata_handler(readstat_metadata_t *md,void *state)
{
  struct KNO_READSTAT *rs = (kno_readstat) state;
  int n_vars = md->var_count;
  lispval *schema = u8_alloc_n(n_vars,lispval);
  int i = 0; while (i<n_vars) {schema[i]=KNO_INT2FIX(i); i++;}
  struct KNO_SCHEMAP *template;
  lispval dfptr = kno_make_schemap
    (NULL,n_vars,KNO_DATAFRAME_TEMPLATE_FLAGS,schema,NULL);
  if (KNO_ABORTED(dfptr)) {
    u8_free(schema);
    return dfptr;}
  else template= (kno_schemap) dfptr;
  rs->rs_dataframe = template;
  rs->rs_n_vars    = n_vars;
  lispval annotations = rs->annotations;
  if (md->var_count>=0) {
    kno_store(annotations,KNOSYM(nvars),KNO_INT(md->var_count));}
  if (md->row_count>=0) {
    kno_store(annotations,KNOSYM(rows),KNO_INT(md->row_count));}
  if (md->table_name)
    store_string(annotations,KNOSYM(tablename),md->table_name);
  if (md->file_label)
    store_string(annotations,KNOSYM(label),md->file_label);
  if (md->file_encoding)
    store_string(annotations,KNOSYM(encname),md->file_encoding);
  kno_store(annotations,KNOSYM(is64bit),(md->is64bit)?(KNO_TRUE):(KNO_FALSE));
  if (md->file_format_version>0)
    kno_store(annotations,KNOSYM(formatversion),KNO_INT(md->file_format_version));
  return READSTAT_HANDLER_OK;
}

static int log_metadata_handler(readstat_metadata_t *md,void *ignored)
{
  u8_log(LOGWARN,"ReadStatMetadata",
	 "md=%p rows=%lld cols=%lld version=%lld name=%s label=%s enc=%s",
	 md,md->row_count,md->var_count,md->file_format_version,
	 md->table_name,md->file_label,md->file_encoding);
  return READSTAT_HANDLER_OK;
}

static int variable_handler(int ix,readstat_variable_t *vd,
			    const char *labels,void *state)
{
  struct KNO_READSTAT *rs = (kno_readstat) state;
  struct KNO_SCHEMAP *template = rs->rs_dataframe;
  lispval *schema = template->table_schema;
  lispval *values = template->table_values;
  int n = template->schema_length, i = vd->index;
  if (i>n) {
    u8_byte details_buf[200];
    kno_seterr("ReadStatError","variable_handler",
	       u8_bprintf(details_buf,"Index %d for %s is too big (> %d)",
			  i,vd->name,n),
	       KNO_VOID);
    return -1;}
  lispval slotid = kno_intern(vd->name);
  lispval slot_info = kno_make_slotmap(7,0,NULL);
  schema[i]=slotid;
  values[i]=slot_info;
  kno_store(slot_info,KNOSYM(name),slotid);
  store_string(slot_info,KNOSYM(label),vd->label);
  kno_store(slot_info,KNOSYM_TYPE,get_readstat_typesym(vd->type));
  return READSTAT_HANDLER_OK;
}

static int log_variable_handler(int ix,readstat_variable_t *vd,const 
				char *labels,void *ignored)
{
  u8_log(LOGWARN,"ReadStatVariable",
	 "ix=%d vd=%p ix=%d name=%s type=%s format=%s label=%s off=%lld",
	 ix,vd,vd->index,vd->name,get_readstat_typename(vd->type),
	 vd->format,vd->label,vd->offset);
  return READSTAT_HANDLER_OK;
}

/* Getting data */

static void finish_observation(kno_readstat rs)
{
  if ( (rs->rs_obvsid >= 0) && (rs->rs_observation) ) {
    lispval observation = (lispval) rs->rs_observation;
    lispval callback = rs->rs_callback;
    lispval aggregate = rs->rs_output;
    if ( (KNO_EMPTYP(aggregate)) || (KNO_CHOICEP(aggregate)) ||
	 (KNO_PRECHOICEP(aggregate)) ) {
      KNO_ADD_TO_CHOICE(aggregate,observation);
      rs->rs_output=aggregate;}
    else if ( (KNO_EMPTY_LISTP(aggregate)) || (KNO_PAIRP(aggregate)) ) {
      aggregate = kno_init_pair(NULL,observation,aggregate);
      rs->rs_output=aggregate;}
    else if (KNO_VOIDP(callback)) {
      KNO_ADD_TO_CHOICE(aggregate,observation);
      rs->rs_output=aggregate;}
    else NO_ELSE;
    if (KNO_APPLICABLEP(callback)) {
      lispval args[3]={observation,KNO_INT(rs->rs_obvsid),((lispval)rs)};
      lispval result = kno_apply(callback,3,args);
      if (KNO_TROUBLEP(result)) {
	u8_exception ex = u8_pop_exception();
	u8_log(LOGERR,"ReadStatCallbackError",
	       "%s<%s>(%s) applying %q to observation %lld= %q",
	       ex->u8x_cond,ex->u8x_context,ex->u8x_details,
	       callback,rs->rs_obvsid,observation);
	u8_free_exception(ex,0);}}
    rs->rs_observation=NULL;
    kno_decref(observation);}
}

static void init_observation(kno_readstat rs,long long obsv)
{
  if (rs->rs_obvsid == obsv) return;
  else if (obsv<0) return;
  else if (rs->rs_obvsid >=0 )
    finish_observation(rs);
  else NO_ELSE;
  rs->rs_obvsid = obsv;
  struct KNO_SCHEMAP *df = rs->rs_dataframe;
  int n = df->schema_length;
  lispval sv = kno_make_schemap
    (NULL,df->schema_length,KNO_DATAFRAME_SCHEMAP,df->table_schema,NULL);
  struct KNO_SCHEMAP *observation = (kno_schemap) sv;
  lispval *values = observation->table_values;
  int i = 0; while (i<n) values[i++]=KNO_VOID;
  rs->rs_values = values;
  rs->rs_observation = observation;
  rs->rs_seen_count=0;
}

static int value_handler(int obs_index,
			 readstat_variable_t *vd,
			 readstat_value_t val,
			 void *state)
{
  struct KNO_READSTAT *rs = (kno_readstat) state;
  if (obs_index != rs->rs_obvsid) {
    if (rs->rs_obvsid>=0)
      finish_observation(rs);
    init_observation(rs,obs_index);}
  int var_index = vd->index;
  lispval value = get_lisp_value(&val);
  lispval *values = rs->rs_values;
  values[var_index]=value;
  return READSTAT_HANDLER_OK;
}

static int log_value_handler(int obs_index,
			     readstat_variable_t *vd,
			     readstat_value_t val,
			     void *state)
{
  struct KNO_READSTAT *rs = (kno_readstat) state;
  u8_string valstring = get_valstring(&val);
  u8_log(LOGWARN,"ReadStatValueVar",
	 "obs=%d kd=%p#%d name='%s' type=%s label='%s' vtype=%s "
	 "tag=%d =%s %s%s",
	 obs_index,vd,vd->index,vd->name,get_readstat_typename(vd->type),vd->label,
	 get_readstat_typename(val.type),val.tag,
	 valstring,
	 (val.is_system_missing)?(" system_missing"):(""),
	 (val.is_tagged_missing)?(" tagged_missing"):(""));
  u8_free(valstring);
  return READSTAT_HANDLER_OK;
}

kno_readstat get_parser(lispval opts)
{
  readstat_parser_t *parser=readstat_parser_init();
  struct KNO_READSTAT *result=u8_alloc(struct KNO_READSTAT);
  KNO_INIT_CONS(result,kno_readstat_type);
  lispval annotations = result->annotations  = kno_make_slotmap(17,0,NULL);
  kno_store(annotations,KNOSYM_OPTS,opts);
  result->rs_parser=parser;
  result->rs_source=NULL;
  result->rs_type="not-initialized";
  result->rs_n_vars = -1;
  result->rs_bits = 0;
  result->rs_dataframe   = NULL;
  result->rs_obvsid = -1;
  result->rs_observation = NULL;
  lispval callback = kno_getopt(opts,KNOSYM(callback),KNO_VOID);
  lispval aggregate = kno_getopt(opts,KNOSYM(aggregate),KNO_VOID);
  result->rs_callback    = callback;
  if ( ( KNO_VOIDP(callback)) && (KNO_VOIDP(aggregate)) )
    aggregate=KNO_EMPTY;
  result->rs_output=aggregate;
#if 0
  readstat_set_metadata_handler(parser,metadata_handler);
  readstat_set_variable_handler(parser,variable_handler);
  readstat_set_value_handler(parser,log_value_handler);
#endif
  readstat_set_metadata_handler(parser,metadata_handler);
  readstat_set_variable_handler(parser,variable_handler);
  readstat_set_value_handler(parser,value_handler);
  return result;
}

static void recycle_readstat(struct KNO_RAW_CONS *c)
{
  struct KNO_READSTAT *rs = (struct KNO_READSTAT *)c;
  readstat_parser_free(rs->rs_parser); rs->rs_parser=NULL;
  if (rs->rs_source) { u8_free(rs->rs_source); rs->rs_source=NULL; }
  kno_decref(rs->annotations);
  if (rs->rs_observation) {
    kno_decref(((lispval)rs->rs_observation));}
  kno_decref((lispval)(rs->rs_observation));
  kno_decref(rs->rs_output);
  if (!(KNO_STATIC_CONSP(c))) u8_free(c);
}

static int unparse_readstat(struct U8_OUTPUT *out,lispval x)
{
  struct KNO_READSTAT *rs = (struct KNO_READSTAT *)x;
  u8_printf(out,"#<READSTAT/%s '%s' %llx>",rs->rs_type,rs->rs_source,rs);
  return 1;
}

/* Readstat properties */

DEFC_PRIM("readstat-source",readstat_source,
	  KNO_MAX_ARGS(1)|KNO_MIN_ARGS(1),
	  "Gets the source of a readstat object",
	  {"rs",KNO_READSTAT_TYPE,KNO_VOID})
static lispval readstat_source(lispval arg)
{
  kno_readstat rs = (kno_readstat) arg;
  if (rs->rs_source)
    return knostring(rs->rs_source);
  else return KNO_FALSE;
}

DEFC_PRIM("readstat-type",readstat_type,
	  KNO_MAX_ARGS(1)|KNO_MIN_ARGS(1),
	  "Gets the file type of a readstat object",
	  {"rs",KNO_READSTAT_TYPE,KNO_VOID})
static lispval readstat_type(lispval arg)
{
  kno_readstat rs = (kno_readstat) arg;
  if (rs->rs_type)
    return kno_intern(rs->rs_type);
  else return KNO_FALSE;
}

DEFC_PRIM("readstat-records",readstat_records,
	  KNO_MAX_ARGS(1)|KNO_MIN_ARGS(1),
	  "Gets the file type of a readstat object",
	  {"rs",KNO_READSTAT_TYPE,KNO_VOID})
static lispval readstat_records(lispval arg)
{
  kno_readstat rs = (kno_readstat) arg;
  return kno_simplify_choice(rs->rs_output);
}

/* Readstat openers */

DEFC_PRIM("readstat/dta",readstat_dta,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .dta file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_dta(lispval path,lispval opts)
{
  kno_readstat rs = get_parser(opts);
  if (rs) rs->rs_type="dta"; else return KNO_ERROR;
  readstat_error_t rv = readstat_parse_dta(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/dta",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

DEFC_PRIM("readstat/sas7bdat",readstat_sas7bdat,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .sas7bdat file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_sas7bdat(lispval path,lispval opts)
{
  kno_readstat rs = get_parser(opts);
  if (rs) rs->rs_type="sas7bdat"; else return KNO_ERROR;
  readstat_error_t rv = readstat_parse_sas7bdat(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/sas7bdat",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

static int readstat_initialized = 0;

KNO_EXPORT int kno_init_readstat()
{
  if (readstat_initialized) return 0;
  readstat_initialized = 1;
  kno_init_scheme();

  kno_readstat_type = kno_register_cons_type("readstat_parser",KNO_READSTAT_TYPE);
  kno_unparsers[kno_readstat_type] = unparse_readstat;
  kno_recyclers[kno_readstat_type] = recycle_readstat;

  readstat_module = kno_new_cmodule("readstat",0,kno_init_readstat);

  link_local_cprims();

  kno_finish_module(readstat_module);

  u8_register_source_file(_FILEINFO);

  return 1;
}

static void link_local_cprims()
{
  KNO_LINK_CPRIM("readstat/dta",readstat_dta,2,readstat_module);
  KNO_LINK_CPRIM("readstat/sas7bdat",readstat_sas7bdat,2,readstat_module);
  KNO_LINK_CPRIM("readstat-source",readstat_source,1,readstat_module);
  KNO_LINK_CPRIM("readstat-type",readstat_source,1,readstat_module);
  KNO_LINK_CPRIM("readstat-records",readstat_records,1,readstat_module);
}
