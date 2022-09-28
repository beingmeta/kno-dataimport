/* -*- Mode: C; Character-encoding: utf-8; -*- */

/* readstat.c
   This implements Kno bindings to the ReadStat C library
   Copyright (C) 2021-2022 Kenneth Haase
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
#include <libu8/u8convert.h>
#include <libu8/u8printf.h>
#include <libu8/u8crypto.h>

#include <math.h>
#include <limits.h>
#include <readstat.h>

/* Compatability */

#if (KNO_MAJOR_VERSION < 2210)
#define KNO_PROCP KNO_FUNCTIONP
#define kno_proc kno_function
#endif

#ifndef KNO_FUTURE_MONOTONIC
#ifdef KNO_FUTURE_ONESHOT
#define KNO_FUTURE_MONOTONIC KNO_FUTURE_ONESHOT
#endif
#endif

/* 
   TODO:
   * Merge callback and output, include futures as output.
   * Add handling for missing and labelled values
   * Handle ctime/mtime metadata
   * Use encoding when provided (do we have to?)
   * Add measure and alignment to schema
 */

KNO_EXPORT int kno_init_creadstat(void) KNO_LIBINIT_FN;

kno_lisp_type kno_readstat_type;
#define KNO_READSTAT_TYPE     0xEC7900L

static lispval creadstat_module;

typedef struct KNO_READSTAT {
  KNO_ANNOTATED_HEADER;
  u8_string rs_source;
  u8_context rs_type;
  unsigned int rs_bits;
  int rs_n_vars;
  int rs_n_slots;
  readstat_parser_t *rs_parser;
  u8_encoding rs_text_encoding;
  lispval rs_idslot;
  lispval rs_vlabels;
  struct KNO_SCHEMAP *rs_dataframe;
  long long rs_obsid;
  struct KNO_SCHEMAP *rs_observation;
  lispval *rs_values;
  lispval rs_output;
  int rs_counter;} *kno_readstat;

#define KNO_READSTAT_FOLDCASE 0x100

DEF_KNOSYM(nvars); DEF_KNOSYM(rows); DEF_KNOSYM(label); DEF_KNOSYM(tablename);
DEF_KNOSYM(encname); DEF_KNOSYM(is64bit); DEF_KNOSYM(idslot);
DEF_KNOSYM(formatversion); DEF_KNOSYM(name); DEF_KNOSYM(format);
DEF_KNOSYM(ctime); DEF_KNOSYM(mtime); DEF_KNOSYM(labels);
DEF_KNOSYM(labelset); DEF_KNOSYM(missing_ranges);
DEF_KNOSYM(measure); DEF_KNOSYM(nominal); DEF_KNOSYM(ordinal); DEF_KNOSYM(scale);
DEF_KNOSYM(compression); DEF_KNOSYM(binary);
DEF_KNOSYM(alignment); DEF_KNOSYM(left); DEF_KNOSYM(right); DEF_KNOSYM(center);
DEF_KNOSYM(offset); DEF_KNOSYM(decimals);
DEF_KNOSYM(storage_width); DEF_KNOSYM(user_width); DEF_KNOSYM(display_width);
DEF_KNOSYM(skip);
DEF_KNOSYM(string); DEF_KNOSYM(stringref);
DEF_KNOSYM(int8); DEF_KNOSYM(int16); DEF_KNOSYM(int32);
DEF_KNOSYM(float); DEF_KNOSYM(double);
DEF_KNOSYM(slotid); DEF_KNOSYM(output); DEF_KNOSYM(foldcase);

static lispval system_missing_value;
static lispval tagged_missing_values[26];

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
  if (val->is_system_missing)
    return system_missing_value;
  else if ( (val->is_tagged_missing) && (val->tag>='a') && (val->tag<='z'))
    return tagged_missing_values[val->tag-'a'];
  else NO_ELSE; /* just continue */
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
  int n_vars = md->var_count, n_slots = n_vars;
  lispval *schema = u8_alloc_n(n_slots,lispval);
  if (! ((KNO_VOIDP(rs->rs_idslot))||(KNO_FALSEP(rs->rs_idslot))) ) {
    n_slots++;}
  int i = 0; while (i<n_slots) {schema[i]=KNO_INT2FIX(i); i++;}
  struct KNO_SCHEMAP *template;
  lispval dfptr = kno_make_schemap
    (NULL,n_slots,KNO_DATAFRAME_TEMPLATE_FLAGS,schema,NULL);
  if (KNO_ABORTED(dfptr)) {
    u8_free(schema);
    return dfptr;}
  else template= (kno_schemap) dfptr;
  rs->rs_dataframe = template;
  rs->rs_n_vars    = n_vars;
  rs->rs_n_slots   = n_slots;
  lispval annotations = rs->annotations;
  if (md->creation_time>0)  {
    lispval timestamp = kno_time2timestamp(md->creation_time);
    kno_store(annotations,KNOSYM(ctime),timestamp);
    kno_decref(timestamp);}
  if (md->modified_time>0)  {
    lispval timestamp = kno_time2timestamp(md->modified_time);
    kno_store(annotations,KNOSYM(mtime),timestamp);
    kno_decref(timestamp);}
  if (rs->rs_n_slots>rs->rs_n_vars) {
    lispval *schema = template->table_schema;
    schema[rs->rs_n_vars]=rs->rs_idslot;}
  if (md->var_count>=0) {
    kno_store(annotations,KNOSYM(nvars),KNO_INT(md->var_count));}
  if (md->row_count>=0) {
    kno_store(annotations,KNOSYM(rows),KNO_INT(md->row_count));}
  if (md->table_name)
    store_string(annotations,KNOSYM(tablename),md->table_name);
  if (md->file_label)
    store_string(annotations,KNOSYM(label),md->file_label);
  if (md->file_encoding) {
    store_string(annotations,KNOSYM(encname),md->file_encoding);
    rs->rs_text_encoding=u8_get_encoding(md->file_encoding);}
  switch (md->compression) {
  case READSTAT_COMPRESS_ROWS:
    kno_store(annotations,KNOSYM(compression),KNOSYM(rows));
    break;
  case READSTAT_COMPRESS_BINARY:
    kno_store(annotations,KNOSYM(compression),KNOSYM(binary));
    break;
  }
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

#define COPY_INT_PROP(vd,field)						\
  if (vd->field>0) kno_store(slot_info,KNOSYM(field),KNO_INT(vd->field))

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
  lispval slotid = ((rs->rs_bits)&(KNO_READSTAT_FOLDCASE)) ?
    (kno_getsym(vd->name)) : (kno_intern(vd->name));
  lispval slot_info = kno_make_slotmap(7,0,NULL);
  schema[i]=slotid;
  values[i]=slot_info;
  kno_store(slot_info,KNOSYM(slotid),slotid);
  store_string(slot_info,KNOSYM(name),vd->name);
  store_string(slot_info,KNOSYM(format),vd->format);
  store_string(slot_info,KNOSYM(label),vd->label);
  COPY_INT_PROP(vd,offset);
  COPY_INT_PROP(vd,storage_width);
  COPY_INT_PROP(vd,user_width);
  COPY_INT_PROP(vd,display_width);
  COPY_INT_PROP(vd,decimals);
  COPY_INT_PROP(vd,skip);
  if (vd->label_set) {
    u8_string label_set_name = vd->label_set->name;
    store_string(slot_info,KNOSYM(labelset),label_set_name);}
  if (vd->missingness.missing_ranges_count) {
    int n = vd->missingness.missing_ranges_count;
    lispval vec = kno_make_vector(n,NULL);
    int i = 0; while (i<n) {
      lispval v = get_lisp_value(&(vd->missingness.missing_ranges[i]));
      KNO_VECTOR_SET(vec,i,v);
      i++;}
    kno_store(slot_info,KNOSYM(missing_ranges),vec);
    kno_decref(vec);}
  switch (vd->measure) {
  case READSTAT_MEASURE_NOMINAL:
    kno_store(slot_info,KNOSYM(measure),KNOSYM(nominal));
    break;
  case READSTAT_MEASURE_ORDINAL:
    kno_store(slot_info,KNOSYM(measure),KNOSYM(ordinal));
    break;
  case READSTAT_MEASURE_SCALE:
    kno_store(slot_info,KNOSYM(measure),KNOSYM(scale));
    break;
  }
  switch (vd->alignment) {
  case READSTAT_ALIGNMENT_LEFT:
    kno_store(slot_info,KNOSYM(alignment),KNOSYM(left));
    break;
  case READSTAT_ALIGNMENT_RIGHT:
    kno_store(slot_info,KNOSYM(alignment),KNOSYM(right));
    break;
  case READSTAT_ALIGNMENT_CENTER:
    kno_store(slot_info,KNOSYM(alignment),KNOSYM(center));
    break;
  }
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

static void add_value_label(kno_readstat rs,
			    const char *labelset,
			    const char *label,
			    lispval v);

static int label_handler(const char *labelset,
			 readstat_value_t value,
			 const char *label,
			 void *state)
{
  struct KNO_READSTAT *rs = (kno_readstat) state;
  lispval v = get_lisp_value(&value);
  add_value_label(rs,labelset,label,v);
  kno_decref(v);
  return READSTAT_HANDLER_OK;
}

static int log_label_handler(char *labelset,readstat_value_t value,char *label,void *ignored)
{
  lispval v = get_lisp_value(&value);
  u8_log(LOGWARN,"ReadStatLabel","Label in %s maps %q to %s",labelset,v,label);
  kno_decref(v);
  return READSTAT_HANDLER_OK;
}

/* Getting data */

static void output_observation
(kno_readstat rs,lispval observation,int obsid)
{
  lispval output=rs->rs_output;
  if (KNO_PROCP(output)) {
    lispval args[3]={observation,KNO_INT(obsid),((lispval)rs)};
    int arity = ((kno_proc)output)->fcn_arity;
    int call_width = (arity<0) ? (3) : (arity<3) ? (arity) : (3);
    lispval result = kno_apply(output,call_width,args);
    if (KNO_TROUBLEP(result)) {
      u8_exception ex = u8_pop_exception();
      u8_log(LOGERR,"ReadStatCallbackError",
	     "%s<%s>(%s) applying %q to observation %lld= %q",
	     ex->u8x_cond,ex->u8x_context,ex->u8x_details,
	     output,obsid,observation);
      u8_free_exception(ex,0);}
    else kno_decref(result);
    kno_decref(observation);}
  else if (KNO_APPLICABLEP(output)) {
    lispval args[3]={observation,KNO_INT(obsid),((lispval)rs)};
    lispval result = kno_apply(output,3,args);
    if (KNO_TROUBLEP(result)) {
      u8_exception ex = u8_pop_exception();
      u8_log(LOGERR,"ReadStatCallbackError",
	     "%s<%s>(%s) applying %q to observation %lld= %q",
	     ex->u8x_cond,ex->u8x_context,ex->u8x_details,
	     output,obsid,observation);
      u8_free_exception(ex,0);}
    else kno_decref(result);
    kno_decref(observation);}
  else if (KNO_PRECHOICEP(output)) {
    KNO_ADD_TO_CHOICE(output,observation);}
  else if (KNO_TYPEP(output,kno_future_type)) {
    kno_change_future((kno_future)output,observation,KNO_FUTURE_MONOTONIC);
    kno_decref(observation);}
  else if ( (KNO_PAIRP(output)) || (output == KNO_EMPTY_LIST) )
    rs->rs_output = kno_init_pair(NULL,observation,output);
  else NO_ELSE;
}

static void finish_observation(kno_readstat rs)
{
  if ( (rs->rs_obsid >= 0) && (rs->rs_observation) ) {
    lispval observation = (lispval) rs->rs_observation;
    rs->rs_observation = NULL;
    int obsid = rs->rs_obsid; rs->rs_obsid = -1;
    output_observation(rs,observation,obsid);}
}

static void init_observation(kno_readstat rs,long long obsv)
{
  if (rs->rs_obsid == obsv) return;
  else if (obsv<0) return;
  else if (rs->rs_obsid >=0 )
    finish_observation(rs);
  else NO_ELSE;
  rs->rs_obsid = obsv;
  struct KNO_SCHEMAP *df = rs->rs_dataframe;
  int n = df->schema_length;
  lispval sv = kno_make_schemap
    (NULL,df->schema_length,KNO_DATAFRAME_SCHEMAP,df->table_schema,NULL);
  struct KNO_SCHEMAP *observation = (kno_schemap) sv;
  lispval *values = observation->table_values;
  int i = 0; while (i<n) values[i++]=KNO_VOID;
  rs->rs_values = values;
  rs->rs_observation = observation;
  /* Initialize the observation field if specified */
  if (rs->rs_n_slots>rs->rs_n_vars) values[rs->rs_n_vars]=KNO_INT(obsv);
  rs->rs_counter++;
}

static int value_handler(int obs_index,
			 readstat_variable_t *vd,
			 readstat_value_t val,
			 void *state)
{
  struct KNO_READSTAT *rs = (kno_readstat) state;
  if (obs_index != rs->rs_obsid) {
    if (rs->rs_obsid>=0)
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

kno_readstat create_readstat(lispval opts)
{
  readstat_parser_t *parser=readstat_parser_init();
  struct KNO_READSTAT *result=u8_alloc(struct KNO_READSTAT);
  KNO_INIT_CONS(result,kno_readstat_type);
  lispval annotations = result->annotations  = kno_make_slotmap(17,0,NULL);
  kno_store(annotations,KNOSYM_OPTS,opts);
  result->rs_source=NULL;
  result->rs_type="not-initialized";
  result->rs_bits = 0;
  result->rs_n_vars = -1;
  result->rs_parser = parser;
  result->rs_text_encoding = NULL;
  result->rs_idslot = kno_getopt(opts,KNOSYM(idslot),KNO_VOID);
  result->rs_vlabels = kno_getopt(opts,KNOSYM(labels),KNO_VOID);

  result->rs_dataframe   = NULL;
  result->rs_obsid = -1;
  result->rs_observation = NULL;

  lispval output = kno_getopt(opts,KNOSYM(output),KNO_VOID);
  if ( (KNO_APPLICABLEP(output)) ||
       (KNO_TYPEP(output,kno_future_type)) ||
       (KNO_EMPTY_LISTP(output)) )
    result->rs_output = output;
  else if ( (KNO_VOIDP(output)) || (KNO_EMPTYP(output)) )
    result->rs_output    = kno_init_prechoice(NULL,100,1);
  else result->rs_output = KNO_EMPTY_LIST;
  result->rs_counter = 0;

  lispval foldcase = (kno_getopt(opts,KNOSYM(foldcase),KNO_TRUE));
  if (KNO_TRUEP(foldcase))
    result->rs_bits |= KNO_READSTAT_FOLDCASE;
  else NO_ELSE;
  kno_decref(foldcase);

#if 0
  readstat_set_metadata_handler(parser,metadata_handler);
  readstat_set_variable_handler(parser,variable_handler);
  readstat_set_value_handler(parser,log_value_handler);
#endif
  readstat_set_metadata_handler(parser,metadata_handler);
  readstat_set_variable_handler(parser,variable_handler);
  readstat_set_value_label_handler(parser,label_handler);
  readstat_set_value_handler(parser,value_handler);
  return result;
}

static void add_value_label(kno_readstat rs,
			    const char *labelset,
			    const char *label,
			    lispval v)
{
  lispval labels = rs->rs_vlabels;
  if (KNO_VOIDP(labels))
    labels=rs->rs_vlabels=kno_make_slotmap(3,0,NULL);
  lispval label_set_name = knostring(labelset);
  lispval label_set = kno_get(labels,label_set_name,KNO_VOID);
  if (KNO_VOIDP(label_set)) {
    label_set=kno_make_slotmap(16,0,NULL);
    kno_store(labels,label_set_name,label_set);}
  lispval label_string = knostring(label);
  kno_store(label_set,label_string,v);
  kno_decref(label_string);
  kno_decref(label_set_name);
  kno_decref(label_set);
}

static void recycle_readstat(struct KNO_RAW_CONS *c)
{
  struct KNO_READSTAT *rs = (struct KNO_READSTAT *)c;
  readstat_parser_free(rs->rs_parser); rs->rs_parser=NULL;
  if (rs->rs_source) { u8_free(rs->rs_source); rs->rs_source=NULL; }
  kno_decref(rs->annotations);
  kno_decref(rs->rs_vlabels);
  if (rs->rs_observation) {
    kno_decref(((lispval)rs->rs_observation));}
  kno_decref((lispval)(rs->rs_observation));
  kno_decref(rs->rs_output);
  if (!(KNO_STATIC_CONSP(c))) u8_free(c);
}

static int unparse_readstat(struct U8_OUTPUT *out,lispval x)
{
  struct KNO_READSTAT *rs = (struct KNO_READSTAT *)x;
  u8_printf(out,"#<READSTAT/%s '%s' #!%llx>",rs->rs_type,rs->rs_source,rs);
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

DEFC_PRIM("readstat-dataframe",readstat_dataframe,
	  KNO_MAX_ARGS(1)|KNO_MIN_ARGS(1),
	  "Gets the defining dataframe of a readstat object",
	  {"rs",KNO_READSTAT_TYPE,KNO_VOID})
static lispval readstat_dataframe(lispval arg)
{
  kno_readstat rs = (kno_readstat) arg;
  if (rs->rs_dataframe) {
    lispval df = (lispval) (rs->rs_dataframe);
    return kno_incref(df);}
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

DEFC_PRIM("readstat-output",readstat_output,
	  KNO_MAX_ARGS(1)|KNO_MIN_ARGS(1),
	  "Gets the output of the readstat object",
	  {"rs",KNO_READSTAT_TYPE,KNO_VOID})
static lispval readstat_output(lispval arg)
{
  kno_readstat rs = (kno_readstat) arg;
  return kno_simplify_choice(rs->rs_output);
}

DEFC_PRIM("readstat-count",readstat_count,
	  KNO_MAX_ARGS(1)|KNO_MIN_ARGS(1),
	  "Gets the number of records processed by the readstat object",
	  {"rs",KNO_READSTAT_TYPE,KNO_VOID})
static lispval readstat_count(lispval arg)
{
  kno_readstat rs = (kno_readstat) arg;
  return KNO_INT(rs->rs_counter);
}

DEFC_PRIM("readstat-labels",readstat_labels,
	  KNO_MAX_ARGS(1)|KNO_MIN_ARGS(1),
	  "Gets the labels of the readstat object",
	  {"rs",KNO_READSTAT_TYPE,KNO_VOID})
static lispval readstat_labels(lispval arg)
{
  kno_readstat rs = (kno_readstat) arg;
  return kno_incref(rs->rs_vlabels);
}

/* Readstat openers */

DEFC_PRIM("readstat/load/dta",readstat_dta,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .dta file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_dta(lispval path,lispval opts)
{
  kno_readstat rs = create_readstat(opts);
  if (rs) rs->rs_type="dta"; else return KNO_ERROR;
  rs->rs_source = u8_strdup(KNO_CSTRING(path));
  readstat_error_t rv = readstat_parse_dta(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/load/dta",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

DEFC_PRIM("readstat/load/sav",readstat_sav,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .sav file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_sav(lispval path,lispval opts)
{
  kno_readstat rs = create_readstat(opts);
  if (rs) rs->rs_type="sav"; else return KNO_ERROR;
  rs->rs_source = u8_strdup(KNO_CSTRING(path));
  readstat_error_t rv = readstat_parse_sav(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/load/sav",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

DEFC_PRIM("readstat/load/por",readstat_por,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .por file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_por(lispval path,lispval opts)
{
  kno_readstat rs = create_readstat(opts);
  if (rs) rs->rs_type="por"; else return KNO_ERROR;
  rs->rs_source = u8_strdup(KNO_CSTRING(path));
  readstat_error_t rv = readstat_parse_por(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/load/por",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

DEFC_PRIM("readstat/load/sas7bdat",readstat_sas7bdat,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .sas7bdat file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_sas7bdat(lispval path,lispval opts)
{
  kno_readstat rs = create_readstat(opts);
  if (rs) rs->rs_type="sas7bdat"; else return KNO_ERROR;
  rs->rs_source = u8_strdup(KNO_CSTRING(path));
  readstat_error_t rv = readstat_parse_sas7bdat(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/load/sas7bdat",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

DEFC_PRIM("readstat/load/sas7bcat",readstat_sas7bcat,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .sas7bcat file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_sas7bcat(lispval path,lispval opts)
{
  kno_readstat rs = create_readstat(opts);
  if (rs) rs->rs_type="sas7bcat"; else return KNO_ERROR;
  rs->rs_source = u8_strdup(KNO_CSTRING(path));
  readstat_error_t rv = readstat_parse_sas7bcat(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/load/sas7bcat",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

DEFC_PRIM("readstat/load/xport",readstat_xport,
	  KNO_MAX_ARGS(2)|KNO_MIN_ARGS(1),
	  "Opens a Stata .xport file",
	  {"path",kno_string_type,KNO_VOID},
	  {"opts",kno_opts_type,KNO_FALSE})
static lispval readstat_xport(lispval path,lispval opts)
{
  kno_readstat rs = create_readstat(opts);
  if (rs) rs->rs_type="xport"; else return KNO_ERROR;
  rs->rs_source = u8_strdup(KNO_CSTRING(path));
  readstat_error_t rv = readstat_parse_xport(rs->rs_parser,KNO_CSTRING(path),(void *)rs);
  if (rv == READSTAT_HANDLER_OK)
    return (lispval) rs;
  else {
    lispval rsv = (lispval) rs;
    kno_seterr("ReadStatError","readstat/load/xport",readstat_error_message(rv),path);
    kno_decref(rsv);
    return KNO_ERROR_VALUE;}
}

static int readstat_initialized = 0;

KNO_EXPORT int kno_init_creadstat()
{
  if (readstat_initialized) return 0;
  readstat_initialized = 1;
  kno_init_scheme();

  kno_readstat_type = kno_register_cons_type("readstat_db",KNO_READSTAT_TYPE);
  kno_unparsers[kno_readstat_type] = unparse_readstat;
  kno_recyclers[kno_readstat_type] = recycle_readstat;

  system_missing_value = kno_register_constant("#missing_value");
  char *tagged_missing_template="#missing_value_?";
  size_t template_tail=strlen(tagged_missing_template)-1;
  int i = 0; while (i< 26) {
    u8_byte *const_name=u8_strdup(tagged_missing_template);
    const_name[template_tail]='a'+i;
    tagged_missing_values[i]=kno_register_constant(const_name);
    i++;}

  creadstat_module = kno_new_cmodule("creadstat",0,kno_init_creadstat);

  link_local_cprims();

  kno_finish_module(creadstat_module);

  u8_register_source_file(_FILEINFO);

  return 1;
}

static void link_local_cprims()
{
  KNO_LINK_CPRIM("readstat/load/dta",readstat_dta,2,creadstat_module);
  KNO_LINK_CPRIM("readstat/load/sav",readstat_sav,2,creadstat_module);
  KNO_LINK_CPRIM("readstat/load/por",readstat_por,2,creadstat_module);
  KNO_LINK_CPRIM("readstat/load/sas7bdat",readstat_sas7bdat,2,creadstat_module);
  KNO_LINK_CPRIM("readstat/load/sas7bcat",readstat_sas7bcat,2,creadstat_module);
  KNO_LINK_CPRIM("readstat/load/xport",readstat_xport,2,creadstat_module);
  KNO_LINK_CPRIM("readstat-source",readstat_source,1,creadstat_module);
  KNO_LINK_CPRIM("readstat-dataframe",readstat_dataframe,1,creadstat_module);
  KNO_LINK_CPRIM("readstat-type",readstat_source,1,creadstat_module);
  KNO_LINK_CPRIM("readstat-labels",readstat_labels,1,creadstat_module);
  KNO_LINK_CPRIM("readstat-output",readstat_output,1,creadstat_module);
  KNO_LINK_CPRIM("readstat-count",readstat_count,1,creadstat_module);
}
