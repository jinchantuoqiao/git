/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file

  @brief
  This file defines all numerical functions
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mysql_priv.h"
#include "slave.h"				// for wait_for_master_pos
#include "rpl_mi.h"
#include <m_ctype.h>
#include <hash.h>
#include <time.h>
#include <ft_global.h>
#include <my_bit.h>

#include "sp_head.h"
#include "sp_rcontext.h"
#include "sp.h"

#ifdef NO_EMBEDDED_ACCESS_CHECKS
#define sp_restore_security_context(A,B) while (0) {}
#endif

bool check_reserved_words(LEX_STRING *name)
{
  if (!my_strcasecmp(system_charset_info, name->str, "GLOBAL") ||
      !my_strcasecmp(system_charset_info, name->str, "LOCAL") ||
      !my_strcasecmp(system_charset_info, name->str, "SESSION"))
    return TRUE;
  return FALSE;
}


/**
  @return
    TRUE if item is a constant
*/

bool
eval_const_cond(COND *cond)
{
  return ((Item_func*) cond)->val_int() ? TRUE : FALSE;
}


void Item_func::set_arguments(List<Item> &list)
{
  allowed_arg_cols= 1;
  arg_count=list.elements;
  args= tmp_arg;                                // If 2 arguments
  if (arg_count <= 2 || (args=(Item**) sql_alloc(sizeof(Item*)*arg_count)))
  {
    List_iterator_fast<Item> li(list);
    Item *item;
    Item **save_args= args;

    while ((item=li++))
    {
      *(save_args++)= item;
      with_sum_func|=item->with_sum_func;
    }
  }
  list.empty();					// Fields are used
}

Item_func::Item_func(List<Item> &list)
  :allowed_arg_cols(1)
{
  set_arguments(list);
}

Item_func::Item_func(THD *thd, Item_func *item)
  :Item_result_field(thd, item),
   allowed_arg_cols(item->allowed_arg_cols),
   arg_count(item->arg_count),
   used_tables_cache(item->used_tables_cache),
   not_null_tables_cache(item->not_null_tables_cache),
   const_item_cache(item->const_item_cache)
{
  if (arg_count)
  {
    if (arg_count <=2)
      args= tmp_arg;
    else
    {
      if (!(args=(Item**) thd->alloc(sizeof(Item*)*arg_count)))
	return;
    }
    memcpy((char*) args, (char*) item->args, sizeof(Item*)*arg_count);
  }
}


/*
  Resolve references to table column for a function and its argument

  SYNOPSIS:
  fix_fields()
  thd		Thread object
  ref		Pointer to where this object is used.  This reference
		is used if we want to replace this object with another
		one (for example in the summary functions).

  DESCRIPTION
    Call fix_fields() for all arguments to the function.  The main intention
    is to allow all Item_field() objects to setup pointers to the table fields.

    Sets as a side effect the following class variables:
      maybe_null	Set if any argument may return NULL
      with_sum_func	Set if any of the arguments contains a sum function
      used_tables_cache Set to union of the tables used by arguments

      str_value.charset If this is a string function, set this to the
			character set for the first argument.
			If any argument is binary, this is set to binary

   If for any item any of the defaults are wrong, then this can
   be fixed in the fix_length_and_dec() function that is called
   after this one or by writing a specialized fix_fields() for the
   item.

  RETURN VALUES
  FALSE	ok
  TRUE	Got error.  Stored with my_error().
*/

bool
Item_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  Item **arg,**arg_end;
#ifndef EMBEDDED_LIBRARY			// Avoid compiler warning
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function
#endif

  used_tables_cache= not_null_tables_cache= 0;
  const_item_cache=1;

  /*
    Use stack limit of STACK_MIN_SIZE * 2 since
    on some platforms a recursive call to fix_fields
    requires more than STACK_MIN_SIZE bytes (e.g. for
    MIPS, it takes about 22kB to make one recursive
    call to Item_func::fix_fields())
  */
  if (check_stack_overrun(thd, STACK_MIN_SIZE * 2, buff))
    return TRUE;				// Fatal error if flag is set!
  if (arg_count)
  {						// Print purify happy
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      Item *item;
      /*
	We can't yet set item to *arg as fix_fields may change *arg
	We shouldn't call fix_fields() twice, so check 'fixed' field first
      */
      if ((!(*arg)->fixed && (*arg)->fix_fields(thd, arg)))
	return TRUE;				/* purecov: inspected */
      item= *arg;

      if (allowed_arg_cols)
      {
        if (item->check_cols(allowed_arg_cols))
          return 1;
      }
      else
      {
        /*  we have to fetch allowed_arg_cols from first argument */
        DBUG_ASSERT(arg == args); // it is first argument
        allowed_arg_cols= item->cols();
        DBUG_ASSERT(allowed_arg_cols); // Can't be 0 any more
      }

      if (item->maybe_null)
	maybe_null=1;

      with_sum_func= with_sum_func || item->with_sum_func;
      used_tables_cache|=     item->used_tables();
      not_null_tables_cache|= item->not_null_tables();
      const_item_cache&=      item->const_item();
      with_subselect|=        item->with_subselect;
    }
  }
  fix_length_and_dec();
  if (thd->is_error()) // An error inside fix_length_and_dec occured
    return TRUE;
  fixed= 1;
  return FALSE;
}


bool Item_func::walk(Item_processor processor, bool walk_subquery,
                     uchar *argument)
{
  if (arg_count)
  {
    Item **arg,**arg_end;
    for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
    {
      if ((*arg)->walk(processor, walk_subquery, argument))
	return 1;
    }
  }
  return (this->*processor)(argument);
}

void Item_func::traverse_cond(Cond_traverser traverser,
                              void *argument, traverse_order order)
{
  if (arg_count)
  {
    Item **arg,**arg_end;

    switch (order) {
    case(PREFIX):
      (*traverser)(this, argument);
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      break;
    case (POSTFIX):
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      (*traverser)(this, argument);
    }
  }
  else
    (*traverser)(this, argument);
}


/**
  Transform an Item_func object with a transformer callback function.

    The function recursively applies the transform method to each
    argument of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 
  @param transformer   the transformer callback function to be applied to
                       the nodes of the tree of the object
  @param argument      parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node
*/

Item *Item_func::transform(Item_transformer transformer, uchar *argument)
{
  DBUG_ASSERT(!current_thd->is_stmt_prepare());

  if (arg_count)
  {
    Item **arg,**arg_end;
    for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
    {
      Item *new_item= (*arg)->transform(transformer, argument);
      if (!new_item)
	return 0;

      /*
        THD::change_item_tree() should be called only if the tree was
        really transformed, i.e. when a new item has been created.
        Otherwise we'll be allocating a lot of unnecessary memory for
        change records at each execution.
      */
      if (*arg != new_item)
        current_thd->change_item_tree(arg, new_item);
    }
  }
  return (this->*transformer)(argument);
}


/**
  Compile Item_func object with a processor and a transformer
  callback functions.

    First the function applies the analyzer to the root node of
    the Item_func object. Then if the analizer succeeeds (returns TRUE)
    the function recursively applies the compile method to each argument
    of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 

  @param analyzer      the analyzer callback function to be applied to the
                       nodes of the tree of the object
  @param[in,out] arg_p parameter to be passed to the processor
  @param transformer   the transformer callback function to be applied to the
                       nodes of the tree of the object
  @param arg_t         parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node
*/

Item *Item_func::compile(Item_analyzer analyzer, uchar **arg_p,
                         Item_transformer transformer, uchar *arg_t)
{
  if (!(this->*analyzer)(arg_p))
    return 0;
  if (arg_count)
  {
    Item **arg,**arg_end;
    for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
    {
      /* 
        The same parameter value of arg_p must be passed
        to analyze any argument of the condition formula.
      */   
      uchar *arg_v= *arg_p;
      Item *new_item= (*arg)->compile(analyzer, &arg_v, transformer, arg_t);
      if (new_item && *arg != new_item)
        current_thd->change_item_tree(arg, new_item);
    }
  }
  return (this->*transformer)(arg_t);
}

/**
  See comments in Item_cmp_func::split_sum_func()
*/

void Item_func::split_sum_func(THD *thd, Item **ref_pointer_array,
                               List<Item> &fields)
{
  Item **arg, **arg_end;
  for (arg= args, arg_end= args+arg_count; arg != arg_end ; arg++)
    (*arg)->split_sum_func2(thd, ref_pointer_array, fields, arg, TRUE);
}


void Item_func::update_used_tables()
{
  used_tables_cache=0;
  const_item_cache=1;
  for (uint i=0 ; i < arg_count ; i++)
  {
    args[i]->update_used_tables();
    used_tables_cache|=args[i]->used_tables();
    const_item_cache&=args[i]->const_item();
  }
}


table_map Item_func::used_tables() const
{
  return used_tables_cache;
}


table_map Item_func::not_null_tables() const
{
  return not_null_tables_cache;
}


void Item_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name());
  str->append('(');
  print_args(str, 0, query_type);
  str->append(')');
}


void Item_func::print_args(String *str, uint from, enum_query_type query_type)
{
  for (uint i=from ; i < arg_count ; i++)
  {
    if (i != from)
      str->append(',');
    args[i]->print(str, query_type);
  }
}


void Item_func::print_op(String *str, enum_query_type query_type)
{
  str->append('(');
  for (uint i=0 ; i < arg_count-1 ; i++)
  {
    args[i]->print(str, query_type);
    str->append(' ');
    str->append(func_name());
    str->append(' ');
  }
  args[arg_count-1]->print(str, query_type);
  str->append(')');
}


bool Item_func::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;
  if (item->type() != FUNC_ITEM)
    return 0;
  Item_func *item_func=(Item_func*) item;
  Item_func::Functype func_type;
  if ((func_type= functype()) != item_func->functype() ||
      arg_count != item_func->arg_count ||
      (func_type != Item_func::FUNC_SP &&
       func_name() != item_func->func_name()) ||
      (func_type == Item_func::FUNC_SP &&
       my_strcasecmp(system_charset_info, func_name(), item_func->func_name())))
    return 0;
  for (uint i=0; i < arg_count ; i++)
    if (!args[i]->eq(item_func->args[i], binary_cmp))
      return 0;
  return 1;
}


Field *Item_func::tmp_table_field(TABLE *table)
{
  Field *field= NULL;

  switch (result_type()) {
  case INT_RESULT:
    if (max_length > MY_INT32_NUM_DECIMAL_DIGITS)
      field= new Field_longlong(max_length, maybe_null, name, unsigned_flag);
    else
      field= new Field_long(max_length, maybe_null, name, unsigned_flag);
    break;
  case REAL_RESULT:
    field= new Field_double(max_length, maybe_null, name, decimals);
    break;
  case STRING_RESULT:
    return make_string_field(table);
    break;
  case DECIMAL_RESULT:
    field= Field_new_decimal::create_from_item(this);
    break;
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    field= 0;
    break;
  }
  if (field)
    field->init(table);
  return field;
}


bool Item_func::is_expensive_processor(uchar *arg)
{
  return is_expensive();
}


my_decimal *Item_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed);
  longlong nr= val_int();
  if (null_value)
    return 0; /* purecov: inspected */
  int2my_decimal(E_DEC_FATAL_ERROR, nr, unsigned_flag, decimal_value);
  return decimal_value;
}


String *Item_real_func::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  str->set_real(nr,decimals, &my_charset_bin);
  return str;
}


my_decimal *Item_real_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed);
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  double2my_decimal(E_DEC_FATAL_ERROR, nr, decimal_value);
  return decimal_value;
}


void Item_func::fix_num_length_and_dec()
{
  uint fl_length= 0;
  decimals=0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(decimals,args[i]->decimals);
    set_if_bigger(fl_length, args[i]->max_length);
  }
  max_length=float_length(decimals);
  if (fl_length > max_length)
  {
    decimals= NOT_FIXED_DEC;
    max_length= float_length(NOT_FIXED_DEC);
  }
}


void Item_func_numhybrid::fix_num_length_and_dec()
{}


/**
  Set max_length/decimals of function if function is fixed point and
  result length/precision depends on argument ones.
*/

void Item_func::count_decimal_length()
{
  int max_int_part= 0;
  decimals= 0;
  unsigned_flag= 1;
  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(decimals, args[i]->decimals);
    set_if_bigger(max_int_part, args[i]->decimal_int_part());
    set_if_smaller(unsigned_flag, args[i]->unsigned_flag);
  }
  int precision= min(max_int_part + decimals, DECIMAL_MAX_PRECISION);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


/**
  Set max_length of if it is maximum length of its arguments.
*/

void Item_func::count_only_length()
{
  max_length= 0;
  unsigned_flag= 0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(max_length, args[i]->max_length);
    set_if_bigger(unsigned_flag, args[i]->unsigned_flag);
  }
}


/**
  Set max_length/decimals of function if function is floating point and
  result length/precision depends on argument ones.
*/

void Item_func::count_real_length()
{
  uint32 length= 0;
  decimals= 0;
  max_length= 0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (decimals != NOT_FIXED_DEC)
    {
      set_if_bigger(decimals, args[i]->decimals);
      set_if_bigger(length, (args[i]->max_length - args[i]->decimals));
    }
    set_if_bigger(max_length, args[i]->max_length);
  }
  if (decimals != NOT_FIXED_DEC)
  {
    max_length= length;
    length+= decimals;
    if (length < max_length)  // If previous operation gave overflow
      max_length= UINT_MAX32;
    else
      max_length= length;
  }
}



void Item_func::signal_divide_by_null()
{
  THD *thd= current_thd;
  if (thd->variables.sql_mode & MODE_ERROR_FOR_DIVISION_BY_ZERO)
    push_warning(thd, MYSQL_ERROR::WARN_LEVEL_ERROR, ER_DIVISION_BY_ZERO,
                 ER(ER_DIVISION_BY_ZERO));
  null_value= 1;
}


Item *Item_func::get_tmp_table_item(THD *thd)
{
  if (!with_sum_func && !const_item())
    return new Item_field(result_field);
  return copy_or_same(thd);
}

double Item_int_func::val_real()
{
  DBUG_ASSERT(fixed == 1);

  return unsigned_flag ? (double) ((ulonglong) val_int()) : (double) val_int();
}


String *Item_int_func::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, &my_charset_bin);
  return str;
}


void Item_func_connection_id::fix_length_and_dec()
{
  Item_int_func::fix_length_and_dec();
  max_length= 10;
}


bool Item_func_connection_id::fix_fields(THD *thd, Item **ref)
{
  if (Item_int_func::fix_fields(thd, ref))
    return TRUE;
  thd->thread_specific_used= TRUE;
  value= thd->variables.pseudo_thread_id;
  return FALSE;
}


/**
  Check arguments here to determine result's type for a numeric
  function of two arguments.
*/

void Item_num_op::find_num_type(void)
{
  DBUG_ENTER("Item_num_op::find_num_type");
  DBUG_PRINT("info", ("name %s", func_name()));
  DBUG_ASSERT(arg_count == 2);
  Item_result r0= args[0]->result_type();
  Item_result r1= args[1]->result_type();

  if (r0 == REAL_RESULT || r1 == REAL_RESULT ||
      r0 == STRING_RESULT || r1 ==STRING_RESULT)
  {
    count_real_length();
    max_length= float_length(decimals);
    hybrid_type= REAL_RESULT;
  }
  else if (r0 == DECIMAL_RESULT || r1 == DECIMAL_RESULT)
  {
    hybrid_type= DECIMAL_RESULT;
    result_precision();
  }
  else
  {
    DBUG_ASSERT(r0 == INT_RESULT && r1 == INT_RESULT);
    decimals= 0;
    hybrid_type=INT_RESULT;
    result_precision();
  }
  DBUG_PRINT("info", ("Type: %s",
             (hybrid_type == REAL_RESULT ? "REAL_RESULT" :
              hybrid_type == DECIMAL_RESULT ? "DECIMAL_RESULT" :
              hybrid_type == INT_RESULT ? "INT_RESULT" :
              "--ILLEGAL!!!--")));
  DBUG_VOID_RETURN;
}


/**
  Set result type for a numeric function of one argument
  (can be also used by a numeric function of many arguments, if the result
  type depends only on the first argument)
*/

void Item_func_num1::find_num_type()
{
  DBUG_ENTER("Item_func_num1::find_num_type");
  DBUG_PRINT("info", ("name %s", func_name()));
  switch (hybrid_type= args[0]->result_type()) {
  case INT_RESULT:
    unsigned_flag= args[0]->unsigned_flag;
    break;
  case STRING_RESULT:
  case REAL_RESULT:
    hybrid_type= REAL_RESULT;
    max_length= float_length(decimals);
    break;
  case DECIMAL_RESULT:
    break;
  default:
    DBUG_ASSERT(0);
  }
  DBUG_PRINT("info", ("Type: %s",
                      (hybrid_type == REAL_RESULT ? "REAL_RESULT" :
                       hybrid_type == DECIMAL_RESULT ? "DECIMAL_RESULT" :
                       hybrid_type == INT_RESULT ? "INT_RESULT" :
                       "--ILLEGAL!!!--")));
  DBUG_VOID_RETURN;
}


void Item_func_num1::fix_num_length_and_dec()
{
  decimals= args[0]->decimals;
  max_length= args[0]->max_length;
}


void Item_func_numhybrid::fix_length_and_dec()
{
  fix_num_length_and_dec();
  find_num_type();
}


String *Item_func_numhybrid::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    if (!(val= decimal_op(&decimal_value)))
      return 0;                                 // null is set
    my_decimal_round(E_DEC_FATAL_ERROR, val, decimals, FALSE, val);
    my_decimal2string(E_DEC_FATAL_ERROR, val, 0, 0, 0, str);
    break;
  }
  case INT_RESULT:
  {
    longlong nr= int_op();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_int(nr, unsigned_flag, &my_charset_bin);
    break;
  }
  case REAL_RESULT:
  {
    double nr= real_op();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_real(nr,decimals,&my_charset_bin);
    break;
  }
  case STRING_RESULT:
    return str_op(&str_value);
  default:
    DBUG_ASSERT(0);
  }
  return str;
}


double Item_func_numhybrid::val_real()
{
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    double result;
    if (!(val= decimal_op(&decimal_value)))
      return 0.0;                               // null is set
    my_decimal2double(E_DEC_FATAL_ERROR, val, &result);
    return result;
  }
  case INT_RESULT:
  {
    longlong result= int_op();
    return unsigned_flag ? (double) ((ulonglong) result) : (double) result;
  }
  case REAL_RESULT:
    return real_op();
  case STRING_RESULT:
  {
    char *end_not_used;
    int err_not_used;
    String *res= str_op(&str_value);
    return (res ? my_strntod(res->charset(), (char*) res->ptr(), res->length(),
			     &end_not_used, &err_not_used) : 0.0);
  }
  default:
    DBUG_ASSERT(0);
  }
  return 0.0;
}


longlong Item_func_numhybrid::val_int()
{
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
  {
    my_decimal decimal_value, *val;
    if (!(val= decimal_op(&decimal_value)))
      return 0;                                 // null is set
    longlong result;
    my_decimal2int(E_DEC_FATAL_ERROR, val, unsigned_flag, &result);
    return result;
  }
  case INT_RESULT:
    return int_op();
  case REAL_RESULT:
    return (longlong) rint(real_op());
  case STRING_RESULT:
  {
    int err_not_used;
    String *res;
    if (!(res= str_op(&str_value)))
      return 0;

    char *end= (char*) res->ptr() + res->length();
    CHARSET_INFO *cs= res->charset();
    return (*(cs->cset->strtoll10))(cs, res->ptr(), &end, &err_not_used);
  }
  default:
    DBUG_ASSERT(0);
  }
  return 0;
}


my_decimal *Item_func_numhybrid::val_decimal(my_decimal *decimal_value)
{
  my_decimal *val= decimal_value;
  DBUG_ASSERT(fixed == 1);
  switch (hybrid_type) {
  case DECIMAL_RESULT:
    val= decimal_op(decimal_value);
    break;
  case INT_RESULT:
  {
    longlong result= int_op();
    int2my_decimal(E_DEC_FATAL_ERROR, result, unsigned_flag, decimal_value);
    break;
  }
  case REAL_RESULT:
  {
    double result= (double)real_op();
    double2my_decimal(E_DEC_FATAL_ERROR, result, decimal_value);
    break;
  }
  case STRING_RESULT:
  {
    String *res;
    if (!(res= str_op(&str_value)))
      return NULL;

    str2my_decimal(E_DEC_FATAL_ERROR, (char*) res->ptr(),
                   res->length(), res->charset(), decimal_value);
    break;
  }  
  case ROW_RESULT:
  default:
    DBUG_ASSERT(0);
  }
  return val;
}


void Item_func_signed::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as signed)"));

}


longlong Item_func_signed::val_int_from_str(int *error)
{
  char buff[MAX_FIELD_WIDTH], *end, *start;
  uint32 length;
  String tmp(buff,sizeof(buff), &my_charset_bin), *res;
  longlong value;

  /*
    For a string result, we must first get the string and then convert it
    to a longlong
  */

  if (!(res= args[0]->val_str(&tmp)))
  {
    null_value= 1;
    *error= 0;
    return 0;
  }
  null_value= 0;
  start= (char *)res->ptr();
  length= res->length();

  end= start + length;
  value= my_strtoll10(start, &end, error);
  if (*error > 0 || end != start+ length)
  {
    char err_buff[128];
    String err_tmp(err_buff,(uint32) sizeof(err_buff), system_charset_info);
    err_tmp.copy(start, length, system_charset_info);
    push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                        ER_TRUNCATED_WRONG_VALUE,
                        ER(ER_TRUNCATED_WRONG_VALUE), "INTEGER",
                        err_tmp.c_ptr());
  }
  return value;
}


longlong Item_func_signed::val_int()
{
  longlong value;
  int error;

  if (args[0]->cast_to_int_type() != STRING_RESULT ||
      args[0]->result_as_longlong())
  {
    value= args[0]->val_int();
    null_value= args[0]->null_value; 
    return value;
  }

  value= val_int_from_str(&error);
  if (value < 0 && error == 0)
  {
    push_warning(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "Cast to signed converted positive out-of-range integer to "
                 "it's negative complement");
  }
  return value;
}


void Item_func_unsigned::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as unsigned)"));

}


longlong Item_func_unsigned::val_int()
{
  longlong value;
  int error;

  if (args[0]->cast_to_int_type() == DECIMAL_RESULT)
  {
    my_decimal tmp, *dec= args[0]->val_decimal(&tmp);
    if (!(null_value= args[0]->null_value))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, 1, &value);
    else
      value= 0;
    return value;
  }
  else if (args[0]->cast_to_int_type() != STRING_RESULT ||
           args[0]->result_as_longlong())
  {
    value= args[0]->val_int();
    null_value= args[0]->null_value; 
    return value;
  }

  value= val_int_from_str(&error);
  if (error < 0)
    push_warning(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "Cast to unsigned converted negative integer to it's "
                 "positive complement");
  return value;
}


String *Item_decimal_typecast::val_str(String *str)
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  if (null_value)
    return NULL;
  my_decimal2string(E_DEC_FATAL_ERROR, tmp, 0, 0, 0, str);
  return str;
}


double Item_decimal_typecast::val_real()
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  double res;
  if (null_value)
    return 0.0;
  my_decimal2double(E_DEC_FATAL_ERROR, tmp, &res);
  return res;
}


longlong Item_decimal_typecast::val_int()
{
  my_decimal tmp_buf, *tmp= val_decimal(&tmp_buf);
  longlong res;
  if (null_value)
    return 0;
  my_decimal2int(E_DEC_FATAL_ERROR, tmp, unsigned_flag, &res);
  return res;
}


my_decimal *Item_decimal_typecast::val_decimal(my_decimal *dec)
{
  my_decimal tmp_buf, *tmp= args[0]->val_decimal(&tmp_buf);
  bool sign;
  uint precision;

  if ((null_value= args[0]->null_value))
    return NULL;
  my_decimal_round(E_DEC_FATAL_ERROR, tmp, decimals, FALSE, dec);
  sign= dec->sign();
  if (unsigned_flag)
  {
    if (sign)
    {
      my_decimal_set_zero(dec);
      goto err;
    }
  }
  precision= my_decimal_length_to_precision(max_length,
                                            decimals, unsigned_flag);
  if (precision - decimals < (uint) my_decimal_intg(dec))
  {
    max_my_decimal(dec, precision, decimals);
    dec->sign(sign);
    goto err;
  }
  return dec;

err:
  push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_ERROR,
                      ER_WARN_DATA_OUT_OF_RANGE,
                      ER(ER_WARN_DATA_OUT_OF_RANGE),
                      name, 1L);
  return dec;
}


void Item_decimal_typecast::print(String *str, enum_query_type query_type)
{
  char len_buf[20*3 + 1];
  char *end;

  uint precision= my_decimal_length_to_precision(max_length, decimals,
                                                 unsigned_flag);
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as decimal("));

  end=int10_to_str(precision, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(',');

  end=int10_to_str(decimals, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(')');
  str->append(')');
}


double Item_func_plus::real_op()
{
  double value= args[0]->val_real() + args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return fix_result(value);
}


longlong Item_func_plus::int_op()
{
  longlong value=args[0]->val_int()+args[1]->val_int();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0;
  return value;
}


/**
  Calculate plus of two decimals.

  @param decimal_value	Buffer that can be used to store result

  @retval
    0  Value was NULL;  In this case null_value is set
  @retval
    \# Value of operation as a decimal
*/

my_decimal *Item_func_plus::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     (my_decimal_add(E_DEC_FATAL_ERROR, decimal_value, val1,
                                     val2) > 3))))
    return decimal_value;
  return 0;
}

/**
  Set precision of results for additive operations (+ and -)
*/
void Item_func_additive_op::result_precision()
{
  decimals= max(args[0]->decimals, args[1]->decimals);
  int arg1_int= args[0]->decimal_precision() - args[0]->decimals;
  int arg2_int= args[1]->decimal_precision() - args[1]->decimals;
  int precision= max(arg1_int, arg2_int) + 1 + decimals;

  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


/**
  The following function is here to allow the user to force
  subtraction of UNSIGNED BIGINT to return negative values.
*/

void Item_func_minus::fix_length_and_dec()
{
  Item_num_op::fix_length_and_dec();
  if (unsigned_flag &&
      (current_thd->variables.sql_mode & MODE_NO_UNSIGNED_SUBTRACTION))
    unsigned_flag=0;
}


double Item_func_minus::real_op()
{
  double value= args[0]->val_real() - args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return fix_result(value);
}


longlong Item_func_minus::int_op()
{
  longlong value=args[0]->val_int() - args[1]->val_int();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0;
  return value;
}


/**
  See Item_func_plus::decimal_op for comments.
*/

my_decimal *Item_func_minus::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2= 

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     (my_decimal_sub(E_DEC_FATAL_ERROR, decimal_value, val1,
                                     val2) > 3))))
    return decimal_value;
  return 0;
}


double Item_func_mul::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real() * args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return fix_result(value);
}


longlong Item_func_mul::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong value=args[0]->val_int()*args[1]->val_int();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0;
  return value;
}


/** See Item_func_plus::decimal_op for comments. */

my_decimal *Item_func_mul::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if (!(null_value= (args[1]->null_value ||
                     (my_decimal_mul(E_DEC_FATAL_ERROR, decimal_value, val1,
                                    val2) > 3))))
    return decimal_value;
  return 0;
}


void Item_func_mul::result_precision()
{
  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  decimals= min(args[0]->decimals + args[1]->decimals, DECIMAL_MAX_SCALE);
  uint est_prec = args[0]->decimal_precision() + args[1]->decimal_precision();
  uint precision= min(est_prec, DECIMAL_MAX_PRECISION);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


double Item_func_div::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0;
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return fix_result(value/val2);
}


my_decimal *Item_func_div::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;
  int err;

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if ((null_value= args[1]->null_value))
    return 0;
  if ((err= my_decimal_div(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, decimal_value,
                           val1, val2, prec_increment)) > 3)
  {
    if (err == E_DEC_DIV_ZERO)
      signal_divide_by_null();
    null_value= 1;
    return 0;
  }
  return decimal_value;
}


void Item_func_div::result_precision()
{
  uint precision=min(args[0]->decimal_precision() + 
                     args[1]->decimals + prec_increment,
                     DECIMAL_MAX_PRECISION);

  /* Integer operations keep unsigned_flag if one of arguments is unsigned */
  if (result_type() == INT_RESULT)
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
  else
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
  decimals= min(args[0]->decimals + prec_increment, DECIMAL_MAX_SCALE);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


void Item_func_div::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_div::fix_length_and_dec");
  prec_increment= current_thd->variables.div_precincrement;
  Item_num_op::fix_length_and_dec();
  switch(hybrid_type) {
  case REAL_RESULT:
  {
    decimals=max(args[0]->decimals,args[1]->decimals)+prec_increment;
    set_if_smaller(decimals, NOT_FIXED_DEC);
    uint tmp=float_length(decimals);
    if (decimals == NOT_FIXED_DEC)
      max_length= tmp;
    else
    {
      max_length=args[0]->max_length - args[0]->decimals + decimals;
      set_if_smaller(max_length,tmp);
    }
    break;
  }
  case INT_RESULT:
    hybrid_type= DECIMAL_RESULT;
    DBUG_PRINT("info", ("Type changed: DECIMAL_RESULT"));
    result_precision();
    break;
  case DECIMAL_RESULT:
    result_precision();
    break;
  default:
    DBUG_ASSERT(0);
  }
  maybe_null= 1; // devision by zero
  DBUG_VOID_RETURN;
}


/* Integer division */
longlong Item_func_int_div::val_int()
{
  DBUG_ASSERT(fixed == 1);
  longlong value=args[0]->val_int();
  longlong val2=args[1]->val_int();
  if ((null_value= (args[0]->null_value || args[1]->null_value)))
    return 0;
  if (val2 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  if (unsigned_flag)
    return  ((ulonglong) value / (ulonglong) val2);
  else if (value == LONGLONG_MIN && val2 == -1)
    return LONGLONG_MIN;
  else
    return value / val2;
}


void Item_func_int_div::fix_length_and_dec()
{
  Item_result argtype= args[0]->result_type();
  /* use precision ony for the data type it is applicable for and valid */
  max_length=args[0]->max_length -
    (argtype == DECIMAL_RESULT || argtype == INT_RESULT ?
     args[0]->decimals : 0);
  maybe_null=1;
  unsigned_flag=args[0]->unsigned_flag | args[1]->unsigned_flag;
}


longlong Item_func_mod::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong value=  args[0]->val_int();
  longlong val2= args[1]->val_int();
  longlong result;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0; /* purecov: inspected */
  if (val2 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  if (args[0]->unsigned_flag)
    result= args[1]->unsigned_flag ? 
      ((ulonglong) value) % ((ulonglong) val2) : ((ulonglong) value) % val2;
  else result= args[1]->unsigned_flag ?
         value % ((ulonglong) val2) :
         (val2 == -1) ? 0 : value % val2;

  return result;
}

double Item_func_mod::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2=  args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0; /* purecov: inspected */
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return fmod(value,val2);
}


my_decimal *Item_func_mod::decimal_op(my_decimal *decimal_value)
{
  my_decimal value1, *val1;
  my_decimal value2, *val2;

  val1= args[0]->val_decimal(&value1);
  if ((null_value= args[0]->null_value))
    return 0;
  val2= args[1]->val_decimal(&value2);
  if ((null_value= args[1]->null_value))
    return 0;
  switch (my_decimal_mod(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, decimal_value,
                         val1, val2)) {
  case E_DEC_TRUNCATED:
  case E_DEC_OK:
    return decimal_value;
  case E_DEC_DIV_ZERO:
    signal_divide_by_null();
  default:
    null_value= 1;
    return 0;
  }
}


void Item_func_mod::result_precision()
{
  decimals= max(args[0]->decimals, args[1]->decimals);
  max_length= max(args[0]->max_length, args[1]->max_length);
}


void Item_func_mod::fix_length_and_dec()
{
  Item_num_op::fix_length_and_dec();
  maybe_null= 1;
  unsigned_flag= args[0]->unsigned_flag;
}


double Item_func_neg::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return -value;
}


longlong Item_func_neg::int_op()
{
  longlong value= args[0]->val_int();
  null_value= args[0]->null_value;
  return -value;
}


my_decimal *Item_func_neg::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= args[0]->null_value))
  {
    my_decimal2decimal(value, decimal_value);
    my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}


void Item_func_neg::fix_num_length_and_dec()
{
  decimals= args[0]->decimals;
  /* 1 add because sign can appear */
  max_length= args[0]->max_length + 1;
}


void Item_func_neg::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_neg::fix_length_and_dec");
  Item_func_num1::fix_length_and_dec();

  /*
    If this is in integer context keep the context as integer if possible
    (This is how multiplication and other integer functions works)
    Use val() to get value as arg_type doesn't mean that item is
    Item_int or Item_real due to existence of Item_param.
  */
  if (hybrid_type == INT_RESULT && args[0]->const_item())
  {
    longlong val= args[0]->val_int();
    if ((ulonglong) val >= (ulonglong) LONGLONG_MIN &&
        ((ulonglong) val != (ulonglong) LONGLONG_MIN ||
          args[0]->type() != INT_ITEM))        
    {
      /*
        Ensure that result is converted to DECIMAL, as longlong can't hold
        the negated number
      */
      hybrid_type= DECIMAL_RESULT;
      DBUG_PRINT("info", ("Type changed: DECIMAL_RESULT"));
    }
  }
  unsigned_flag= 0;
  DBUG_VOID_RETURN;
}


double Item_func_abs::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return fabs(value);
}


longlong Item_func_abs::int_op()
{
  longlong value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  return (value >= 0) || unsigned_flag ? value : -value;
}


my_decimal *Item_func_abs::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= args[0]->null_value))
  {
    my_decimal2decimal(value, decimal_value);
    if (decimal_value->sign())
      my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}


void Item_func_abs::fix_length_and_dec()
{
  Item_func_num1::fix_length_and_dec();
  unsigned_flag= args[0]->unsigned_flag;
}


/** Gateway to natural LOG function. */
double Item_func_ln::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log(value);
}

/** 
  Extended but so slower LOG function.

  We have to check if all values are > zero and first one is not one
  as these are the cases then result is not a number.
*/ 
double Item_func_log::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  if (arg_count == 2)
  {
    double value2= args[1]->val_real();
    if ((null_value= args[1]->null_value))
      return 0.0;
    if (value2 <= 0.0 || value == 1.0)
    {
      signal_divide_by_null();
      return 0.0;
    }
    return log(value2) / log(value);
  }
  return log(value);
}

double Item_func_log2::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();

  if ((null_value=args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log(value) / M_LN2;
}

double Item_func_log10::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log10(value);
}

double Item_func_exp::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0; /* purecov: inspected */
  return fix_result(exp(value));
}

double Item_func_sqrt::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || value < 0)))
    return 0.0; /* purecov: inspected */
  return sqrt(value);
}

double Item_func_pow::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value=(args[0]->null_value || args[1]->null_value)))
    return 0.0; /* purecov: inspected */
  return fix_result(pow(value,val2));
}

// Trigonometric functions

double Item_func_acos::val_real()
{
  DBUG_ASSERT(fixed == 1);
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return acos(value);
}

double Item_func_asin::val_real()
{
  DBUG_ASSERT(fixed == 1);
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return asin(value);
}

double Item_func_atan::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  if (arg_count == 2)
  {
    double val2= args[1]->val_real();
    if ((null_value=args[1]->null_value))
      return 0.0;
    return fix_result(atan2(value,val2));
  }
  return atan(value);
}

double Item_func_cos::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return cos(value);
}

double Item_func_sin::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return sin(value);
}

double Item_func_tan::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return fix_result(tan(value));
}


// Shift-functions, same as << and >> in C/C++


longlong Item_func_shift_left::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint shift;
  ulonglong res= ((ulonglong) args[0]->val_int() <<
		  (shift=(uint) args[1]->val_int()));
  if (args[0]->null_value || args[1]->null_value)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (shift < sizeof(longlong)*8 ? (longlong) res : LL(0));
}

longlong Item_func_shift_right::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint shift;
  ulonglong res= (ulonglong) args[0]->val_int() >>
    (shift=(uint) args[1]->val_int());
  if (args[0]->null_value || args[1]->null_value)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (shift < sizeof(longlong)*8 ? (longlong) res : LL(0));
}


longlong Item_func_bit_neg::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong res= (ulonglong) args[0]->val_int();
  if ((null_value=args[0]->null_value))
    return 0;
  return ~res;
}


// Conversion functions

void Item_func_integer::fix_length_and_dec()
{
  max_length=args[0]->max_length - args[0]->decimals+1;
  uint tmp=float_length(decimals);
  set_if_smaller(max_length,tmp);
  decimals=0;
}

void Item_func_int_val::fix_num_length_and_dec()
{
  ulonglong tmp_max_length= (ulonglong ) args[0]->max_length - 
    (args[0]->decimals ? args[0]->decimals + 1 : 0) + 2;
  max_length= tmp_max_length > (ulonglong) max_field_size ?
    max_field_size : (uint32) tmp_max_length;
  uint tmp= float_length(decimals);
  set_if_smaller(max_length,tmp);
  decimals= 0;
}


void Item_func_int_val::find_num_type()
{
  DBUG_ENTER("Item_func_int_val::find_num_type");
  DBUG_PRINT("info", ("name %s", func_name()));
  switch(hybrid_type= args[0]->result_type())
  {
  case STRING_RESULT:
  case REAL_RESULT:
    hybrid_type= REAL_RESULT;
    max_length= float_length(decimals);
    break;
  case INT_RESULT:
  case DECIMAL_RESULT:
    /*
      -2 because in most high position can't be used any digit for longlong
      and one position for increasing value during operation
    */
    if ((args[0]->max_length - args[0]->decimals) >=
        (DECIMAL_LONGLONG_DIGITS - 2))
    {
      hybrid_type= DECIMAL_RESULT;
    }
    else
    {
      unsigned_flag= args[0]->unsigned_flag;
      hybrid_type= INT_RESULT;
    }
    break;
  default:
    DBUG_ASSERT(0);
  }
  DBUG_PRINT("info", ("Type: %s",
                      (hybrid_type == REAL_RESULT ? "REAL_RESULT" :
                       hybrid_type == DECIMAL_RESULT ? "DECIMAL_RESULT" :
                       hybrid_type == INT_RESULT ? "INT_RESULT" :
                       "--ILLEGAL!!!--")));

  DBUG_VOID_RETURN;
}


longlong Item_func_ceiling::int_op()
{
  longlong result;
  switch (args[0]->result_type()) {
  case INT_RESULT:
    result= args[0]->val_int();
    null_value= args[0]->null_value;
    break;
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec;
    if ((dec= Item_func_ceiling::decimal_op(&dec_buf)))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
    else
      result= 0;
    break;
  }
  default:
    result= (longlong)Item_func_ceiling::real_op();
  };
  return result;
}


double Item_func_ceiling::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return ceil(value);
}


my_decimal *Item_func_ceiling::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= (args[0]->null_value ||
                     my_decimal_ceiling(E_DEC_FATAL_ERROR, value,
                                        decimal_value) > 1)))
    return decimal_value;
  return 0;
}


longlong Item_func_floor::int_op()
{
  longlong result;
  switch (args[0]->result_type()) {
  case INT_RESULT:
    result= args[0]->val_int();
    null_value= args[0]->null_value;
    break;
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec;
    if ((dec= Item_func_floor::decimal_op(&dec_buf)))
      my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
    else
      result= 0;
    break;
  }
  default:
    result= (longlong)Item_func_floor::real_op();
  };
  return result;
}


double Item_func_floor::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return floor(value);
}


my_decimal *Item_func_floor::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  if (!(null_value= (args[0]->null_value ||
                     my_decimal_floor(E_DEC_FATAL_ERROR, value,
                                      decimal_value) > 1)))
    return decimal_value;
  return 0;
}


void Item_func_round::fix_length_and_dec()
{
  int      decimals_to_set;
  longlong val1;
  bool     val1_unsigned;
  
  unsigned_flag= args[0]->unsigned_flag;
  if (!args[1]->const_item())
  {
    decimals= args[0]->decimals;
    max_length= float_length(decimals);
    if (args[0]->result_type() == DECIMAL_RESULT)
    {
      max_length++;
      hybrid_type= DECIMAL_RESULT;
    }
    else
      hybrid_type= REAL_RESULT;
    return;
  }

  val1= args[1]->val_int();
  if ((null_value= args[1]->is_null()))
    return;

  val1_unsigned= args[1]->unsigned_flag;
  if (val1 < 0)
    decimals_to_set= val1_unsigned ? INT_MAX : 0;
  else
    decimals_to_set= (val1 > INT_MAX) ? INT_MAX : (int) val1;

  if (args[0]->decimals == NOT_FIXED_DEC)
  {
    decimals= min(decimals_to_set, NOT_FIXED_DEC);
    max_length= float_length(decimals);
    hybrid_type= REAL_RESULT;
    return;
  }
  
  switch (args[0]->result_type()) {
  case REAL_RESULT:
  case STRING_RESULT:
    hybrid_type= REAL_RESULT;
    decimals= min(decimals_to_set, NOT_FIXED_DEC);
    max_length= float_length(decimals);
    break;
  case INT_RESULT:
    if ((!decimals_to_set && truncate) || (args[0]->decimal_precision() < DECIMAL_LONGLONG_DIGITS))
    {
      int length_can_increase= test(!truncate && (val1 < 0) && !val1_unsigned);
      max_length= args[0]->max_length + length_can_increase;
      /* Here we can keep INT_RESULT */
      hybrid_type= INT_RESULT;
      decimals= 0;
      break;
    }
    /* fall through */
  case DECIMAL_RESULT:
  {
    hybrid_type= DECIMAL_RESULT;
    decimals_to_set= min(DECIMAL_MAX_SCALE, decimals_to_set);
    int decimals_delta= args[0]->decimals - decimals_to_set;
    int precision= args[0]->decimal_precision();
    int length_increase= ((decimals_delta <= 0) || truncate) ? 0:1;

    precision-= decimals_delta - length_increase;
    decimals= min(decimals_to_set, DECIMAL_MAX_SCALE);
    max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                             decimals,
                                                             unsigned_flag);
    break;
  }
  default:
    DBUG_ASSERT(0); /* This result type isn't handled */
  }
}

double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate)
{
  double tmp;
  bool dec_negative= (dec < 0) && !dec_unsigned;
  ulonglong abs_dec= dec_negative ? -dec : dec;
  /*
    tmp2 is here to avoid return the value with 80 bit precision
    This will fix that the test round(0.1,1) = round(0.1,1) is true
  */
  volatile double tmp2;

  tmp=(abs_dec < array_elements(log_10) ?
       log_10[abs_dec] : pow(10.0,(double) abs_dec));

  if (dec_negative && my_isinf(tmp))
    tmp2= 0;
  else if (!dec_negative && my_isinf(value * tmp))
    tmp2= value;
  else if (truncate)
  {
    if (value >= 0)
      tmp2= dec < 0 ? floor(value/tmp)*tmp : floor(value*tmp)/tmp;
    else
      tmp2= dec < 0 ? ceil(value/tmp)*tmp : ceil(value*tmp)/tmp;
  }
  else
    tmp2=dec < 0 ? rint(value/tmp)*tmp : rint(value*tmp)/tmp;
  return tmp2;
}


double Item_func_round::real_op()
{
  double value= args[0]->val_real();

  if (!(null_value= args[0]->null_value || args[1]->null_value))
    return my_double_round(value, args[1]->val_int(), args[1]->unsigned_flag,
                           truncate);

  return 0.0;
}

/*
  Rounds a given value to a power of 10 specified as the 'to' argument,
  avoiding overflows when the value is close to the ulonglong range boundary.
*/

static inline ulonglong my_unsigned_round(ulonglong value, ulonglong to)
{
  ulonglong tmp= value / to * to;
  return (value - tmp < (to >> 1)) ? tmp : tmp + to;
}


longlong Item_func_round::int_op()
{
  longlong value= args[0]->val_int();
  longlong dec= args[1]->val_int();
  decimals= 0;
  ulonglong abs_dec;
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;
  if ((dec >= 0) || args[1]->unsigned_flag)
    return value; // integer have not digits after point

  abs_dec= -dec;
  longlong tmp;
  
  if(abs_dec >= array_elements(log_10_int))
    return 0;
  
  tmp= log_10_int[abs_dec];
  
  if (truncate)
    value= (unsigned_flag) ?
      ((ulonglong) value / tmp) * tmp : (value / tmp) * tmp;
  else
    value= (unsigned_flag || value >= 0) ?
      my_unsigned_round((ulonglong) value, tmp) :
      -(longlong) my_unsigned_round((ulonglong) -value, tmp);
  return value;
}


my_decimal *Item_func_round::decimal_op(my_decimal *decimal_value)
{
  my_decimal val, *value= args[0]->val_decimal(&val);
  longlong dec= args[1]->val_int();
  if (dec >= 0 || args[1]->unsigned_flag)
    dec= min((ulonglong) dec, decimals);
  else if (dec < INT_MIN)
    dec= INT_MIN;
    
  if (!(null_value= (args[0]->null_value || args[1]->null_value ||
                     my_decimal_round(E_DEC_FATAL_ERROR, value, (int) dec,
                                      truncate, decimal_value) > 1))) 
    return decimal_value;
  return 0;
}


void Item_func_rand::seed_random(Item *arg)
{
  /*
    TODO: do not do reinit 'rand' for every execute of PS/SP if
    args[0] is a constant.
  */
  uint32 tmp= (uint32) arg->val_int();
  randominit(rand, (uint32) (tmp*0x10001L+55555555L),
             (uint32) (tmp*0x10000001L));
}


bool Item_func_rand::fix_fields(THD *thd,Item **ref)
{
  if (Item_real_func::fix_fields(thd, ref))
    return TRUE;
  used_tables_cache|= RAND_TABLE_BIT;
  if (arg_count)
  {					// Only use argument once in query
    /*
      Allocate rand structure once: we must use thd->stmt_arena
      to create rand in proper mem_root if it's a prepared statement or
      stored procedure.

      No need to send a Rand log event if seed was given eg: RAND(seed),
      as it will be replicated in the query as such.
    */
    if (!rand && !(rand= (struct rand_struct*)
                   thd->stmt_arena->alloc(sizeof(*rand))))
      return TRUE;
  }
  else
  {
    /*
      Save the seed only the first time RAND() is used in the query
      Once events are forwarded rather than recreated,
      the following can be skipped if inside the slave thread
    */
    if (!thd->rand_used)
    {
      thd->rand_used= 1;
      thd->rand_saved_seed1= thd->rand.seed1;
      thd->rand_saved_seed2= thd->rand.seed2;
    }
    rand= &thd->rand;
  }
  return FALSE;
}

void Item_func_rand::update_used_tables()
{
  Item_real_func::update_used_tables();
  used_tables_cache|= RAND_TABLE_BIT;
}


double Item_func_rand::val_real()
{
  DBUG_ASSERT(fixed == 1);
  if (arg_count)
  {
    if (!args[0]->const_item())
      seed_random(args[0]);
    else if (first_eval)
    {
      /*
        Constantness of args[0] may be set during JOIN::optimize(), if arg[0]
        is a field item of "constant" table. Thus, we have to evaluate
        seed_random() for constant arg there but not at the fix_fields method.
      */
      first_eval= FALSE;
      seed_random(args[0]);
    }
  }
  return my_rnd(rand);
}

longlong Item_func_sign::val_int()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  null_value=args[0]->null_value;
  return value < 0.0 ? -1 : (value > 0 ? 1 : 0);
}


double Item_func_units::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0;
  return value*mul+add;
}


void Item_func_min_max::fix_length_and_dec()
{
  int max_int_part=0;
  bool datetime_found= FALSE;
  decimals=0;
  max_length=0;
  maybe_null=0;
  cmp_type=args[0]->result_type();

  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(max_length, args[i]->max_length);
    set_if_bigger(decimals, args[i]->decimals);
    set_if_bigger(max_int_part, args[i]->decimal_int_part());
    if (args[i]->maybe_null)
      maybe_null=1;
    cmp_type=item_cmp_type(cmp_type,args[i]->result_type());
    if (args[i]->result_type() != ROW_RESULT && args[i]->is_datetime())
    {
      datetime_found= TRUE;
      if (!datetime_item || args[i]->field_type() == MYSQL_TYPE_DATETIME)
        datetime_item= args[i];
    }
  }
  if (cmp_type == STRING_RESULT)
  {
    agg_arg_charsets(collation, args, arg_count, MY_COLL_CMP_CONV, 1);
    if (datetime_found)
    {
      thd= current_thd;
      compare_as_dates= TRUE;
    }
  }
  else if ((cmp_type == DECIMAL_RESULT) || (cmp_type == INT_RESULT))
    max_length= my_decimal_precision_to_length_no_truncation(max_int_part +
                                                             decimals, decimals,
                                                             unsigned_flag);
  else if (cmp_type == REAL_RESULT)
    max_length= float_length(decimals);
  cached_field_type= agg_field_type(args, arg_count);
}


/*
  Compare item arguments in the DATETIME context.

  SYNOPSIS
    cmp_datetimes()
    value [out]   found least/greatest DATE/DATETIME value

  DESCRIPTION
    Compare item arguments as DATETIME values and return the index of the
    least/greatest argument in the arguments array.
    The correct integer DATE/DATETIME value of the found argument is
    stored to the value pointer, if latter is provided.

  RETURN
   0	If one of arguments is NULL or there was a execution error
   #	index of the least/greatest argument
*/

uint Item_func_min_max::cmp_datetimes(ulonglong *value)
{
  longlong UNINIT_VAR(min_max);
  uint min_max_idx= 0;

  for (uint i=0; i < arg_count ; i++)
  {
    Item **arg= args + i;
    bool is_null;
    longlong res= get_datetime_value(thd, &arg, 0, datetime_item, &is_null);

    /* Check if we need to stop (because of error or KILL)  and stop the loop */
    if (thd->is_error())
    {
      null_value= 1;
      return 0;
    }

    if ((null_value= args[i]->null_value))
      return 0;
    if (i == 0 || (res < min_max ? cmp_sign : -cmp_sign) > 0)
    {
      min_max= res;
      min_max_idx= i;
    }
  }
  if (value)
  {
    *value= min_max;
    if (datetime_item->field_type() == MYSQL_TYPE_DATE)
      *value/= 1000000L;
  }
  return min_max_idx;
}


String *Item_func_min_max::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  if (compare_as_dates)
  {
    String *str_res;
    uint min_max_idx= cmp_datetimes(NULL);
    if (null_value)
      return 0;
    str_res= args[min_max_idx]->val_str(str);
    if (args[min_max_idx]->null_value)
    {
      // check if the call to val_str() above returns a NULL value
      null_value= 1;
      return NULL;
    }
    str_res->set_charset(collation.collation);
    return str_res;
  }
  switch (cmp_type) {
  case INT_RESULT:
  {
    longlong nr=val_int();
    if (null_value)
      return 0;
    str->set_int(nr, unsigned_flag, &my_charset_bin);
    return str;
  }
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec_val= val_decimal(&dec_buf);
    if (null_value)
      return 0;
    my_decimal2string(E_DEC_FATAL_ERROR, dec_val, 0, 0, 0, str);
    return str;
  }
  case REAL_RESULT:
  {
    double nr= val_real();
    if (null_value)
      return 0; /* purecov: inspected */
    str->set_real(nr,decimals,&my_charset_bin);
    return str;
  }
  case STRING_RESULT:
  {
    String *UNINIT_VAR(res);
    for (uint i=0; i < arg_count ; i++)
    {
      if (i == 0)
	res=args[i]->val_str(str);
      else
      {
	String *res2;
	res2= args[i]->val_str(res == str ? &tmp_value : str);
	if (res2)
	{
	  int cmp= sortcmp(res,res2,collation.collation);
	  if ((cmp_sign < 0 ? cmp : -cmp) < 0)
	    res=res2;
	}
      }
      if ((null_value= args[i]->null_value))
        return 0;
    }
    res->set_charset(collation.collation);
    return res;
  }
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    return 0;
  }
  return 0;					// Keep compiler happy
}


double Item_func_min_max::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double value=0.0;
  if (compare_as_dates)
  {
    ulonglong result= 0;
    (void)cmp_datetimes(&result);
    return (double)result;
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      value= args[i]->val_real();
    else
    {
      double tmp= args[i]->val_real();
      if (!args[i]->null_value && (tmp < value ? cmp_sign : -cmp_sign) > 0)
	value=tmp;
    }
    if ((null_value= args[i]->null_value))
      break;
  }
  return value;
}


longlong Item_func_min_max::val_int()
{
  DBUG_ASSERT(fixed == 1);
  longlong value=0;
  if (compare_as_dates)
  {
    ulonglong result= 0;
    (void)cmp_datetimes(&result);
    return (longlong)result;
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      value=args[i]->val_int();
    else
    {
      longlong tmp=args[i]->val_int();
      if (!args[i]->null_value && (tmp < value ? cmp_sign : -cmp_sign) > 0)
	value=tmp;
    }
    if ((null_value= args[i]->null_value))
      break;
  }
  return value;
}


my_decimal *Item_func_min_max::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed == 1);
  my_decimal tmp_buf, *tmp, *UNINIT_VAR(res);

  if (compare_as_dates)
  {
    ulonglong value= 0;
    (void)cmp_datetimes(&value);
    ulonglong2decimal(value, dec);
    return dec;
  }
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      res= args[i]->val_decimal(dec);
    else
    {
      tmp= args[i]->val_decimal(&tmp_buf);      // Zero if NULL
      if (tmp && (my_decimal_cmp(tmp, res) * cmp_sign) < 0)
      {
        if (tmp == &tmp_buf)
        {
          /* Move value out of tmp_buf as this will be reused on next loop */
          my_decimal2decimal(tmp, dec);
          res= dec;
        }
        else
          res= tmp;
      }
    }
    if ((null_value= args[i]->null_value))
    {
      res= 0;
      break;
    }
  }
  return res;
}


longlong Item_func_length::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->length();
}


longlong Item_func_char_length::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->numchars();
}


longlong Item_func_coercibility::val_int()
{
  DBUG_ASSERT(fixed == 1);
  null_value= 0;
  return (longlong) args[0]->collation.derivation;
}


void Item_func_locate::fix_length_and_dec()
{
  max_length= MY_INT32_NUM_DECIMAL_DIGITS;
  agg_arg_charsets(cmp_collation, args, 2, MY_COLL_CMP_CONV, 1);
}


longlong Item_func_locate::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *a=args[0]->val_str(&value1);
  String *b=args[1]->val_str(&value2);
  if (!a || !b)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  /* must be longlong to avoid truncation */
  longlong start=  0; 
  longlong start0= 0;
  my_match_t match;

  if (arg_count == 3)
  {
    start0= start= args[2]->val_int() - 1;

    if ((start < 0) || (start > a->length()))
      return 0;

    /* start is now sufficiently valid to pass to charpos function */
    start= a->charpos((int) start);

    if (start + b->length() > a->length())
      return 0;
  }

  if (!b->length())				// Found empty string at start
    return start + 1;
  
  if (!cmp_collation.collation->coll->instr(cmp_collation.collation,
                                            a->ptr()+start,
                                            (uint) (a->length()-start),
                                            b->ptr(), b->length(),
                                            &match, 1))
    return 0;
  return (longlong) match.mb_len + start0 + 1;
}


void Item_func_locate::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("locate("));
  args[1]->print(str, query_type);
  str->append(',');
  args[0]->print(str, query_type);
  if (arg_count == 3)
  {
    str->append(',');
    args[2]->print(str, query_type);
  }
  str->append(')');
}


longlong Item_func_field::val_int()
{
  DBUG_ASSERT(fixed == 1);

  if (cmp_type == STRING_RESULT)
  {
    String *field;
    if (!(field= args[0]->val_str(&value)))
      return 0;
    for (uint i=1 ; i < arg_count ; i++)
    {
      String *tmp_value=args[i]->val_str(&tmp);
      if (tmp_value && !sortcmp(field,tmp_value,cmp_collation.collation))
        return (longlong) (i);
    }
  }
  else if (cmp_type == INT_RESULT)
  {
    longlong val= args[0]->val_int();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_int() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  else if (cmp_type == DECIMAL_RESULT)
  {
    my_decimal dec_arg_buf, *dec_arg,
               dec_buf, *dec= args[0]->val_decimal(&dec_buf);
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count; i++)
    {
      dec_arg= args[i]->val_decimal(&dec_arg_buf);
      if (!args[i]->null_value && !my_decimal_cmp(dec_arg, dec))
        return (longlong) (i);
    }
  }
  else
  {
    double val= args[0]->val_real();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_real() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  return 0;
}


void Item_func_field::fix_length_and_dec()
{
  maybe_null=0; max_length=3;
  cmp_type= args[0]->result_type();
  for (uint i=1; i < arg_count ; i++)
    cmp_type= item_cmp_type(cmp_type, args[i]->result_type());
  if (cmp_type == STRING_RESULT)
    agg_arg_charsets(cmp_collation, args, arg_count, MY_COLL_CMP_CONV, 1);
}


longlong Item_func_ascii::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (longlong) (res->length() ? (uchar) (*res)[0] : (uchar) 0);
}

longlong Item_func_ord::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  if (!res->length()) return 0;
#ifdef USE_MB
  if (use_mb(res->charset()))
  {
    register const char *str=res->ptr();
    register uint32 n=0, l=my_ismbchar(res->charset(),str,str+res->length());
    if (!l)
      return (longlong)((uchar) *str);
    while (l--)
      n=(n<<8)|(uint32)((uchar) *str++);
    return (longlong) n;
  }
#endif
  return (longlong) ((uchar) (*res)[0]);
}

	/* Search after a string in a string of strings separated by ',' */
	/* Returns number of found type >= 1 or 0 if not found */
	/* This optimizes searching in enums to bit testing! */

void Item_func_find_in_set::fix_length_and_dec()
{
  decimals=0;
  max_length=3;					// 1-999
  if (args[0]->const_item() && args[1]->type() == FIELD_ITEM)
  {
    Field *field= ((Item_field*) args[1])->field;
    if (field->real_type() == MYSQL_TYPE_SET)
    {
      String *find=args[0]->val_str(&value);
      if (find)
      {
	enum_value= find_type(((Field_enum*) field)->typelib,find->ptr(),
			      find->length(), 0);
	enum_bit=0;
	if (enum_value)
	  enum_bit=LL(1) << (enum_value-1);
      }
    }
  }
  agg_arg_charsets(cmp_collation, args, 2, MY_COLL_CMP_CONV, 1);
}

static const char separator=',';

longlong Item_func_find_in_set::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if (enum_value)
  {
    ulonglong tmp=(ulonglong) args[1]->val_int();
    if (!(null_value=args[1]->null_value || args[0]->null_value))
    {
      if (tmp & enum_bit)
	return enum_value;
    }
    return 0L;
  }

  String *find=args[0]->val_str(&value);
  String *buffer=args[1]->val_str(&value2);
  if (!find || !buffer)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;

  int diff;
  if ((diff=buffer->length() - find->length()) >= 0)
  {
    my_wc_t wc= 0;
    CHARSET_INFO *cs= cmp_collation.collation;
    const char *str_begin= buffer->ptr();
    const char *str_end= buffer->ptr();
    const char *real_end= str_end+buffer->length();
    const uchar *find_str= (const uchar *) find->ptr();
    uint find_str_len= find->length();
    int position= 0;
    while (1)
    {
      int symbol_len;
      if ((symbol_len= cs->cset->mb_wc(cs, &wc, (uchar*) str_end, 
                                       (uchar*) real_end)) > 0)
      {
        const char *substr_end= str_end + symbol_len;
        bool is_last_item= (substr_end == real_end);
        bool is_separator= (wc == (my_wc_t) separator);
        if (is_separator || is_last_item)
        {
          position++;
          if (is_last_item && !is_separator)
            str_end= substr_end;
          if (!my_strnncoll(cs, (const uchar *) str_begin,
                            (uint) (str_end - str_begin),
                            find_str, find_str_len))
            return (longlong) position;
          else
            str_begin= substr_end;
        }
        str_end= substr_end;
      }
      else if (str_end - str_begin == 0 &&
               find_str_len == 0 &&
               wc == (my_wc_t) separator)
        return (longlong) ++position;
      else
        return LL(0);
    }
  }
  return 0;
}

longlong Item_func_bit_count::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong value= (ulonglong) args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0; /* purecov: inspected */
  return (longlong) my_count_bits(value);
}


/****************************************************************************
** Functions to handle dynamic loadable functions
** Original source by: Alexis Mikhailov <root@medinf.chuvashia.su>
** Rewritten by monty.
****************************************************************************/

#ifdef HAVE_DLOPEN

void udf_handler::cleanup()
{
  if (!not_original)
  {
    if (initialized)
    {
      if (u_d->func_deinit != NULL)
      {
        Udf_func_deinit deinit= u_d->func_deinit;
        (*deinit)(&initid);
      }
      free_udf(u_d);
      initialized= FALSE;
    }
    if (buffers)				// Because of bug in ecc
      delete [] buffers;
    buffers= 0;
  }
}


bool
udf_handler::fix_fields(THD *thd, Item_result_field *func,
			uint arg_count, Item **arguments)
{
#ifndef EMBEDDED_LIBRARY			// Avoid compiler warning
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function
#endif
  DBUG_ENTER("Item_udf_func::fix_fields");

  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    DBUG_RETURN(TRUE);				// Fatal error flag is set!

  udf_func *tmp_udf=find_udf(u_d->name.str,(uint) u_d->name.length,1);

  if (!tmp_udf)
  {
    my_error(ER_CANT_FIND_UDF, MYF(0), u_d->name.str);
    DBUG_RETURN(TRUE);
  }
  u_d=tmp_udf;
  args=arguments;

  /* Fix all arguments */
  func->maybe_null=0;
  used_tables_cache=0;
  const_item_cache=1;

  if ((f_args.arg_count=arg_count))
  {
    if (!(f_args.arg_type= (Item_result*)
	  sql_alloc(f_args.arg_count*sizeof(Item_result))))

    {
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
    uint i;
    Item **arg,**arg_end;
    for (i=0, arg=arguments, arg_end=arguments+arg_count;
	 arg != arg_end ;
	 arg++,i++)
    {
      if (!(*arg)->fixed &&
          (*arg)->fix_fields(thd, arg))
	DBUG_RETURN(1);
      // we can't assign 'item' before, because fix_fields() can change arg
      Item *item= *arg;
      if (item->check_cols(1))
	DBUG_RETURN(TRUE);
      /*
	TODO: We should think about this. It is not always
	right way just to set an UDF result to return my_charset_bin
	if one argument has binary sorting order.
	The result collation should be calculated according to arguments
	derivations in some cases and should not in other cases.
	Moreover, some arguments can represent a numeric input
	which doesn't effect the result character set and collation.
	There is no a general rule for UDF. Everything depends on
        the particular user defined function.
      */
      if (item->collation.collation->state & MY_CS_BINSORT)
	func->collation.set(&my_charset_bin);
      if (item->maybe_null)
	func->maybe_null=1;
      func->with_sum_func= func->with_sum_func || item->with_sum_func;
      used_tables_cache|=item->used_tables();
      const_item_cache&=item->const_item();
      f_args.arg_type[i]=item->result_type();
    }
    //TODO: why all following memory is not allocated with 1 call of sql_alloc?
    if (!(buffers=new String[arg_count]) ||
	!(f_args.args= (char**) sql_alloc(arg_count * sizeof(char *))) ||
	!(f_args.lengths= (ulong*) sql_alloc(arg_count * sizeof(long))) ||
	!(f_args.maybe_null= (char*) sql_alloc(arg_count * sizeof(char))) ||
	!(num_buffer= (char*) sql_alloc(arg_count *
					ALIGN_SIZE(sizeof(double)))) ||
	!(f_args.attributes= (char**) sql_alloc(arg_count * sizeof(char *))) ||
	!(f_args.attribute_lengths= (ulong*) sql_alloc(arg_count *
						       sizeof(long))))
    {
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
  }
  func->fix_length_and_dec();
  initid.max_length=func->max_length;
  initid.maybe_null=func->maybe_null;
  initid.const_item=const_item_cache;
  initid.decimals=func->decimals;
  initid.ptr=0;

  if (u_d->func_init)
  {
    char init_msg_buff[MYSQL_ERRMSG_SIZE];
    char *to=num_buffer;
    for (uint i=0; i < arg_count; i++)
    {
      /*
       For a constant argument i, args->args[i] points to the argument value. 
       For non-constant, args->args[i] is NULL.
      */
      f_args.args[i]= NULL;         /* Non-const unless updated below. */

      f_args.lengths[i]= arguments[i]->max_length;
      f_args.maybe_null[i]= (char) arguments[i]->maybe_null;
      f_args.attributes[i]= arguments[i]->name;
      f_args.attribute_lengths[i]= arguments[i]->name_length;

      if (arguments[i]->const_item())
      {
        switch (arguments[i]->result_type()) 
        {
        case STRING_RESULT:
        case DECIMAL_RESULT:
        {
          String *res= arguments[i]->val_str(&buffers[i]);
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= (char*) res->c_ptr();
          f_args.lengths[i]= res->length();
          break;
        }
        case INT_RESULT:
          *((longlong*) to)= arguments[i]->val_int();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(longlong));
          break;
        case REAL_RESULT:
          *((double*) to)= arguments[i]->val_real();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(double));
          break;
        case ROW_RESULT:
        default:
          // This case should never be chosen
          DBUG_ASSERT(0);
          break;
        }
      }
    }
    Udf_func_init init= u_d->func_init;
    if ((error=(uchar) init(&initid, &f_args, init_msg_buff)))
    {
      my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
               u_d->name.str, init_msg_buff);
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
    func->max_length=min(initid.max_length,MAX_BLOB_WIDTH);
    func->maybe_null=initid.maybe_null;
    const_item_cache=initid.const_item;
    /* 
      Keep used_tables_cache in sync with const_item_cache.
      See the comment in Item_udf_func::update_used tables.
    */  
    if (!const_item_cache && !used_tables_cache)
      used_tables_cache= RAND_TABLE_BIT;
    func->decimals=min(initid.decimals,NOT_FIXED_DEC);
  }
  initialized=1;
  if (error)
  {
    my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
             u_d->name.str, ER(ER_UNKNOWN_ERROR));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


bool udf_handler::get_arguments()
{
  if (error)
    return 1;					// Got an error earlier
  char *to= num_buffer;
  uint str_count=0;
  for (uint i=0; i < f_args.arg_count; i++)
  {
    f_args.args[i]=0;
    switch (f_args.arg_type[i]) {
    case STRING_RESULT:
    case DECIMAL_RESULT:
      {
	String *res=args[i]->val_str(&buffers[str_count++]);
	if (!(args[i]->null_value))
	{
	  f_args.args[i]=    (char*) res->ptr();
	  f_args.lengths[i]= res->length();
	  break;
	}
      }
    case INT_RESULT:
      *((longlong*) to) = args[i]->val_int();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(longlong));
      }
      break;
    case REAL_RESULT:
      *((double*) to)= args[i]->val_real();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(double));
      }
      break;
    case ROW_RESULT:
    default:
      // This case should never be chosen
      DBUG_ASSERT(0);
      break;
    }
  }
  return 0;
}

/**
  @return
    (String*)NULL in case of NULL values
*/
String *udf_handler::val_str(String *str,String *save_str)
{
  uchar is_null_tmp=0;
  ulong res_length;
  DBUG_ENTER("udf_handler::val_str");

  if (get_arguments())
    DBUG_RETURN(0);
  char * (*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  if ((res_length=str->alloced_length()) < MAX_FIELD_WIDTH)
  {						// This happens VERY seldom
    if (str->alloc(MAX_FIELD_WIDTH))
    {
      error=1;
      DBUG_RETURN(0);
    }
  }
  char *res=func(&initid, &f_args, (char*) str->ptr(), &res_length,
		 &is_null_tmp, &error);
  DBUG_PRINT("info", ("udf func returned, res_length: %lu", res_length));
  if (is_null_tmp || !res || error)		// The !res is for safety
  {
    DBUG_PRINT("info", ("Null or error"));
    DBUG_RETURN(0);
  }
  if (res == str->ptr())
  {
    str->length(res_length);
    DBUG_PRINT("exit", ("str: %s", str->ptr()));
    DBUG_RETURN(str);
  }
  save_str->set(res, res_length, str->charset());
  DBUG_PRINT("exit", ("save_str: %s", save_str->ptr()));
  DBUG_RETURN(save_str);
}


/*
  For the moment, UDF functions are returning DECIMAL values as strings
*/

my_decimal *udf_handler::val_decimal(my_bool *null_value, my_decimal *dec_buf)
{
  char buf[DECIMAL_MAX_STR_LENGTH+1], *end;
  ulong res_length= DECIMAL_MAX_STR_LENGTH;

  if (get_arguments())
  {
    *null_value=1;
    return 0;
  }
  char *(*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  char *res= func(&initid, &f_args, buf, &res_length, &is_null, &error);
  if (is_null || error)
  {
    *null_value= 1;
    return 0;
  }
  end= res+ res_length;
  str2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf, &end);
  return dec_buf;
}


void Item_udf_func::cleanup()
{
  udf.cleanup();
  Item_func::cleanup();
}


void Item_udf_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name());
  str->append('(');
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (i != 0)
      str->append(',');
    args[i]->print_item_w_name(str, query_type);
  }
  str->append(')');
}


double Item_func_udf_float::val_real()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_float::val");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
		     args[0]->result_type(), arg_count));
  DBUG_RETURN(udf.val(&null_value));
}


String *Item_func_udf_float::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  double nr= val_real();
  if (null_value)
    return 0;					/* purecov: inspected */
  str->set_real(nr,decimals,&my_charset_bin);
  return str;
}


longlong Item_func_udf_int::val_int()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_int::val_int");
  DBUG_RETURN(udf.val_int(&null_value));
}


String *Item_func_udf_int::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, &my_charset_bin);
  return str;
}


longlong Item_func_udf_decimal::val_int()
{
  my_decimal dec_buf, *dec= udf.val_decimal(&null_value, &dec_buf);
  longlong result;
  if (null_value)
    return 0;
  my_decimal2int(E_DEC_FATAL_ERROR, dec, unsigned_flag, &result);
  return result;
}


double Item_func_udf_decimal::val_real()
{
  my_decimal dec_buf, *dec= udf.val_decimal(&null_value, &dec_buf);
  double result;
  if (null_value)
    return 0.0;
  my_decimal2double(E_DEC_FATAL_ERROR, dec, &result);
  return result;
}


my_decimal *Item_func_udf_decimal::val_decimal(my_decimal *dec_buf)
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_udf_decimal::val_decimal");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
                     args[0]->result_type(), arg_count));

  DBUG_RETURN(udf.val_decimal(&null_value, dec_buf));
}


String *Item_func_udf_decimal::val_str(String *str)
{
  my_decimal dec_buf, *dec= udf.val_decimal(&null_value, &dec_buf);
  if (null_value)
    return 0;
  if (str->length() < DECIMAL_MAX_STR_LENGTH)
    str->length(DECIMAL_MAX_STR_LENGTH);
  my_decimal_round(E_DEC_FATAL_ERROR, dec, decimals, FALSE, &dec_buf);
  my_decimal2string(E_DEC_FATAL_ERROR, &dec_buf, 0, 0, '0', str);
  return str;
}


void Item_func_udf_decimal::fix_length_and_dec()
{
  fix_num_length_and_dec();
}


/* Default max_length is max argument length */

void Item_func_udf_str::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_udf_str::fix_length_and_dec");
  max_length=0;
  for (uint i = 0; i < arg_count; i++)
    set_if_bigger(max_length,args[i]->max_length);
  DBUG_VOID_RETURN;
}

String *Item_func_udf_str::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *res=udf.val_str(str,&str_value);
  null_value = !res;
  return res;
}


/**
  @note
  This has to come last in the udf_handler methods, or C for AIX
  version 6.0.0.0 fails to compile with debugging enabled. (Yes, really.)
*/

udf_handler::~udf_handler()
{
  /* Everything should be properly cleaned up by this moment. */
  DBUG_ASSERT(not_original || !(initialized || buffers));
}

#else
bool udf_handler::get_arguments() { return 0; }
#endif /* HAVE_DLOPEN */

/*
** User level locks
*/

pthread_mutex_t LOCK_user_locks;
static HASH hash_user_locks;

class User_level_lock
{
  uchar *key;
  size_t key_length;

public:
  int count;
  bool locked;
  pthread_cond_t cond;
  my_thread_id thread_id;
  void set_thread(THD *thd) { thread_id= thd->thread_id; }

  User_level_lock(const uchar *key_arg,uint length, ulong id) 
    :key_length(length),count(1),locked(1), thread_id(id)
  {
    key= (uchar*) my_memdup(key_arg,length,MYF(0));
    pthread_cond_init(&cond,NULL);
    if (key)
    {
      if (my_hash_insert(&hash_user_locks,(uchar*) this))
      {
	my_free(key,MYF(0));
	key=0;
      }
    }
  }
  ~User_level_lock()
  {
    if (key)
    {
      hash_delete(&hash_user_locks,(uchar*) this);
      my_free(key, MYF(0));
    }
    pthread_cond_destroy(&cond);
  }
  inline bool initialized() { return key != 0; }
  friend void item_user_lock_release(User_level_lock *ull);
  friend uchar *ull_get_key(const User_level_lock *ull, size_t *length,
                            my_bool not_used);
};

uchar *ull_get_key(const User_level_lock *ull, size_t *length,
                   my_bool not_used __attribute__((unused)))
{
  *length= ull->key_length;
  return ull->key;
}


static bool item_user_lock_inited= 0;

void item_user_lock_init(void)
{
  pthread_mutex_init(&LOCK_user_locks,MY_MUTEX_INIT_SLOW);
  hash_init(&hash_user_locks,system_charset_info,
	    16,0,0,(hash_get_key) ull_get_key,NULL,0);
  item_user_lock_inited= 1;
}

void item_user_lock_free(void)
{
  if (item_user_lock_inited)
  {
    item_user_lock_inited= 0;
    hash_free(&hash_user_locks);
    pthread_mutex_destroy(&LOCK_user_locks);
  }
}

void item_user_lock_release(User_level_lock *ull)
{
  ull->locked=0;
  ull->thread_id= 0;
  if (--ull->count)
    pthread_cond_signal(&ull->cond);
  else
    delete ull;
}

/**
  Wait until we are at or past the given position in the master binlog
  on the slave.
*/

longlong Item_master_pos_wait::val_int()
{
  DBUG_ASSERT(fixed == 1);
  THD* thd = current_thd;
  String *log_name = args[0]->val_str(&value);
  int event_count= 0;

  null_value=0;
  if (thd->slave_thread || !log_name || !log_name->length())
  {
    null_value = 1;
    return 0;
  }
#ifdef HAVE_REPLICATION
  longlong pos = (ulong)args[1]->val_int();
  longlong timeout = (arg_count==3) ? args[2]->val_int() : 0 ;
  if ((event_count = active_mi->rli.wait_for_pos(thd, log_name, pos, timeout)) == -2)
  {
    null_value = 1;
    event_count=0;
  }
#endif
  return event_count;
}


/**
  Get a user level lock.  If the thread has an old lock this is first released.

  @retval
    1    : Got lock
  @retval
    0    : Timeout
  @retval
    NULL : Error
*/

longlong Item_func_get_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  longlong timeout=args[1]->val_int();
  struct timespec abstime;
  THD *thd=current_thd;
  User_level_lock *ull;
  int error;
  DBUG_ENTER("Item_func_get_lock::val_int");

  /*
    In slave thread no need to get locks, everything is serialized. Anyway
    there is no way to make GET_LOCK() work on slave like it did on master
    (i.e. make it return exactly the same value) because we don't have the
    same other concurrent threads environment. No matter what we return here,
    it's not guaranteed to be same as on master.
  */
  if (thd->slave_thread)
    DBUG_RETURN(1);

  pthread_mutex_lock(&LOCK_user_locks);

  if (!res || !res->length())
  {
    pthread_mutex_unlock(&LOCK_user_locks);
    null_value=1;
    DBUG_RETURN(0);
  }
  DBUG_PRINT("info", ("lock %.*s, thd=%ld", res->length(), res->ptr(),
                      (long) thd->real_id));
  null_value=0;

  if (thd->ull)
  {
    item_user_lock_release(thd->ull);
    thd->ull=0;
  }

  if (!(ull= ((User_level_lock *) hash_search(&hash_user_locks,
                                              (uchar*) res->ptr(),
                                              (size_t) res->length()))))
  {
    ull= new User_level_lock((uchar*) res->ptr(), (size_t) res->length(),
                             thd->thread_id);
    if (!ull || !ull->initialized())
    {
      delete ull;
      pthread_mutex_unlock(&LOCK_user_locks);
      null_value=1;				// Probably out of memory
      DBUG_RETURN(0);
    }
    ull->set_thread(thd);
    thd->ull=ull;
    pthread_mutex_unlock(&LOCK_user_locks);
    DBUG_PRINT("info", ("made new lock"));
    DBUG_RETURN(1);				// Got new lock
  }
  ull->count++;
  DBUG_PRINT("info", ("ull->count=%d", ull->count));

  /*
    Structure is now initialized.  Try to get the lock.
    Set up control struct to allow others to abort locks.
  */
  thd_proc_info(thd, "User lock");
  thd->mysys_var->current_mutex= &LOCK_user_locks;
  thd->mysys_var->current_cond=  &ull->cond;

  set_timespec(abstime,timeout);
  error= 0;
  while (ull->locked && !thd->killed)
  {
    DBUG_PRINT("info", ("waiting on lock"));
    error= pthread_cond_timedwait(&ull->cond,&LOCK_user_locks,&abstime);
    if (error == ETIMEDOUT || error == ETIME)
    {
      DBUG_PRINT("info", ("lock wait timeout"));
      break;
    }
    error= 0;
  }

  if (ull->locked)
  {
    if (!--ull->count)
    {
      DBUG_ASSERT(0);
      delete ull;				// Should never happen
    }
    if (!error)                                 // Killed (thd->killed != 0)
    {
      error=1;
      null_value=1;				// Return NULL
    }
  }
  else                                          // We got the lock
  {
    ull->locked=1;
    ull->set_thread(thd);
    ull->thread_id= thd->thread_id;
    thd->ull=ull;
    error=0;
    DBUG_PRINT("info", ("got the lock"));
  }
  pthread_mutex_unlock(&LOCK_user_locks);

  pthread_mutex_lock(&thd->mysys_var->mutex);
  thd_proc_info(thd, 0);
  thd->mysys_var->current_mutex= 0;
  thd->mysys_var->current_cond=  0;
  pthread_mutex_unlock(&thd->mysys_var->mutex);

  DBUG_RETURN(!error ? 1 : 0);
}


/**
  Release a user level lock.
  @return
    - 1 if lock released
    - 0 if lock wasn't held
    - (SQL) NULL if no such lock
*/

longlong Item_func_release_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  User_level_lock *ull;
  longlong result;
  THD *thd=current_thd;
  DBUG_ENTER("Item_func_release_lock::val_int");
  if (!res || !res->length())
  {
    null_value=1;
    DBUG_RETURN(0);
  }
  DBUG_PRINT("info", ("lock %.*s", res->length(), res->ptr()));
  null_value=0;

  result=0;
  pthread_mutex_lock(&LOCK_user_locks);
  if (!(ull= ((User_level_lock*) hash_search(&hash_user_locks,
                                             (const uchar*) res->ptr(),
                                             (size_t) res->length()))))
  {
    null_value=1;
  }
  else
  {
    DBUG_PRINT("info", ("ull->locked=%d ull->thread=%lu thd=%lu", 
                        (int) ull->locked,
                        (long)ull->thread_id,
                        (long)thd->thread_id));
    if (ull->locked && current_thd->thread_id == ull->thread_id)
    {
      DBUG_PRINT("info", ("release lock"));
      result=1;					// Release is ok
      item_user_lock_release(ull);
      thd->ull=0;
    }
  }
  pthread_mutex_unlock(&LOCK_user_locks);
  DBUG_RETURN(result);
}


longlong Item_func_last_insert_id::val_int()
{
  THD *thd= current_thd;
  DBUG_ASSERT(fixed == 1);
  if (arg_count)
  {
    longlong value= args[0]->val_int();
    null_value= args[0]->null_value;
    /*
      LAST_INSERT_ID(X) must affect the client's mysql_insert_id() as
      documented in the manual. We don't want to touch
      first_successful_insert_id_in_cur_stmt because it would make
      LAST_INSERT_ID(X) take precedence over an generated auto_increment
      value for this row.
    */
    thd->arg_of_last_insert_id_function= TRUE;
    thd->first_successful_insert_id_in_prev_stmt= value;
    return value;
  }
  return
    static_cast<longlong>(thd->read_first_successful_insert_id_in_prev_stmt());
}


bool Item_func_last_insert_id::fix_fields(THD *thd, Item **ref)
{
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return Item_int_func::fix_fields(thd, ref);
}


/* This function is just used to test speed of different functions */

longlong Item_func_benchmark::val_int()
{
  DBUG_ASSERT(fixed == 1);
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff), &my_charset_bin);
  my_decimal tmp_decimal;
  THD *thd=current_thd;
  ulonglong loop_count;

  loop_count= (ulonglong) args[0]->val_int();

  if (args[0]->null_value ||
      (!args[0]->unsigned_flag && (((longlong) loop_count) < 0)))
  {
    if (!args[0]->null_value)
    {
      char buff[22];
      llstr(((longlong) loop_count), buff);
      push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_ERROR,
                          ER_WRONG_VALUE_FOR_TYPE, ER(ER_WRONG_VALUE_FOR_TYPE),
                          "count", buff, "benchmark");
    }

    null_value= 1;
    return 0;
  }

  null_value=0;
  for (ulonglong loop=0 ; loop < loop_count && !thd->killed; loop++)
  {
    switch (args[1]->result_type()) {
    case REAL_RESULT:
      (void) args[1]->val_real();
      break;
    case INT_RESULT:
      (void) args[1]->val_int();
      break;
    case STRING_RESULT:
      (void) args[1]->val_str(&tmp);
      break;
    case DECIMAL_RESULT:
      (void) args[1]->val_decimal(&tmp_decimal);
      break;
    case ROW_RESULT:
    default:
      // This case should never be chosen
      DBUG_ASSERT(0);
      return 0;
    }
  }
  return 0;
}


void Item_func_benchmark::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("benchmark("));
  args[0]->print(str, query_type);
  str->append(',');
  args[1]->print(str, query_type);
  str->append(')');
}


/** This function is just used to create tests with time gaps. */

longlong Item_func_sleep::val_int()
{
  THD *thd= current_thd;
  struct timespec abstime;
  pthread_cond_t cond;
  int error;

  DBUG_ASSERT(fixed == 1);

  double time= args[0]->val_real();
  /*
    On 64-bit OSX pthread_cond_timedwait() waits forever
    if passed abstime time has already been exceeded by 
    the system time.
    When given a very short timeout (< 10 mcs) just return 
    immediately.
    We assume that the lines between this test and the call 
    to pthread_cond_timedwait() will be executed in less than 0.00001 sec.
  */
  if (time < 0.00001)
    return 0;
    
  set_timespec_nsec(abstime, (ulonglong)(time * ULL(1000000000)));

  pthread_cond_init(&cond, NULL);
  pthread_mutex_lock(&LOCK_user_locks);

  thd_proc_info(thd, "User sleep");
  thd->mysys_var->current_mutex= &LOCK_user_locks;
  thd->mysys_var->current_cond=  &cond;

  error= 0;
  while (!thd->killed)
  {
    error= pthread_cond_timedwait(&cond, &LOCK_user_locks, &abstime);
    if (error == ETIMEDOUT || error == ETIME)
      break;
    error= 0;
  }
  thd_proc_info(thd, 0);
  pthread_mutex_unlock(&LOCK_user_locks);
  pthread_mutex_lock(&thd->mysys_var->mutex);
  thd->mysys_var->current_mutex= 0;
  thd->mysys_var->current_cond=  0;
  pthread_mutex_unlock(&thd->mysys_var->mutex);

  pthread_cond_destroy(&cond);

  return test(!error); 		// Return 1 killed
}


#define extra_size sizeof(double)

static user_var_entry *get_variable(HASH *hash, LEX_STRING &name,
				    bool create_if_not_exists)
{
  user_var_entry *entry;

  if (!(entry = (user_var_entry*) hash_search(hash, (uchar*) name.str,
					      name.length)) &&
      create_if_not_exists)
  {
    uint size=ALIGN_SIZE(sizeof(user_var_entry))+name.length+1+extra_size;
    if (!hash_inited(hash))
      return 0;
    if (!(entry = (user_var_entry*) my_malloc(size,MYF(MY_WME))))
      return 0;
    entry->name.str=(char*) entry+ ALIGN_SIZE(sizeof(user_var_entry))+
      extra_size;
    entry->name.length=name.length;
    entry->value=0;
    entry->length=0;
    entry->update_query_id=0;
    entry->collation.set(NULL, DERIVATION_IMPLICIT, 0);
    entry->unsigned_flag= 0;
    /*
      If we are here, we were called from a SET or a query which sets a
      variable. Imagine it is this:
      INSERT INTO t SELECT @a:=10, @a:=@a+1.
      Then when we have a Item_func_get_user_var (because of the @a+1) so we
      think we have to write the value of @a to the binlog. But before that,
      we have a Item_func_set_user_var to create @a (@a:=10), in this we mark
      the variable as "already logged" (line below) so that it won't be logged
      by Item_func_get_user_var (because that's not necessary).
    */
    entry->used_query_id=current_thd->query_id;
    entry->type=STRING_RESULT;
    memcpy(entry->name.str, name.str, name.length+1);
    if (my_hash_insert(hash,(uchar*) entry))
    {
      my_free((char*) entry,MYF(0));
      return 0;
    }
  }
  return entry;
}


void Item_func_set_user_var::cleanup()
{
  Item_func::cleanup();
  entry= NULL;
}


bool Item_func_set_user_var::set_entry(THD *thd, bool create_if_not_exists)
{
  if (entry && thd->thread_id == entry_thread_id)
    goto end; // update entry->update_query_id for PS
  if (!(entry= get_variable(&thd->user_vars, name, create_if_not_exists)))
  {
    entry_thread_id= 0;
    return TRUE;
  }
  entry_thread_id= thd->thread_id;
  /* 
     Remember the last query which updated it, this way a query can later know
     if this variable is a constant item in the query (it is if update_query_id
     is different from query_id).
  */
end:
  entry->update_query_id= thd->query_id;
  return FALSE;
}


/*
  When a user variable is updated (in a SET command or a query like
  SELECT @a:= ).
*/

bool Item_func_set_user_var::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  /* fix_fields will call Item_func_set_user_var::fix_length_and_dec */
  if (Item_func::fix_fields(thd, ref) || set_entry(thd, TRUE))
    return TRUE;
  /*
    As it is wrong and confusing to associate any 
    character set with NULL, @a should be latin2
    after this query sequence:

      SET @a=_latin2'string';
      SET @a=NULL;

    I.e. the second query should not change the charset
    to the current default value, but should keep the 
    original value assigned during the first query.
    In order to do it, we don't copy charset
    from the argument if the argument is NULL
    and the variable has previously been initialized.
  */
  null_item= (args[0]->type() == NULL_ITEM);
  if (!entry->collation.collation || !null_item)
    entry->collation.set(args[0]->collation.collation, DERIVATION_IMPLICIT);
  collation.set(entry->collation.collation, DERIVATION_IMPLICIT);
  cached_result_type= args[0]->result_type();
  return FALSE;
}


void
Item_func_set_user_var::fix_length_and_dec()
{
  maybe_null=args[0]->maybe_null;
  max_length=args[0]->max_length;
  decimals=args[0]->decimals;
  unsigned_flag= args[0]->unsigned_flag;
  collation.set(args[0]->collation.collation, DERIVATION_IMPLICIT);
}


/*
  Mark field in read_map

  NOTES
    This is used by filesort to register used fields in a a temporary
    column read set or to register used fields in a view
*/

bool Item_func_set_user_var::register_field_in_read_map(uchar *arg)
{
  if (result_field)
  {
    TABLE *table= (TABLE *) arg;
    if (result_field->table == table || !table)
      bitmap_set_bit(result_field->table->read_set, result_field->field_index);
  }
  return 0;
}


/**
  Set value to user variable.

  @param entry          pointer to structure representing variable
  @param set_null       should we set NULL value ?
  @param ptr            pointer to buffer with new value
  @param length         length of new value
  @param type           type of new value
  @param cs             charset info for new value
  @param dv             derivation for new value
  @param unsigned_arg   indiates if a value of type INT_RESULT is unsigned

  @retval
    false   success
  @retval
    true    failure
*/

static bool
update_hash(user_var_entry *entry, bool set_null, void *ptr, uint length,
            Item_result type, CHARSET_INFO *cs, Derivation dv,
            bool unsigned_arg)
{
  if (set_null)
  {
    char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
    if (entry->value && entry->value != pos)
      my_free(entry->value,MYF(0));
    entry->value= 0;
    entry->length= 0;
  }
  else
  {
    if (type == STRING_RESULT)
      length++;					// Store strings with end \0
    if (length <= extra_size)
    {
      /* Save value in value struct */
      char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
      if (entry->value != pos)
      {
	if (entry->value)
	  my_free(entry->value,MYF(0));
	entry->value=pos;
      }
    }
    else
    {
      /* Allocate variable */
      if (entry->length != length)
      {
	char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
	if (entry->value == pos)
	  entry->value=0;
        entry->value= (char*) my_realloc(entry->value, length,
                                         MYF(MY_ALLOW_ZERO_PTR | MY_WME));
        if (!entry->value)
	  return 1;
      }
    }
    if (type == STRING_RESULT)
    {
      length--;					// Fix length change above
      entry->value[length]= 0;			// Store end \0
    }
    memmove(entry->value, ptr, length);
    if (type == DECIMAL_RESULT)
      ((my_decimal*)entry->value)->fix_buffer_pointer();
    entry->length= length;
    entry->collation.set(cs, dv);
    entry->unsigned_flag= unsigned_arg;
  }
  entry->type=type;
  return 0;
}


bool
Item_func_set_user_var::update_hash(void *ptr, uint length,
                                    Item_result res_type,
                                    CHARSET_INFO *cs, Derivation dv,
                                    bool unsigned_arg)
{
  /*
    If we set a variable explicitely to NULL then keep the old
    result type of the variable
  */
  if ((null_value= args[0]->null_value) && null_item)
    res_type= entry->type;                      // Don't change type of item
  if (::update_hash(entry, (null_value= args[0]->null_value),
                    ptr, length, res_type, cs, dv, unsigned_arg))
  {
    current_thd->fatal_error();     // Probably end of memory
    null_value= 1;
    return 1;
  }
  return 0;
}


/** Get the value of a variable as a double. */

double user_var_entry::val_real(my_bool *null_value)
{
  if ((*null_value= (value == 0)))
    return 0.0;

  switch (type) {
  case REAL_RESULT:
    return *(double*) value;
  case INT_RESULT:
    return (double) *(longlong*) value;
  case DECIMAL_RESULT:
  {
    double result;
    my_decimal2double(E_DEC_FATAL_ERROR, (my_decimal *)value, &result);
    return result;
  }
  case STRING_RESULT:
    return my_atof(value);                      // This is null terminated
  case ROW_RESULT:
    DBUG_ASSERT(1);				// Impossible
    break;
  }
  return 0.0;					// Impossible
}


/** Get the value of a variable as an integer. */

longlong user_var_entry::val_int(my_bool *null_value) const
{
  if ((*null_value= (value == 0)))
    return LL(0);

  switch (type) {
  case REAL_RESULT:
    return (longlong) *(double*) value;
  case INT_RESULT:
    return *(longlong*) value;
  case DECIMAL_RESULT:
  {
    longlong result;
    my_decimal2int(E_DEC_FATAL_ERROR, (my_decimal *)value, 0, &result);
    return result;
  }
  case STRING_RESULT:
  {
    int error;
    return my_strtoll10(value, (char**) 0, &error);// String is null terminated
  }
  case ROW_RESULT:
    DBUG_ASSERT(1);				// Impossible
    break;
  }
  return LL(0);					// Impossible
}


/** Get the value of a variable as a string. */

String *user_var_entry::val_str(my_bool *null_value, String *str,
				uint decimals)
{
  if ((*null_value= (value == 0)))
    return (String*) 0;

  switch (type) {
  case REAL_RESULT:
    str->set_real(*(double*) value, decimals, &my_charset_bin);
    break;
  case INT_RESULT:
    if (!unsigned_flag)
      str->set(*(longlong*) value, &my_charset_bin);
    else
      str->set(*(ulonglong*) value, &my_charset_bin);
    break;
  case DECIMAL_RESULT:
    my_decimal2string(E_DEC_FATAL_ERROR, (my_decimal *)value, 0, 0, 0, str);
    break;
  case STRING_RESULT:
    if (str->copy(value, length, collation.collation))
      str= 0;					// EOM error
  case ROW_RESULT:
    DBUG_ASSERT(1);				// Impossible
    break;
  }
  return(str);
}

/** Get the value of a variable as a decimal. */

my_decimal *user_var_entry::val_decimal(my_bool *null_value, my_decimal *val)
{
  if ((*null_value= (value == 0)))
    return 0;

  switch (type) {
  case REAL_RESULT:
    double2my_decimal(E_DEC_FATAL_ERROR, *(double*) value, val);
    break;
  case INT_RESULT:
    int2my_decimal(E_DEC_FATAL_ERROR, *(longlong*) value, 0, val);
    break;
  case DECIMAL_RESULT:
    my_decimal2decimal((my_decimal *) value, val);
    break;
  case STRING_RESULT:
    str2my_decimal(E_DEC_FATAL_ERROR, value, length, collation.collation, val);
    break;
  case ROW_RESULT:
    DBUG_ASSERT(1);				// Impossible
    break;
  }
  return(val);
}

/**
  This functions is invoked on SET \@variable or
  \@variable:= expression.

  Evaluate (and check expression), store results.

  @note
    For now it always return OK. All problem with value evaluating
    will be caught by thd->is_error() check in sql_set_variables().

  @retval
    FALSE OK.
*/

bool
Item_func_set_user_var::check(bool use_result_field)
{
  DBUG_ENTER("Item_func_set_user_var::check");
  if (use_result_field && !result_field)
    use_result_field= FALSE;

  switch (cached_result_type) {
  case REAL_RESULT:
  {
    save_result.vreal= use_result_field ? result_field->val_real() :
                        args[0]->val_real();
    break;
  }
  case INT_RESULT:
  {
    save_result.vint= use_result_field ? result_field->val_int() :
                       args[0]->val_int();
    unsigned_flag= use_result_field ? ((Field_num*)result_field)->unsigned_flag:
                    args[0]->unsigned_flag;
    break;
  }
  case STRING_RESULT:
  {
    save_result.vstr= use_result_field ? result_field->val_str(&value) :
                       args[0]->val_str(&value);
    break;
  }
  case DECIMAL_RESULT:
  {
    save_result.vdec= use_result_field ?
                       result_field->val_decimal(&decimal_buff) :
                       args[0]->val_decimal(&decimal_buff);
    break;
  }
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    break;
  }
  DBUG_RETURN(FALSE);
}


/**
  @brief Evaluate and store item's result.
  This function is invoked on "SELECT ... INTO @var ...".
  
  @param    item    An item to get value from.
*/

void Item_func_set_user_var::save_item_result(Item *item)
{
  DBUG_ENTER("Item_func_set_user_var::save_item_result");

  switch (cached_result_type) {
  case REAL_RESULT:
    save_result.vreal= item->val_result();
    break;
  case INT_RESULT:
    save_result.vint= item->val_int_result();
    unsigned_flag= item->unsigned_flag;
    break;
  case STRING_RESULT:
    save_result.vstr= item->str_result(&value);
    break;
  case DECIMAL_RESULT:
    save_result.vdec= item->val_decimal_result(&decimal_buff);
    break;
  case ROW_RESULT:
  default:
    // Should never happen
    DBUG_ASSERT(0);
    break;
  }
  DBUG_VOID_RETURN;
}


/**
  This functions is invoked on
  SET \@variable or \@variable:= expression.

  @note
    We have to store the expression as such in the variable, independent of
    the value method used by the user

  @retval
    0	OK
  @retval
    1	EOM Error

*/

bool
Item_func_set_user_var::update()
{
  bool res= 0;
  DBUG_ENTER("Item_func_set_user_var::update");

  switch (cached_result_type) {
  case REAL_RESULT:
  {
    res= update_hash((void*) &save_result.vreal,sizeof(save_result.vreal),
		     REAL_RESULT, &my_charset_bin, DERIVATION_IMPLICIT, 0);
    break;
  }
  case INT_RESULT:
  {
    res= update_hash((void*) &save_result.vint, sizeof(save_result.vint),
                     INT_RESULT, &my_charset_bin, DERIVATION_IMPLICIT,
                     unsigned_flag);
    break;
  }
  case STRING_RESULT:
  {
    if (!save_result.vstr)					// Null value
      res= update_hash((void*) 0, 0, STRING_RESULT, &my_charset_bin,
		       DERIVATION_IMPLICIT, 0);
    else
      res= update_hash((void*) save_result.vstr->ptr(),
		       save_result.vstr->length(), STRING_RESULT,
		       save_result.vstr->charset(),
		       DERIVATION_IMPLICIT, 0);
    break;
  }
  case DECIMAL_RESULT:
  {
    if (!save_result.vdec)					// Null value
      res= update_hash((void*) 0, 0, DECIMAL_RESULT, &my_charset_bin,
                       DERIVATION_IMPLICIT, 0);
    else
      res= update_hash((void*) save_result.vdec,
                       sizeof(my_decimal), DECIMAL_RESULT,
                       &my_charset_bin, DERIVATION_IMPLICIT, 0);
    break;
  }
  case ROW_RESULT:
  default:
    // This case should never be chosen
    DBUG_ASSERT(0);
    break;
  }
  DBUG_RETURN(res);
}


double Item_func_set_user_var::val_real()
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int()
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_int(&null_value);
}

String *Item_func_set_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal(my_decimal *val)
{
  DBUG_ASSERT(fixed == 1);
  check(0);
  update();					// Store expression
  return entry->val_decimal(&null_value, val);
}


double Item_func_set_user_var::val_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_int(&null_value);
}

bool Item_func_set_user_var::val_bool_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_int(&null_value) != 0;
}

String *Item_func_set_user_var::str_result(String *str)
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal_result(my_decimal *val)
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return entry->val_decimal(&null_value, val);
}


bool Item_func_set_user_var::is_null_result()
{
  DBUG_ASSERT(fixed == 1);
  check(TRUE);
  update();					// Store expression
  return is_null();
}


void Item_func_set_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(@"));
  str->append(name.str, name.length);
  str->append(STRING_WITH_LEN(":="));
  args[0]->print(str, query_type);
  str->append(')');
}


void Item_func_set_user_var::print_as_stmt(String *str,
                                           enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("set @"));
  str->append(name.str, name.length);
  str->append(STRING_WITH_LEN(":="));
  args[0]->print(str, query_type);
  str->append(')');
}

bool Item_func_set_user_var::send(Protocol *protocol, String *str_arg)
{
  if (result_field)
  {
    check(1);
    update();
    return protocol->store(result_field);
  }
  return Item::send(protocol, str_arg);
}

void Item_func_set_user_var::make_field(Send_field *tmp_field)
{
  if (result_field)
  {
    result_field->make_field(tmp_field);
    DBUG_ASSERT(tmp_field->table_name != 0);
    if (Item::name)
      tmp_field->col_name=Item::name;               // Use user supplied name
  }
  else
    Item::make_field(tmp_field);
}


/*
  Save the value of a user variable into a field

  SYNOPSIS
    save_in_field()
      field           target field to save the value to
      no_conversion   flag indicating whether conversions are allowed

  DESCRIPTION
    Save the function value into a field and update the user variable
    accordingly. If a result field is defined and the target field doesn't
    coincide with it then the value from the result field will be used as
    the new value of the user variable.

    The reason to have this method rather than simply using the result
    field in the val_xxx() methods is that the value from the result field
    not always can be used when the result field is defined.
    Let's consider the following cases:
    1) when filling a tmp table the result field is defined but the value of it
    is undefined because it has to be produced yet. Thus we can't use it.
    2) on execution of an INSERT ... SELECT statement the save_in_field()
    function will be called to fill the data in the new record. If the SELECT
    part uses a tmp table then the result field is defined and should be
    used in order to get the correct result.

    The difference between the SET_USER_VAR function and regular functions
    like CONCAT is that the Item_func objects for the regular functions are
    replaced by Item_field objects after the values of these functions have
    been stored in a tmp table. Yet an object of the Item_field class cannot
    be used to update a user variable.
    Due to this we have to handle the result field in a special way here and
    in the Item_func_set_user_var::send() function.

  RETURN VALUES
    FALSE       Ok
    TRUE        Error
*/

int Item_func_set_user_var::save_in_field(Field *field, bool no_conversions,
                                          bool can_use_result_field)
{
  bool use_result_field= (!can_use_result_field ? 0 :
                          (result_field && result_field != field));
  int error;

  /* Update the value of the user variable */
  check(use_result_field);
  update();

  if (result_type() == STRING_RESULT ||
      (result_type() == REAL_RESULT &&
      field->result_type() == STRING_RESULT))
  {
    String *result;
    CHARSET_INFO *cs= collation.collation;
    char buff[MAX_FIELD_WIDTH];		// Alloc buffer for small columns
    str_value.set_quick(buff, sizeof(buff), cs);
    result= entry->val_str(&null_value, &str_value, decimals);

    if (null_value)
    {
      str_value.set_quick(0, 0, cs);
      return set_field_to_null_with_conversions(field, no_conversions);
    }

    /* NOTE: If null_value == FALSE, "result" must be not NULL.  */

    field->set_notnull();
    error=field->store(result->ptr(),result->length(),cs);
    str_value.set_quick(0, 0, cs);
  }
  else if (result_type() == REAL_RESULT)
  {
    double nr= entry->val_real(&null_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store(nr);
  }
  else if (result_type() == DECIMAL_RESULT)
  {
    my_decimal decimal_value;
    my_decimal *val= entry->val_decimal(&null_value, &decimal_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store_decimal(val);
  }
  else
  {
    longlong nr= entry->val_int(&null_value);
    if (null_value)
      return set_field_to_null_with_conversions(field, no_conversions);
    field->set_notnull();
    error=field->store(nr, unsigned_flag);
  }
  return error;
}


String *
Item_func_get_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_get_user_var::val_str");
  if (!var_entry)
    DBUG_RETURN((String*) 0);			// No such variable
  DBUG_RETURN(var_entry->val_str(&null_value, str, decimals));
}


double Item_func_get_user_var::val_real()
{
  DBUG_ASSERT(fixed == 1);
  if (!var_entry)
    return 0.0;					// No such variable
  return (var_entry->val_real(&null_value));
}


my_decimal *Item_func_get_user_var::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed == 1);
  if (!var_entry)
    return 0;
  return var_entry->val_decimal(&null_value, dec);
}


longlong Item_func_get_user_var::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if (!var_entry)
    return LL(0);				// No such variable
  return (var_entry->val_int(&null_value));
}


/**
  Get variable by name and, if necessary, put the record of variable 
  use into the binary log.

  When a user variable is invoked from an update query (INSERT, UPDATE etc),
  stores this variable and its value in thd->user_var_events, so that it can be
  written to the binlog (will be written just before the query is written, see
  log.cc).

  @param      thd        Current thread
  @param      name       Variable name
  @param[out] out_entry  variable structure or NULL. The pointer is set
                         regardless of whether function succeeded or not.

  @retval
    0  OK
  @retval
    1  Failed to put appropriate record into binary log

*/

int get_var_with_binlog(THD *thd, enum_sql_command sql_command,
                        LEX_STRING &name, user_var_entry **out_entry)
{
  BINLOG_USER_VAR_EVENT *user_var_event;
  user_var_entry *var_entry;
  var_entry= get_variable(&thd->user_vars, name, 0);

  /*
    Any reference to user-defined variable which is done from stored
    function or trigger affects their execution and the execution of the
    calling statement. We must log all such variables even if they are 
    not involved in table-updating statements.
  */
  if (!(opt_bin_log && 
       (is_update_query(sql_command) || thd->in_sub_stmt)))
  {
    *out_entry= var_entry;
    return 0;
  }

  if (!var_entry)
  {
    /*
      If the variable does not exist, it's NULL, but we want to create it so
      that it gets into the binlog (if it didn't, the slave could be
      influenced by a variable of the same name previously set by another
      thread).
      We create it like if it had been explicitly set with SET before.
      The 'new' mimics what sql_yacc.yy does when 'SET @a=10;'.
      sql_set_variables() is what is called from 'case SQLCOM_SET_OPTION'
      in dispatch_command()). Instead of building a one-element list to pass to
      sql_set_variables(), we could instead manually call check() and update();
      this would save memory and time; but calling sql_set_variables() makes
      one unique place to maintain (sql_set_variables()). 

      Manipulation with lex is necessary since free_underlaid_joins
      is going to release memory belonging to the main query.
    */

    List<set_var_base> tmp_var_list;
    LEX *sav_lex= thd->lex, lex_tmp;
    thd->lex= &lex_tmp;
    lex_start(thd);
    tmp_var_list.push_back(new set_var_user(new Item_func_set_user_var(name,
                                                                       new Item_null())));
    /* Create the variable */
    if (sql_set_variables(thd, &tmp_var_list))
    {
      thd->lex= sav_lex;
      goto err;
    }
    thd->lex= sav_lex;
    if (!(var_entry= get_variable(&thd->user_vars, name, 0)))
      goto err;
  }
  else if (var_entry->used_query_id == thd->query_id ||
           mysql_bin_log.is_query_in_union(thd, var_entry->used_query_id))
  {
    /* 
       If this variable was already stored in user_var_events by this query
       (because it's used in more than one place in the query), don't store
       it.
    */
    *out_entry= var_entry;
    return 0;
  }

  uint size;
  /*
    First we need to store value of var_entry, when the next situation
    appears:
    > set @a:=1;
    > insert into t1 values (@a), (@a:=@a+1), (@a:=@a+1);
    We have to write to binlog value @a= 1.

    We allocate the user_var_event on user_var_events_alloc pool, not on
    the this-statement-execution pool because in SPs user_var_event objects 
    may need to be valid after current [SP] statement execution pool is
    destroyed.
  */
  size= ALIGN_SIZE(sizeof(BINLOG_USER_VAR_EVENT)) + var_entry->length;
  if (!(user_var_event= (BINLOG_USER_VAR_EVENT *)
        alloc_root(thd->user_var_events_alloc, size)))
    goto err;

  user_var_event->value= (char*) user_var_event +
    ALIGN_SIZE(sizeof(BINLOG_USER_VAR_EVENT));
  user_var_event->user_var_event= var_entry;
  user_var_event->type= var_entry->type;
  user_var_event->charset_number= var_entry->collation.collation->number;
  if (!var_entry->value)
  {
    /* NULL value*/
    user_var_event->length= 0;
    user_var_event->value= 0;
  }
  else
  {
    user_var_event->length= var_entry->length;
    memcpy(user_var_event->value, var_entry->value,
           var_entry->length);
  }
  /* Mark that this variable has been used by this query */
  var_entry->used_query_id= thd->query_id;
  if (insert_dynamic(&thd->user_var_events, (uchar*) &user_var_event))
    goto err;

  *out_entry= var_entry;
  return 0;

err:
  *out_entry= var_entry;
  return 1;
}

void Item_func_get_user_var::fix_length_and_dec()
{
  THD *thd=current_thd;
  int error;
  maybe_null=1;
  decimals=NOT_FIXED_DEC;
  max_length=MAX_BLOB_WIDTH;

  error= get_var_with_binlog(thd, thd->lex->sql_command, name, &var_entry);

  /*
    If the variable didn't exist it has been created as a STRING-type.
    'var_entry' is NULL only if there occured an error during the call to
    get_var_with_binlog.
  */
  if (var_entry)
  {
    m_cached_result_type= var_entry->type;
    unsigned_flag= var_entry->unsigned_flag;
    max_length= var_entry->length;

    collation.set(var_entry->collation);
    switch(m_cached_result_type) {
    case REAL_RESULT:
      max_length= DBL_DIG + 8;
      break;
    case INT_RESULT:
      max_length= MAX_BIGINT_WIDTH;
      decimals=0;
      break;
    case STRING_RESULT:
      max_length= MAX_BLOB_WIDTH - 1;
      break;
    case DECIMAL_RESULT:
      max_length= DECIMAL_MAX_STR_LENGTH;
      decimals= DECIMAL_MAX_SCALE;
      break;
    case ROW_RESULT:                            // Keep compiler happy
    default:
      DBUG_ASSERT(0);
      break;
    }
  }
  else
  {
    collation.set(&my_charset_bin, DERIVATION_IMPLICIT);
    null_value= 1;
    m_cached_result_type= STRING_RESULT;
    max_length= MAX_BLOB_WIDTH;
  }

  if (error)
    thd->fatal_error();

  return;
}


bool Item_func_get_user_var::const_item() const
{
  return (!var_entry || current_thd->query_id != var_entry->update_query_id);
}


enum Item_result Item_func_get_user_var::result_type() const
{
  return m_cached_result_type;
}


void Item_func_get_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(@"));
  str->append(name.str,name.length);
  str->append(')');
}


bool Item_func_get_user_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_user_var *other=(Item_func_get_user_var*) item;
  return (name.length == other->name.length &&
	  !memcmp(name.str, other->name.str, name.length));
}


bool Item_func_get_user_var::set_value(THD *thd,
                                       sp_rcontext * /*ctx*/, Item **it)
{
  Item_func_set_user_var *suv= new Item_func_set_user_var(get_name(), *it);
  /*
    Item_func_set_user_var is not fixed after construction, call
    fix_fields().
  */
  return (!suv || suv->fix_fields(thd, it) || suv->check(0) || suv->update());
}


bool Item_user_var_as_out_param::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  DBUG_ASSERT(thd->lex->exchange);
  if (Item::fix_fields(thd, ref) ||
      !(entry= get_variable(&thd->user_vars, name, 1)))
    return TRUE;
  entry->type= STRING_RESULT;
  /*
    Let us set the same collation which is used for loading
    of fields in LOAD DATA INFILE.
    (Since Item_user_var_as_out_param is used only there).
  */
  entry->collation.set(thd->lex->exchange->cs ? 
                       thd->lex->exchange->cs :
                       thd->variables.collation_database);
  entry->update_query_id= thd->query_id;
  return FALSE;
}


void Item_user_var_as_out_param::set_null_value(CHARSET_INFO* cs)
{
  if (::update_hash(entry, TRUE, 0, 0, STRING_RESULT, cs,
                    DERIVATION_IMPLICIT, 0 /* unsigned_arg */))
    current_thd->fatal_error();			// Probably end of memory
}


void Item_user_var_as_out_param::set_value(const char *str, uint length,
                                           CHARSET_INFO* cs)
{
  if (::update_hash(entry, FALSE, (void*)str, length, STRING_RESULT, cs,
                    DERIVATION_IMPLICIT, 0 /* unsigned_arg */))
    current_thd->fatal_error();			// Probably end of memory
}


double Item_user_var_as_out_param::val_real()
{
  DBUG_ASSERT(0);
  return 0.0;
}


longlong Item_user_var_as_out_param::val_int()
{
  DBUG_ASSERT(0);
  return 0;
}


String* Item_user_var_as_out_param::val_str(String *str)
{
  DBUG_ASSERT(0);
  return 0;
}


my_decimal* Item_user_var_as_out_param::val_decimal(my_decimal *decimal_buffer)
{
  DBUG_ASSERT(0);
  return 0;
}


void Item_user_var_as_out_param::print(String *str, enum_query_type query_type)
{
  str->append('@');
  str->append(name.str,name.length);
}


Item_func_get_system_var::
Item_func_get_system_var(sys_var *var_arg, enum_var_type var_type_arg,
                       LEX_STRING *component_arg, const char *name_arg,
                       size_t name_len_arg)
  :var(var_arg), var_type(var_type_arg), orig_var_type(var_type_arg),
  component(*component_arg), cache_present(0)
{
  /* set_name() will allocate the name */
  set_name(name_arg, (uint) name_len_arg, system_charset_info);
}


bool Item_func_get_system_var::is_written_to_binlog()
{
  return var->is_written_to_binlog(var_type);
}


void Item_func_get_system_var::update_null_value()
{
  THD *thd= current_thd;
  int save_no_errors= thd->no_errors;
  thd->no_errors= TRUE;
  Item::update_null_value();
  thd->no_errors= save_no_errors;
}


void Item_func_get_system_var::fix_length_and_dec()
{
  char *cptr;
  maybe_null= TRUE;
  max_length= 0;

  if (var->check_type(var_type))
  {
    if (var_type != OPT_DEFAULT)
    {
      my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0),
               var->name, var_type == OPT_GLOBAL ? "SESSION" : "GLOBAL");
      return;
    }
    /* As there was no local variable, return the global value */
    var_type= OPT_GLOBAL;
  }

  switch (var->show_type())
  {
    case SHOW_LONG:
    case SHOW_INT:
    case SHOW_HA_ROWS:
      unsigned_flag= TRUE;
      max_length= MY_INT64_NUM_DECIMAL_DIGITS;
      decimals=0;
      break;
    case SHOW_LONGLONG:
      unsigned_flag= TRUE;
      max_length= MY_INT64_NUM_DECIMAL_DIGITS;
      decimals=0;
      break;
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
      pthread_mutex_lock(&LOCK_global_system_variables);
      cptr= var->show_type() == SHOW_CHAR_PTR ? 
        *(char**) var->value_ptr(current_thd, var_type, &component) :
        (char*) var->value_ptr(current_thd, var_type, &component);
      if (cptr)
        max_length= strlen(cptr) * system_charset_info->mbmaxlen;
      pthread_mutex_unlock(&LOCK_global_system_variables);
      collation.set(system_charset_info, DERIVATION_SYSCONST);
      decimals=NOT_FIXED_DEC;
      break;
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
      unsigned_flag= FALSE;
      max_length= 1;
      decimals=0;
      break;
    case SHOW_DOUBLE:
      unsigned_flag= FALSE;
      decimals= 6;
      max_length= DBL_DIG + 6;
      break;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name);
      break;
  }
}


void Item_func_get_system_var::print(String *str, enum_query_type query_type)
{
  str->append(name, name_length);
}


enum Item_result Item_func_get_system_var::result_type() const
{
  switch (var->show_type())
  {
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
      return INT_RESULT;
    case SHOW_CHAR: 
    case SHOW_CHAR_PTR: 
      return STRING_RESULT;
    case SHOW_DOUBLE:
      return REAL_RESULT;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name);
      return STRING_RESULT;                   // keep the compiler happy
  }
}


enum_field_types Item_func_get_system_var::field_type() const
{
  switch (var->show_type())
  {
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
      return MYSQL_TYPE_LONGLONG;
    case SHOW_CHAR: 
    case SHOW_CHAR_PTR: 
      return MYSQL_TYPE_VARCHAR;
    case SHOW_DOUBLE:
      return MYSQL_TYPE_DOUBLE;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name);
      return MYSQL_TYPE_VARCHAR;              // keep the compiler happy
  }
}


/*
  Uses var, var_type, component, cache_present, used_query_id, thd,
  cached_llval, null_value, cached_null_value
*/
#define get_sys_var_safe(type) \
do { \
  type value; \
  pthread_mutex_lock(&LOCK_global_system_variables); \
  value= *(type*) var->value_ptr(thd, var_type, &component); \
  pthread_mutex_unlock(&LOCK_global_system_variables); \
  cache_present |= GET_SYS_VAR_CACHE_LONG; \
  used_query_id= thd->query_id; \
  cached_llval= null_value ? 0 : (longlong) value; \
  cached_null_value= null_value; \
  return cached_llval; \
} while (0)


longlong Item_func_get_system_var::val_int()
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      return cached_llval;
    } 
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      cached_llval= (longlong) cached_dval;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_llval= longlong_from_string_with_check (cached_strval.charset(),
                                                       cached_strval.c_ptr(),
                                                       cached_strval.c_ptr() +
                                                       cached_strval.length());
      else
        cached_llval= 0;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
  }

  switch (var->show_type())
  {
    case SHOW_INT:      get_sys_var_safe (uint);
    case SHOW_LONG:     get_sys_var_safe (ulong);
    case SHOW_LONGLONG: get_sys_var_safe (ulonglong);
    case SHOW_HA_ROWS:  get_sys_var_safe (ha_rows);
    case SHOW_BOOL:     get_sys_var_safe (bool);
    case SHOW_MY_BOOL:  get_sys_var_safe (my_bool);
    case SHOW_DOUBLE:
      {
        double dval= val_real();

        used_query_id= thd->query_id;
        cached_llval= (longlong) dval;
        cache_present|= GET_SYS_VAR_CACHE_LONG;
        return cached_llval;
      }
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
      {
        String *str_val= val_str(NULL);

        if (str_val && str_val->length())
          cached_llval= longlong_from_string_with_check (system_charset_info,
                                                          str_val->c_ptr(), 
                                                          str_val->c_ptr() + 
                                                          str_val->length());
        else
        {
          null_value= TRUE;
          cached_llval= 0;
        }

        cache_present|= GET_SYS_VAR_CACHE_LONG;
        return cached_llval;
      }

    default:            
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name); 
      return 0;                               // keep the compiler happy
  }
}


String* Item_func_get_system_var::val_str(String* str)
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set (cached_llval, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set_real (cached_dval, decimals, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
  }

  str= &cached_strval;
  switch (var->show_type())
  {
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
    {
      pthread_mutex_lock(&LOCK_global_system_variables);
      char *cptr= var->show_type() == SHOW_CHAR_PTR ? 
        *(char**) var->value_ptr(thd, var_type, &component) :
        (char*) var->value_ptr(thd, var_type, &component);
      if (cptr)
      {
        if (str->copy(cptr, strlen(cptr), collation.collation))
        {
          null_value= TRUE;
          str= NULL;
        }
      }
      else
      {
        null_value= TRUE;
        str= NULL;
      }
      pthread_mutex_unlock(&LOCK_global_system_variables);
      break;
    }

    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
      str->set (val_int(), collation.collation);
      break;
    case SHOW_DOUBLE:
      str->set_real (val_real(), decimals, collation.collation);
      break;

    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name);
      str= NULL;
      break;
  }

  cache_present|= GET_SYS_VAR_CACHE_STRING;
  used_query_id= thd->query_id;
  cached_null_value= null_value;
  return str;
}


double Item_func_get_system_var::val_real()
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      cached_dval= (double)cached_llval;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_dval= double_from_string_with_check (cached_strval.charset(),
                                                    cached_strval.c_ptr(),
                                                    cached_strval.c_ptr() +
                                                    cached_strval.length());
      else
        cached_dval= 0;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
  }

  switch (var->show_type())
  {
    case SHOW_DOUBLE:
      pthread_mutex_lock(&LOCK_global_system_variables);
      cached_dval= *(double*) var->value_ptr(thd, var_type, &component);
      pthread_mutex_unlock(&LOCK_global_system_variables);
      used_query_id= thd->query_id;
      cached_null_value= null_value;
      if (null_value)
        cached_dval= 0;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
      {
        char *cptr;

        pthread_mutex_lock(&LOCK_global_system_variables);
        cptr= var->show_type() == SHOW_CHAR ? 
          (char*) var->value_ptr(thd, var_type, &component) :
          *(char**) var->value_ptr(thd, var_type, &component);
        if (cptr)
          cached_dval= double_from_string_with_check (system_charset_info, 
                                                cptr, cptr + strlen (cptr));
        else
        {
          null_value= TRUE;
          cached_dval= 0;
        }
        pthread_mutex_unlock(&LOCK_global_system_variables);
        used_query_id= thd->query_id;
        cached_null_value= null_value;
        cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
        return cached_dval;
      }
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
        cached_dval= (double) val_int();
        cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
        used_query_id= thd->query_id;
        cached_null_value= null_value;
        return cached_dval;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name);
      return 0;
  }
}


bool Item_func_get_system_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_system_var *other=(Item_func_get_system_var*) item;
  return (var == other->var && var_type == other->var_type);
}


void Item_func_get_system_var::cleanup()
{
  Item_func::cleanup();
  cache_present= 0;
  var_type= orig_var_type;
  cached_strval.free();
}


longlong Item_func_inet_aton::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint byte_result = 0;
  ulonglong result = 0;			// We are ready for 64 bit addresses
  const char *p,* end;
  char c = '.'; // we mark c to indicate invalid IP in case length is 0
  char buff[36];
  int dot_count= 0;

  String *s,tmp(buff,sizeof(buff),&my_charset_bin);
  if (!(s = args[0]->val_str(&tmp)))		// If null value
    goto err;
  null_value=0;

  end= (p = s->ptr()) + s->length();
  while (p < end)
  {
    c = *p++;
    int digit = (int) (c - '0');		// Assume ascii
    if (digit >= 0 && digit <= 9)
    {
      if ((byte_result = byte_result * 10 + digit) > 255)
	goto err;				// Wrong address
    }
    else if (c == '.')
    {
      dot_count++;
      result= (result << 8) + (ulonglong) byte_result;
      byte_result = 0;
    }
    else
      goto err;					// Invalid character
  }
  if (c != '.')					// IP number can't end on '.'
  {
    /*
      Handle short-forms addresses according to standard. Examples:
      127		-> 0.0.0.127
      127.1		-> 127.0.0.1
      127.2.1		-> 127.2.0.1
    */
    switch (dot_count) {
    case 1: result<<= 8; /* Fall through */
    case 2: result<<= 8; /* Fall through */
    }
    return (result << 8) + (ulonglong) byte_result;
  }

err:
  null_value=1;
  return 0;
}


void Item_func_match::init_search(bool no_order)
{
  DBUG_ENTER("Item_func_match::init_search");

  /* Check if init_search() has been called before */
  if (ft_handler)
  {
    /*
      We should reset ft_handler as it is cleaned up
      on destruction of FT_SELECT object
      (necessary in case of re-execution of subquery).
      TODO: FT_SELECT should not clean up ft_handler.
    */
    if (join_key)
      table->file->ft_handler= ft_handler;
    DBUG_VOID_RETURN;
  }

  if (key == NO_SUCH_KEY)
  {
    List<Item> fields;
    fields.push_back(new Item_string(" ",1, cmp_collation.collation));
    for (uint i=1; i < arg_count; i++)
      fields.push_back(args[i]);
    concat_ws=new Item_func_concat_ws(fields);
    /*
      Above function used only to get value and do not need fix_fields for it:
      Item_string - basic constant
      fields - fix_fields() was already called for this arguments
      Item_func_concat_ws - do not need fix_fields() to produce value
    */
    concat_ws->quick_fix_field();
  }

  if (master)
  {
    join_key=master->join_key=join_key|master->join_key;
    master->init_search(no_order);
    ft_handler=master->ft_handler;
    join_key=master->join_key;
    DBUG_VOID_RETURN;
  }

  String *ft_tmp= 0;

  // MATCH ... AGAINST (NULL) is meaningless, but possible
  if (!(ft_tmp=key_item()->val_str(&value)))
  {
    ft_tmp= &value;
    value.set("",0,cmp_collation.collation);
  }

  if (ft_tmp->charset() != cmp_collation.collation)
  {
    uint dummy_errors;
    search_value.copy(ft_tmp->ptr(), ft_tmp->length(), ft_tmp->charset(),
                      cmp_collation.collation, &dummy_errors);
    ft_tmp= &search_value;
  }

  if (join_key && !no_order)
    flags|=FT_SORTED;
  ft_handler=table->file->ft_init_ext(flags, key, ft_tmp);

  if (join_key)
    table->file->ft_handler=ft_handler;

  DBUG_VOID_RETURN;
}


bool Item_func_match::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  Item *UNINIT_VAR(item);                        // Safe as arg_count is > 1

  maybe_null=1;
  join_key=0;

  /*
    const_item is assumed in quite a bit of places, so it would be difficult
    to remove;  If it would ever to be removed, this should include
    modifications to find_best and auto_close as complement to auto_init code
    above.
   */
  if (Item_func::fix_fields(thd, ref) ||
      !args[0]->const_during_execution())
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"AGAINST");
    return TRUE;
  }

  const_item_cache=0;
  for (uint i=1 ; i < arg_count ; i++)
  {
    item=args[i];
    if (item->type() == Item::REF_ITEM)
      args[i]= item= *((Item_ref *)item)->ref;
    if (item->type() != Item::FIELD_ITEM)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "AGAINST");
      return TRUE;
    }
  }
  /*
    Check that all columns come from the same table.
    We've already checked that columns in MATCH are fields so
    PARAM_TABLE_BIT can only appear from AGAINST argument.
  */
  if ((used_tables_cache & ~PARAM_TABLE_BIT) != item->used_tables())
    key=NO_SUCH_KEY;

  if (key == NO_SUCH_KEY && !(flags & FT_BOOL))
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"MATCH");
    return TRUE;
  }
  table=((Item_field *)item)->field->table;
  if (!(table->file->ha_table_flags() & HA_CAN_FULLTEXT))
  {
    my_error(ER_TABLE_CANT_HANDLE_FT, MYF(0));
    return 1;
  }
  table->fulltext_searched=1;
  return agg_arg_collations_for_comparison(cmp_collation,
                                           args+1, arg_count-1, 0);
}

bool Item_func_match::fix_index()
{
  Item_field *item;
  uint ft_to_key[MAX_KEY], ft_cnt[MAX_KEY], fts=0, keynr;
  uint max_cnt=0, mkeys=0, i;

  if (key == NO_SUCH_KEY)
    return 0;
  
  if (!table) 
    goto err;

  for (keynr=0 ; keynr < table->s->keys ; keynr++)
  {
    if ((table->key_info[keynr].flags & HA_FULLTEXT) &&
        (flags & FT_BOOL ? table->keys_in_use_for_query.is_set(keynr) :
                           table->s->keys_in_use.is_set(keynr)))

    {
      ft_to_key[fts]=keynr;
      ft_cnt[fts]=0;
      fts++;
    }
  }

  if (!fts)
    goto err;

  for (i=1; i < arg_count; i++)
  {
    item=(Item_field*)args[i];
    for (keynr=0 ; keynr < fts ; keynr++)
    {
      KEY *ft_key=&table->key_info[ft_to_key[keynr]];
      uint key_parts=ft_key->key_parts;

      for (uint part=0 ; part < key_parts ; part++)
      {
	if (item->field->eq(ft_key->key_part[part].field))
	  ft_cnt[keynr]++;
      }
    }
  }

  for (keynr=0 ; keynr < fts ; keynr++)
  {
    if (ft_cnt[keynr] > max_cnt)
    {
      mkeys=0;
      max_cnt=ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
    if (max_cnt && ft_cnt[keynr] == max_cnt)
    {
      mkeys++;
      ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
  }

  for (keynr=0 ; keynr <= mkeys ; keynr++)
  {
    // partial keys doesn't work
    if (max_cnt < arg_count-1 ||
        max_cnt < table->key_info[ft_to_key[keynr]].key_parts)
      continue;

    key=ft_to_key[keynr];

    return 0;
  }

err:
  if (flags & FT_BOOL)
  {
    key=NO_SUCH_KEY;
    return 0;
  }
  my_message(ER_FT_MATCHING_KEY_NOT_FOUND,
             ER(ER_FT_MATCHING_KEY_NOT_FOUND), MYF(0));
  return 1;
}


bool Item_func_match::eq(const Item *item, bool binary_cmp) const
{
  if (item->type() != FUNC_ITEM ||
      ((Item_func*)item)->functype() != FT_FUNC ||
      flags != ((Item_func_match*)item)->flags)
    return 0;

  Item_func_match *ifm=(Item_func_match*) item;

  if (key == ifm->key && table == ifm->table &&
      key_item()->eq(ifm->key_item(), binary_cmp))
    return 1;

  return 0;
}


double Item_func_match::val_real()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_func_match::val");
  if (ft_handler == NULL)
    DBUG_RETURN(-1.0);

  if (key != NO_SUCH_KEY && table->null_row) /* NULL row from an outer join */
    DBUG_RETURN(0.0);

  if (join_key)
  {
    if (table->file->ft_handler)
      DBUG_RETURN(ft_handler->please->get_relevance(ft_handler));
    join_key=0;
  }

  if (key == NO_SUCH_KEY)
  {
    String *a= concat_ws->val_str(&value);
    if ((null_value= (a == 0)) || !a->length())
      DBUG_RETURN(0);
    DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
				      (uchar *)a->ptr(), a->length()));
  }
  DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
                                                 table->record[0], 0));
}

void Item_func_match::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(match "));
  print_args(str, 1, query_type);
  str->append(STRING_WITH_LEN(" against ("));
  args[0]->print(str, query_type);
  if (flags & FT_BOOL)
    str->append(STRING_WITH_LEN(" in boolean mode"));
  else if (flags & FT_EXPAND)
    str->append(STRING_WITH_LEN(" with query expansion"));
  str->append(STRING_WITH_LEN("))"));
}

longlong Item_func_bit_xor::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong arg1= (ulonglong) args[0]->val_int();
  ulonglong arg2= (ulonglong) args[1]->val_int();
  if ((null_value= (args[0]->null_value || args[1]->null_value)))
    return 0;
  return (longlong) (arg1 ^ arg2);
}


/***************************************************************************
  System variables
****************************************************************************/

/**
  Return value of an system variable base[.name] as a constant item.

  @param thd			Thread handler
  @param var_type		global / session
  @param name		        Name of base or system variable
  @param component		Component.

  @note
    If component.str = 0 then the variable name is in 'name'

  @return
    - 0  : error
    - #  : constant item
*/


Item *get_system_var(THD *thd, enum_var_type var_type, LEX_STRING name,
		     LEX_STRING component)
{
  sys_var *var;
  LEX_STRING *base_name, *component_name;

  if (component.str)
  {
    base_name= &component;
    component_name= &name;
  }
  else
  {
    base_name= &name;
    component_name= &component;			// Empty string
  }

  if (!(var= find_sys_var(thd, base_name->str, base_name->length)))
    return 0;
  if (component.str)
  {
    if (!var->is_struct())
    {
      my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), base_name->str);
      return 0;
    }
  }
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);

  set_if_smaller(component_name->length, MAX_SYS_VAR_LENGTH);

  return new Item_func_get_system_var(var, var_type, component_name,
                                      NULL, 0);
}


/**
  Check a user level lock.

  Sets null_value=TRUE on error.

  @retval
    1		Available
  @retval
    0		Already taken, or error
*/

longlong Item_func_is_free_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  User_level_lock *ull;

  null_value=0;
  if (!res || !res->length())
  {
    null_value=1;
    return 0;
  }
  
  pthread_mutex_lock(&LOCK_user_locks);
  ull= (User_level_lock *) hash_search(&hash_user_locks, (uchar*) res->ptr(),
                                       (size_t) res->length());
  pthread_mutex_unlock(&LOCK_user_locks);
  if (!ull || !ull->locked)
    return 1;
  return 0;
}

longlong Item_func_is_used_lock::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *res=args[0]->val_str(&value);
  User_level_lock *ull;

  null_value=1;
  if (!res || !res->length())
    return 0;
  
  pthread_mutex_lock(&LOCK_user_locks);
  ull= (User_level_lock *) hash_search(&hash_user_locks, (uchar*) res->ptr(),
                                       (size_t) res->length());
  pthread_mutex_unlock(&LOCK_user_locks);
  if (!ull || !ull->locked)
    return 0;

  null_value=0;
  return ull->thread_id;
}


longlong Item_func_row_count::val_int()
{
  DBUG_ASSERT(fixed == 1);
  THD *thd= current_thd;

  return thd->row_count_func;
}




Item_func_sp::Item_func_sp(Name_resolution_context *context_arg, sp_name *name)
  :Item_func(), context(context_arg), m_name(name), m_sp(NULL), sp_result_field(NULL)
{
  maybe_null= 1;
  m_name->init_qname(current_thd);
  dummy_table= (TABLE*) sql_calloc(sizeof(TABLE)+ sizeof(TABLE_SHARE));
  dummy_table->s= (TABLE_SHARE*) (dummy_table+1);
}


Item_func_sp::Item_func_sp(Name_resolution_context *context_arg,
                           sp_name *name, List<Item> &list)
  :Item_func(list), context(context_arg), m_name(name), m_sp(NULL),sp_result_field(NULL)
{
  maybe_null= 1;
  m_name->init_qname(current_thd);
  dummy_table= (TABLE*) sql_calloc(sizeof(TABLE)+ sizeof(TABLE_SHARE));
  dummy_table->s= (TABLE_SHARE*) (dummy_table+1);
}


void
Item_func_sp::cleanup()
{
  if (sp_result_field)
  {
    delete sp_result_field;
    sp_result_field= NULL;
  }
  m_sp= NULL;
  dummy_table->alias= NULL;
  Item_func::cleanup();
}

const char *
Item_func_sp::func_name() const
{
  THD *thd= current_thd;
  /* Calculate length to avoid reallocation of string for sure */
  uint len= (((m_name->m_explicit_name ? m_name->m_db.length : 0) +
              m_name->m_name.length)*2 + //characters*quoting
             2 +                         // ` and `
             (m_name->m_explicit_name ?
              3 : 0) +                   // '`', '`' and '.' for the db
             1 +                         // end of string
             ALIGN_SIZE(1));             // to avoid String reallocation
  String qname((char *)alloc_root(thd->mem_root, len), len,
               system_charset_info);

  qname.length(0);
  if (m_name->m_explicit_name)
  {
    append_identifier(thd, &qname, m_name->m_db.str, m_name->m_db.length);
    qname.append('.');
  }
  append_identifier(thd, &qname, m_name->m_name.str, m_name->m_name.length);
  return qname.ptr();
}


int my_missing_function_error(const LEX_STRING &token, const char *func_name)
{
  if (token.length && is_lex_native_function (&token))
    return my_error(ER_FUNC_INEXISTENT_NAME_COLLISION, MYF(0), func_name);
  else
    return my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "FUNCTION", func_name);
}


/**
  @brief Initialize the result field by creating a temporary dummy table
    and assign it to a newly created field object. Meta data used to
    create the field is fetched from the sp_head belonging to the stored
    proceedure found in the stored procedure functon cache.
  
  @note This function should be called from fix_fields to init the result
    field. It is some what related to Item_field.

  @see Item_field

  @param thd A pointer to the session and thread context.

  @return Function return error status.
  @retval TRUE is returned on an error
  @retval FALSE is returned on success.
*/

bool
Item_func_sp::init_result_field(THD *thd)
{
  LEX_STRING empty_name= { C_STRING_WITH_LEN("") };
  TABLE_SHARE *share;
  DBUG_ENTER("Item_func_sp::init_result_field");

  DBUG_ASSERT(m_sp == NULL);
  DBUG_ASSERT(sp_result_field == NULL);

  if (!(m_sp= sp_find_routine(thd, TYPE_ENUM_FUNCTION, m_name,
                               &thd->sp_func_cache, TRUE)))
  {
    my_missing_function_error (m_name->m_name, m_name->m_qname.str);
    context->process_error(thd);
    DBUG_RETURN(TRUE);
  }

  /*
     A Field need to be attached to a Table.
     Below we "create" a dummy table by initializing 
     the needed pointers.
   */
  
  share= dummy_table->s;
  dummy_table->alias = "";
  dummy_table->maybe_null = maybe_null;
  dummy_table->in_use= thd;
  dummy_table->copy_blobs= TRUE;
  share->table_cache_key = empty_name;
  share->table_name = empty_name;

  if (!(sp_result_field= m_sp->create_result_field(max_length, name,
                                                   dummy_table)))
  {
   DBUG_RETURN(TRUE);
  }
  
  if (sp_result_field->pack_length() > sizeof(result_buf))
  {
    void *tmp;
    if (!(tmp= sql_alloc(sp_result_field->pack_length())))
      DBUG_RETURN(TRUE);
    sp_result_field->move_field((uchar*) tmp);
  }
  else
    sp_result_field->move_field(result_buf);
  
  sp_result_field->null_ptr= (uchar *) &null_value;
  sp_result_field->null_bit= 1;
  DBUG_RETURN(FALSE);
}


/**
  @brief Initialize local members with values from the Field interface.

  @note called from Item::fix_fields.
*/

void Item_func_sp::fix_length_and_dec()
{
  DBUG_ENTER("Item_func_sp::fix_length_and_dec");

  DBUG_ASSERT(sp_result_field);
  decimals= sp_result_field->decimals();
  max_length= sp_result_field->field_length;
  collation.set(sp_result_field->charset());
  maybe_null= 1;
  unsigned_flag= test(sp_result_field->flags & UNSIGNED_FLAG);

  DBUG_VOID_RETURN;
}


/**
  @brief Execute function & store value in field.

  @return Function returns error status.
  @retval FALSE on success.
  @retval TRUE if an error occurred.
*/

bool
Item_func_sp::execute()
{
  THD *thd= current_thd;
  
  /* Execute function and store the return value in the field. */

  if (execute_impl(thd))
  {
    null_value= 1;
    context->process_error(thd);
    if (thd->killed)
      thd->send_kill_message();
    return TRUE;
  }

  /* Check that the field (the value) is not NULL. */

  null_value= sp_result_field->is_null();

  return null_value;
}


/**
   @brief Execute function and store the return value in the field.

   @note This function was intended to be the concrete implementation of
    the interface function execute. This was never realized.

   @return The error state.
   @retval FALSE on success
   @retval TRUE if an error occurred.
*/
bool
Item_func_sp::execute_impl(THD *thd)
{
  bool err_status= TRUE;
  Sub_statement_state statement_state;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *save_security_ctx= thd->security_ctx;
#endif
  enum enum_sp_data_access access=
    (m_sp->m_chistics->daccess == SP_DEFAULT_ACCESS) ?
     SP_DEFAULT_ACCESS_MAPPING : m_sp->m_chistics->daccess;

  DBUG_ENTER("Item_func_sp::execute_impl");

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (context->security_ctx)
  {
    /* Set view definer security context */
    thd->security_ctx= context->security_ctx;
  }
#endif
  if (sp_check_access(thd))
    goto error;

  /*
    Throw an error if a non-deterministic function is called while
    statement-based replication (SBR) is active.
  */

  if (!m_sp->m_chistics->detistic && !trust_function_creators &&
      (access == SP_CONTAINS_SQL || access == SP_MODIFIES_SQL_DATA) &&
      (mysql_bin_log.is_open() &&
       thd->variables.binlog_format == BINLOG_FORMAT_STMT))
  {
    my_error(ER_BINLOG_UNSAFE_ROUTINE, MYF(0));
    goto error;
  }

  /*
    Disable the binlogging if this is not a SELECT statement. If this is a
    SELECT, leave binlogging on, so execute_function() code writes the
    function call into binlog.
  */
  thd->reset_sub_statement_state(&statement_state, SUB_STMT_FUNCTION);
  err_status= m_sp->execute_function(thd, args, arg_count, sp_result_field); 
  thd->restore_sub_statement_state(&statement_state);

error:
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  thd->security_ctx= save_security_ctx;
#endif

  DBUG_RETURN(err_status);
}


void
Item_func_sp::make_field(Send_field *tmp_field)
{
  DBUG_ENTER("Item_func_sp::make_field");
  DBUG_ASSERT(sp_result_field);
  sp_result_field->make_field(tmp_field);
  if (name)
    tmp_field->col_name= name;
  DBUG_VOID_RETURN;
}


enum enum_field_types
Item_func_sp::field_type() const
{
  DBUG_ENTER("Item_func_sp::field_type");
  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field->type());
}

Item_result
Item_func_sp::result_type() const
{
  DBUG_ENTER("Item_func_sp::result_type");
  DBUG_PRINT("info", ("m_sp = %p", (void *) m_sp));
  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field->result_type());
}

longlong Item_func_found_rows::val_int()
{
  DBUG_ASSERT(fixed == 1);
  return current_thd->found_rows();
}


Field *
Item_func_sp::tmp_table_field(TABLE *t_arg)
{
  DBUG_ENTER("Item_func_sp::tmp_table_field");

  DBUG_ASSERT(sp_result_field);
  DBUG_RETURN(sp_result_field);
}


/**
  @brief Checks if requested access to function can be granted to user.
    If function isn't found yet, it searches function first.
    If function can't be found or user don't have requested access
    error is raised.

  @param thd thread handler

  @return Indication if the access was granted or not.
  @retval FALSE Access is granted.
  @retval TRUE Requested access can't be granted or function doesn't exists.
    
*/

bool
Item_func_sp::sp_check_access(THD *thd)
{
  DBUG_ENTER("Item_func_sp::sp_check_access");
  DBUG_ASSERT(m_sp);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (check_routine_access(thd, EXECUTE_ACL,
			   m_sp->m_db.str, m_sp->m_name.str, 0, FALSE))
    DBUG_RETURN(TRUE);
#endif

  DBUG_RETURN(FALSE);
}


bool
Item_func_sp::fix_fields(THD *thd, Item **ref)
{
  bool res;
  DBUG_ENTER("Item_func_sp::fix_fields");
  DBUG_ASSERT(fixed == 0);
 
  /*
    We must call init_result_field before Item_func::fix_fields() 
    to make m_sp and result_field members available to fix_length_and_dec(),
    which is called from Item_func::fix_fields().
  */
  res= init_result_field(thd);

  if (res)
    DBUG_RETURN(res);

  res= Item_func::fix_fields(thd, ref);

  if (res)
    DBUG_RETURN(res);

  if (thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW)
  {
    /*
      Here we check privileges of the stored routine only during view
      creation, in order to validate the view.  A runtime check is
      perfomed in Item_func_sp::execute(), and this method is not
      called during context analysis.  Notice, that during view
      creation we do not infer into stored routine bodies and do not
      check privileges of its statements, which would probably be a
      good idea especially if the view has SQL SECURITY DEFINER and
      the used stored procedure has SQL SECURITY DEFINER.
    */
    res= sp_check_access(thd);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /*
      Try to set and restore the security context to see whether it's valid
    */
    Security_context *save_secutiry_ctx;
    res= set_routine_security_ctx(thd, m_sp, false, &save_secutiry_ctx);
    if (!res)
      m_sp->m_security_ctx.restore_security_context(thd, save_secutiry_ctx);
    
#endif /* ! NO_EMBEDDED_ACCESS_CHECKS */
  }

  if (!m_sp->m_chistics->detistic)
  {
    used_tables_cache |= RAND_TABLE_BIT;
    const_item_cache= FALSE;
  }

  DBUG_RETURN(res);
}


void Item_func_sp::update_used_tables()
{
  Item_func::update_used_tables();

  if (!m_sp->m_chistics->detistic)
  {
    used_tables_cache |= RAND_TABLE_BIT;
    const_item_cache= FALSE;
  }
}


/*
  uuid_short handling.

  The short uuid is defined as a longlong that contains the following bytes:

  Bytes  Comment
  1      Server_id & 255
  4      Startup time of server in seconds
  3      Incrementor

  This means that an uuid is guaranteed to be unique
  even in a replication environment if the following holds:

  - The last byte of the server id is unique
  - If you between two shutdown of the server don't get more than
    an average of 2^24 = 16M calls to uuid_short() per second.
*/

ulonglong uuid_value;

void uuid_short_init()
{
  uuid_value= ((((ulonglong) server_id) << 56) + 
               (((ulonglong) server_start_time) << 24));
}


longlong Item_func_uuid_short::val_int()
{
  ulonglong val;
  pthread_mutex_lock(&LOCK_uuid_generator);
  val= uuid_value++;
  pthread_mutex_unlock(&LOCK_uuid_generator);
  return (longlong) val;
}
