/*++
Copyright (c) 2016 Microsoft Corporation

Module Name:

    bv_bounds_tactic.cpp

Abstract:

    Contextual bounds simplification tactic.

Author:

    Nikolaj Bjorner (nbjorner) 2016-2-12


--*/

#include "bv_bounds_tactic.h"
#include "ctx_simplify_tactic.h"
#include "bv_decl_plugin.h"
#include "ast_pp.h"
#include <climits>

static uint64_t uMaxInt(unsigned sz) {
    SASSERT(sz <= 64);
    return ULLONG_MAX >> (64u - sz);
}

namespace {

struct interval {
    // l < h: [l, h]
    // l > h: [0, h] U [l, UMAX_INT]
    uint64_t l, h;
    unsigned sz;
    bool tight;

    interval() {}
    interval(uint64_t l, uint64_t h, unsigned sz, bool tight = false) : l(l), h(h), sz(sz), tight(tight) {
        // canonicalize full set
        if (is_wrapped() && l == h + 1) {
            this->l = 0;
            this->h = uMaxInt(sz);
        }
        SASSERT(invariant());
    }

    bool invariant() const {
        return l <= uMaxInt(sz) && h <= uMaxInt(sz) &&
               (!is_wrapped() || l != h+1);
    }

    bool is_full() const { return l == 0 && h == uMaxInt(sz); }
    bool is_wrapped() const { return l > h; }
    bool is_singleton() const { return l == h; }

    bool operator==(const interval& b) const {
        SASSERT(sz == b.sz);
        return l == b.l && h == b.h && tight == b.tight;
    }
    bool operator!=(const interval& b) const { return !(*this == b); }

    bool implies(const interval& b) const {
        if (b.is_full())
            return true;
        if (is_full())
            return false;

        if (is_wrapped()) {
            // l >= b.l >= b.h >= h
            return b.is_wrapped() && h <= b.h && l >= b.l;
        } else if (b.is_wrapped()) {
            // b.l > b.h >= h >= l
            // h >= l >= b.l > b.h
            return h <= b.h || l >= b.l;
        } else {
            // 
            return l >= b.l && h <= b.h;
        }
    }

    /// return false if intersection is unsat
    bool intersect(const interval& b, interval& result) const {
        if (is_full() || *this == b) {
            result = b;
            return true;
        }
        if (b.is_full()) {
            result = *this;
            return true;
        }

        if (is_wrapped()) {
            if (b.is_wrapped()) {
                if (h >= b.l) {
                    result = b;
                } else if (b.h >= l) {
                    result = *this;
                } else {
                    result = interval(std::max(l, b.l), std::min(h, b.h), sz);
                }
            } else {
                return b.intersect(*this, result);
            }
        } else if (b.is_wrapped()) {
            // ... b.h ... l ... h ... b.l ..
            if (h < b.l && l > b.h) {
                return false;
            }
            // ... l ... b.l ... h ...
            if (h >= b.l && l <= b.h) {
                result = b;
            } else if (h >= b.l) {
                result = interval(b.l, h, sz);
            } else {
                // ... l .. b.h .. h .. b.l ...
                SASSERT(l <= b.h);
                result = interval(l, std::min(h, b.h), sz);
            }
        } else {
            if (l > b.h || h < b.l)
                return false;

            // 0 .. l.. l' ... h ... h'
            result = interval(std::max(l, b.l), std::min(h, b.h), sz, tight && b.tight);
        }
        return true;
    }

    /// return false if negation is empty
    bool negate(interval& result) const {
        if (!tight) {
            result = interval(0, uMaxInt(sz), true);
            return true;
        }

        if (is_full())
            return false;
        if (l == 0) {
            result = interval(h + 1, uMaxInt(sz), sz);
        } else if (uMaxInt(sz) == h) {
            result = interval(0, l - 1, sz);
        } else {
            result = interval(h + 1, l - 1, sz);
        }
        return true;
    }
};

std::ostream& operator<<(std::ostream& o, const interval& I) {
    o << "[" << I.l << ", " << I.h << "]";
    return o;
}


struct undo_bound {
    expr* e;
    interval b;
    bool fresh;
    undo_bound(expr* e, const interval& b, bool fresh) : e(e), b(b), fresh(fresh) {}
};

class bv_bounds_simplifier : public ctx_simplify_tactic::simplifier {
    typedef obj_map<expr, interval> map;
    typedef obj_map<expr, bool> expr_set;
    typedef obj_map<expr, expr_set*> expr_list_map;

    ast_manager&       m;
    params_ref         m_params;
    bool               m_propagate_eq;
    bv_util            m_bv;
    vector<undo_bound> m_scopes;
    map                m_bound;
    expr_list_map      m_expr_vars;
    expr_set           m_bound_exprs;

    bool is_number(expr *e, uint64_t& n, unsigned& sz) const {
        rational r;
        if (m_bv.is_numeral(e, r, sz) && sz <= 64) {
            n = r.get_uint64();
            return true;
        }
        return false;
    }

    bool is_bound(expr *e, expr*& v, interval& b) const {
        uint64_t n;
        expr *lhs, *rhs;
        unsigned sz;

        if (m_bv.is_bv_ule(e, lhs, rhs)) {
            if (is_number(lhs, n, sz)) { // C ule x <=> x uge C
                if (m_bv.is_numeral(rhs))
                    return false;
                b = interval(n, uMaxInt(sz), sz, true);
                v = rhs;
                return true;
            }
            if (is_number(rhs, n, sz)) { // x ule C
                b = interval(0, n, sz, true);
                v = lhs;
                return true;
            }
        } else if (m_bv.is_bv_sle(e, lhs, rhs)) {
            if (is_number(lhs, n, sz)) { // C sle x <=> x sge C
                if (m_bv.is_numeral(rhs))
                    return false;
                b = interval(n, (1ull << (sz-1)) - 1, sz, true);
                v = rhs;
                return true;
            }
            if (is_number(rhs, n, sz)) { // x sle C
                b = interval(1ull << (sz-1), n, sz, true);
                v = lhs;
                return true;
            }
        } else if (m.is_eq(e, lhs, rhs)) {
            if (is_number(lhs, n, sz)) {
                if (m_bv.is_numeral(rhs))
                    return false;
                b = interval(n, n, sz, true);
                v = rhs;
                return true;
            }
            if (is_number(rhs, n, sz)) {
                b = interval(n, n, sz, true);
                v = lhs;
                return true;
            }
        }
        return false;
    }

    expr_set* get_expr_vars(expr* t) {
        expr_set*& entry = m_expr_vars.insert_if_not_there2(t, 0)->get_data().m_value;
        if (entry)
            return entry;

        expr_set* set = alloc(expr_set);
        entry = set;

        if (!m_bv.is_numeral(t))
            set->insert(t, true);

        if (!is_app(t))
            return set;

        app* a = to_app(t);
        for (unsigned i = 0; i < a->get_num_args(); ++i) {
            expr_set* set_arg = get_expr_vars(a->get_arg(i));
            for (expr_set::iterator I = set_arg->begin(), E = set_arg->end(); I != E; ++I) {
                set->insert(I->m_key, true);
            }
        }
        return set;
    }

    bool expr_has_bounds(expr* t) {
        bool has_bounds = false;
        if (m_bound_exprs.find(t, has_bounds))
            return has_bounds;

        app* a = to_app(t);
        if ((m_bv.is_bv_ule(t) || m_bv.is_bv_sle(t) || m.is_eq(t)) &&
            (m_bv.is_numeral(a->get_arg(0)) || m_bv.is_numeral(a->get_arg(1)))) {
            has_bounds = true;
        }

        for (unsigned i = 0; !has_bounds && i < a->get_num_args(); ++i) {
            has_bounds = expr_has_bounds(a->get_arg(i));
        }

        m_bound_exprs.insert(t, has_bounds);
        return has_bounds;
    }

public:
    bv_bounds_simplifier(ast_manager& m, params_ref const& p) : m(m), m_params(p), m_bv(m) {
        updt_params(p);
    }

    virtual void updt_params(params_ref const & p) {
        m_propagate_eq = p.get_bool("propagate_eq", false);
    }

    static void get_param_descrs(param_descrs& r) {
        r.insert("propagate-eq", CPK_BOOL, "(default: false) propagate equalities from inequalities");
    }

    virtual ~bv_bounds_simplifier() {
        for (expr_list_map::iterator I = m_expr_vars.begin(), E = m_expr_vars.end(); I != E; ++I) {
            dealloc(I->m_value);
        }
    }

    virtual bool assert_expr(expr * t, bool sign) {
        while (m.is_not(t, t)) {
            sign = !sign;
        }

        interval b;
        expr* t1;
        if (is_bound(t, t1, b)) {
            SASSERT(!m_bv.is_numeral(t1));
            if (sign)
                VERIFY(b.negate(b));

            TRACE("bv", tout << (sign?"(not ":"") << mk_pp(t, m) << (sign ? ")" : "") << ": " << mk_pp(t1, m) << " in " << b << "\n";);
            map::obj_map_entry* e = m_bound.find_core(t1);
            if (e) {
                interval& old = e->get_data().m_value;
                interval intr;
                if (!old.intersect(b, intr))
                    return false;
                if (old == intr)
                    return true;
                m_scopes.insert(undo_bound(t1, old, false));
                old = intr;
            } else {
                m_bound.insert(t1, b);
                m_scopes.insert(undo_bound(t1, interval(), true));
            }
        }
        return true;
    }

    virtual bool simplify(expr* t, expr_ref& result) {
        expr* t1;
        interval b;

        if (m_bound.find(t, b) && b.is_singleton()) {
            result = m_bv.mk_numeral(b.l, m_bv.get_bv_size(t));
            return true;
        }

        if (!m.is_bool(t))
            return false;

        bool sign = false;
        while (m.is_not(t, t)) {
            sign = !sign;
        }

        if (!is_bound(t, t1, b))
            return false;

        if (sign && b.tight) {
            sign = false;
            if (!b.negate(b)) {
                result = m.mk_false();
                return true;
            }
        }

        interval ctx, intr;
        result = 0;

        if (b.is_full() && b.tight) {
            result = m.mk_true();
        } else if (m_bound.find(t1, ctx)) {
            if (ctx.implies(b)) {
                result = m.mk_true();
            } else if (!b.intersect(ctx, intr)) {
                result = m.mk_false();
            } else if (m_propagate_eq && intr.is_singleton()) {
                result = m.mk_eq(t1, m_bv.mk_numeral(rational(intr.l, rational::ui64()),
                                                     m.get_sort(t1)));
            }
        }

        CTRACE("bv", result != 0, tout << mk_pp(t, m) << " " << b << " (ctx: " << ctx << ") (intr: " << intr << "): " << result << "\n";);
        if (sign && result != 0)
            result = m.mk_not(result);
        return result != 0;
    }

    virtual bool may_simplify(expr* t) {
        if (m_bv.is_numeral(t))
            return false;

        while (m.is_not(t, t));

        expr_set* used_exprs = get_expr_vars(t);
        for (map::iterator I = m_bound.begin(), E = m_bound.end(); I != E; ++I) {
            if (I->m_value.is_singleton() && used_exprs->contains(I->m_key))
                return true;
        }

        expr* t1;
        interval b;
        // skip common case: single bound constraint without any context for simplification
        if (is_bound(t, t1, b)) {
            return b.is_full() || m_bound.contains(t1);
        }
        return expr_has_bounds(t);
    }

    virtual void pop(unsigned num_scopes) {
        TRACE("bv", tout << "pop: " << num_scopes << "\n";);
        if (m_scopes.empty())
            return;
        unsigned target = m_scopes.size() - num_scopes;
        if (target == 0) {
            m_bound.reset();
            m_scopes.reset();
            return;
        }
        for (unsigned i = m_scopes.size()-1; i >= target; --i) {
            undo_bound& undo = m_scopes[i];
            SASSERT(m_bound.contains(undo.e));
            if (undo.fresh) {
                m_bound.erase(undo.e);
            } else {
                m_bound.insert(undo.e, undo.b);
            }
        }
        m_scopes.shrink(target);
    }

    virtual simplifier * translate(ast_manager & m) {
        return alloc(bv_bounds_simplifier, m, m_params);
    }

    virtual unsigned scope_level() const {
        return m_scopes.size();
    }
};

}

tactic * mk_bv_bounds_tactic(ast_manager & m, params_ref const & p) {
    return clean(alloc(ctx_simplify_tactic, m, alloc(bv_bounds_simplifier, m, p), p));
}