/* {{{

    This file is part of gapc (GAPC - Grammars, Algebras, Products - Compiler;
      a system to compile algebraic dynamic programming programs)

    Copyright (C) 2011-2023  Stefan Janssen
         email: stefan.m.janssen@gmail.com or stefan.janssen@computational.bio.uni-giessen.de

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

}}} */

#include "cyk.hh"

static const char *MUTEX = "mutex";

Statement::Fn_Call *mutex_lock() {
  Statement::Fn_Call *fn = new Statement::Fn_Call("lock_shared");
  fn->add_arg(new std::string(MUTEX));
  fn->is_obj = Bool(true);
  return fn;
}
Statement::Fn_Call *mutex_unlock() {
  Statement::Fn_Call *fn = new Statement::Fn_Call("unlock_shared");
  fn->add_arg(new std::string(MUTEX));
  fn->is_obj = Bool(true);
  return fn;
}

std::tuple<std::list<Statement::Base*>*, Statement::Var_Decl*> get_tile_computation(const AST &ast, std::string name_maxtilen, Statement::Var_Decl *input_seq) {
  Statement::Var_Decl *tile_size = new Statement::Var_Decl(
        new Type::Size(),
        "tile_size",
        new Expr::Const(32));

  std::list<Statement::Base*> *res = new std::list<Statement::Base*>();

  if (!(ast.checkpoint && ast.checkpoint->cyk)) {
    res->push_back(tile_size);
    res->push_back(new Statement::CustomeCode("#ifdef TILE_SIZE"));
    res->push_back(new Statement::Var_Assign(*tile_size, new Expr::Vacc(new std::string("TILE_SIZE"))));
    res->push_back(new Statement::CustomeCode("#endif"));
  }
  res->push_back(new Statement::Fn_Call(Statement::Fn_Call::ASSERT, *tile_size));
  Expr::Fn_Call *end = new Expr::Fn_Call(new std::string("size"));
    end->add_arg(input_seq->name);
    end->is_obj = Bool(true);
  Statement::Var_Decl *max_tiles = new Statement::Var_Decl(
      new Type::Size(),
      "max_tiles",
      new Expr::Div(end, new Expr::Vacc(*tile_size)));
  res->push_back(max_tiles);
  Statement::Var_Decl *max_tiles_n = new Statement::Var_Decl(
      new Type::Int(),
      name_maxtilen,
      new Expr::Times(new Expr::Vacc(*max_tiles), new Expr::Vacc(*tile_size)));
  res->push_back(max_tiles_n);

  return std::make_tuple(res, tile_size);
}

// deep copy of a list of statements
std::list<Statement::Base*> *copy_statements(std::list<Statement::Base*> *other) {
  std::list<Statement::Base*> *co = new std::list<Statement::Base*>();
  for (std::list<Statement::Base*>::iterator i = other->begin(); i != other->end(); ++i) {
    co->push_back((*i)->copy());
  }
  return co;
}

/* data structure to bundle a Statement::For and a Statement::Var_Decl which
 * constitute a for loop to iterate over NT indices and the last index the loop
 * not yet iterated over.
 */
class CYKloop {
 public:
  Statement::For *loop;  // the constructed for loop statement
  Statement::Var_Decl *end_state;  // the variable declaration of index the loop did not reach
  CYKloop(Statement::For *loop, Statement::Var_Decl *end_state) : loop(loop), end_state(end_state) {
    assert(loop->var_decl->name == end_state->name);
  }
};

CYKloop get_for_column(Expr::Vacc *running_boundary, Statement::Var_Decl *input_seq, Expr::Base *start, Expr::Base *end, bool for_openMP, bool endp1, bool with_checkpoint) {
  // create loop variable addressing the DP column (=2nd index)
  // e.g.: for (unsigned int t_0_j = 0; t_0_j < t_0_seq.size(); ++t_0_j) {
  Type::Base *t = new Type::Size();
  if (with_checkpoint && !for_openMP) {
    t = new Type::External("");  // ugly hack to avoid redeclaration of variable
    start = new Expr::Cond(
        new Expr::Vacc(new std::string(*running_boundary->name() + "_loaded++")),
        !endp1 ? new Expr::Const(0) : start,
        running_boundary);
  }

  Statement::Var_Decl *var_col = new Statement::Var_Decl(
      t,
      running_boundary,
      start);

  // create end point for loop variable
  if (!for_openMP) {
    end = new Expr::Fn_Call(new std::string("size"));
    dynamic_cast<Expr::Fn_Call*>(end)->add_arg(input_seq->name);
    dynamic_cast<Expr::Fn_Call*>(end)->is_obj = Bool(true);
  }

  // create condition of For loop
  Expr::Less *cond_col = new Expr::Less(new Expr::Vacc(*var_col), endp1 ? end->plus(new Expr::Const(1)) : end);

  Statement::For *loop = new Statement::For(var_col, cond_col);

  Statement::Var_Decl *var_nonloop = var_col->clone();
  var_nonloop->rhs = end;

  return CYKloop(loop, var_nonloop);
}

CYKloop get_for_row(Expr::Vacc *running_boundary, Expr::Base *start, Expr::Base *end, bool for_openMP, bool with_checkpoint) {
  // create loop variable addressing the DP row (=1st index)
  // e.g.: for (unsigned int t_0_i = t_0_j + 1; t_0_i > 1; t_0_i--) {
  Type::Base *t = new Type::Size();
  if (for_openMP) {
    t = new Type::Int();
  }
  if (with_checkpoint && !for_openMP) {
    t = new Type::External("");  // ugly hack to avoid redeclaration of variable
    start = new Expr::Cond(
        new Expr::Vacc(new std::string(*running_boundary->name() + "_loaded++")),
        start,
        running_boundary);
  }
  Statement::Var_Decl *var_row = new Statement::Var_Decl(
      t,
      running_boundary,
      start);

  // create condition of For loop
  Expr::Greater *cond_row = new Expr::Greater(new Expr::Vacc(*var_row), end);

  Statement::For *loop = new Statement::For(var_row, cond_row);
  Statement::Var_Assign *x = new Statement::Var_Assign(*var_row, new Expr::Const(new Const::Int(-1)));
  x->set_op(::Expr::Type::PLUS);
  loop->inc = x;

  Statement::Var_Decl *var_nonloop = var_row->clone();
  var_nonloop->rhs = new Expr::Const(1);

  return CYKloop(loop, var_nonloop);
}

Statement::For *get_for_openMP(Expr::Vacc *loopvar, Expr::Base *start, Expr::Base *end, Statement::Var_Decl *inc) {
  Statement::Var_Decl *var = new Statement::Var_Decl(
      new Type::Int(),
      loopvar,
      start);

  // create condition of For loop
  Expr::Less *cond_row = new Expr::Less(new Expr::Vacc(*var), end);

  Statement::For *loop = new Statement::For(var, cond_row);
  Statement::Var_Assign *x = new Statement::Var_Assign(*var, *inc);
  x->set_op(::Expr::Type::PLUS);
  loop->inc = x;

  return loop;
}

/*
 * Construct the loop traversal structure for CYK parsing of one track as below.
 * Note that this general structure gets recursively nested for multiple tracks!
 * The result will "only" contain loops, but they are empty for now.
 * Call function add_nt_call() to populate loops with concrete NT calls,
 * which depends on the NT actual table dimensions.
 * for (t_x_j ... {
 *   for (t_x_i ... {
 *     calls to triangular cells = A
 *     nt_tabulated_foo(t_x_i+1, t_x_j, ...)
 *   }
 *   calls to top row = B
 *   nt_tabulated_foo(0, t_x_j, ...)
 * }
 * for (t_x_i ... {
 *   calls to last column = C
 *   nt_tabulated_foo(t_x_i, x_n, ...)
 * }
 * calls to top right cell = D
 * nt_tabulated_foo(0, x_n, ...)
 *
 *   |  0  1  2  3   4  5          |  0  1  2  3  4  5
 * --|-------------------        --|------------------
 * 0 |  0  2  5  9  14 20        0 |  B  B  B  B  B  D
 * 1 |     1  4  8  13 19        1 |     A  A  A  A  C
 * 2 |        3  7  12 18        2 |        A  A  A  C
 * 3 |           6  11 17        3 |           A  A  C
 * 4 |              10 16        4 |              A  C
 * 5 |                 15        5 |                 C
 */
std::list<Statement::Base*> *cyk_traversal_singlethread_singletrack(size_t track, const AST &ast, Statement::Var_Decl *seq, std::list<Statement::Base*> *nested_stmts, bool with_checkpoint) {
  std::list<Statement::Base*> *stmts = new std::list<Statement::Base*>();

  Expr::Base *row_start = (*ast.grammar()->topological_ord().begin())->right_indices.at(track)->vacc()->plus(new Expr::Const(1));

  // A: major cells in triangle below first row, left of last columns
  // A: t_x_i = row index
  std::list<Statement::Base*> *co = copy_statements(nested_stmts);
  CYKloop row = get_for_row(ast.grammar()->left_running_indices[track], row_start, new Expr::Const(1), false, with_checkpoint);
  row.loop->statements.insert(row.loop->statements.end(), co->begin(), co->end());

  // A: t_x_j = column index
  //std::tuple<Statement::For*, Statement::Var_Decl*> col = get_for_column(ast.grammar()->right_running_indices[track], seq, new Expr::Const(0), nullptr, false, false, with_checkpoint);
  CYKloop col = get_for_column(ast.grammar()->right_running_indices[track], seq, new Expr::Const(0), nullptr, false, false, with_checkpoint);
  col.loop->statements.push_back(row.loop);
  col.loop->statements.push_back(row.end_state);

  // B: first row
  co = copy_statements(nested_stmts);
  col.loop->statements.insert(col.loop->statements.end(), co->begin(), co->end());
  stmts->push_back(col.loop);
  stmts->push_back(col.end_state);

  // C: last column
  CYKloop rowC = get_for_row(ast.grammar()->left_running_indices[track], row_start, new Expr::Const(1), false, with_checkpoint);
  co = copy_statements(nested_stmts);
  rowC.loop->statements.insert(rowC.loop->statements.end(), co->begin(), co->end());
  stmts->push_back(rowC.loop);
  stmts->push_back(rowC.end_state);

  // D: top right cell
  co = copy_statements(nested_stmts);
  stmts->insert(stmts->end(), co->begin(), co->end());

  return stmts;
}

// recursively reverse iterate through tracks and create nested for loop structures
std::list<Statement::Base*> *cyk_traversal_singlethread(const AST &ast) {
  std::list<Statement::Base*> *stmts = new std::list<Statement::Base*>();

  std::vector<Statement::Var_Decl*>::const_reverse_iterator it_stmt_seq = ast.seq_decls.rbegin();
  for (int track = ast.grammar()->axiom->tracks() - 1; track >= 0; track--, ++it_stmt_seq) {
    stmts = cyk_traversal_singlethread_singletrack(track, ast, *it_stmt_seq, stmts, ast.checkpoint && ast.checkpoint->cyk);
  }

  return stmts;
}



/*
 *  A: tile_size = 4, input = aaaaccccgggg
 *    |  0   1   2   3   4   5   6   7   8   9  10  11  12
 * ---|----------------------------------------------------
 *  0 |  0   2   5   9
 *  1 |      1   4   8
 *  2 |          3   7
 *  3 |              6
 *  4 |                 10  12  15  19
 *  5 |                     11  14  18
 *  6 |                         13  17
 *  7 |                             16
 *  8 |                                 20  22  25  29
 *  9 |                                     21  24  28
 * 10 |                                         23  27
 * 11 |                                             26
 * 12 |
 *
 *  B: tile_size = 4, input = aaaaccccgggg
 *    |  0   1   2   3   4   5   6   7   8   9  10  11  12
 * ---|----------------------------------------------------
 *  0 |                 33  37  41  45  65  69  73  77
 *  1 |                 32  36  40  44  64  68  72  76
 *  2 |                 31  35  39  43  63  67  71  75
 *  3 |                 30  34  38  42  62  66  70  74
 *  4 |                                 49  53  57  61
 *  5 |                                 48  52  56  60
 *  6 |                                 47  51  55  59
 *  7 |                                 46  50  54  58
 *  8 |
 *  9 |
 * 10 |
 * 11 |
 * 12 |
 *

 */
std::list<Statement::Base*> *cyk_traversal_multithread_parallel(const AST &ast, Statement::Var_Decl *seq, Statement::Var_Decl *tile_size, std::string *name_maxtilen, bool with_checkpoint) {
  size_t track = 0; // as openMP currently only works for single track grammars
  std::list<Statement::Base*> *stmts = new std::list<Statement::Base*>();

  Expr::Base *row_start = (*ast.grammar()->topological_ord().begin())->right_indices.at(track)->vacc()->plus(new Expr::Const(1));

  Expr::Vacc *z = new Expr::Vacc(new std::string("z"));
  Expr::Vacc *y = new Expr::Vacc(new std::string("y"));
  Statement::Var_Decl *x = new Statement::Var_Decl(new Type::Size(), "x", (y->minus(z))->plus(new Expr::Vacc(*tile_size)));

  // A
  CYKloop row = get_for_row(ast.grammar()->left_running_indices[track], row_start, z, true, with_checkpoint);
//  add_nts(ast.grammar()->axiom->tracks(), std::get<0>(row)->statements, ast.grammar()->topological_ord(), BOTH);

  CYKloop col = get_for_column(ast.grammar()->right_running_indices[track], seq, z, z->plus(new Expr::Vacc(*tile_size)), true, false, with_checkpoint);
  col.loop->statements.push_back(row.loop);

  Expr::Base *start_z = new Expr::Const(0);
  if (with_checkpoint) {
    start_z = new Expr::Vacc(new std::string("outer_loop_1_idx_start"));
  }
  Statement::For *loop_z = get_for_openMP(z, start_z, new Expr::Vacc(name_maxtilen), tile_size);
  if (with_checkpoint) {
    loop_z->statements.push_back(mutex_lock());
  }
  loop_z->statements.push_back(col.loop);
  if (with_checkpoint) {
    loop_z->statements.push_back(new Statement::CustomeCode("#pragma omp ordered"));
    Statement::Block *blk_omp = new Statement::Block();
    blk_omp->statements.push_back(new Statement::CustomeCode("// force omp to wait for all threads to finish their current batch (of size tile_size)"));
    blk_omp->statements.push_back(new Statement::CustomeCode("outer_loop_1_idx += tile_size;"));
    blk_omp->statements.push_back(mutex_unlock());
    loop_z->statements.push_back(blk_omp);
  }

  stmts->push_back(loop_z);

  // B
  CYKloop rowB = get_for_row(ast.grammar()->left_running_indices[track], new Expr::Vacc(*x), (new Expr::Vacc(*x))->minus(new Expr::Vacc(*tile_size)), true, with_checkpoint);
//  add_nts(ast.grammar()->axiom->tracks(), std::get<0>(rowB)->statements, ast.grammar()->topological_ord(), BOTH);

  CYKloop colB = get_for_column(ast.grammar()->right_running_indices[track], seq, y, y->plus(new Expr::Vacc(*tile_size)), true, false, with_checkpoint);
  colB.loop->statements.push_back(rowB.loop);

  Expr::Vacc *start_y = z;
  if (with_checkpoint) {
    start_y = new Expr::Vacc(new std::string("inner_loop_2_idx_loaded ? z : inner_loop_2_idx_start"));
  }
  Statement::For *loop_y = get_for_openMP(y, start_y, new Expr::Vacc(name_maxtilen), tile_size);
  // produce: unsigned int x = y - z + tile_size;
  if (with_checkpoint) {
    loop_y->statements.push_back(new Statement::CustomeCode("++inner_loop_2_idx_loaded;"));
    loop_y->statements.push_back(mutex_lock());
  }
  loop_y->statements.push_back(x);
  loop_y->statements.push_back(colB.loop);
  if (with_checkpoint) {
    loop_y->statements.push_back(new Statement::CustomeCode("#pragma omp ordered"));
    Statement::Block *blk_omp2 = new Statement::Block();
    blk_omp2->statements.push_back(new Statement::CustomeCode("inner_loop_2_idx += tile_size;"));
    blk_omp2->statements.push_back(new Statement::CustomeCode("outer_loop_2_idx = z;"));
    blk_omp2->statements.push_back(mutex_unlock());
    loop_y->statements.push_back(blk_omp2);
  }


  Expr::Vacc *start_z2 = new Expr::Vacc(*tile_size);
  if (with_checkpoint) {
    start_z2 = new Expr::Vacc(new std::string("outer_loop_2_idx_start"));
  }
  loop_z = get_for_openMP(z, start_z2, new Expr::Vacc(name_maxtilen), tile_size);
  if (with_checkpoint) {
    loop_z->statements.push_back(new Statement::CustomeCode("#pragma omp for ordered schedule(dynamic)"));
  } else {
    loop_z->statements.push_back(new Statement::CustomeCode("#pragma omp for"));
  }
  loop_z->statements.push_back(loop_y);
  if (with_checkpoint) {
    loop_z->statements.push_back(new Statement::CustomeCode("inner_loop_2_idx = z + tile_size;"));
  }

  stmts->push_back(loop_z);

  return stmts;
}


/*
 *  C: tile_size = 4, input = aaaaccccgggg
 *    |  0   1   2   3   4   5   6   7   8   9  10  11  12
 * ---|----------------------------------------------------
 *  0 | 78  80  83  87  92  98 105 113 122 132 143 155 168
 *  1 |     79  82  86  91  97 104 112 121 131 142 154 167
 *  2 |         81  85  90  96 103 111 120 130 141 153 166
 *  3 |             84  89  95 102 110 119 129 140 152 165
 *  4 |                 88  94 101 109 118 128 139 151 164
 *  5 |                     93 100 108 117 127 138 150 163
 *  6 |                         99 107 116 126 137 149 162
 *  7 |                            106 115 125 136 148 161
 *  8 |                                114 124 135 147 160
 *  9 |                                    123 134 146 159
 * 10 |                                        133 145 158
 * 11 |                                            144 157
 * 12 |                                                156
 *
 */
std::list<Statement::Base*> *cyk_traversal_multithread_serial(const AST &ast, Statement::Var_Decl *seq, Statement::Var_Decl *tile_size, std::string *name_maxtilen, bool with_checkpoint) {
  size_t track = 0; // as openMP currently only works for single track grammars
  std::list<Statement::Base*> *stmts = new std::list<Statement::Base*>();

  Expr::Base *row_start = (*ast.grammar()->topological_ord().begin())->right_indices.at(track)->vacc()->plus(new Expr::Const(1));

  CYKloop row = get_for_row(ast.grammar()->left_running_indices[track], row_start, new Expr::Const(0), false, with_checkpoint);
  if (with_checkpoint) {
    row.loop->statements.push_back(mutex_lock());
  }

  CYKloop col = get_for_column(ast.grammar()->right_running_indices[track], seq, new Expr::Vacc(name_maxtilen), nullptr, false, true, with_checkpoint);
  col.loop->statements.push_back(row.loop);

  stmts->push_back(col.loop);
  stmts->push_back(col.end_state);

  CYKloop first_row = get_for_row(ast.grammar()->left_running_indices[track], row_start, new Expr::Const(0), false, with_checkpoint);
  if (with_checkpoint) {
    first_row.loop->statements.push_back(mutex_lock());
  }
  stmts->push_back(first_row.loop);
  stmts->push_back(first_row.end_state);

  CYKloop first_col = get_for_column(ast.grammar()->right_running_indices[track], seq, new Expr::Vacc(name_maxtilen), nullptr, false, true, with_checkpoint);
  if (with_checkpoint) {
    first_col.loop->statements.push_back(mutex_lock());
  }
  stmts->push_back(first_col.loop);

  return stmts;
}

size_t count_nt_calls_and_loops(Statement::For *loop) {
  size_t res = 0;
  for (std::list<Statement::Base*>::const_iterator i = loop->statements.begin(); i != loop->statements.end(); ++i) {
    if ((*i)->is(Statement::FN_CALL) && (dynamic_cast<Statement::Fn_Call*>(*i)->name().find("nt_tabulate_", 0) == 0)) {
      res++;
    }
    if ((*i)->is(Statement::FOR)) {
      res++;
    }
  }
  return res;
}

std::list<Statement::Base*> *add_nt_calls(std::list<Statement::Base*> &stmts, std::list<std::string*> *loop_vars, std::list<Symbol::NT*> orderedNTs, bool with_checkpoint, bool for_openMP, bool openMP_serial) {
  bool contains_nested_for = false;
  for (std::list<Statement::Base*>::iterator s = stmts.begin(); s != stmts.end(); ++s) {
    // recurse into next for loop
    if ((*s)->is(Statement::FOR)) {
      contains_nested_for = true;
      Statement::For *fl = dynamic_cast<Statement::For*>(*s);
      std::list<std::string*> *next_loop_vars = new std::list<std::string*>();
      next_loop_vars->insert(next_loop_vars->end(), loop_vars->begin(), loop_vars->end());
      if (!for_openMP || (fl->var_decl->name->find("t_", 0) == 0)) {
        // openMP code adds in loops that do not traverse NT indices. Only add
        // loop variable, if it regard to NT indices, which all start with t_
        // e.g. t_0_i or t_1_j
        next_loop_vars->push_back(fl->var_decl->name);
      }
      std::list<Statement::Base*> *new_stmts = add_nt_calls(fl->statements, next_loop_vars, orderedNTs, with_checkpoint, for_openMP, openMP_serial);
      if (new_stmts->size() > (with_checkpoint ? 1 : 0)) {
        fl->statements.insert(fl->statements.end(), new_stmts->begin(), new_stmts->end());
      } else {
        // remove for loops without any NT calls
        if (!contains_nested_for) {
          s = stmts.erase(s);
        }
      }
    }
  }

  // remove completely empty loops
  for (std::list<Statement::Base*>::iterator s = stmts.begin(); s != stmts.end(); ++s) {
    if ((*s)->is(Statement::FOR)) {
      if (count_nt_calls_and_loops(dynamic_cast<Statement::For*>(*s)) == 0) {
        s = stmts.erase(s);
      }
    }
  }

  if (for_openMP && contains_nested_for) {
    return new std::list<Statement::Base*>();
  }

  // add NTs
  std::list<Statement::Base*> *nt_stmts = new std::list<Statement::Base*>();
  if (with_checkpoint && !for_openMP) {
    nt_stmts->push_back(new Statement::CustomeCode("std::lock_guard<fair_mutex> lock(mutex);"));
  }
  for (std::list<Symbol::NT*>::const_iterator i = orderedNTs.begin(); i != orderedNTs.end(); ++i) {
    if (!(*i)->is_tabulated()) {
      continue;
    }
    std::list<Expr::Base*> *args = new std::list<Expr::Base*>();
    size_t used_indices = 0;
    size_t nt_has_indices = 0;
    for (size_t t = 0; t < (*i)->tracks(); ++t) {
      if (!(*i)->tables()[t].delete_left_index()) {
        Expr::Vacc *idx = (*i)->left_indices.at(t)->vacc();
        if (std::find(loop_vars->begin(), loop_vars->end(), idx->name()) != loop_vars->end()) {
          used_indices++;
        }
        nt_has_indices++;
        args->push_back(idx->minus(new Expr::Const(1)));
      }
      if (!(*i)->tables()[t].delete_right_index()) {
        Expr::Vacc *idx = (*i)->right_indices.at(t)->vacc();
        if (std::find(loop_vars->begin(), loop_vars->end(), idx->name()) != loop_vars->end()) {
          used_indices++;
        }
        nt_has_indices++;
        args->push_back(idx);
      }
    }
    if (used_indices == loop_vars->size()) {
      if (!openMP_serial || (nt_has_indices == loop_vars->size())) {
        Statement::Fn_Call *nt_call = new Statement::Fn_Call((*(*i)->code_list().rbegin())->name, args, Loc());
        nt_stmts->push_back(nt_call);
      }
    }
  }
  if (with_checkpoint && for_openMP && openMP_serial) {
    nt_stmts->push_back(mutex_unlock());
  }

  return nt_stmts;
}

Fn_Def *print_CYK(const AST &ast) {
  Fn_Def *fn_cyk = new Fn_Def(new Type::RealVoid(), new std::string("cyk"));

  if (ast.checkpoint && ast.checkpoint->cyk) {
  /*
    define a boolean marker (as an int) for every loop idx
    to allow for the loading of the checkpointed loop indices;
    if the user wants to load a checkpoint (load_checkpoint == true)
    and the loaded idx value doesn't equal the default value 0
    (meaning that the checkpointed program made enough progress
     to get to the loop where the current idx lives),
    the markers will be set to "false" (== 0), which indicates
    that the respective loop idx hasn't been loaded yet and
    should be loaded when it is first requested;
    if the user does not want to load a checkpoint
    (load_checkpoint == false) or the load idx values are still 0,
    the respective markers will be set to "true" (== 1);
    this means that all idx values are already assumed to be
    loaded and won't be loaded when they are first requested;
    this ensures that the idx values start at whatever value
    they would normally start with
   */
    for (size_t track = 0; track < ast.grammar()->axiom->tracks(); ++track) {
      fn_cyk->stmts.push_back(new Statement::Var_Decl(
          new Type::Int(), *(ast.grammar()->left_running_indices.at(track)->name()) + std::string("_loaded"), new Expr::Or(
              new Expr::Not(new Expr::Vacc(new std::string("load_checkpoint"))),
              new Expr::Not(ast.grammar()->left_running_indices.at(track)))));

      fn_cyk->stmts.push_back(new Statement::Var_Decl(
          new Type::Int(), *(ast.grammar()->right_running_indices.at(track)->name()) + std::string("_loaded"), new Expr::Or(
              new Expr::Not(new Expr::Vacc(new std::string("load_checkpoint"))),
              new Expr::Not(ast.grammar()->right_running_indices.at(track)))));
    }
  }

  // ==== single thread version
  fn_cyk->stmts.push_back(new Statement::CustomeCode("#ifndef _OPENMP"));
  // recursively reverse iterate through tracks and create nested for loop structures
  std::list<Statement::Base*> *stmts = cyk_traversal_singlethread(ast);
  // add NT calls to traversal structure
  std::list<Statement::Base*> *new_stmts = add_nt_calls(*stmts, new std::list<std::string*>(), ast.grammar()->topological_ord(), ast.checkpoint && ast.checkpoint->cyk, false, false);
  stmts->insert(stmts->end(), new_stmts->begin(), new_stmts->end());
  // finally add traversal structure with NT calls to function body
  fn_cyk->stmts.insert(fn_cyk->stmts.end(), stmts->begin(), stmts->end());

  // ==== multi thread version (only single-track possible for now)
  fn_cyk->stmts.push_back(new Statement::CustomeCode("#else"));
  // FIXME generalize for multi-track ...
  if (ast.grammar()->axiom->tracks() == 1) {
    // FIXME abstract from unsigned int, int -> perhaps wait for OpenMP 3
    // since OpenMP < 3 doesn't allow unsigned int in workshared fors

    // header
    if (ast.checkpoint && ast.checkpoint->cyk) {
      fn_cyk->stmts.push_back(new Statement::CustomeCode("unsigned int tile_size = 32;"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("#ifdef TILE_SIZE"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("tile_size = TILE_SIZE;"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("#endif"));

      fn_cyk->stmts.push_back(new Statement::CustomeCode("int outer_loop_1_idx_loaded = !load_checkpoint || !outer_loop_1_idx;"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("int outer_loop_2_idx_loaded = !load_checkpoint || !outer_loop_2_idx;"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("int inner_loop_2_idx_loaded = !load_checkpoint || !inner_loop_2_idx;"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("int outer_loop_1_idx_start = (outer_loop_1_idx_loaded++) ? 0 : outer_loop_1_idx;"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("int outer_loop_2_idx_start = (outer_loop_2_idx_loaded++) ? tile_size : outer_loop_2_idx;"));
      fn_cyk->stmts.push_back(new Statement::CustomeCode("int inner_loop_2_idx_start = inner_loop_2_idx;"));
    }
    fn_cyk->stmts.push_back(new Statement::CustomeCode("#pragma omp parallel"));
    Statement::Block *blk_parallel = new Statement::Block();
    std::vector<Statement::Var_Decl*>::const_reverse_iterator it_stmt_seq = ast.seq_decls.rbegin();
    std::string *name_maxtilen = new std::string("max_tiles_n");
    std::tuple<std::list<Statement::Base*>*, Statement::Var_Decl*> stmts_tilesize = get_tile_computation(ast, *name_maxtilen, *it_stmt_seq);
    blk_parallel->statements.insert(blk_parallel->statements.end(), std::get<0>(stmts_tilesize)->begin(), std::get<0>(stmts_tilesize)->end());
    if (ast.checkpoint && ast.checkpoint->cyk) {
      blk_parallel->statements.push_back(new Statement::CustomeCode("#pragma omp for ordered schedule(dynamic)"));
    } else {
      blk_parallel->statements.push_back(new Statement::CustomeCode("#pragma omp for"));
    }
    blk_parallel->statements.push_back(new Statement::CustomeCode("// OPENMP < 3 requires signed int here ..."));

    // parallel part
    std::list<Statement::Base*> *stmts = cyk_traversal_multithread_parallel(ast, *it_stmt_seq, std::get<1>(stmts_tilesize), name_maxtilen, ast.checkpoint && ast.checkpoint->cyk);
    // inject NT calls
    std::list<Statement::Base*> *new_stmts = add_nt_calls(*stmts, new std::list<std::string*>(), ast.grammar()->topological_ord(), ast.checkpoint && ast.checkpoint->cyk, true, false);
    stmts->insert(stmts->end(), new_stmts->begin(), new_stmts->end());

    blk_parallel->statements.insert(blk_parallel->statements.end(), stmts->begin(), stmts->end());
    blk_parallel->statements.push_back(new Statement::CustomeCode("// end parallel"));
    fn_cyk->stmts.push_back(blk_parallel);

    // serial part
    fn_cyk->stmts.insert(fn_cyk->stmts.end(), std::get<0>(stmts_tilesize)->begin(), std::get<0>(stmts_tilesize)->end());
    stmts = cyk_traversal_multithread_serial(ast, *it_stmt_seq, std::get<1>(stmts_tilesize), name_maxtilen, ast.checkpoint && ast.checkpoint->cyk);
    // inject NT calls
    std::list<Statement::Base*> *new_serial_stmts = add_nt_calls(*stmts, new std::list<std::string*>(), ast.grammar()->topological_ord(), ast.checkpoint && ast.checkpoint->cyk, true, true);
    stmts->insert(stmts->end(), new_serial_stmts->begin(), new_serial_stmts->end());
    fn_cyk->stmts.insert(fn_cyk->stmts.end(), stmts->begin(), stmts->end());
  }

  fn_cyk->stmts.push_back(new Statement::CustomeCode("#endif"));

  return fn_cyk;
}
