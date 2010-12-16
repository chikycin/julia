#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#ifdef BOEHM_GC
#include <gc.h>
#endif
#include "llt.h"
#include "julia.h"
#include "builtin_proto.h"

static jl_value_t *eval(jl_value_t *e, jl_value_t **locals, size_t nl);
static jl_value_t *eval_body(jl_array_t *stmts, jl_value_t **locals, size_t nl);

jl_value_t *jl_interpret_toplevel_expr(jl_value_t *e)
{
    return eval(e, NULL, 0);
}

jl_value_t *jl_interpret_toplevel_expr_with(jl_value_t *e,
                                            jl_value_t **locals, size_t nl)
{
    return eval(e, locals, nl);
}

static jl_value_t *do_call(jl_function_t *f, jl_value_t **args, size_t nargs,
                           jl_value_t **locals, size_t nl)
{
    jl_value_t **argv = alloca((nargs+1) * sizeof(jl_value_t*));
    size_t i;
    argv[0] = (jl_value_t*)f;
    for(i=1; i < nargs+1; i++) argv[i] = NULL;
    JL_GC_PUSHARGS(argv, nargs+1);
    for(i=0; i < nargs; i++)
        argv[i+1] = eval(args[i], locals, nl);
    jl_value_t *result = jl_apply(f, &argv[1], nargs);
    JL_GC_POP();
    return result;
}

static jl_value_t *eval(jl_value_t *e, jl_value_t **locals, size_t nl)
{
    if (jl_is_symbol(e)) {
        jl_value_t **bp = NULL;
        size_t i;
        for(i=0; i < nl; i++) {
            if (locals[i*2] == e) {
                bp = &locals[i*2+1];
                break;
            }
        }
        if (bp == NULL)
            bp = jl_get_bindingp(jl_system_module, (jl_sym_t*)e);
        if (*bp == NULL)
            jl_errorf("%s not defined", ((jl_sym_t*)e)->name);
        return *bp;
    }
    if (!jl_is_expr(e)) {
        if (jl_is_lambda_info(e)) {
            return jl_new_closure_internal((jl_lambda_info_t*)e,
                                           (jl_value_t*)jl_null);
        }
        return e;
    }
    jl_expr_t *ex = (jl_expr_t*)e;
    jl_value_t **args = &jl_cellref(ex->args,0);
    if (ex->head == call_sym) {
        jl_function_t *f = (jl_function_t*)eval(args[0], locals, nl);
        if (!jl_is_func(f))
            jl_error("apply: expected function");
        return do_call(f, &args[1], ex->args->length-1, locals, nl);
    }
    else if (ex->head == assign_sym) {
        jl_value_t *sym = args[0];
        size_t i;
        for(i=0; i < nl; i++) {
            if (locals[i*2] == sym) {
                locals[i*2+1] = eval(args[1], locals, nl);
                return (jl_value_t*)jl_null;
            }
        }
        jl_value_t **bp = jl_get_bindingp(jl_system_module, (jl_sym_t*)sym);
        if (*bp==NULL || !jl_is_func(*bp))
            *bp = eval(args[1], locals, nl);
        return (jl_value_t*)jl_null;
    }
    else if (ex->head == top_sym) {
        jl_value_t **bp = jl_get_bindingp(jl_system_module, (jl_sym_t*)args[0]);
        if (*bp == NULL)
            jl_errorf("%s not defined", ((jl_sym_t*)args[0])->name);
        return *bp;
    }
    else if (ex->head == symbol_sym) {
        return eval(jl_exprarg(ex,0), locals, nl);
    }
    else if (ex->head == quote_sym) {
        return args[0];
    }
    else if (ex->head == null_sym) {
        return (jl_value_t*)jl_null;
    }
    else if (ex->head == body_sym) {
        return eval_body(ex->args, locals, nl);
    }
    else if (ex->head == unbound_sym) {
        jl_value_t **bp = jl_get_bindingp(jl_system_module, (jl_sym_t*)args[0]);
        if (*bp == NULL)
            return jl_true;
        return jl_false;
    }
    else if (ex->head == static_typeof_sym) {
        return (jl_value_t*)jl_any_type;
    }
    else if (ex->head == macro_sym) {
        // macro definition
        jl_sym_t *nm = (jl_sym_t*)eval(args[0], locals, nl);
        assert(jl_is_symbol(nm));
        jl_function_t *f = (jl_function_t*)eval(args[1], locals, nl);
        assert(jl_is_function(f));
        jl_set_expander(jl_system_module, nm, f);
        return (jl_value_t*)jl_null;
    }
    jl_error("not supported");
    return (jl_value_t*)jl_null;
}

static int label_idx(jl_value_t *tgt, jl_array_t *stmts)
{
    size_t j;
    for(j=0; j < stmts->length; j++) {
        jl_value_t *l = jl_cellref(stmts,j);
        if (jl_is_expr(l) && ((jl_expr_t*)l)->head==label_sym &&
            jl_exprarg(l,0)==tgt)
            break;
    }
    assert(j < stmts->length);
    return j;
}

static jl_value_t *eval_body(jl_array_t *stmts, jl_value_t **locals, size_t nl)
{
    size_t i=0;
    while (1) {
        jl_value_t *stmt = jl_cellref(stmts,i);
        if (jl_is_expr(stmt)) {
            jl_sym_t *head = ((jl_expr_t*)stmt)->head;
            if (head == label_sym) {
            }
            else if (head == goto_sym) {
                i = label_idx(jl_exprarg(stmt,0), stmts);
                continue;
            }
            else if (head == goto_ifnot_sym) {
                jl_value_t *cond = eval(jl_exprarg(stmt,0), locals, nl);
                if (cond == jl_false) {
                    i = label_idx(jl_exprarg(stmt,1), stmts);
                    continue;
                }
            }
            else if (head == return_sym) {
                return eval(jl_exprarg(stmt,0), locals, nl);
            }
            else {
                eval(stmt, locals, nl);
            }
        }
        else {
            eval(stmt, locals, nl);
        }
        i++;
    }
    assert(0);
    return NULL;
}

jl_value_t *jl_interpret_toplevel_thunk(jl_lambda_info_t *lam)
{
    jl_expr_t *ast = (jl_expr_t*)lam->ast;
    jl_array_t *stmts = jl_lam_body(ast)->args;
    jl_array_t *l = jl_lam_locals(ast);
    jl_value_t **names = &((jl_value_t**)l->data)[0];
    jl_value_t **locals = (jl_value_t**)alloca(l->length*2*sizeof(void*));
    jl_value_t *r = (jl_value_t*)jl_null;
    size_t i=0;
    for(i=0; i < l->length; i++) {
        locals[i*2]   = names[i];
        locals[i*2+1] = NULL;
    }
    JL_GC_PUSHARGS(locals, l->length*2);
    r = eval_body(stmts, locals, l->length);
    JL_GC_POP();
    return r;
}
