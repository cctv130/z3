/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    weighted_maxsat.cpp

Abstract:

    Weighted MAXSAT module

Author:

    Nikolaj Bjorner (nbjorner) 2014-4-17

Notes:

--*/

#include <typeinfo>
#include "weighted_maxsat.h"
#include "smt_theory.h"
#include "smt_context.h"
#include "ast_pp.h"
#include "theory_wmaxsat.h"
#include "opt_params.hpp"
#include "pb_decl_plugin.h"
#include "uint_set.h"
#include "pb_preprocess_tactic.h"
#include "simplify_tactic.h"
#include "tactical.h"
#include "tactic.h"
#include "model_smt2_pp.h"
#include "pb_sls.h"
#include "qfbv_tactic.h"
#include "card2bv_tactic.h"
#include "tactic2solver.h"
#include "nnf_tactic.h"
#include "opt_sls_solver.h"

namespace opt {


    // ---------------------------------------------
    // base class with common utilities used
    // by maxsmt solvers
    // 
    class maxsmt_solver_base : public maxsmt_solver {
    protected:
        ref<solver>      m_s;
        ast_manager&     m;
        volatile bool    m_cancel;
        expr_ref_vector  m_soft;
        vector<rational> m_weights;
        rational         m_lower;
        rational         m_upper;
        model_ref        m_model;
        ref<filter_model_converter> m_mc;    // model converter to remove fresh variables
        svector<bool>    m_assignment;       // truth assignment to soft constraints
        params_ref       m_params;           // config
        bool             m_enable_sls;       // config
        bool             m_enable_sat;       // config
        bool             m_sls_enabled;
        bool             m_sat_enabled;
    public:
        maxsmt_solver_base(solver* s, ast_manager& m): 
            m_s(s), m(m), m_cancel(false), m_soft(m),
            m_enable_sls(false), m_enable_sat(false),
            m_sls_enabled(false), m_sat_enabled(false) {
            m_s->get_model(m_model);
            SASSERT(m_model);
        }

        virtual ~maxsmt_solver_base() {}
        virtual rational get_lower() const { return m_lower; }
        virtual rational get_upper() const { return m_upper; }
        virtual bool get_assignment(unsigned index) const { return m_assignment[index]; }
        virtual void set_cancel(bool f) { m_cancel = f; m_s->set_cancel(f); }
        virtual void collect_statistics(statistics& st) const { 
            if (m_sls_enabled || m_sat_enabled) {
                m_s->collect_statistics(st);
            }
        }
        virtual void get_model(model_ref& mdl) { mdl = m_model.get(); }
        virtual void updt_params(params_ref& p) {
            m_params.copy(p);
            s().updt_params(p);
            opt_params _p(p);
            m_enable_sat = _p.enable_sat();
            m_enable_sls = _p.enable_sls();
        }       
        virtual void init_soft(vector<rational> const& weights, expr_ref_vector const& soft) {
            m_weights.reset();
            m_soft.reset();
            m_weights.append(weights);
            m_soft.append(soft);
        }
        void add_hard(expr* e){ s().assert_expr(e); }        
        solver& s() { return *m_s; }
        void set_converter(filter_model_converter* mc) { m_mc = mc; }

        void init() {
            m_lower.reset();
            m_upper.reset();
            m_assignment.reset();
            for (unsigned i = 0; i < m_weights.size(); ++i) {
                m_upper += m_weights[i];
                m_assignment.push_back(false);
            }
        }
        expr* mk_not(expr* e) {
            if (m.is_not(e, e)) {
                return e;
            }
            else {
                return m.mk_not(e);
            }
        }

        struct is_bv {
            struct found {};
            ast_manager& m;
            pb_util      pb;
            bv_util      bv;
            is_bv(ast_manager& m): m(m), pb(m), bv(m) {}
            void operator()(var *) { throw found(); }
            void operator()(quantifier *) { throw found(); }
            void operator()(app *n) {
                family_id fid = n->get_family_id();
                if (fid != m.get_basic_family_id() &&
                    fid != pb.get_family_id() &&
                    fid != bv.get_family_id() &&
                    !is_uninterp_const(n)) {
                    throw found();
                }
            }
        };

        bool probe_bv() {
            expr_fast_mark1 visited;
            is_bv proc(m);
            try {
                unsigned sz = s().get_num_assertions();
                for (unsigned i = 0; i < sz; i++) {
                    quick_for_each_expr(proc, visited, s().get_assertion(i));
                }
                sz = m_soft.size();
                for (unsigned i = 0; i < sz; ++i) {
                    quick_for_each_expr(proc, visited, m_soft[i].get());
                }
            }
            catch (is_bv::found) {
                return false;
            }
            return true;
        }

        void enable_bvsat() {
            if (m_enable_sat && !m_sat_enabled && probe_bv()) {
                tactic_ref pb2bv = mk_card2bv_tactic(m, m_params);
                tactic_ref bv2sat = mk_qfbv_tactic(m, m_params);
                tactic_ref tac = and_then(pb2bv.get(), bv2sat.get());
                solver* sat_solver = mk_tactic2solver(m, tac.get(), m_params);
                unsigned sz = s().get_num_assertions();
                for (unsigned i = 0; i < sz; ++i) {
                    sat_solver->assert_expr(s().get_assertion(i));
                }   
                unsigned lvl = m_s->get_scope_level();
                while (lvl > 0) { sat_solver->push(); --lvl; }
                m_s = sat_solver;
                m_sat_enabled = true;
            }
        }
        
        void enable_sls() {
            if (m_enable_sls && !m_sls_enabled && probe_bv()) {
                m_params.set_uint("restarts", 20);
                unsigned lvl = m_s->get_scope_level();
                sls_solver* sls = alloc(sls_solver, m, m_s.get(), m_soft, m_weights, m_params);
                m_s = sls;
                while (lvl > 0) { m_s->push(); --lvl; }
                m_sls_enabled = true;
                sls->opt(m_model);
            }
        }
        

    };

    // ------------------------------------------------------
    // Morgado, Heras, Marques-Silva 2013
    // (initial version without model-based optimizations)
    //
    class bcd2 : public maxsmt_solver_base {
        struct wcore {
            expr*           m_r;
            unsigned_vector m_R;
            rational        m_lower;
            rational        m_mid;
            rational        m_upper;
        };
        typedef obj_hashtable<expr> expr_set;

        pb_util             pb;
        expr_ref_vector     m_soft_aux;
        obj_map<expr, unsigned> m_relax2index; // expr |-> index
        obj_map<expr, unsigned> m_soft2index;  // expr |-> index
        expr_ref_vector     m_trail;
        expr_ref_vector     m_soft_constraints;
        expr_set            m_asm_set;
        vector<wcore>       m_cores;
        vector<rational>    m_sigmas;
        rational            m_den;    // least common multiplier of original denominators 
        bool                m_enable_lazy;   // enable adding soft constraints lazily (called 'mgbcd2')
        unsigned_vector     m_lazy_soft;     // soft constraints to add lazily.

        void set2asms(expr_set const& set, expr_ref_vector & es) const {
            es.reset();
            expr_set::iterator it = set.begin(), end = set.end();
            for (; it != end; ++it) {
                es.push_back(m.mk_not(*it));
            }
        }
        virtual void init_soft(vector<rational> const& weights, expr_ref_vector const& soft) {
            maxsmt_solver_base::init_soft(weights, soft);

            // normalize weights to be integral:
            m_den = rational::one();
            for (unsigned i = 0; i < m_weights.size(); ++i) {
                m_den = lcm(m_den, denominator(m_weights[i]));
            }
            if (!m_den.is_one()) {
                for (unsigned i = 0; i < m_weights.size(); ++i) {
                    m_weights[i] = m_den*m_weights[i];
                    SASSERT(m_weights[i].is_int());
                }                
            }
        }
        void init_bcd() {
            m_trail.reset();
            m_asm_set.reset();
            m_cores.reset();
            m_sigmas.reset();
            m_lazy_soft.reset();
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                m_sigmas.push_back(m_weights[i]);
                m_soft_aux.push_back(mk_fresh());
                if (m_enable_lazy) {
                    m_lazy_soft.push_back(i);
                }
                else {
                    enable_soft_constraint(i);
                }
            }
            m_upper += rational(1);                 
        }

    public:
        bcd2(solver* s, ast_manager& m): 
            maxsmt_solver_base(s, m),
            pb(m),
            m_soft_aux(m),
            m_trail(m),
            m_soft_constraints(m),
            m_enable_lazy(true) {
            m_enable_lazy = true;
        }

        virtual ~bcd2() {}

        virtual lbool operator()() {
            expr_ref fml(m), r(m);
            lbool is_sat = l_undef;
            expr_ref_vector asms(m);
            bool first = true;
            enable_sls();
            solver::scoped_push _scope1(s());
            init();
            init_bcd();
            while (m_lower < m_upper) {
                IF_VERBOSE(1, verbose_stream() << "(wmaxsat.bcd2 [" << m_lower << ":" << m_upper << "])\n";);
                assert_soft();
                solver::scoped_push _scope2(s());
                TRACE("opt", display(tout););
                assert_cores();
                set2asms(m_asm_set, asms);                
                if (m_cancel) {
                    normalize_bounds();
                    return l_undef;
                }
                is_sat = s().check_sat(asms.size(), asms.c_ptr());
                switch(is_sat) {
                case l_undef: 
                    normalize_bounds();
                    return l_undef;
                case l_true: {
                    svector<bool> assignment;
                    update_assignment(assignment);
                    first = false;
                    if (check_lazy_soft(assignment)) {
                        update_sigmas();
                    }
                    break;
                }
                case l_false: {
                    ptr_vector<expr> unsat_core;
                    uint_set subC, soft;
                    s().get_unsat_core(unsat_core);
                    core2indices(unsat_core, subC, soft);
                    SASSERT(unsat_core.size() == subC.num_elems() + soft.num_elems());
                    if (soft.num_elems() == 0 && subC.num_elems() == 1) {
                        unsigned s = *subC.begin();
                        wcore& c_s = m_cores[s];
                        c_s.m_lower = refine(c_s.m_R, c_s.m_mid);
                        c_s.m_mid = div(c_s.m_lower + c_s.m_upper, rational(2));
                    }
                    else {
                        wcore c_s;
                        rational delta = min_of_delta(subC);
                        rational lower = sum_of_lower(subC);
                        union_Rs(subC, c_s.m_R);
                        r = mk_fresh();
                        relax(subC, soft, c_s.m_R, delta);
                        c_s.m_lower = refine(c_s.m_R, lower + delta - rational(1));
                        c_s.m_upper = rational(first?1:0);
                        c_s.m_upper += sum_of_sigmas(c_s.m_R);
                        c_s.m_mid = div(c_s.m_lower + c_s.m_upper, rational(2));
                        c_s.m_r = r;
                        m_asm_set.insert(r);
                        subtract(m_cores, subC);
                        m_relax2index.insert(r, m_cores.size());
                        m_cores.push_back(c_s);
                    }
                    break;
                }
                }
                m_lower = compute_lower();
            }
            normalize_bounds();
            if (first) {
                return is_sat;
            }
            else {
                return l_true;
            }
        }


    private:

        void enable_soft_constraint(unsigned i) {
            expr_ref fml(m);
            expr* r = m_soft_aux[i].get();
            m_soft2index.insert(r, i);
            fml = m.mk_or(r, m_soft[i].get()); 
            m_soft_constraints.push_back(fml);
            m_asm_set.insert(r);
            SASSERT(m_weights[i].is_int());     
        }

        void assert_soft() {
            for (unsigned i = 0; i < m_soft_constraints.size(); ++i) {
                s().assert_expr(m_soft_constraints[i].get()); 
            }
            m_soft_constraints.reset();
        }

        bool check_lazy_soft(svector<bool> const& assignment) {
            bool all_satisfied = true;
            for (unsigned i = 0; i < m_lazy_soft.size(); ++i) {
                unsigned j = m_lazy_soft[i];
                if (!assignment[j]) {
                    enable_soft_constraint(j);
                    m_lazy_soft[i] = m_lazy_soft.back();
                    m_lazy_soft.pop_back();
                    --i;
                    all_satisfied = false;
                }
            }
            return all_satisfied;
        }

        void normalize_bounds() {
            m_lower /= m_den;
            m_upper /= m_den;
        }

        expr* mk_fresh() {
            app_ref r(m);
            r = m.mk_fresh_const("r", m.mk_bool_sort());
            m_trail.push_back(r);
            m_mc->insert(r->get_decl());                
            return r;
        }

        void update_assignment(svector<bool>& new_assignment) {
            expr_ref val(m);
            rational new_upper(0);
            model_ref model;            
            new_assignment.reset();
            s().get_model(model);
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                VERIFY(model->eval(m_soft[i].get(), val));    
                new_assignment.push_back(m.is_true(val));                            
                if (!new_assignment[i]) {
                    new_upper += m_weights[i];
                }
            }
            if (new_upper < m_upper) {
                m_upper = new_upper;
                m_model = model;
                m_assignment.reset();
                m_assignment.append(new_assignment);
            }
        }

        void update_sigmas() {
            for (unsigned i = 0; i < m_cores.size(); ++i) {
                wcore& c_i = m_cores[i];
                unsigned_vector const& R = c_i.m_R;
                c_i.m_upper.reset();
                for (unsigned j = 0; j < R.size(); ++j) {
                    unsigned r_j = R[j];
                    if (!m_assignment[r_j]) {
                        c_i.m_upper += m_weights[r_j];
                        m_sigmas[r_j] = m_weights[r_j];
                    }
                    else {
                        m_sigmas[r_j].reset();
                    }
                }
                c_i.m_mid = div(c_i.m_lower + c_i.m_upper, rational(2));
            }
        }

        /**
         * Minimum of two (positive) numbers. Zero is treated as +infinity.
         */
        rational min_z(rational const& a, rational const& b) {
            if (a.is_zero()) return b;
            if (b.is_zero()) return a;
            if (a < b) return a;
            return b;
        }

        rational min_of_delta(uint_set const& subC) {
            rational delta(0);
            for (uint_set::iterator it = subC.begin(); it != subC.end(); ++it) {
                unsigned j = *it;
                wcore const& core = m_cores[j];
                rational new_delta = rational(1) + core.m_upper - core.m_mid; 
                SASSERT(new_delta.is_pos());
                delta = min_z(delta, new_delta);                
            }
            return delta;
        }

        rational sum_of_lower(uint_set const& subC) {
            rational lower(0);
            for (uint_set::iterator it = subC.begin(); it != subC.end(); ++it) {
                lower += m_cores[*it].m_lower;
            }
            return lower;
        }

        rational sum_of_sigmas(unsigned_vector const& R) {
            rational sum(0);
            for (unsigned i = 0; i < R.size(); ++i) {
                sum += m_sigmas[R[i]];
            }
            return sum;
        }
        void union_Rs(uint_set const& subC, unsigned_vector& R) {
            for (uint_set::iterator it = subC.begin(); it != subC.end(); ++it) {
                R.append(m_cores[*it].m_R);
            }
        }
        rational compute_lower() {
            rational result(0);
            for (unsigned i = 0; i < m_cores.size(); ++i) {
                result += m_cores[i].m_lower;
            }
            return result;
        }
        void subtract(vector<wcore>& cores, uint_set const& subC) {
            unsigned j = 0;                        
            for (unsigned i = 0; i < cores.size(); ++i) {
                if (subC.contains(i)) {
                    m_asm_set.remove(cores[i].m_r);
                }
                else {
                    if (j != i) {
                        cores[j] = cores[i];
                    }
                    ++j;
                }
            }
            cores.resize(j);
            for (unsigned i = 0; i < cores.size(); ++i) {
                m_relax2index.insert(cores[i].m_r, i);
            }
        }
        void core2indices(ptr_vector<expr> const& core, uint_set& subC, uint_set& soft) {
            for (unsigned i = 0; i < core.size(); ++i) {
                unsigned j;
                expr* a;
                VERIFY(m.is_not(core[i], a));
                if (m_relax2index.find(a, j)) {
                    subC.insert(j);
                }
                else {
                    VERIFY(m_soft2index.find(a, j));
                    soft.insert(j);
                }
            }
        }            
        rational refine(unsigned_vector const& idx, rational v) {
            return v + rational(1);
        }
        void relax(uint_set& subC, uint_set& soft, unsigned_vector& R, rational& delta) {
            for (uint_set::iterator it = soft.begin(); it != soft.end(); ++it) {                
                R.push_back(*it);
                delta = min_z(delta, m_weights[*it]);
                m_asm_set.remove(m_soft_aux[*it].get());
            }
        }
        void assert_cores() {
            for (unsigned i = 0; i < m_cores.size(); ++i) {
                assert_core(m_cores[i]);
            }
        }
        void assert_core(wcore const& core) {
            expr_ref fml(m);
            vector<rational> ws;
            ptr_vector<expr> rs;
            for (unsigned j = 0; j < core.m_R.size(); ++j) {
                unsigned idx = core.m_R[j];
                ws.push_back(m_weights[idx]);
                rs.push_back(m_soft_aux[idx].get());   // TBD: check
            }
            fml = pb.mk_le(ws.size(), ws.c_ptr(), rs.c_ptr(), core.m_mid);
            fml = m.mk_or(core.m_r, fml);
            s().assert_expr(fml);
        }
        void display(std::ostream& out) {
            out << "[" << m_lower << ":" << m_upper << "]\n";
            s().display(out);
            out << "\n";
            for (unsigned i = 0; i < m_cores.size(); ++i) {
                wcore const& c = m_cores[i];
                out << mk_pp(c.m_r, m) << ": ";
                for (unsigned j = 0; j < c.m_R.size(); ++j) {
                    out << c.m_R[j] << " (" << m_sigmas[c.m_R[j]] << ") ";
                }
                out << "[" << c.m_lower << ":" << c.m_mid << ":" << c.m_upper << "]\n";
            }
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                out << mk_pp(m_soft[i].get(), m) << " " << m_weights[i] << "\n";
            }
        }
    };

    // ----------------------------------
    // incrementally add pseudo-boolean 
    // lower bounds.

    class pbmax : public maxsmt_solver_base {
        bool m_use_aux;
    public:
        pbmax(solver* s, ast_manager& m, bool use_aux): 
            maxsmt_solver_base(s, m), m_use_aux(use_aux) {
        }
        
        virtual ~pbmax() {}

        lbool operator()() {
            enable_bvsat();
            enable_sls();

            TRACE("opt", s().display(tout); tout << "\n";
                  for (unsigned i = 0; i < m_soft.size(); ++i) {
                      tout << mk_pp(m_soft[i].get(), m) << " " << m_weights[i] << "\n";
                  }
                  );
            pb_util u(m);
            expr_ref fml(m), val(m);
            app_ref b(m);
            expr_ref_vector nsoft(m);
            init();            
            if (m_use_aux) {
                s().push();
            }
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                if (m_use_aux) {
                    b = m.mk_fresh_const("b", m.mk_bool_sort());
                    m_mc->insert(b->get_decl());
                    fml = m.mk_or(m_soft[i].get(), b);
                    s().assert_expr(fml);
                    nsoft.push_back(b);
                }
                else {
                    nsoft.push_back(mk_not(m_soft[i].get()));
                }
            }
            lbool is_sat = l_true;
            while (l_true == is_sat) {
                IF_VERBOSE(1, verbose_stream() << "(wmaxsat.pb solve with upper bound: " << m_upper << ")\n";);
                m_upper.reset();
                for (unsigned i = 0; i < m_soft.size(); ++i) {
                    VERIFY(m_model->eval(nsoft[i].get(), val));
                    TRACE("opt", tout << "eval " << mk_pp(m_soft[i].get(), m) << " " << val << "\n";);
                    m_assignment[i] = !m.is_true(val);
                    if (!m_assignment[i]) {
                        m_upper += m_weights[i];
                    }
                }                     
                TRACE("opt", tout << "new upper: " << m_upper << "\n";);
                
                fml = u.mk_lt(nsoft.size(), m_weights.c_ptr(), nsoft.c_ptr(), m_upper);

                TRACE("opt", s().display(tout<<"looping\n"););
                solver::scoped_push _scope2(s());
                s().assert_expr(fml);
                is_sat = s().check_sat(0,0);
                if (m_cancel) {
                    is_sat = l_undef;
                }
                if (is_sat == l_true) {
                    s().get_model(m_model);
                }
            }            
            if (is_sat == l_false) {
                is_sat = l_true;
                m_lower = m_upper;
            }
            if (m_use_aux) {
                s().pop(1);
            }
            TRACE("opt", tout << "lower: " << m_lower << "\n";);
            return is_sat;
        }
    };

    // ------------------------------------------------------
    // AAAI 2010
    class wpm2 : public maxsmt_solver_base {
        scoped_ptr<maxsmt_solver_base> maxs;
    public:
        wpm2(solver* s, ast_manager& m, maxsmt_solver_base* _maxs): 
            maxsmt_solver_base(s, m), maxs(_maxs) {
        }

        virtual ~wpm2() {}

        lbool operator()() {
            enable_sls();
            IF_VERBOSE(1, verbose_stream() << "(wmaxsat.wpm2 solve)\n";);
            solver::scoped_push _s(s());
            pb_util u(m);
            app_ref fml(m), a(m), b(m), c(m);
            expr_ref val(m);
            expr_ref_vector block(m), ans(m), al(m), am(m);
            obj_map<expr, unsigned> ans_index;
            vector<rational> amk;
            vector<uint_set> sc;
            init();
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                rational w = m_weights[i];

                b = m.mk_fresh_const("b", m.mk_bool_sort());
                m_mc->insert(b->get_decl());
                block.push_back(b);
                expr* bb = b;

                a = m.mk_fresh_const("a", m.mk_bool_sort());
                m_mc->insert(a->get_decl());
                ans.push_back(a);
                ans_index.insert(a, i);
                fml = m.mk_or(m_soft[i].get(), b, m.mk_not(a));
                s().assert_expr(fml);
                
                c = m.mk_fresh_const("c", m.mk_bool_sort());
                m_mc->insert(c->get_decl());
                fml = m.mk_implies(c, u.mk_le(1,&w,&bb,rational(0)));
                s().assert_expr(fml);

                sc.push_back(uint_set());
                sc.back().insert(i);                
                am.push_back(c);
                amk.push_back(rational(0));
            }

            while (true) {
                expr_ref_vector asms(m);
                ptr_vector<expr> core;
                asms.append(ans);
                asms.append(am);
                lbool is_sat = s().check_sat(asms.size(), asms.c_ptr());
                TRACE("opt", 
                      tout << "\nassumptions: ";
                      for (unsigned i = 0; i < asms.size(); ++i) {
                          tout << mk_pp(asms[i].get(), m) << " ";
                      }
                      tout << "\n" << is_sat << "\n";
                      tout << "upper: " << m_upper << "\n";
                      tout << "lower: " << m_lower << "\n";
                      if (is_sat == l_true) {
                          model_ref mdl;
                          s().get_model(mdl);
                          model_smt2_pp(tout, m, *(mdl.get()), 0);
                      });

                if (m_cancel && is_sat != l_false) {
                    is_sat = l_undef;
                }
                if (is_sat == l_true) {
                    m_upper = m_lower;
                    s().get_model(m_model);
                    for (unsigned i = 0; i < block.size(); ++i) {
                        VERIFY(m_model->eval(m_soft[i].get(), val));
                        TRACE("opt", tout << mk_pp(block[i].get(), m) << " " << val << "\n";);
                        m_assignment[i] = m.is_true(val);
                    }
                }
                if (is_sat != l_false) {
                    return is_sat;
                }
                s().get_unsat_core(core);
                if (core.empty()) {
                    return l_false;
                }
                TRACE("opt",
                      tout << "core: ";
                      for (unsigned i = 0; i < core.size(); ++i) {
                          tout << mk_pp(core[i],m) << " ";
                      }
                      tout << "\n";);
                uint_set A;
                for (unsigned i = 0; i < core.size(); ++i) {
                    unsigned j;
                    if (ans_index.find(core[i], j)) {
                        A.insert(j);
                    }
                }
                if (A.empty()) {
                    return l_false;
                }
                uint_set B;
                rational k(0);
                rational old_lower(m_lower);
                for (unsigned i = 0; i < sc.size(); ++i) {
                    uint_set t(sc[i]);
                    t &= A;
                    if (!t.empty()) {
                        B |= sc[i];
                        k += amk[i];
                        m_lower -= amk[i];
                        sc[i] = sc.back();
                        sc.pop_back();
                        am[i] = am.back();
                        am.pop_back();
                        amk[i] = amk.back();
                        amk.pop_back();
                        --i;
                    }
                }
                vector<rational> ws;
                expr_ref_vector  bs(m);
                for (unsigned i = 0; i < m_soft.size(); ++i) {
                    if (B.contains(i)) {
                        ws.push_back(m_weights[i]);
                        bs.push_back(block[i].get());
                    }
                }
                TRACE("opt", tout << "at most bound: " << k << "\n";);
                is_sat = new_bound(al, ws, bs, k);
                if (is_sat != l_true) {
                    return is_sat;
                }
                m_lower += k;
                SASSERT(m_lower > old_lower);
                TRACE("opt", tout << "new bound: " << m_lower << "\n";);
                expr_ref B_le_k(m), B_ge_k(m);
                B_le_k = u.mk_le(ws.size(), ws.c_ptr(), bs.c_ptr(), k);
                B_ge_k = u.mk_ge(ws.size(), ws.c_ptr(), bs.c_ptr(), k);
                s().assert_expr(B_ge_k);
                al.push_back(B_ge_k);
                IF_VERBOSE(1, verbose_stream() << "(wmaxsat.wpm2 lower bound: " << m_lower << ")\n";);
                IF_VERBOSE(2, verbose_stream() << "New lower bound: " << B_ge_k << "\n";);

                c = m.mk_fresh_const("c", m.mk_bool_sort());
                m_mc->insert(c->get_decl());
                fml = m.mk_implies(c, B_le_k);
                s().assert_expr(fml);
                sc.push_back(B);
                am.push_back(c);
                amk.push_back(k);
            }            
        }

        virtual void set_cancel(bool f) { 
            maxsmt_solver_base::set_cancel(f); 
            maxs->set_cancel(f); 
        }

    private:
        lbool new_bound(expr_ref_vector const& al, 
                        vector<rational> const& ws, 
                        expr_ref_vector const& bs, 
                        rational& k) {
            pb_util u(m);
            expr_ref_vector al2(m);
            al2.append(al);
            // w_j*b_j > k
            al2.push_back(m.mk_not(u.mk_le(ws.size(), ws.c_ptr(), bs.c_ptr(), k)));            
            return bound(al2, ws, bs, k);
        }

        // 
        // minimal k, such that  al & w_j*b_j >= k is sat
        // minimal k, such that  al & 3*x + 4*y >= k is sat
        // minimal k, such that al & (or (not x) w3) & (or (not y) w4)
        //
        lbool bound(expr_ref_vector const& al, 
                    vector<rational> const& ws, 
                    expr_ref_vector const& bs,
                    rational& k) {
            expr_ref_vector nbs(m);
            opt_solver::scoped_push _sc(maxs->s());
            for (unsigned i = 0; i < al.size(); ++i) {
                maxs->add_hard(al[i]);
            }
            for (unsigned i = 0; i < bs.size(); ++i) {
                nbs.push_back(mk_not(bs[i]));
            }    
            TRACE("opt", 
                  maxs->s().display(tout);
                  tout << "\n";
                  for (unsigned i = 0; i < bs.size(); ++i) {
                      tout << mk_pp(bs[i], m) << " " << ws[i] << "\n";
                  });
            maxs->init_soft(ws, nbs);
            lbool is_sat = (*maxs)();
            SASSERT(maxs->get_lower() > k);
            k = maxs->get_lower();
            return is_sat;
        }
    };

    class sls : public maxsmt_solver_base {
    public:
        sls(solver* s, ast_manager& m): 
            maxsmt_solver_base(s, m) {
        }
        virtual ~sls() {}
        lbool operator()() {
            IF_VERBOSE(1, verbose_stream() << "(sls solve)\n";);
            enable_bvsat();
            enable_sls();
            init();
            lbool is_sat = s().check_sat(0, 0);
            if (is_sat == l_true) {
                s().get_model(m_model);
                m_upper.reset();
                for (unsigned i = 0; i < m_soft.size(); ++i) {
                    expr_ref tmp(m);
                    m_model->eval(m_soft[i].get(), tmp, true);
                    m_assignment[i] = m.is_true(tmp);
                    if (!m_assignment[i]) {
                        m_upper += m_weights[i];
                    }
                }
            }
            return is_sat;
        }

    };


    class maxsmt_solver_wbase : public maxsmt_solver_base {
        smt::context& ctx;
    public:
        maxsmt_solver_wbase(solver* s, ast_manager& m, smt::context& ctx): 
            maxsmt_solver_base(s, m), ctx(ctx) {}
        ~maxsmt_solver_wbase() {}

        class scoped_ensure_theory {
            smt::theory_wmaxsat* m_wth;
        public:
            scoped_ensure_theory(maxsmt_solver_wbase& s) {
                m_wth = s.ensure_theory();
            }
            ~scoped_ensure_theory() {
                m_wth->reset();
            }
            smt::theory_wmaxsat& operator()() { return *m_wth; }
        };
        
        smt::theory_wmaxsat* ensure_theory() {
            smt::theory_wmaxsat* wth = get_theory();
            if (wth) {
                wth->reset();
            }
            else {
                wth = alloc(smt::theory_wmaxsat, m, m_mc);
                ctx.register_plugin(wth);
            }
            return wth;
        }
        smt::theory_wmaxsat* get_theory() const {
            smt::theory_id th_id = m.get_family_id("weighted_maxsat");
            smt::theory* th = ctx.get_theory(th_id);               
            if (th) {
                return dynamic_cast<smt::theory_wmaxsat*>(th);
            }
            else {
                return 0;
            }
        }
    };

    // ----------------------------------------------------------
    // weighted max-sat using a custom theory solver for max-sat.
    // NB. it is quite similar to pseudo-Boolean propagation.


    class wmax : public maxsmt_solver_wbase {
    public:
        wmax(solver* s, ast_manager& m, smt::context& ctx): maxsmt_solver_wbase(s, m, ctx) {}
        virtual ~wmax() {}

        lbool operator()() {
            TRACE("opt", tout << "weighted maxsat\n";);
            scoped_ensure_theory wth(*this);
            solver::scoped_push _s(s());
            lbool is_sat = l_true;
            bool was_sat = false;
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                wth().assert_weighted(m_soft[i].get(), m_weights[i]);
            }
            solver::scoped_push __s(s());
            while (l_true == is_sat) {
                IF_VERBOSE(1, verbose_stream() << "(wmax " << m_upper << ")\n";);
                is_sat = s().check_sat(0,0);
                if (m_cancel) {
                    is_sat = l_undef;
                }
                if (is_sat == l_true) {
                    if (wth().is_optimal()) {
                        m_upper = wth().get_min_cost();
                        s().get_model(m_model);
                    }
                    expr_ref fml = wth().mk_block();
                    s().assert_expr(fml);
                    was_sat = true;
                }
            }
            if (was_sat) {
                wth().get_assignment(m_assignment);
            }
            if (is_sat == l_false && was_sat) {
                is_sat = l_true;
            }
            m_upper = wth().get_min_cost();
            if (is_sat == l_true) {
                m_lower = m_upper;
            }
            TRACE("opt", tout << "min cost: " << m_upper << "\n";);
            return is_sat;
        }
    };

    struct wmaxsmt::imp {
        ast_manager&     m;
        ref<opt_solver>  s;                     // solver state that contains hard constraints
        expr_ref_vector                         m_soft;             // set of soft constraints
        vector<rational>                        m_weights;          // their weights
        symbol                                  m_engine;           // config
        mutable params_ref                      m_params;           // config
        mutable scoped_ptr<maxsmt_solver_base>  m_maxsmt;           // underlying maxsmt solver

        imp(ast_manager& m, 
            opt_solver*  s, 
            expr_ref_vector const& soft_constraints, 
            vector<rational> const& weights):
            m(m), 
            s(s), 
            m_soft(soft_constraints), 
            m_weights(weights)
        {
        }

        maxsmt_solver_base& maxsmt() const {
            if (m_maxsmt) {
                return *m_maxsmt;
            }
            if (m_engine == symbol("pwmax")) {
                m_maxsmt = alloc(pbmax, s.get(), m, true);
            }
            else if (m_engine == symbol("pbmax")) {
                m_maxsmt = alloc(pbmax, s.get(), m, false);
            }
            else if (m_engine == symbol("wpm2")) {
                maxsmt_solver_base* s2 = alloc(pbmax, s.get(), m, false);
                m_maxsmt = alloc(wpm2, s.get(), m, s2);
            }
            else if (m_engine == symbol("bcd2")) {
                m_maxsmt = alloc(bcd2, s.get(), m);
            }
            // TBD: this is experimental one-round version of SLS
            else if (m_engine == symbol("sls")) {
                m_maxsmt = alloc(sls, s.get(), m);
            }
            else if (m_engine == symbol::null || m_engine == symbol("wmax")) {
                m_maxsmt = alloc(wmax, s.get(), m, s->get_context());
            }
            else {
                IF_VERBOSE(0, verbose_stream() << "(unknown engine " << m_engine << " using default 'wmax')\n";);
                m_maxsmt = alloc(wmax, s.get(), m, s->get_context());
            }
            m_maxsmt->updt_params(m_params);
            m_maxsmt->init_soft(m_weights, m_soft);
            m_maxsmt->set_converter(s->mc_ref().get());
            return *m_maxsmt;
        }

        ~imp() {}

        /**
           Takes solver with hard constraints added.
           Returns a maximal satisfying subset of weighted soft_constraints
           that are still consistent with the solver state.
        */        
        lbool operator()() {
            return maxsmt()();
        }
        rational get_lower() const {
            return maxsmt().get_lower();
        }
        rational get_upper() const {
            return maxsmt().get_upper();
        }
        void get_model(model_ref& mdl) {
            if (m_maxsmt) m_maxsmt->get_model(mdl);
        }
        void set_cancel(bool f) {
            if (m_maxsmt) m_maxsmt->set_cancel(f);
        }
        bool get_assignment(unsigned index) const {
            return maxsmt().get_assignment(index);
        }
        void collect_statistics(statistics& st) const {
            if (m_maxsmt) m_maxsmt->collect_statistics(st);
        }
        void updt_params(params_ref& p) {
            opt_params _p(p);
            m_engine = _p.wmaxsat_engine();        
            m_maxsmt = 0;
        }
    };

    wmaxsmt::wmaxsmt(ast_manager& m, 
                     opt_solver* s, 
                     expr_ref_vector& soft_constraints, 
                     vector<rational> const& weights) {
        m_imp = alloc(imp, m, s, soft_constraints, weights);
    }
    wmaxsmt::~wmaxsmt() {
        dealloc(m_imp);
    }    
    lbool wmaxsmt::operator()() {
        return (*m_imp)();
    }
    rational wmaxsmt::get_lower() const {
        return m_imp->get_lower();
    }
    rational wmaxsmt::get_upper() const {
        return m_imp->get_upper();
    }
    bool wmaxsmt::get_assignment(unsigned idx) const {
        return m_imp->get_assignment(idx);
    }
    void wmaxsmt::set_cancel(bool f) {
        m_imp->set_cancel(f);
    }
    void wmaxsmt::collect_statistics(statistics& st) const {
        m_imp->collect_statistics(st);
    }
    void wmaxsmt::get_model(model_ref& mdl) {
        m_imp->get_model(mdl);
    }
    void wmaxsmt::updt_params(params_ref& p) {
        m_imp->updt_params(p);
    }    
};


#if 0

    /**
       Iteratively increase cost until there is an assignment during
       final_check that satisfies min_cost.
       
       Takes: log (n / log(n)) iterations
    */        
    class iwmax : public maxsmt_solver_wbase {
    public:
        
        iwmax(solver* s, ast_manager& m, smt::context& ctx): maxsmt_solver_wbase(s, m, ctx) {}
        virtual ~iwmax() {}
        
        lbool operator()() {
            pb_util 
            solver::scoped_push _s(s());
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                wth().assert_weighted(m_soft[i].get(), m_weights[i]);
            }
            solver::scoped_push __s(s());
            rational cost = wth().get_min_cost();
            rational log_cost(1), tmp(1);
            while (tmp < cost) {
                ++log_cost;
                tmp *= rational(2);
            }
            expr_ref_vector bounds(m);
            expr_ref bound(m);
            lbool result = l_false;
            unsigned nsc = 0;
            m_upper = cost;
            while (result == l_false) {
                bound = wth().set_min_cost(log_cost);
                s().push();
                ++nsc;
                IF_VERBOSE(1, verbose_stream() << "(wmaxsat.iwmax min cost: " << log_cost << ")\n";);
                TRACE("opt", tout << "cost: " << log_cost << " " << bound << "\n";);
                bounds.push_back(bound);
                result = conditional_solve(wth(), bound);
                if (result == l_false) {
                    m_lower = log_cost;        
                }
                if (log_cost > cost) {
                    break;
                }
                log_cost *= rational(2);
                if (m_cancel) {
                    result = l_undef;
                }
            }
            s().pop(nsc);
            return result;
        }
    private:
        lbool conditional_solve(smt::theory_wmaxsat& wth, expr* cond) {
            bool was_sat = false;
            lbool is_sat = l_true;
            while (l_true == is_sat) {
                is_sat = s().check_sat(1,&cond);
                if (m_cancel) {
                    is_sat = l_undef;
                }
                if (is_sat == l_true) {
                    if (wth.is_optimal()) {
                        s().get_model(m_model);
                        was_sat = true;
                    }
                    expr_ref fml = wth.mk_block();
                    s().assert_expr(fml);
                }
            }
            if (was_sat) {
                wth.get_assignment(m_assignment);
            }
            if (is_sat == l_false && was_sat) {
                is_sat = l_true;
            }
            if (is_sat == l_true) {
                m_lower = m_upper = wth.get_min_cost();
            }
            TRACE("opt", tout << "min cost: " << m_upper << " sat: " << is_sat << "\n";);
            return is_sat;            
        }

    };

        // ------------------------------------------------------
        // Version from CP'13        
        lbool wpm2b_solve() {
            IF_VERBOSE(1, verbose_stream() << "(wmaxsat.wpm2b solve)\n";);
            solver::scoped_push _s(s());
            pb_util u(m);
            app_ref fml(m), a(m), b(m), c(m);
            expr_ref val(m);
            expr_ref_vector block(m), ans(m), am(m), soft(m);
            obj_map<expr, unsigned> ans_index;
            vector<rational> amk;
            vector<uint_set> sc;    // vector of indices used in at last constraints
            expr_ref_vector  al(m); // vector of at least constraints.
            rational wmax;
            init();
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                rational w = m_weights[i];
                if (wmax < w) wmax = w;
                b = m.mk_fresh_const("b", m.mk_bool_sort());
                block.push_back(b);
                expr* bb = b;
                s.mc().insert(b->get_decl());
                a = m.mk_fresh_const("a", m.mk_bool_sort());
                s.mc().insert(a->get_decl());
                ans.push_back(a);
                ans_index.insert(a, i);
                soft.push_back(0); // assert soft constraints lazily.
                
                c = m.mk_fresh_const("c", m.mk_bool_sort());
                s.mc().insert(c->get_decl());
                fml = m.mk_implies(c, u.mk_le(1,&w,&bb,rational(0)));
                s.assert_expr(fml);

                sc.push_back(uint_set());
                sc.back().insert(i);                
                am.push_back(c);

                al.push_back(u.mk_ge(1,&w,&bb,rational(0)));
                s.assert_expr(al.back());

                amk.push_back(rational(0));
            }
            ++wmax;

            while (true) {
                enable_soft(soft, block, ans, wmax);
                expr_ref_vector asms(m);
                asms.append(ans);
                asms.append(am);
                lbool is_sat = s().check_sat(asms.size(), asms.c_ptr());
                if (m_cancel && is_sat != l_false) {
                    is_sat = l_undef;
                }
                if (is_sat == l_undef) {
                    return l_undef;
                }
                if (is_sat == l_true && wmax.is_zero()) {
                    m_upper = m_lower;
                    updt_model(s);
                    for (unsigned i = 0; i < block.size(); ++i) {
                        VERIFY(m_model->eval(block[i].get(), val));
                        m_assignment[i] = m.is_false(val);
                    }
                    return l_true;
                }
                if (is_sat == l_true) {
                    rational W(0);
                    for (unsigned i = 0; i < m_weights.size(); ++i) {
                        if (m_weights[i] < wmax) W += m_weights[i];
                    }
                    harden(am, W);
                    wmax = decrease(wmax);
                    continue;
                }
                SASSERT(is_sat == l_false);
                ptr_vector<expr> core;
                s.get_unsat_core(core);
                if (core.empty()) {
                    return l_false;
                }
                uint_set A;
                for (unsigned i = 0; i < core.size(); ++i) {
                    unsigned j;
                    if (ans_index.find(core[i], j) && soft[j].get()) {
                        A.insert(j);
                    }
                }
                if (A.empty()) {
                    return l_false;
                }
                uint_set B;
                rational k;
                for (unsigned i = 0; i < sc.size(); ++i) {
                    uint_set t(sc[i]);
                    t &= A;
                    if (!t.empty()) {
                        B |= sc[i];
                        m_lower -= amk[i];
                        k += amk[i];
                        sc[i] = sc.back();
                        sc.pop_back();
                        am[i] = am.back();
                        am.pop_back();
                        amk[i] = amk.back();
                        amk.pop_back();
                        --i;
                    }
                }
                vector<rational> ws;
                expr_ref_vector  bs(m);
                for (unsigned i = 0; i < m_soft.size(); ++i) {
                    if (B.contains(i)) {
                        ws.push_back(m_weights[i]);
                        bs.push_back(block[i].get());
                    }
                }

                expr_ref_vector al2(al);
                for (unsigned i = 0; i < s.get_num_assertions(); ++i) {
                    al2.push_back(s.get_assertion(i));
                }
                is_sat = new_bound(al2, ws, bs, k);
                if (is_sat != l_true) {
                    return is_sat;
                }
                m_lower += k;
                expr_ref B_le_k(m), B_ge_k(m);
                B_le_k = u.mk_le(ws.size(), ws.c_ptr(), bs.c_ptr(), k);
                B_ge_k = u.mk_ge(ws.size(), ws.c_ptr(), bs.c_ptr(), k);
                s.assert_expr(B_ge_k);
                al.push_back(B_ge_k);
                IF_VERBOSE(1, verbose_stream() << "(wmaxsat.wpm2 lower bound: " << m_lower << ")\n";);
                IF_VERBOSE(2, verbose_stream() << "New lower bound: " << B_ge_k << "\n";);

                c = m.mk_fresh_const("c", m.mk_bool_sort());
                s.mc().insert(c->get_decl());
                fml = m.mk_implies(c, B_le_k);
                s.assert_expr(fml);
                sc.push_back(B);
                am.push_back(c);
                amk.push_back(k);
            }            
        }

        void harden(expr_ref_vector& am, rational const& W) {
            // TBD 
        }
        
        rational decrease(rational const& wmax) {
            rational wmin(0);
            for (unsigned i = 0; i < m_weights.size(); ++i) {
                rational w = m_weights[i];
                if (w < wmax && wmin < w) wmin = w;
            }
            return wmin;
        }
   

        // enable soft constraints that have reached wmax.
        void enable_soft(expr_ref_vector& soft, 
                         expr_ref_vector const& block, 
                         expr_ref_vector const& ans, 
                         rational wmax) {
            for (unsigned i = 0; i < m_soft.size(); ++i) {
                rational w = m_weights[i];
                if (w >= wmax && !soft[i].get()) {
                    soft[i] = m.mk_or(m_soft[i].get(), block[i], m.mk_not(ans[i]));
                    s.assert_expr(soft[i].get());
                }           
            }     
        }                               



#endif
