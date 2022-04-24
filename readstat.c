/* -*- Mode: C; Character-encoding: utf-8; -*- */

/* pqprims.c
   This implements Kno bindings to the Postgres C library
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
  readstat_parser_t *rs_parser;
  u8_string rs_source;
  u8_context rs_type;} *kno_readstat;

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

static int log_metadata_handler(readstat_metadata_t *md,void *ignored)
{
  u8_log(LOGWARN,"ReadStatMetadata","md=%p rows=%lld cols=%lld version=%lld name=%s label=%s enc=%s",
	 md,md->row_count,md->var_count,md->file_format_version,md->table_name,md->file_label,md->file_encoding);
  return READSTAT_HANDLER_OK;
}

static int log_variable_handler(int ix,readstat_variable_t *vd,const char *labels,void *ignored)
{
  u8_log(LOGWARN,"ReadStatVariable","ix=%d vd=%p ix=%d name=%s type=%s format=%s label=%s off=%lld",
	 ix,vd,vd->index,vd->name,get_readstat_typename(vd->type),vd->format,vd->label,vd->offset);
  return READSTAT_HANDLER_OK;
}

static int log_value_handler(int obs_index,
			     readstat_variable_t *vd,
			     readstat_value_t val,
			     void *ignored)
{
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
  result->annotations=KNO_VOID;
  result->rs_parser=parser;
  result->rs_source=NULL;
  result->rs_type="not-initialized";
  readstat_set_metadata_handler(parser,log_metadata_handler);
  readstat_set_variable_handler(parser,log_variable_handler);
  readstat_set_value_handler(parser,log_value_handler);
  return result;
}

static void recycle_readstat(struct KNO_RAW_CONS *c)
{
  struct KNO_READSTAT *rs = (struct KNO_READSTAT *)c;
  readstat_parser_free(rs->rs_parser); rs->rs_parser=NULL;
  if (rs->rs_source) { u8_free(rs->rs_source); rs->rs_source=NULL; }
  kno_decref(rs->annotations);
  if (!(KNO_STATIC_CONSP(c))) u8_free(c);
}

static int unparse_readstat(struct U8_OUTPUT *out,lispval x)
{
  struct KNO_READSTAT *rs = (struct KNO_READSTAT *)x;
  u8_printf(out,"#<READSTAT/%s '%s' %llx>",rs->rs_type,rs->rs_source,rs);
  return 1;
}

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
}
